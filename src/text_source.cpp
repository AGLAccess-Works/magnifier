#include "text_source.h"

#include <ole2.h>  // WIN32_LEAN_AND_MEAN leaves out what UIAutomation.h needs
#include <UIAutomation.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <spellcheck.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <cmath>
#include <map>
#include <set>

#include "fonts.h"
#include "image_io.h"
#include "ocr.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kScanInterval = 3.0;      // seconds between routine rescans
constexpr double kOcrMinGap = 0.1;         // seconds between OCR passes while the screen changes
constexpr int kMaxLines = 300, kMaxRunsPerLine = 64;
bool g_traceScan = false;  // set by --scan-probe
std::atomic<bool> g_traceOcr = false;  // set by the text tests: log each OCR word's look-alike check

double Seconds() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / f.QuadPart;
}

bool Intersects(const RectD& a, const RectD& b) { return a.r > b.l && a.l < b.r && a.b > b.t && a.t < b.b; }

// Stand-ins chosen for app text in fonts that are not installed (web fonts),
// by reported style: shared by the checker and the OCR threads, so every
// line and word of a style is drawn alike.
struct Substitutes {
    struct Entry {
        std::wstring family;
        int weight = 400;
    };
    std::mutex mu;
    std::map<std::wstring, Entry> byStyle;

    static std::wstring Key(const std::wstring& font, double sizePx, int weight, bool italic) {
        wchar_t k[64];
        swprintf_s(k, L"|%.0f|%d|%d", sizePx * 2, weight, italic ? 1 : 0);
        return font + k;
    }
    bool Find(const std::wstring& key, Entry& out) {
        std::lock_guard l(mu);
        auto it = byStyle.find(key);
        if (it == byStyle.end()) return false;
        out = it->second;
        return true;
    }
    void Set(const std::wstring& key, Entry e) {
        std::lock_guard l(mu);
        byStyle[key] = std::move(e);
    }
};
Substitutes g_substitutes;

RectD Expand(const RectD& r, double fx, double fy) {
    double dx = r.Width() * fx, dy = r.Height() * fy;
    return {r.l - dx, r.t - dy, r.r + dx, r.b + dy};
}

// ---------------------------------------------------------------- pixels

struct Rgb {
    double r, g, b;
};
Rgb ToRgb(uint32_t bgra) {
    return {((bgra >> 16) & 0xFF) / 255.0, ((bgra >> 8) & 0xFF) / 255.0, (bgra & 0xFF) / 255.0};
}
uint32_t ToHex(const Rgb& c) {
    auto q = [](double v) { return (uint32_t)std::clamp((int)std::lround(v * 255), 0, 255); };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}
double Luma(const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

// Integer pixel bounds of r, clipped to the frame.
bool PixelBounds(const CpuFrame& f, const RectD& r, int& x0, int& y0, int& x1, int& y1) {
    x0 = (std::max)((int)std::floor(r.l), f.left);
    y0 = (std::max)((int)std::floor(r.t), f.top);
    x1 = (std::min)((int)std::ceil(r.r), f.left + f.width);
    y1 = (std::min)((int)std::ceil(r.b), f.top + f.height);
    return x1 > x0 && y1 > y0;
}

// True if the pixels under r are (nearly) the same in both frames. A blinking
// caret, about one pixel wide, is tolerated.
bool Unchanged(const CpuFrame& a, const CpuFrame& b, const RectD& r) {
    int x0, y0, x1, y1;
    if (!PixelBounds(a, r, x0, y0, x1, y1) || !b.Contains({(double)x0, (double)y0, (double)x1, (double)y1}))
        return false;
    int changed = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            uint32_t p = a.At(x, y), q = b.At(x, y);
            if (p == q) continue;
            int d = (std::max)({std::abs((int)(p & 0xFF) - (int)(q & 0xFF)),
                                std::abs((int)((p >> 8) & 0xFF) - (int)((q >> 8) & 0xFF)),
                                std::abs((int)((p >> 16) & 0xFF) - (int)((q >> 16) & 0xFF))});
            if (d > 24) ++changed;
        }
    return changed <= 2 * (y1 - y0) + 4;
}

// Checks that r holds plain text: one background colour, one ink colour and
// anti-aliased mixes of the two (ClearType fringes allowed), with some ink.
// Fills in the measured background and ink colours.
bool PlainText(const CpuFrame& f, const RectD& r, uint32_t appForeground, bool haveForeground, uint32_t& bgOut,
               uint32_t& fgOut, BgSlope* slope = nullptr) {
    int x0, y0, x1, y1;
    if (!PixelBounds(f, r, x0, y0, x1, y1)) return false;
    const int total = (x1 - x0) * (y1 - y0);
    if (total < 16) return false;

    // Background: the most common colour (5 bits per channel).
    std::map<uint32_t, int> counts;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) ++counts[f.At(x, y) & 0xF0F0F0];
    auto best = std::max_element(counts.begin(), counts.end(),
                                 [](const auto& a, const auto& b) { return a.second < b.second; });
    if (best->second < total * 0.3) return false;  // no dominant background
    Rgb bgSum = {0, 0, 0};
    int bgN = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if ((f.At(x, y) & 0xF0F0F0) == best->first) {
                Rgb c = ToRgb(f.At(x, y));
                bgSum.r += c.r, bgSum.g += c.g, bgSum.b += c.b, ++bgN;
            }
    Rgb bg = {bgSum.r / bgN, bgSum.g / bgN, bgSum.b / bgN};
    double yBg = Luma(bg);

    // Ink: the app's foreground if it really contrasts with the background,
    // otherwise the pixel furthest from the background in brightness.
    Rgb fg = ToRgb(appForeground);
    if (!haveForeground || std::fabs(Luma(fg) - yBg) < 0.25) {
        double furthest = -1;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                Rgb c = ToRgb(f.At(x, y));
                double d = std::fabs(Luma(c) - yBg);
                if (d > furthest) furthest = d, fg = c;
            }
    }
    double yFg = Luma(fg);
    if (std::fabs(yFg - yBg) < 0.2) return false;

    // Every pixel should be a mix of the two. Brightness must sit between
    // them; colour may stray a little (ClearType).
    int ink = 0, stray = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            Rgb c = ToRgb(f.At(x, y));
            double t = (Luma(c) - yBg) / (yFg - yBg);
            if (t > 0.3) ++ink;
            Rgb mix = {bg.r + (fg.r - bg.r) * std::clamp(t, 0.0, 1.0), bg.g + (fg.g - bg.g) * std::clamp(t, 0.0, 1.0),
                       bg.b + (fg.b - bg.b) * std::clamp(t, 0.0, 1.0)};
            double chroma = (std::max)({std::fabs(c.r - mix.r), std::fabs(c.g - mix.g), std::fabs(c.b - mix.b)});
            if (t < -0.15 || t > 1.15 || chroma > 0.55) ++stray;
        }
    if (stray > total * 0.02 || ink < total * 0.01) return false;

    // The redraw repaints the box, so the background must be flat out to the
    // box's edge, or change smoothly enough to repaint as a gradient (a
    // window's shadow, a tinted panel): a plane of colour is fitted to the
    // pixels well clear of ink (ClearType fringes beside a stroke are near
    // the background in brightness but not in colour) and must match them.
    // Text on photos and textures fails, and keeps the smoothed pixels.
    Rgb centre = bg;
    BgSlope fit;
    {
        int ex0, ey0, ex1, ey1;
        PixelBounds(f, {r.l - 2, r.t - 2, r.r + 2, r.b + 2}, ex0, ey0, ex1, ey1);
        const int ew = ex1 - ex0, eh = ey1 - ey0;
        std::vector<char> clear((size_t)ew * eh);
        for (int y = 0; y < eh; ++y)
            for (int x = 0; x < ew; ++x) {
                const double t = (Luma(ToRgb(f.At(ex0 + x, ey0 + y))) - yBg) / (yFg - yBg);
                clear[(size_t)y * ew + x] = t > -0.15 && t < 0.08;
            }
        auto away = [&](int x, int y) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (!clear[(size_t)(y + dy) * ew + x + dx]) return false;
            return true;
        };
        const double cx = (r.l + r.r) / 2, cy = (r.t + r.b) / 2;
        // Least squares: value = a + b (x - cx) + c (y - cy), per channel.
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0, sv[3] = {}, sxv[3] = {}, syv[3] = {};
        for (int y = 1; y + 1 < eh; ++y)
            for (int x = 1; x + 1 < ew; ++x) {
                if (!away(x, y)) continue;
                const double px = ex0 + x + 0.5 - cx, py = ey0 + y + 0.5 - cy;
                const Rgb c = ToRgb(f.At(ex0 + x, ey0 + y));
                const double v[3] = {c.r, c.g, c.b};
                n += 1, sx += px, sy += py, sxx += px * px, sxy += px * py, syy += py * py;
                for (int k = 0; k < 3; ++k) sv[k] += v[k], sxv[k] += px * v[k], syv[k] += py * v[k];
            }
        if (n >= 12) {
            const double det = n * (sxx * syy - sxy * sxy) - sx * (sx * syy - sxy * sy) + sy * (sx * sxy - sxx * sy);
            double a[3], gx[3], gy[3];
            for (int k = 0; k < 3; ++k) {
                if (std::fabs(det) < 1e-6) {
                    a[k] = sv[k] / n, gx[k] = gy[k] = 0;  // too few rows or columns for a slope
                    continue;
                }
                // Cramer's rule on the 3x3 normal equations.
                a[k] = (sv[k] * (sxx * syy - sxy * sxy) - sx * (sxv[k] * syy - sxy * syv[k]) + sy * (sxv[k] * sxy - sxx * syv[k])) / det;
                gx[k] = (n * (sxv[k] * syy - sxy * syv[k]) - sv[k] * (sx * syy - sxy * sy) + sy * (sx * syv[k] - sxv[k] * sy)) / det;
                gy[k] = (n * (sxx * syv[k] - sxv[k] * sxy) - sx * (sx * syv[k] - sxv[k] * sy) + sv[k] * (sx * sxy - sxx * sy)) / det;
            }
            int off = 0;
            for (int y = 1; y + 1 < eh; ++y)
                for (int x = 1; x + 1 < ew; ++x) {
                    if (!away(x, y)) continue;
                    const double px = ex0 + x + 0.5 - cx, py = ey0 + y + 0.5 - cy;
                    const Rgb c = ToRgb(f.At(ex0 + x, ey0 + y));
                    const double v[3] = {c.r, c.g, c.b};
                    double worst = 0;
                    for (int k = 0; k < 3; ++k) worst = (std::max)(worst, std::fabs(v[k] - (a[k] + gx[k] * px + gy[k] * py)));
                    if (worst > 4.5 / 255) ++off;
                }
            if (off > n * 0.03) return false;
            centre = {std::clamp(a[0], 0.0, 1.0), std::clamp(a[1], 0.0, 1.0), std::clamp(a[2], 0.0, 1.0)};
            // A slope too small to see across the box is treated as flat.
            const double w = ex1 - ex0, h = ey1 - ey0;
            bool visible = false;
            for (int k = 0; k < 3; ++k) visible |= std::fabs(gx[k]) * w > 2.0 / 255 || std::fabs(gy[k]) * h > 2.0 / 255;
            if (visible)
                for (int k = 0; k < 3; ++k) fit.dx[k] = (float)gx[k], fit.dy[k] = (float)gy[k];
            else
                centre = bg;
        }
    }
    if (!fit.Flat() && !slope) return false;  // the caller cannot draw a gradient
    if (slope) *slope = fit;
    bgOut = ToHex(centre);
    fgOut = ToHex(fg);
    return true;
}

// ---------------------------------------------------------------- redraw check

// Draws a run the way the renderer will, but at its original size, and
// compares it with the screen. Both images are blurred slightly so a similar
// font still matches, while different content (an icon reported as a letter,
// stale text) does not. The best of nine one-pixel nudges is kept.
struct Verifier {
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteFontCollection> fonts;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IWICImagingFactory> wic;
    std::unique_ptr<FontResolver> resolver;
    // Fonts the apps in view report for their own text: Electron and web
    // apps draw their control labels in these too.
    std::vector<std::wstring> appFonts;
    // Windows' spell checker in the user's language, to tell a real word
    // from a look-alike misreading ("modern" from "rnodern"). May be absent.
    ComPtr<ISpellChecker> speller;
    // The surface redraws are drawn on (see DrawModel), kept between draws
    // and grown as needed: making one per draw cost more than the drawing.
    ComPtr<IWICBitmap> canvas;
    ComPtr<ID2D1RenderTarget> canvasRt;
    ComPtr<ID2D1SolidColorBrush> white;
    int canvasW = 0, canvasH = 0;

    bool Init() {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))) ||
            FAILED(dwrite->GetSystemFontCollection(&fonts, FALSE)) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
            return false;
        resolver = std::make_unique<FontResolver>(fonts.Get());
        ComPtr<ISpellCheckerFactory> spelling;
        if (SUCCEEDED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&spelling)))) {
            wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"en-GB";
            GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
            for (const wchar_t* lang : {(const wchar_t*)locale, L"en-GB", L"en-US"}) {
                BOOL ok = FALSE;
                if (SUCCEEDED(spelling->IsSupported(lang, &ok)) && ok &&
                    SUCCEEDED(spelling->CreateSpellChecker(lang, &speller)))
                    break;
            }
        }
        return true;
    }

    // Whether the spell checker knows word (letters, perhaps hyphenated;
    // false without a spell checker).
    bool IsWord(const std::wstring& word) {
        if (!speller || word.empty()) return false;
        ComPtr<IEnumSpellingError> errors;
        if (FAILED(speller->Check(word.c_str(), &errors)) || !errors) return false;
        ComPtr<ISpellingError> error;
        return errors->Next(&error) != S_OK;
    }

    // A word the spell checker suggests for word that is at most two letters
    // different ("Outbox" for "Outbu"), or empty.
    std::wstring NearWord(const std::wstring& word) {
        ComPtr<IEnumString> suggestions;
        if (!speller || FAILED(speller->Suggest(word.c_str(), &suggestions)) || !suggestions) return L"";
        for (int i = 0; i < 8; ++i) {
            LPOLESTR s = nullptr;
            if (suggestions->Next(1, &s, nullptr) != S_OK || !s) break;
            std::wstring w = s;
            CoTaskMemFree(s);
            if (EditDistance(word, w) <= 2) return w;
        }
        return L"";
    }

    static int EditDistance(const std::wstring& a, const std::wstring& b) {
        std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
        for (size_t i = 1; i <= a.size(); ++i) {
            cur[0] = (int)i;
            for (size_t j = 1; j <= b.size(); ++j)
                cur[j] = (std::min)({prev[j] + 1, cur[j - 1] + 1,
                                     prev[j - 1] + (towlower(a[i - 1]) == towlower(b[j - 1]) ? 0 : 1)});
            std::swap(prev, cur);
        }
        return prev[b.size()];
    }
};

void BoxBlur(std::vector<float>& v, int w, int h) {
    std::vector<float> t(v.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            int n = 0;
            for (int k = -1; k <= 1; ++k)
                if (x + k >= 0 && x + k < w) s += v[(size_t)y * w + x + k], ++n;
            t[(size_t)y * w + x] = s / n;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            int n = 0;
            for (int k = -1; k <= 1; ++k)
                if (y + k >= 0 && y + k < h) s += t[(size_t)(y + k) * w + x], ++n;
            v[(size_t)y * w + x] = s / n;
        }
}

// ---------------------------------------------------------------- UIA

struct Uia {
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationTreeWalker> walker;
    ComPtr<IUnknown> mixed, notSupported;
};

bool Variant(IUIAutomationTextRange* r, TEXTATTRIBUTEID id, VARIANT& v) {
    VariantInit(&v);
    if (FAILED(r->GetAttributeValue(id, &v))) return false;
    if (v.vt == VT_UNKNOWN || v.vt == VT_EMPTY) {  // mixed or not supported
        VariantClear(&v);
        return false;
    }
    return true;
}

bool RangeRect(IUIAutomationTextRange* r, RectD& out) {
    SAFEARRAY* sa = nullptr;
    if (FAILED(r->GetBoundingRectangles(&sa)) || !sa) return false;
    double* d = nullptr;
    LONG lo = 0, hi = -1;
    SafeArrayGetLBound(sa, 1, &lo);
    SafeArrayGetUBound(sa, 1, &hi);
    LONG n = hi - lo + 1;
    bool ok = false;
    if (n >= 4 && SUCCEEDED(SafeArrayAccessData(sa, (void**)&d))) {
        // One rectangle per line; a run inside one line has exactly one.
        if (n == 4 && d[2] > 0 && d[3] > 0) {
            out = {d[0], d[1], d[0] + d[2], d[1] + d[3]};
            ok = true;
        }
        SafeArrayUnaccessData(sa);
    }
    SafeArrayDestroy(sa);
    return ok;
}

std::wstring RangeText(IUIAutomationTextRange* r) {
    BSTR b = nullptr;
    std::wstring s;
    if (SUCCEEDED(r->GetText(-1, &b)) && b) s.assign(b, SysStringLen(b));
    SysFreeString(b);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

double MonitorDpi(Vec2 p) {
    HMONITOR m = MonitorFromPoint({(LONG)p.x, (LONG)p.y}, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy);
    return dx;
}

// The nearest element at or above the point that supports the Text pattern.
ComPtr<IUIAutomationTextPattern> FindTextHost(Uia& uia, Vec2 p) {
    ComPtr<IUIAutomationElement> e;
    if (FAILED(uia.automation->ElementFromPoint({(LONG)p.x, (LONG)p.y}, &e)) || !e) return nullptr;
    int pid = 0;
    e->get_CurrentProcessId(&pid);
    if ((DWORD)pid == GetCurrentProcessId()) return nullptr;
    for (int depth = 0; e && depth < 15; ++depth) {
        ComPtr<IUIAutomationTextPattern> tp;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&tp))) && tp) return tp;
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(uia.walker->GetParentElement(e.Get(), &parent))) break;
        e = parent;
    }
    return nullptr;
}

// ---------------------------------------------------------------- reference pixels

// Remembers the pixels under a run (its draw area plus a pixel of margin).
void TakeCrop(const CpuFrame& f, TextRun& r) {
    RectD area = {r.rect.l + r.offsetX - 1, r.rect.t + r.offsetY - 1, r.rect.r + r.offsetX + 1,
                  r.rect.b + r.offsetY + 1};
    int x0, y0, x1, y1;
    r.ref.reset();
    if (!PixelBounds(f, area, x0, y0, x1, y1)) return;
    auto px = std::make_shared<std::vector<uint32_t>>((size_t)(x1 - x0) * (y1 - y0));
    for (int y = y0; y < y1; ++y)
        memcpy(&(*px)[(size_t)(y - y0) * (x1 - x0)], &f.pixels[(size_t)(y - f.top) * f.width + (x0 - f.left)],
               (size_t)(x1 - x0) * 4);
    r.refX = x0, r.refY = y0, r.refW = x1 - x0, r.refH = y1 - y0;
    r.ref = std::move(px);
}

// True if the run's pixels are as they were when it was verified (a blinking
// caret, about a pixel wide, is tolerated).
bool CropUnchanged(const TextRun& r, const CpuFrame& f) {
    if (!r.ref || r.refX < f.left || r.refY < f.top || r.refX + r.refW > f.left + f.width ||
        r.refY + r.refH > f.top + f.height)
        return false;
    int changed = 0;
    const int allowed = 2 * r.refH + 4;
    for (int y = 0; y < r.refH; ++y) {
        const uint32_t* a = &(*r.ref)[(size_t)y * r.refW];
        const uint32_t* b = &f.pixels[(size_t)(r.refY + y - f.top) * f.width + (r.refX - f.left)];
        if (memcmp(a, b, (size_t)r.refW * 4) == 0) continue;
        for (int x = 0; x < r.refW; ++x) {
            uint32_t p = a[x], q = b[x];
            if (p == q) continue;
            int d = (std::max)({std::abs((int)(p & 0xFF) - (int)(q & 0xFF)),
                                std::abs((int)((p >> 8) & 0xFF) - (int)((q >> 8) & 0xFF)),
                                std::abs((int)((p >> 16) & 0xFF) - (int)((q >> 16) & 0xFF))});
            if (d > 24 && ++changed > allowed) return false;
        }
    }
    return true;
}

// The background (most common colour) and ink (furthest in brightness) of a box.
bool BoxColours(const CpuFrame& f, int x0, int y0, int x1, int y1, uint32_t& bgOut, uint32_t& fgOut) {
    // Coarse colour groups (4 bits a channel) so a slightly noisy background,
    // like the Start menu's acrylic, still has one dominant group.
    std::map<uint32_t, int> counts;
    const int total = (x1 - x0) * (y1 - y0);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) ++counts[f.At(x, y) & 0xF0F0F0];
    auto best = std::max_element(counts.begin(), counts.end(),
                                 [](const auto& a, const auto& b) { return a.second < b.second; });
    if (best->second < total * 0.25) return false;
    Rgb sum = {0, 0, 0};
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if ((f.At(x, y) & 0xF0F0F0) == best->first) {
                Rgb c = ToRgb(f.At(x, y));
                sum.r += c.r, sum.g += c.g, sum.b += c.b, ++n;
            }
    Rgb bg = {sum.r / n, sum.g / n, sum.b / n}, fg = bg;
    double furthest = -1;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            Rgb c = ToRgb(f.At(x, y));
            double d = std::fabs(Luma(c) - Luma(bg));
            if (d > furthest) furthest = d, fg = c;
        }
    if (furthest < 0.25) return false;
    bgOut = ToHex(bg);
    fgOut = ToHex(fg);
    return true;
}

// Screen ink coverage (0..1) over a w x h area, reads clamped to the frame.
std::vector<float> Coverage(const CpuFrame& f, int x0, int y0, int w, int h, double yBg, double yFg) {
    std::vector<float> c((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sx = std::clamp(x0 + x, f.left, f.left + f.width - 1);
            int sy = std::clamp(y0 + y, f.top, f.top + f.height - 1);
            double v = (Luma(ToRgb(f.At(sx, sy))) - yBg) / (yFg - yBg);
            c[(size_t)y * w + x] = (float)std::clamp(v, 0.0, 1.0);
        }
    return c;
}

// Pearson correlation of a w x h model with a screen buffer at (ox, oy).
double Correlate(const std::vector<float>& m, int w, int h, const std::vector<float>& s, int sw, int sh, int ox,
                 int oy) {
    if (ox < 0 || oy < 0 || ox + w > sw || oy + h > sh) return -1;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    const double n = (double)w * h;
    for (int y = 0; y < h; ++y) {
        const float* a = &m[(size_t)y * w];
        const float* b = &s[(size_t)(oy + y) * sw + ox];
        for (int x = 0; x < w; ++x) sa += a[x], sb += b[x], saa += a[x] * a[x], sbb += b[x] * b[x], sab += a[x] * b[x];
    }
    double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    if (va <= 1e-6 || vb <= 1e-6) return -1;
    return (sab - sa * sb / n) / std::sqrt(va * vb);
}

// Larger text is compared at a coarser scale: a different typeface (a web
// font that is not installed) differs in detail but not in overall shape,
// while different words differ in both.
int BlurPasses(double sizePx) { return std::clamp((int)std::lround(sizePx / 16.0), 1, 4); }

void BoxBlurN(std::vector<float>& v, int w, int h, int passes) {
    for (int i = 0; i < passes; ++i) BoxBlur(v, w, h);
}

// ---------------------------------------------------------------- redraw model

struct Laid {
    ComPtr<IDWriteTextLayout> layout;
    float width = 0, baseline = 0;
};

bool LayOut(Verifier& v, const std::wstring& text, const std::wstring& family, int weight, bool italic,
            double sizePx, Laid& out) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(v.dwrite->CreateTextFormat(family.c_str(), nullptr, (DWRITE_FONT_WEIGHT)weight,
                                          italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, (float)(std::max)(sizePx, 1.0), L"", &format)))
        return false;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (FAILED(v.dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), format.Get(), 1e6f, (float)sizePx * 4,
                                          &out.layout)))
        return false;
    DWRITE_TEXT_METRICS tm;
    DWRITE_LINE_METRICS lm;
    UINT32 lines = 0;
    if (FAILED(out.layout->GetMetrics(&tm)) || FAILED(out.layout->GetLineMetrics(&lm, 1, &lines))) return false;
    out.width = tm.widthIncludingTrailingWhitespace;
    out.baseline = lm.baseline;
    return out.width > 0;
}

// Draws a layout white on black into a w x h coverage buffer.
bool DrawModel(Verifier& v, IDWriteTextLayout* layout, int w, int h, float left, float originY, float scaleX,
               std::vector<float>& cov) {
    if (w <= 0 || h <= 0) return false;
    if (w > v.canvasW || h > v.canvasH) {
        // Grow with room to spare, so most later draws fit.
        const int cw = (std::max)(w + 64, v.canvasW), ch = (std::max)(h + 16, v.canvasH);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        v.canvasRt.Reset();
        v.white.Reset();
        v.canvas.Reset();
        v.canvasW = v.canvasH = 0;
        if (FAILED(v.wic->CreateBitmap(cw, ch, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &v.canvas)) ||
            FAILED(v.d2d->CreateWicBitmapRenderTarget(v.canvas.Get(), &props, &v.canvasRt)) ||
            FAILED(v.canvasRt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &v.white)))
            return false;
        v.canvasRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        v.canvasW = cw, v.canvasH = ch;
    }
    ID2D1RenderTarget* rt = v.canvasRt.Get();
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    // Only the w x h corner is drawn and read; the clip keeps the rest out.
    rt->PushAxisAlignedClip(D2D1::RectF(0, 0, (float)w, (float)h), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->Clear(D2D1::ColorF(0, 0, 0, 1));
    rt->SetTransform(D2D1::Matrix3x2F::Scale(scaleX, 1.0f, D2D1::Point2F(left, 0)));
    rt->DrawTextLayout({left, originY}, layout, v.white.Get());
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->PopAxisAlignedClip();
    if (FAILED(rt->EndDraw())) {
        v.canvasW = v.canvasH = 0;  // made afresh next time
        return false;
    }
    WICRect all = {0, 0, w, h};
    ComPtr<IWICBitmapLock> lock;
    UINT size = 0, stride = 0;
    BYTE* data = nullptr;
    if (FAILED(v.canvas->Lock(&all, WICBitmapLockRead, &lock)) || FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&size, &data)))
        return false;
    cov.resize((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) cov[(size_t)y * w + x] = data[(size_t)y * stride + x * 4 + 2] / 255.0f;
    return true;
}

// Draws a run as the renderer will, at source size, and compares it with the
// screen (both blurred slightly, best of nine one-pixel nudges). Returns the
// correlation and sets offsetX/offsetY; -1 on failure.
double MatchRun(Verifier& v, const CpuFrame& f, TextRun& run, const std::wstring& fallback, bool useAppFont) {
    const int margin = 3;
    const int x0 = (int)std::floor(run.rect.l) - margin, y0 = (int)std::floor(run.rect.t) - margin;
    const int w = (int)std::ceil(run.rect.r) + margin - x0, h = (int)std::ceil(run.rect.b) + margin - y0;
    if (w <= 2 * margin || h <= 2 * margin || w > 4000 || h > 400) return -1;

    ResolvedFont font = v.resolver->Resolve(run.font, fallback, useAppFont, run.weight, run.italic);
    Laid laid;
    if (!LayOut(v, run.text, font.family, run.weight, run.italic, run.sizePx, laid)) return -1;
    const float left = (float)(run.rect.l - x0), top = (float)(run.rect.t - y0);
    TextPlacement place =
        PlaceText(top, (float)run.rect.Height(), (float)run.rect.Width(), laid.width, laid.baseline, font.ascentShare);
    std::vector<float> model;
    if (!DrawModel(v, laid.layout.Get(), w, h, left, place.originY, place.scaleX, model)) return -1;

    const int sw = w + 2, sh = h + 2;  // one extra pixel each side for the nudges
    std::vector<float> screen = Coverage(f, x0 - 1, y0 - 1, sw, sh, Luma(ToRgb(run.background)),
                                         Luma(ToRgb(run.foreground)));
    BoxBlurN(model, w, h, BlurPasses(run.sizePx));
    BoxBlurN(screen, sw, sh, BlurPasses(run.sizePx));
    double best = -1;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            double r = Correlate(model, w, h, screen, sw, sh, 1 + dx, 1 + dy);
            if (r > best) best = r, run.offsetX = dx, run.offsetY = dy;
        }
    return best;
}

// When the app's font is not installed, the substitute's glyph widths differ
// a little, so a long run drifts against the screen even when stretched to
// the right overall width. Place each word separately instead: lay the run
// out once for the expected positions, then search a few pixels around each
// word for where the app really drew it. Returns the length-weighted mean
// match (every word must match reasonably) and fills run.words; -1 on failure.
double MatchWords(Verifier& v, const CpuFrame& f, TextRun& run, const std::wstring& fallback, bool useAppFont) {
    // Word boundaries.
    std::vector<WordPlace> words;
    for (size_t i = 0; i < run.text.size();) {
        while (i < run.text.size() && iswspace(run.text[i])) ++i;
        size_t j = i;
        while (j < run.text.size() && !iswspace(run.text[j])) ++j;
        if (j > i) words.push_back({(int)i, (int)(j - i), 0, 0});
        i = j;
    }
    if (words.size() < 2 || words.size() > 80) return -1;

    ResolvedFont font = v.resolver->Resolve(run.font, fallback, useAppFont, run.weight, run.italic);
    Laid whole;
    if (!LayOut(v, run.text, font.family, run.weight, run.italic, run.sizePx, whole)) return -1;
    TextPlacement place = PlaceText((float)run.rect.t, (float)run.rect.Height(), (float)run.rect.Width(), whole.width,
                                    whole.baseline, font.ascentShare);

    const int search = 4, margin = 3;
    const int y0 = (int)std::floor(run.rect.t) - margin - 1;
    const int h = (int)std::ceil(run.rect.b) + margin + 1 - y0;
    double sum = 0, weight = 0, worst = 2;
    for (WordPlace& w : words) {
        // Expected left edge of the word on screen.
        FLOAT hx = 0, hy = 0;
        DWRITE_HIT_TEST_METRICS hm;
        if (FAILED(whole.layout->HitTestTextPosition(w.start, FALSE, &hx, &hy, &hm))) return -1;
        const double expected = run.rect.l + hx * place.scaleX;
        Laid laid;
        if (!LayOut(v, run.text.substr(w.start, w.length), font.family, run.weight, run.italic, run.sizePx, laid))
            return -1;
        const int mw = (int)std::ceil(laid.width * place.scaleX) + 2 * margin;
        std::vector<float> model;
        if (!DrawModel(v, laid.layout.Get(), mw, h, (float)margin, (float)(place.originY - y0), place.scaleX, model))
            return -1;
        BoxBlurN(model, mw, h, BlurPasses(run.sizePx));

        const int x0 = (int)std::floor(expected) - margin - search;
        const int sw = mw + 2 * search, sh = h + 2;
        std::vector<float> screen = Coverage(f, x0, y0 - 1, sw, sh, Luma(ToRgb(run.background)),
                                             Luma(ToRgb(run.foreground)));
        BoxBlurN(screen, sw, sh, BlurPasses(run.sizePx));
        double best = -1;
        int bestDx = 0, bestDy = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -search; dx <= search; ++dx) {
                double c = Correlate(model, mw, h, screen, sw, sh, search + dx, 1 + dy);
                if (c > best) best = c, bestDx = dx, bestDy = dy;
            }
        // Where the word's layout box starts on screen (the model draws it at
        // the margin, and the screen buffer starts margin+search to the left).
        w.x = std::floor(expected) + bestDx;
        w.dy = bestDy;
        sum += best * w.length;
        weight += w.length;
        worst = (std::min)(worst, best);
    }
    if (worst < 0.4) return -1;  // a word that does not match means the text is not what is drawn
    run.words = std::move(words);
    return sum / weight;
}

// Checks a run again against new pixels; on success refreshes its colours,
// nudge and reference pixels.
bool Reverify(Verifier& v, const CpuFrame& f, TextRun& r, const std::wstring& fallback, bool useAppFont,
              double minMatch) {
    TextRun t = r;
    t.offsetX = t.offsetY = 0;
    if (!PlainText(f, t.rect, t.foreground, true, t.background, t.foreground, &t.bgSlope)) return false;
    t.match = t.words.empty() ? MatchRun(v, f, t, fallback, useAppFont) : MatchWords(v, f, t, fallback, useAppFont);
    if (t.match < minMatch) return false;
    TakeCrop(f, t);
    r = std::move(t);
    return true;
}

// ---------------------------------------------------------------- control labels

struct UiFont {
    std::wstring family = L"Segoe UI";
    double sizePx = 12;
};

UiFont SystemUiFont(double dpi) {
    UiFont u;
    u.sizePx = 12 * dpi / 96;
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)dpi)) {
        if (ncm.lfMessageFont.lfFaceName[0]) u.family = ncm.lfMessageFont.lfFaceName;
        if (ncm.lfMessageFont.lfHeight) u.sizePx = std::abs(ncm.lfMessageFont.lfHeight);
    }
    return u;
}

// A control reports its label but not where in its box the words are drawn,
// nor the font, size or weight. Find them from the pixels: at each place ink
// starts in the box, estimate the size from the height of the ink, line the
// ink tops up and compare redraws; then try semibold and bold, and Windows
// 11's Segoe UI Variable, if regular does not clearly fit. On success the run
// becomes an ordinary placed run.
bool MatchLabel(Verifier& v, const CpuFrame& f, TextRun& r, double dpi, double minMatch, double centreX = -1,
                const std::wstring* familyOverride = nullptr, int blurPasses = 1, const TextRun* hint = nullptr) {
    int bx0, by0, bx1, by1;
    auto why = [&](const wchar_t* reason) {
        if (g_traceScan) Log(L"      '%s' %.0fx%.0f: %s", r.text.substr(0, 30).c_str(), r.source.Width(), r.source.Height(), reason);
        return false;
    };
    if (!PixelBounds(f, r.source, bx0, by0, bx1, by1)) return why(L"off frame");
    const int bw = bx1 - bx0, bh = by1 - by0;
    if (bw < 6 || bh < 6 || bw > 2400 || bh > 160 * dpi / 96) return why(L"box size");
    uint32_t bgHex, fgHex;
    if (!BoxColours(f, bx0, by0, bx1, by1, bgHex, fgHex)) return why(L"no background/ink");

    // Screen margin around the box: about a font height, so a redraw of text
    // that sits close to the control's edge (Start menu tiles) still fits.
    const int m = 3 + (int)std::ceil(SystemUiFont(dpi).sizePx);
    const int sx0 = bx0 - m, sy0 = by0 - m, sw = bw + 2 * m, sh = bh + 2 * m;
    std::vector<float> raw = Coverage(f, sx0, sy0, sw, sh, Luma(ToRgb(bgHex)), Luma(ToRgb(fgHex)));
    std::vector<float> screen = raw;
    BoxBlurN(screen, sw, sh, blurPasses);
    auto ink = [&](int x, int y) { return raw[(size_t)(y + m) * sw + x + m] > 0.45f; };  // box coords

    // Bands of rows with ink (an icon above the words is one band, the words
    // another), and in each band the columns where ink starts after a gap:
    // the candidate places for the text to begin.
    struct Band {
        int top, bottom;
        std::vector<int> starts;
    };
    std::vector<Band> bands;
    {
        int y = 0;
        while (y < bh && bands.size() < 4) {
            auto rowInk = [&](int yy) {
                for (int x = 0; x < bw; ++x)
                    if (ink(x, yy)) return true;
                return false;
            };
            while (y < bh && !rowInk(y)) ++y;
            if (y >= bh) break;
            int top = y, bottom = y, blank = 0;
            for (++y; y < bh; ++y) {
                if (rowInk(y)) bottom = y, blank = 0;
                else if (++blank > 2) break;
            }
            if (bottom - top + 1 < 3) continue;
            Band b = {top, bottom, {}};
            int gap = 2;
            for (int x = 0; x < bw && b.starts.size() < 12; ++x) {
                bool any = false;
                for (int yy = top; yy <= bottom && !any; ++yy) any = ink(x, yy);
                if (any && gap >= 2) b.starts.push_back(x);
                gap = any ? 0 : gap + 1;
            }
            if (!b.starts.empty()) bands.push_back(std::move(b));
        }
    }
    if (bands.empty()) return why(L"no ink bands");

    UiFont ui = SystemUiFont(dpi);
    if (familyOverride) ui.family = *familyOverride;
    const double maxScale = familyOverride ? 6.0 : 4.0;
    const int mm = 2;  // model margin

    // A redraw of the label in one font, weight and size, with its ink extents.
    struct Model {
        std::vector<float> cov;  // blurred
        int w = 0, h = 0, inkLeft = 0, inkTop = 0, inkHeight = 0;
        float width = 0, box = 0;
        std::wstring family;
    };
    std::map<std::wstring, std::unique_ptr<Model>> models;
    auto model = [&](const std::wstring& family, int weight, double size) -> const Model* {
        size = std::round(size * 4) / 4;
        std::wstring key = family + L"|" + std::to_wstring(weight) + L"|" + std::to_wstring(size);
        auto it = models.find(key);
        if (it != models.end()) return it->second.get();
        auto out = std::make_unique<Model>();
        ResolvedFont font = v.resolver->Resolve(family, ui.family, true, weight, false);
        Laid laid;
        if (font.family == family && LayOut(v, r.text, font.family, weight, false, size, laid)) {
            out->family = font.family;
            out->width = laid.width;
            out->box = laid.baseline / font.ascentShare;
            out->w = (int)std::ceil(laid.width) + 2 * mm;
            out->h = (int)std::ceil(out->box) + 2 * mm;
            // A box exactly the font's size: no stretch, layout top at the margin.
            if (DrawModel(v, laid.layout.Get(), out->w, out->h, (float)mm, (float)mm, 1.0f, out->cov)) {
                int left = -1, top = -1, bottom = -1;
                for (int y = 0; y < out->h; ++y)
                    for (int x = 0; x < out->w; ++x)
                        if (out->cov[(size_t)y * out->w + x] > 0.45f) {
                            if (top < 0) top = y;
                            bottom = y;
                            if (left < 0 || x < left) left = x;
                        }
                if (top >= 0) {
                    out->inkLeft = left, out->inkTop = top, out->inkHeight = bottom - top + 1;
                    BoxBlurN(out->cov, out->w, out->h, blurPasses);
                } else {
                    out->cov.clear();
                }
            } else {
                out->cov.clear();
            }
        }
        return (models[key] = std::move(out)).get();
    };

    // Screen ink rows within columns [x0, x1) of the box, inside a band.
    auto inkRows = [&](int x0, int x1, const Band& band, int& top, int& bottom) {
        top = bottom = -1;
        for (int y = band.top; y <= band.bottom; ++y)
            for (int x = (std::max)(x0, 0); x < (std::min)(x1, bw); ++x)
                if (ink(x, y)) {
                    if (top < 0) top = y;
                    bottom = y;
                    break;
                }
        return top >= 0;
    };

    double best = -1;
    const Model* bestModel = nullptr;
    int bestWeight = 400, bestOx = 0, bestOy = 0, bestStart = -1;
    Band bestBand{};
    double bestSize = 0;
    // Places a model with its ink starting at column cx and its ink top on
    // the screen's, and keeps the best of nine one-pixel nudges.
    int nNoModel = 0, nTooBig = 0, nNoRows = 0, nOutside = 0, nTried = 0;
    auto tryModel = [&](const Model* md, int weight, double size, int cx, const Band& band) {
        if (!md || md->cov.empty()) { ++nNoModel; return; }
        if (md->width > bw + 2 || md->box > bh + 6) { ++nTooBig; return; }
        int top, bottom;
        if (!inkRows(cx, cx + (int)md->width, band, top, bottom)) { ++nNoRows; return; }
        ++nTried;
        if (Correlate(md->cov, md->w, md->h, screen, sw, sh, cx + m - md->inkLeft, top + m - md->inkTop) < -0.5) ++nOutside;
        const int ox = cx + m - md->inkLeft, oy = top + m - md->inkTop;
        // If the band is taller than this text (other things share its rows),
        // slide down through it rather than trusting the band's top.
        const int extra = (std::max)(0, (bottom - top + 1) - md->inkHeight);
        for (int dy = -1; dy <= 1 + extra; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                double c = Correlate(md->cov, md->w, md->h, screen, sw, sh, ox + dx, oy + dy);
                if (c > best)
                    best = c, bestModel = md, bestWeight = weight, bestSize = size, bestOx = ox + dx,
                    bestOy = oy + dy, bestStart = cx, bestBand = band;
            }
    };

    // A known style (another word on the same line): one redraw at each
    // place the text could start, and no search if that fits.
    if (hint)
        for (const Band& band : bands)
            for (int start : band.starts)
                tryModel(model(hint->font, hint->weight, hint->sizePx), hint->weight, hint->sizePx, start, band);
    const bool hinted = hint && best >= minMatch;

    // Stage 1: the system UI font, regular weight. At each place the text
    // could start, estimate the size from the height of the ink there.
    const Model* base = model(ui.family, 400, ui.sizePx);
    if (!base || base->cov.empty()) return false;
    if (!hinted) {
    for (const Band& band : bands)
        for (int start : band.starts) {
            int top, bottom;
            if (!inkRows(start, start + (int)base->width, band, top, bottom)) continue;
            double est =
                std::clamp(ui.sizePx * (bottom - top + 1) / base->inkHeight, ui.sizePx * 0.6, ui.sizePx * maxScale);
            for (double k : {0.94, 1.0, 1.06}) tryModel(model(ui.family, 400, est * k), 400, est * k, start, band);
            // The band may hold more than this one line of text; also try sizes
            // near the standard UI font.
            if (est > ui.sizePx * 1.4)
                for (double k : {1.0, 1.17, 1.33})
                    tryModel(model(ui.family, 400, ui.sizePx * k), 400, ui.sizePx * k, start, band);
        }
    // Text centred under a control (centreX is in box coordinates): place the
    // redraw's middle there directly.
    if (centreX >= 0)
        for (const Band& band : bands)
            for (double k : {0.92, 1.0, 1.08, 1.17}) {
                const Model* md = model(ui.family, 400, ui.sizePx * k);
                if (!md || md->cov.empty()) continue;
                int cx = (int)std::lround(centreX - mm - md->width / 2 + md->inkLeft);
                tryModel(md, 400, ui.sizePx * k, cx, band);
            }
    // Stage 2: heavier weights, at the best place and size so far.
    if (best < 0.85 && bestStart >= 0) {
        const double size0 = bestSize;
        const int start0 = bestStart;
        const Band band0 = bestBand;
        for (int weight : {600, 700})
            for (double k : {0.94, 1.0, 1.06})
                tryModel(model(ui.family, weight, size0 * k), weight, size0 * k, start0, band0);
    }
    // Stage 3: Windows 11's Segoe UI Variable if nothing fits clearly yet, and
    // the fonts the apps in view use, at the best weight so far.
    if (bestStart >= 0 && (best < 0.85 || (!familyOverride && !v.appFonts.empty()))) {
        const double size0 = bestSize;
        const int start0 = bestStart, weight0 = bestWeight;
        const Band band0 = bestBand;
        std::vector<std::wstring> families;
        // (Not when a font was asked for: OCR tries several in turn.)
        if (best < 0.85 && !familyOverride) families = {L"Segoe UI Variable Text", L"Segoe UI Variable Display"};
        if (!familyOverride) families.insert(families.end(), v.appFonts.begin(), v.appFonts.end());
        for (const auto& fam : families)
            if (ui.family != fam)
                for (double k : {0.94, 1.0, 1.06})
                    tryModel(model(fam, weight0, size0 * k), weight0, size0 * k, start0, band0);
    }
    }  // !hinted
    if (g_traceScan) {
        std::wstring bandText;
        for (const Band& b : bands) bandText += std::to_wstring(b.top) + L"-" + std::to_wstring(b.bottom) + L"(" + std::to_wstring(b.starts.size()) + L") ";
        Log(L"      '%s': best %.2f size %.1f w%d | box %dx%d bands %s| nomodel %d toobig %d norows %d tried %d outside %d ui %.1f",
            r.text.substr(0, 30).c_str(), best, bestSize, bestWeight, bw, bh, bandText.c_str(), nNoModel, nTooBig, nNoRows,
            nTried, nOutside, ui.sizePx);
    }
    if (best < minMatch || !bestModel) return false;
    // A heavier weight or another font is a guess beyond the system default,
    // so it must fit clearly better before it is trusted.
    if (!hinted && (bestWeight != 400 || bestModel->family != ui.family) && best < 0.88) return false;
    // Much smaller than the system font is rare in real UI: demand a clear fit.
    if (bestSize < ui.sizePx * 0.8 && best < 0.9) return false;

    r.rect = {(double)(sx0 + bestOx + mm), (double)(sy0 + bestOy + mm), sx0 + bestOx + mm + bestModel->width,
              sy0 + bestOy + mm + bestModel->box};
    r.font = bestModel->family;
    r.sizePx = std::round(bestSize * 4) / 4;
    r.weight = bestWeight;
    r.italic = false;
    r.offsetX = r.offsetY = 0;
    r.match = best;
    r.label = false;
    if (!PlainText(f, r.rect, fgHex, true, r.background, r.foreground, &r.bgSlope)) return false;

    // The app may have drawn the label a pixel or two wider than the redraw
    // (different hinting or font). Extend the repaint over any ink touching
    // the text box, inside the control, so no sliver of the original shows.
    const double yB = Luma(ToRgb(r.background)), yF = Luma(ToRgb(r.foreground));
    const int ty0 = (std::max)((int)std::floor(r.rect.t), by0), ty1 = (std::min)((int)std::ceil(r.rect.b), by1);
    auto inkColumn = [&](int x) {
        if (x < bx0 || x >= bx1 || x < f.left || x >= f.left + f.width) return false;
        for (int y = ty0; y < ty1; ++y)
            if ((Luma(ToRgb(f.At(x, y))) - yB) / (yF - yB) > 0.3) return true;
        return false;
    };
    const int maxPad = (int)std::ceil(r.rect.Height() * 0.4);
    int x = (int)std::ceil(r.rect.r);
    while (x - r.rect.r < maxPad && inkColumn(x)) ++x;
    r.padRight = (std::max)(0.0, x - r.rect.r);
    x = (int)std::floor(r.rect.l) - 1;
    while (r.rect.l - x < maxPad && inkColumn(x)) --x;
    r.padLeft = (std::max)(0.0, r.rect.l - (x + 1));
    return true;
}

// Whether a placed run ending in an ellipsis really shows the dots there: in
// their columns the body of the line above the baseline must be empty. (Some
// apps fade a long name out instead, which an ellipsis redraw can match
// loosely.)
bool DotsShown(Verifier& v, const CpuFrame& f, const TextRun& t, size_t dotsLen) {
    Laid laid;
    if (!LayOut(v, t.text, t.font, t.weight, t.italic, t.sizePx, laid)) return false;
    FLOAT hx = 0, hy = 0;
    DWRITE_HIT_TEST_METRICS hm;
    if (FAILED(laid.layout->HitTestTextPosition((UINT32)(t.text.size() - dotsLen), FALSE, &hx, &hy, &hm))) return false;
    const int x0 = (int)std::ceil(t.rect.l + hx + 1), x1 = (int)std::floor(t.rect.l + laid.width - 1);
    const int y0 = (int)std::ceil(t.rect.t + laid.baseline * 0.3), y1 = (int)std::floor(t.rect.t + laid.baseline * 0.75);
    if (x1 - x0 < 2 || y1 - y0 < 2) return false;
    const double yB = Luma(ToRgb(t.background)), yF = Luma(ToRgb(t.foreground));
    const std::vector<float> cov = Coverage(f, x0, y0, x1 - x0, y1 - y0, yB, yF);
    int ink = 0;
    for (float c : cov) ink += c > 0.12f;  // faint too: a fade-out is low contrast
    if (ink > (int)cov.size() / 25) return false;
    // And three clear, full-strength dots on the baseline (a name faded out
    // to nothing is empty above the baseline too).
    const int b0 = (int)std::floor(t.rect.t + laid.baseline - t.sizePx * 0.2), b1 = (int)std::ceil(t.rect.t + laid.baseline + 1);
    const int bx0 = (int)std::floor(t.rect.l + hx), bx1 = (int)std::ceil(t.rect.l + laid.width);
    if (bx1 - bx0 < 3 || b1 - b0 < 2) return false;
    const std::vector<float> dotsCov = Coverage(f, bx0, b0, bx1 - bx0, b1 - b0, yB, yF);
    const int bw = bx1 - bx0;
    for (int d = 0; d < 3; ++d) {
        float peak = 0;
        for (int y = 0; y < b1 - b0; ++y)
            for (int x = bw * d / 3; x < bw * (d + 1) / 3; ++x) peak = (std::max)(peak, dotsCov[(size_t)y * bw + x]);
        if (peak < 0.55f) return false;
    }
    // Three separate marks, not one stroke (an underscore sits there too).
    int marks = 0;
    bool inMark = false;
    for (int x = 0; x < bw; ++x) {
        float col = 0;
        for (int y = 0; y < b1 - b0; ++y) col = (std::max)(col, dotsCov[(size_t)y * bw + x]);
        if (!inMark && col > 0.45f) inMark = true, ++marks;
        else if (inMark && col < 0.3f) inMark = false;
    }
    return marks == 3;
}

// Verifies a control label: its name and the other readings of it, and if
// none fits on one line, the name split at a space with the first part on one
// line and the rest on the line below (a wrapped label). On success c is the
// placed run and extra may hold the second line.
bool CheckLabel(Verifier& v, const CpuFrame& f, TextRun& c, double dpi, double minMatch, std::vector<TextRun>& extra) {
    TextRun best;
    best.match = -1;
    std::vector<std::wstring> texts = {c.text};
    texts.insert(texts.end(), c.alternatives.begin(), c.alternatives.end());
    for (const auto& text : texts) {
        TextRun t = c;
        t.text = text;
        if (MatchLabel(v, f, t, dpi, minMatch) && t.match > best.match) best = std::move(t);
        if (best.match >= 0.9) break;
    }
    if (best.match >= minMatch) {
        c = std::move(best);
        return true;
    }
    // First part on this line and the rest on the next. If the rest is not
    // found, the first part alone is kept for later: a name cut short with
    // an ellipsis (below) explains the pixels better.
    TextRun firstOnly;
    firstOnly.match = -1;
    for (const auto& text : texts) {
        std::vector<size_t> spaces;
        for (size_t k = 0; k < text.size(); ++k)
            if (text[k] == L' ') spaces.push_back(k);
        if (spaces.empty() || spaces.size() > 7) continue;
        for (auto sp = spaces.rbegin(); sp != spaces.rend(); ++sp) {
            TextRun first = c;
            first.text = text.substr(0, *sp);
            if (!MatchLabel(v, f, first, dpi, minMatch)) continue;
            TextRun second = c;
            second.text = text.substr(*sp + 1);
            second.source.t = first.rect.b - 2;
            if (second.source.Height() >= 6 && MatchLabel(v, f, second, dpi, minMatch)) {
                second.key = c.key + L"|2";
                TakeCrop(f, second);
                extra.push_back(std::move(second));
                c = std::move(first);
                return true;
            }
            if (firstOnly.match < 0) firstOnly = std::move(first);
            break;
        }
    }
    // A name that wrapped in the middle of a word (long file names under
    // icons): try each break near the middle, first part on one line, rest
    // on the next.
    const UiFont ui = SystemUiFont(dpi);
    if (c.text.find(L' ') == std::wstring::npos && c.text.size() >= 8 && c.text.size() <= 48 &&
        c.source.Height() > ui.sizePx * 2.5) {
        const size_t n = c.text.size();
        for (size_t cut = n / 3; cut <= n * 2 / 3 + 1 && cut < n; ++cut) {
            TextRun first = c;
            first.text = c.text.substr(0, cut);
            if (!MatchLabel(v, f, first, dpi, (std::max)(minMatch, 0.8))) continue;
            TextRun second = c;
            second.text = c.text.substr(cut);
            second.source.t = first.rect.b - 2;
            if (second.source.Height() >= 6 && MatchLabel(v, f, second, dpi, (std::max)(minMatch, 0.8))) {
                second.key = c.key + L"|2";
                TakeCrop(f, second);
                extra.push_back(std::move(second));
                c = std::move(first);
                return true;
            }
        }
    }
    // A name too long for its control, cut short with an ellipsis
    // ("XboxControllerMouse-1.0.0-win-x64-Port..."): find how much of it fits
    // the box and try that much and a few characters less.
    {
        const double room = c.source.Width() - 2;
        Laid full;
        if (c.text.size() >= 6 && c.text.size() <= 300 && LayOut(v, c.text, ui.family, 400, false, ui.sizePx, full) &&
            full.width > room) {
            auto width = [&](size_t k, const wchar_t* dots) {
                std::wstring t = c.text.substr(0, k);
                while (!t.empty() && t.back() == L' ') t.pop_back();
                Laid l;
                return LayOut(v, t + dots, ui.family, 400, false, ui.sizePx, l) ? l.width : 1e9f;
            };
            for (const wchar_t* dots : {L"...", L"\u2026"}) {
                size_t lo = 1, hi = c.text.size() - 1;  // longest prefix that fits
                while (lo < hi) {
                    const size_t mid = (lo + hi + 1) / 2;
                    if (width(mid, dots) <= room) lo = mid;
                    else hi = mid - 1;
                }
                // The box usually holds an icon and padding too, so the
                // shown part is a little shorter than what would fit.
                for (size_t k = lo; k + 8 > lo && k >= 2; --k) {
                    TextRun t = c;
                    t.text = c.text.substr(0, k);
                    while (!t.text.empty() && t.text.back() == L' ') t.text.pop_back();
                    t.text += dots;
                    if (MatchLabel(v, f, t, dpi, (std::max)(minMatch, 0.8)) && t.match > best.match &&
                        DotsShown(v, f, t, wcslen(dots)))
                        best = std::move(t);
                    if (best.match >= 0.9) break;
                }
                if (best.match >= (std::max)(minMatch, 0.8)) {
                    c = std::move(best);
                    return true;
                }
            }
        }
    }
    if (firstOnly.match >= 0) {
        c = std::move(firstOnly);
        return true;
    }
    // A name with a description after it ("Utilities & Tools category with
    // four items, ..."): the opening words may be all that is shown.
    {
        std::vector<size_t> spaces;
        for (size_t k = 0; k < c.text.size(); ++k)
            if (c.text[k] == L' ') spaces.push_back(k);
        if (spaces.size() >= 2)
            for (size_t i = (std::min)(spaces.size(), (size_t)6); i-- > 0;) {
                TextRun t = c;
                t.text = c.text.substr(0, spaces[i]);
                while (!t.text.empty() && (t.text.back() == L',' || t.text.back() == L'.')) t.text.pop_back();
                if (t.text.size() < 3) continue;
                if (MatchLabel(v, f, t, dpi, (std::max)(minMatch, 0.85))) {
                    c = std::move(t);
                    return true;
                }
            }
    }
    // Some controls report only their icon's box and draw the name below it
    // (Start menu tiles). Last resort: look below and to either side.
    const RectD box = c.source;
    if (box.Width() < 160 && box.Height() < 160) {
        TextRun wide = c;
        wide.source = {box.l - box.Width() * 0.75, box.t, box.r + box.Width() * 0.75, box.b + box.Height() * 0.9};
        for (const auto& text : texts) {
            TextRun t = wide;
            t.text = text;
            // The name is usually centred under the control's box.
            const double centre = box.Center().x - std::floor(wide.source.l);
            if (MatchLabel(v, f, t, dpi, (std::max)(minMatch, 0.8), centre) && t.match > best.match) best = std::move(t);
        }
        if (best.match >= (std::max)(minMatch, 0.8)) {
            c = std::move(best);
            return true;
        }
    }
    return false;
}

// A word read by OCR: nothing but the pixels says what it is, so a redraw
// must match clearly better than for app-reported text before it is shown,
// and short words (where a misreading still matches loosely) better still.
// Web and document fonts are mostly unknown: try a humanist and a neo-
// grotesque sans, compared at a coarser scale for larger text.
double Overlap(const RectD& a, const RectD& b);

// ---------------------------------------------------------------- look-alikes
//
// OCR confuses characters that look alike ("modern" read as "modem", "£" as
// "E", "0" as "O"), and a redraw of the misreading still matches the pixels
// closely overall. So a reading competes with its look-alike alternatives:
// each is drawn in the font, size and place found for the reading and
// compared with the pixels in fine detail. If one matches about as well, the
// pixels cannot tell them apart and the word is left to the scaling filter.
// (The comparison cannot pick the right one: the redraw is in a stand-in
// font, whose shapes differ by more than "rn" and "m" do, so a reading is
// never "corrected", only kept or left alone.) A reading that is not a word
// but has a look-alike that is ("leam", "learn") is left alone too, and every
// letter must match where it stands, which catches garbled readings.

struct Lookalike {
    const wchar_t* from;
    const wchar_t* to;
};
constexpr Lookalike kLookalikes[] = {
    {L"rn", L"m"}, {L"m", L"rn"}, {L"cl", L"d"}, {L"d", L"cl"}, {L"vv", L"w"}, {L"w", L"vv"}, {L"ri", L"n"},
    {L"l", L"I"},  {L"l", L"1"},  {L"I", L"l"},  {L"I", L"1"},  {L"1", L"l"},  {L"1", L"I"},  {L"1", L"7"},
    {L"7", L"1"},  {L"O", L"0"},  {L"0", L"O"},  {L"o", L"0"},  {L"0", L"o"},  {L"D", L"0"},  {L"0", L"D"},
    {L"B", L"8"},  {L"8", L"B"},  {L"S", L"5"},  {L"5", L"S"},  {L"Z", L"2"},  {L"2", L"Z"},  {L"5", L"8"},
    {L"8", L"5"},  {L"6", L"8"},  {L"8", L"6"},  {L"3", L"8"},  {L"8", L"3"},  {L"9", L"8"},  {L"8", L"9"},
    {L"E", L"£"}, {L"£", L"E"}, {L"f", L"£"}, {L"£", L"€"}, {L"€", L"£"},
    {L"g", L"9"}, {L"9", L"g"}, {L"q", L"9"},
    {L".", L","}, {L",", L"."}, {L".", L"-"}, {L"-", L"."},
};

bool Punctuation(wchar_t c) { return wcschr(L".,;:!?'\"()[]-‘’“”…", c) && c; }

// The word without leading and trailing punctuation, if what is left is
// letters, perhaps joined by hyphens or apostrophes ("long-running",
// "line's"); else empty: numbers, codes and "£or" are not dictionary words.
std::wstring Letters(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && Punctuation(s[a])) ++a;
    while (b > a && Punctuation(s[b - 1])) --b;
    std::wstring core = s.substr(a, b - a);
    for (size_t i = 0; i < core.size(); ++i) {
        const bool joiner = wcschr(L"-'’", core[i]) && core[i] && i > 0 && i + 1 < core.size() &&
                            iswalpha(core[i - 1]) && iswalpha(core[i + 1]);
        if (!iswalpha(core[i]) && !joiner) return L"";
    }
    return core;
}

// Look-alikes so close in shape that a redraw in a stand-in font cannot tell
// them apart: between two real words ("modern", "modem") the word is never
// trusted.
bool Indistinct(const wchar_t* from, const wchar_t* to) {
    for (const auto& [a, b] : {std::pair{L"rn", L"m"}, std::pair{L"vv", L"w"}, std::pair{L"ri", L"n"}})
        if ((!wcscmp(from, a) && !wcscmp(to, b)) || (!wcscmp(from, b) && !wcscmp(to, a))) return true;
    return false;
}

class LookalikeCheck {
public:
    LookalikeCheck(Verifier& v, const CpuFrame& f, const TextRun& run, double dpi) : m_v(v), m_f(f), m_run(run), m_dpi(dpi) {}

    // True if the pixels clearly show run.text rather than a look-alike, and
    // every letter of it matches.
    bool Clear() {
        std::vector<std::wstring> texts = {m_run.text};
        const std::wstring letters = Letters(m_run.text);
        // Capitals only ("ID-OOII", "BBC") are codes and abbreviations, which
        // the spell checker passes whatever they say: doubt every letter.
        const bool capitals = std::none_of(letters.begin(), letters.end(), iswlower);
        const bool realWord = !letters.empty() && !capitals && m_v.IsWord(letters);
        Alternatives(m_run.text, realWord, texts);
        // Not a word, but a look-alike of a word, or a letter or two from one:
        // almost surely a misreading ("leam", "Outbu").
        if (!letters.empty() && !capitals && !realWord && m_v.speller) {
            for (size_t i = 1; i < texts.size(); ++i) {
                const std::wstring l = Letters(texts[i]);
                if (!l.empty() && m_v.IsWord(l)) return Trace(L"look-alike of the word", texts[i], 0, 0, 2);
            }
            if (const std::wstring near_ = m_v.NearWord(letters); !near_.empty())
                return Trace(L"near the word", near_, 0, 0, 2);
            // A short non-word that is not an abbreviation ("gg"): marks or an
            // icon read as letters.
            if (letters.size() <= 3 && !std::all_of(letters.begin(), letters.end(), iswupper))
                return Trace(L"a short non-word", L"", 0, 0, 2);
        }
        // A character or two can fit the pixels at another size entirely
        // ("gg" drawn large over "99"): there each look-alike gets its own
        // size and place, and must fit clearly worse.
        if (texts[0].size() <= 2)
            for (size_t i = 1; i < texts.size(); ++i) {
                TextRun t = m_run;
                t.text = texts[i];
                const std::wstring family = m_run.font;
                if (MatchLabel(m_v, m_f, t, m_dpi, 0.7, -1, &family, BlurPasses(m_run.source.Height())) &&
                    t.match > m_run.match - kTinyTie)
                    return Trace(L"another size fits", texts[i], m_run.match, t.match, 2);
            }
        if (!Prepare(texts)) return false;
        Score(0);
        // Each look-alike is compared only where it differs from the reading
        // (the changed letters' columns): over the whole word a one-letter
        // difference is lost among the letters both share.
        double closest = -2, ownThere = 0;
        size_t closestAlt = 0;
        for (size_t i = 1; i < texts.size(); ++i) {
            // Two real words ("modern", "modem"): the stand-in font's shapes
            // differ from the app's, so they need a wider margin, and some
            // look-alikes no margin settles.
            const std::wstring l = Letters(texts[i]);
            const bool words = realWord && texts[0].size() > 1 && !l.empty() && l != letters;
            if (words && m_changes[i].indistinct) return Trace(L"a word indistinct from", texts[i], 0, 0, 2);
            Score(i);
            const auto [own, alt] = Local(i);
            if (alt > own - (words ? kWordsTie : kTie))
                return Trace(words ? L"a word too close to" : L"too close to", texts[i], own, alt, 2);
            if (alt - own > closest) closest = alt - own, ownThere = own, closestAlt = i;
        }
        const double worst = WorstLetter(0);
        if (worst < kLetter) return Trace(L"a letter does not match", L"", 0, 0, worst);
        Trace(L"clear of", closestAlt ? texts[closestAlt] : L"-", ownThere, ownThere + closest, worst);
        return true;
    }

private:
    // Where the reading and a look-alike differ, the look-alike must match
    // this much worse than the reading (correlation over those columns).
    static constexpr double kTie = 0.08;
    static constexpr double kWordsTie = 0.12;  // the same when both are real words
    static constexpr double kLetter = 0.5;     // every letter must match at least this well
    static constexpr double kSmallText = 20;   // em size in pixels below which digits are doubted
    static constexpr double kTinyTie = 0.04;   // whole-word fit margin for one or two characters

    bool Trace(const wchar_t* verdict, const std::wstring& other, double own, double alt, double worst) {
        if (g_traceOcr)
            Log(L"  look-alike '%s' %.3f: %s '%s' %.3f; worst letter %.2f; size %.1f; word %d", m_run.text.c_str(), own,
                verdict, other.c_str(), alt, worst, m_run.sizePx, (int)m_v.IsWord(Letters(m_run.text)));
        return false;
    }
    static constexpr int kPad = 3, kNx = 2, kNy = 1;

    // Single look-alike substitutions of text that could really be there:
    //  - in a dictionary word, only other words ("Install", not "lnstall" or
    //    "lnsta11": a digit inside a word is too rare to doubt every "l" and
    //    "o" for);
    //  - in a number (digits, no letters), only other digits ("14:05" or
    //    "14:08", not "14:0S"); codes mixing letters and digits, anything;
    //  - one digit for another only in small text: OCR reads digits
    //    reliably once they are larger;
    //  - a full stop, comma or hyphen only between digits ("0.75", "0,75");
    //  - a character or two alone, anything ("E" or "£", "gg" or "99"), and
    //    "O" among nothing but zeros ("OO" or "00").
    void Alternatives(const std::wstring& text, bool realWord, std::vector<std::wstring>& out) {
        const bool hasLetter = std::any_of(text.begin(), text.end(), [](wchar_t c) { return iswalpha(c) != 0; });
        const bool hasDigit = std::any_of(text.begin(), text.end(), [](wchar_t c) { return iswdigit(c) != 0; });
        const bool zeros = std::none_of(text.begin(), text.end(), [](wchar_t c) { return iswdigit(c) && c != L'0'; });
        const bool smallText = m_run.sizePx < kSmallText, tiny = text.size() <= 2;
        for (const auto& la : kLookalikes) {
            const size_t n = wcslen(la.from);
            const bool punct = Punctuation(la.from[0]);
            const bool toLetter = std::any_of(la.to, la.to + wcslen(la.to), [](wchar_t c) { return iswalpha(c) != 0; });
            const bool toDigit = iswdigit(la.to[0]) != 0;
            if (!tiny && hasDigit && !hasLetter && toLetter && !(zeros && !wcscmp(la.to, L"O"))) continue;
            // In letters alone (capitals: "BBC", "ID-OOII"), only the shapes
            // that really are the same as digits: l/I/1 and O/0.
            if (!tiny && hasLetter && !hasDigit && toDigit && !wcschr(L"01", la.to[0])) continue;
            if (!smallText && iswdigit(la.from[0]) && iswdigit(la.to[0])) continue;
            // Every occurrence at once too, in short tokens ("gg" or "99",
            // "OO" or "00").
            if (!punct && text.size() <= 4 && text.find(la.from) != text.rfind(la.from)) {
                std::wstring all = text;
                for (size_t at = all.find(la.from); at != std::wstring::npos; at = all.find(la.from, at + wcslen(la.to)))
                    all.replace(at, n, la.to);
                const size_t first = text.find(la.from);
                const std::wstring l = Letters(all);
                if (std::find(out.begin(), out.end(), all) == out.end() &&
                    (!realWord || tiny || (!l.empty() && m_v.IsWord(l)))) {
                    out.push_back(all);
                    m_changes.resize(out.size());
                    // (The change spans from the first to the last occurrence.)
                    m_changes.back() = {(UINT32)first, (UINT32)(text.rfind(la.from) + n - first),
                                        (UINT32)(all.size() - (text.size() - (text.rfind(la.from) + n)) - first),
                                        Indistinct(la.from, la.to)};
                }
            }
            for (size_t at = text.find(la.from); at != std::wstring::npos; at = text.find(la.from, at + 1)) {
                if (punct && !(at > 0 && at + n < text.size() && iswdigit(text[at - 1]) && iswdigit(text[at + n])))
                    continue;
                std::wstring alt = text.substr(0, at) + la.to + text.substr(at + n);
                if (std::find(out.begin(), out.end(), alt) != out.end()) continue;
                if (realWord && !tiny) {
                    const std::wstring l = Letters(alt);
                    if (l.empty() || !m_v.IsWord(l)) continue;
                }
                out.push_back(std::move(alt));
                m_changes.resize(out.size());
                m_changes.back() = {(UINT32)at, (UINT32)n, (UINT32)wcslen(la.to), Indistinct(la.from, la.to)};
                if (out.size() > 40) return;
            }
        }
    }

    // Lays out the texts not yet laid out, and reads the screen around the
    // run wide enough for the widest of them.
    bool Prepare(const std::vector<std::wstring>& texts) {
        for (size_t i = m_laid.size(); i < texts.size(); ++i) {
            Laid l;
            if (!LayOut(m_v, texts[i], m_run.font, m_run.weight, m_run.italic, m_run.sizePx, l)) return false;
            m_laid.push_back(std::move(l));
            m_texts.push_back(texts[i]);
        }
        float widest = 0;
        for (const auto& l : m_laid) widest = (std::max)(widest, l.width);
        const int w = (int)std::ceil(widest) + 2 * kPad, h = (int)std::ceil(m_run.rect.Height()) + 2 * kPad;
        if (w == m_w && h == m_h) return true;
        m_w = w, m_h = h;
        m_x0 = (int)std::floor(m_run.rect.l) - kPad, m_y0 = (int)std::floor(m_run.rect.t) - kPad;
        m_screen = Coverage(m_f, m_x0 - kNx, m_y0 - kNy, m_w + 2 * kNx, m_h + 2 * kNy, Luma(ToRgb(m_run.background)),
                            Luma(ToRgb(m_run.foreground)));
        BoxBlur(m_screen, m_w + 2 * kNx, m_h + 2 * kNy);
        m_models.assign(m_laid.size(), {});
        m_best.assign(m_laid.size(), {0, 0});
        return true;
    }

    // Best correlation of text i over small nudges (remembered for WorstLetter).
    // Width is part of the difference between look-alikes ("rn" is a little
    // wider than "m"), but the stand-in font's letters can be much wider or
    // narrower than the app's ("barn"): a text more than 8% off the
    // reading's width is stretched to it, so its shape is compared instead.
    double Score(size_t i) {
        if (m_models.size() < m_laid.size()) m_models.resize(m_laid.size()), m_best.resize(m_laid.size());
        std::vector<float>& model = m_models[i];
        const float left = (float)(m_run.rect.l - m_x0), top = (float)(m_run.rect.t - m_y0);
        const float ratio = m_laid[0].width / m_laid[i].width;
        const float scaleX = std::fabs(ratio - 1) > 0.08f ? ratio : 1.0f;
        if (!DrawModel(m_v, m_laid[i].layout.Get(), m_w, m_h, left, top, scaleX, model)) return -1;
        BoxBlur(model, m_w, m_h);
        double best = -1;
        for (int dy = -kNy; dy <= kNy; ++dy)
            for (int dx = -kNx; dx <= kNx; ++dx) {
                const double c = Correlate(model, m_w, m_h, m_screen, m_w + 2 * kNx, m_h + 2 * kNy, kNx + dx, kNy + dy);
                if (c > best) best = c, m_best[i] = {dx, dy};
            }
        return best;
    }

    // Columns [x0, x1) of text i's model (unnudged) covering its characters
    // [first, first + count).
    bool Span(size_t i, UINT32 first, UINT32 count, double& x0, double& x1) {
        const float left = (float)(m_run.rect.l - m_x0);
        const float ratio = m_laid[0].width / m_laid[i].width;
        const float scaleX = std::fabs(ratio - 1) > 0.08f ? ratio : 1.0f;
        FLOAT x = 0, y = 0;
        DWRITE_HIT_TEST_METRICS a, b;
        if (FAILED(m_laid[i].layout->HitTestTextPosition(first, FALSE, &x, &y, &a)) ||
            FAILED(m_laid[i].layout->HitTestTextPosition(first + count - 1, FALSE, &x, &y, &b)))
            return false;
        x0 = left + a.left * scaleX + m_best[i].first;
        x1 = left + (b.left + b.width) * scaleX + m_best[i].first;
        return true;
    }

    // Correlation of text i's model with the screen over model columns
    // [c0, c1) (in the reading's frame), at text i's best nudge.
    double Pearson(size_t i, int c0, int c1) {
        const std::vector<float>& model = m_models[i];
        const auto [dx, dy] = m_best[i];
        const int sw = m_w + 2 * kNx;
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
        for (int yy = 0; yy < m_h; ++yy)
            for (int xx = (std::max)(c0, 0); xx < (std::min)(c1, m_w); ++xx) {
                const int mx = xx - dx, my = yy - dy;  // the model pixel shown at screen (xx, yy)
                const double a = mx >= 0 && mx < m_w && my >= 0 && my < m_h ? model[(size_t)my * m_w + mx] : 0.0;
                const double b = m_screen[(size_t)(yy + kNy) * sw + xx + kNx];
                sa += a, sb += b, saa += a * a, sbb += b * b, sab += a * b, ++n;
            }
        const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
        if (n < 4 || va <= 1e-6 || vb <= 1e-6) return 0;
        return (sab - sa * sb / n) / std::sqrt(va * vb);
    }

    // The reading's and look-alike i's correlation over the columns where
    // they differ (both spans, and two pixels either side).
    std::pair<double, double> Local(size_t i) {
        const Change& ch = m_changes[i];
        double r0, r1, a0, a1;
        if (!Span(0, ch.at, ch.from, r0, r1) || !Span(i, ch.at, ch.to, a0, a1)) return {1, -1};
        const int c0 = (int)std::floor((std::min)(r0, a0)) - 2, c1 = (int)std::ceil((std::max)(r1, a1)) + 2;
        return {Pearson(0, c0, c1), Pearson(i, c0, c1)};
    }

    // The poorest match of any letter or digit of text i, each compared over
    // its own columns at the word's best place.
    double WorstLetter(size_t i) {
        const std::wstring& text = m_texts[i];
        const std::vector<float>& model = m_models[i];
        const auto [dx, dy] = m_best[i];
        const float left = (float)(m_run.rect.l - m_x0);
        const int sw = m_w + 2 * kNx;
        double worst = 2;
        for (UINT32 k = 0; k < text.size(); ++k) {
            if (!iswalnum(text[k])) continue;
            FLOAT x = 0, y = 0;
            DWRITE_HIT_TEST_METRICS hm;
            if (FAILED(m_laid[i].layout->HitTestTextPosition(k, FALSE, &x, &y, &hm))) continue;
            const int c0 = (std::max)(0, (int)std::floor(left + hm.left) - 1);
            const int c1 = (std::min)(m_w, (int)std::ceil(left + hm.left + hm.width) + 1);
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
            for (int yy = 0; yy < m_h; ++yy)
                for (int xx = c0; xx < c1; ++xx) {
                    const double a = model[(size_t)yy * m_w + xx];
                    const double b = m_screen[(size_t)(yy + kNy + dy) * sw + xx + kNx + dx];
                    sa += a, sb += b, saa += a * a, sbb += b * b, sab += a * b, ++n;
                }
            const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
            if (n < 4 || va <= 1e-6 || vb <= 1e-6) continue;
            worst = (std::min)(worst, (sab - sa * sb / n) / std::sqrt(va * vb));
        }
        return worst;
    }

    // How look-alike i differs from the reading: characters [at, at + from)
    // of the reading became [at, at + to) of the look-alike.
    struct Change {
        UINT32 at = 0, from = 0, to = 0;
        bool indistinct = false;  // see Indistinct
    };

    Verifier& m_v;
    const CpuFrame& m_f;
    const TextRun& m_run;
    const double m_dpi;
    std::vector<Change> m_changes = {Change{}};  // [0] is the reading itself
    std::vector<std::wstring> m_texts;
    std::vector<Laid> m_laid;
    std::vector<std::vector<float>> m_models;
    std::vector<std::pair<int, int>> m_best;
    std::vector<float> m_screen;
    int m_w = 0, m_h = 0, m_x0 = 0, m_y0 = 0;
};

bool MatchOcrWord(Verifier& v, const CpuFrame& f, TextRun& c, double dpi, const TextRun* hint = nullptr) {
    // Short words and unusual symbols are where misreadings (an icon read as
    // "IZI", "+" read as "*") still match loosely: demand a clearer fit.
    const bool odd = std::any_of(c.text.begin(), c.text.end(), [](wchar_t ch) {
        return !iswalnum(ch) && !wcschr(L".,'\u2019:;!?-()\"\u201C\u201D&/%", ch);
    });
    const double need = c.text.size() <= 3 || odd ? 0.9 : 0.82;
    const int passes = BlurPasses(c.source.Height());
    const double hx = c.source.Height() * 0.35, hy = c.source.Height() * 0.3;
    TextRun best;
    best.match = -1;
    // The line's style (from app text beside it, or what the app reports
    // there) is tried first; a much closer fit found freely wins.
    const double needHinted = need;
    TextRun hinted;
    hinted.match = -1;
    if (hint) {
        TextRun t = c;
        t.source = {c.source.l - hx, c.source.t - hy, c.source.r + hx, c.source.b + hy};
        if (MatchLabel(v, f, t, dpi, needHinted, -1, &hint->font, passes, hint)) hinted = std::move(t);
    }
    // A clear fit in the line's style needs no search. A loose one gives way
    // only to a much closer fit found freely (its size or place is off here):
    // one style across a paragraph matters more than the closest fit.
    TextRun free;
    free.match = -1;
    if (hinted.match < need)
        for (const wchar_t* fam : {L"Segoe UI", L"Arial", L"Georgia", L"Times New Roman", L"Consolas"}) {
            // A serif (headlines, documents) or a monospace font (code,
            // terminals) only where it fits clearly better than the sans
            // fonts; a serif only for larger text (small UI text is not serif).
            const bool serif = !wcscmp(fam, L"Georgia") || !wcscmp(fam, L"Times New Roman");
            const bool other = serif || !wcscmp(fam, L"Consolas");
            if (serif && c.source.Height() < 12 * dpi / 96) continue;
            // Monospace is rare outside code: a word long enough to show its
            // even spacing, fitting clearly.
            const bool mono = !wcscmp(fam, L"Consolas");
            if (mono && c.text.size() < 4) continue;
            TextRun t = c;
            t.source = {c.source.l - hx, c.source.t - hy, c.source.r + hx, c.source.b + hy};
            const std::wstring family = fam;
            if (MatchLabel(v, f, t, dpi, mono ? (std::max)(need, 0.93) : need, -1, &family, passes) &&
                t.match > free.match + (other ? 0.04 : 0))
                free = std::move(t);
            if (free.match >= 0.92) break;
        }
    if (hinted.match >= needHinted && hinted.match + 0.12 >= free.match) best = std::move(hinted);
    else if (free.match >= need) best = std::move(free);
    else return false;
    best.inkTop = c.source.t;
    best.inkBottom = c.source.b;
    // The redraw must sit where OCR saw the word, not on a neighbour.
    if (Overlap(best.rect, c.source) < 0.5 * c.source.Width() * c.source.Height()) return false;
    // Not a look-alike misreading.
    if (!LookalikeCheck(v, f, best, dpi).Clear()) return false;
    best.ocr = true;
    c = std::move(best);
    return true;
}

// Short words alone cannot tell a weight or typeface apart, so OCR words
// on one line are drawn in the line's main style (by text length) wherever
// that style still matches them.
void HarmoniseOcrLine(Verifier& v, const CpuFrame& f, std::vector<TextRun>& runs, uint64_t line) {
    std::map<std::pair<std::wstring, int>, size_t> votes;
    std::vector<double> sizes;
    for (const auto& r : runs)
        if (r.ocr && r.ocrLine == line) {
            votes[{r.font, r.weight}] += r.text.size();
            sizes.push_back(r.sizePx);
        }
    if (sizes.size() < 2) return;
    auto top = std::max_element(votes.begin(), votes.end(),
                                [](const auto& a, const auto& b) { return a.second < b.second; });
    std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
    const double size = sizes[sizes.size() / 2];
    for (auto& r : runs) {
        if (!r.ocr || r.ocrLine != line) continue;
        if (r.font == top->first.first && r.weight == top->first.second && std::fabs(r.sizePx - size) < 1.0) continue;
        TextRun t = r;
        t.font = top->first.first;
        t.weight = top->first.second;
        t.sizePx = size;
        t.match = MatchRun(v, f, t, t.font, true);
        if (t.match >= 0.8) {
            TakeCrop(f, t);
            r = std::move(t);
        }
    }
}

// Pixel fingerprint of an area (every 4th pixel each way), to skip OCR when
// nothing there changed.
uint64_t Fingerprint(const CpuFrame& f, const RectD& area, int step = 4) {
    int x0, y0, x1, y1;
    if (!PixelBounds(f, area, x0, y0, x1, y1)) return 0;
    uint64_t h = 1469598103934665603ull;
    for (int y = y0; y < y1; y += step)
        for (int x = x0; x < x1; x += step) h = (h ^ f.At(x, y)) * 1099511628211ull;
    return h ^ ((uint64_t)x0 << 40) ^ ((uint64_t)y0 << 20) ^ (uint64_t)(x1 * 31 + y1);
}

// The closest stretch of text to word (edit distance with free start and
// end in text): returns the distance and sets from/len.
int ClosestSubstring(const std::wstring& word, const std::wstring& text, size_t& from, size_t& len) {
    const size_t m = word.size(), n = text.size();
    std::vector<int> prev(n + 1, 0), cur(n + 1);
    std::vector<size_t> prevStart(n + 1), curStart(n + 1);
    for (size_t j = 0; j <= n; ++j) prevStart[j] = j;
    for (size_t i = 1; i <= m; ++i) {
        cur[0] = (int)i;
        curStart[0] = 0;
        for (size_t j = 1; j <= n; ++j) {
            const int sub = prev[j - 1] + (word[i - 1] == text[j - 1] ? 0 : 1);
            const int del = prev[j] + 1, ins = cur[j - 1] + 1;
            if (sub <= del && sub <= ins) cur[j] = sub, curStart[j] = prevStart[j - 1];
            else if (del <= ins) cur[j] = del, curStart[j] = prevStart[j];
            else cur[j] = ins, curStart[j] = curStart[j - 1];
        }
        std::swap(prev, cur);
        std::swap(prevStart, curStart);
    }
    int best = INT_MAX;
    for (size_t j = 0; j <= n; ++j)
        if (prev[j] < best) best = prev[j], from = prevStart[j], len = j - prevStart[j];
    return best;
}

// OCR misreads letters now and then ("Xbax" for "Xbox", "CornmiL" for
// "Commit"), and a redraw of a misread word can still match the pixels
// closely. So a word from the main reading is kept only if a second reading
// at another scale saw the same word in the same place, or if text the app
// reports there (a control's label, a document line, even one whose own
// redraw did not fit) has a stretch within a letter or two of it, whose
// spelling is then used. Misreads rarely repeat at another scale.
std::vector<OcrWord> AgreedWords(const std::vector<OcrWord>& main, const std::vector<OcrWord>& second,
                                 const std::vector<TextRun>& reported) {
    std::vector<OcrWord> out;
    for (const auto& w : main) {
        const double area = w.box.Width() * w.box.Height();
        const bool agreed = std::any_of(second.begin(), second.end(), [&](const OcrWord& s) {
            return s.text == w.text &&
                   Overlap(s.box, w.box) > 0.5 * (std::min)(area, s.box.Width() * s.box.Height());
        });
        if (agreed) {
            out.push_back(w);
            continue;
        }
        const int allowed = (std::max)(1, (int)w.text.size() / 4);
        int bestDist = allowed + 1;
        std::wstring bestText;
        for (const auto& r : reported) {
            const RectD near_ = Expand(r.source, 0.05, 0.25);
            if (Overlap(near_, w.box) < 0.6 * area) continue;
            size_t from = 0, len = 0;
            const int d = ClosestSubstring(w.text, r.text, from, len);
            if (d < bestDist && len >= w.text.size() - (std::min)(w.text.size(), (size_t)allowed)) {
                std::wstring t = r.text.substr(from, len);
                if (t.find(L' ') == std::wstring::npos) bestDist = d, bestText = std::move(t);
            }
        }
        if (!bestText.empty()) out.push_back({bestText, w.box, w.line});
    }
    return out;
}

double Overlap(const RectD& a, const RectD& b);

// OCR words become candidates: plain words of reasonable height only.
std::vector<TextRun> OcrCandidates(const std::vector<OcrWord>& words, double dpi) {
    static uint64_t reads = 0;
    ++reads;
    std::vector<TextRun> out;
    for (const auto& w : words) {
        const double hgt = w.box.Height();
        if (hgt < 6 || hgt > 140 * dpi / 96 || w.box.Width() < 3) continue;
        // A lone character is too easily an icon read as a letter, unless it
        // sits between words at ordinary word spacing ("a", "I", "&").
        if (w.text.size() < 2) {
            auto wordBeside = [&](bool left) {
                return std::any_of(words.begin(), words.end(), [&](const OcrWord& o) {
                    if (&o == &w || o.line != w.line || o.text.size() < 2) return false;
                    const double gap = left ? w.box.l - o.box.r : o.box.l - w.box.r;
                    return gap >= -1 && gap <= 0.8 * (std::max)(hgt, o.box.Height());
                });
            };
            if (!iswalnum(w.text[0]) || !wordBeside(true) || !wordBeside(false)) continue;
        }
        if (!std::any_of(w.text.begin(), w.text.end(), [](wchar_t ch) { return iswalnum(ch); })) continue;
        TextRun r;
        r.text = w.text;
        r.source = r.rect = w.box;
        r.label = true;
        r.ocr = true;
        r.ocrLine = (reads << 20) + w.line;
        r.key = L"ocr|" + w.text + L"|" + std::to_wstring((int)w.box.l) + L"," + std::to_wstring((int)w.box.t);
        out.push_back(std::move(r));
    }
    return out;
}

// Checks OCR lines on several threads, each with its own redraw tools.
// Lines are taken in order (nearest the point of attention first) and each
// finished line is delivered at once. In a line the longest word is matched
// first; the rest try its style before searching.
class OcrPool {
public:
    using Deliver = std::function<void(std::vector<TextRun>&&)>;

    void Start(int n) {
        for (int i = 0; i < n; ++i) m_threads.emplace_back([this] { Work(); });
    }
    void Stop() {
        {
            std::lock_guard l(m_mu);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads) t.join();
        m_threads.clear();
    }
    // Blocks until every line is checked. shown: runs already on screen; a
    // line that continues app text tries that text's style first.
    // reported: what the apps say is there (checked or not); a line inside
    // a reported document line tries that line's font next.
    void Verify(const CpuFrame& f, std::vector<std::vector<TextRun>> lines, double dpi,
                const std::vector<TextRun>& shown, const std::vector<TextRun>& reported, Deliver deliver) {
        std::unique_lock l(m_mu);
        m_shown = &shown;
        m_reported = &reported;
        m_frame = &f;
        m_dpi = dpi;
        m_lines = std::move(lines);
        m_next = 0;
        m_deliver = std::move(deliver);
        m_cv.notify_all();
        m_done.wait(l, [&] { return m_stop || (m_next >= m_lines.size() && m_busy == 0); });
        m_frame = nullptr;
        m_lines.clear();
    }

private:
    void Work() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Verifier v;
        const bool ok = v.Init();
        std::unique_lock l(m_mu);
        while (true) {
            m_cv.wait(l, [&] { return m_stop || (m_frame && m_next < m_lines.size()); });
            if (m_stop) break;
            std::vector<TextRun> line = std::move(m_lines[m_next++]);
            const CpuFrame* f = m_frame;
            const double dpi = m_dpi;
            const std::vector<TextRun>* shown = m_shown;
            const std::vector<TextRun>* reported = m_reported;
            Deliver deliver = m_deliver;
            ++m_busy;
            l.unlock();
            std::stable_sort(line.begin(), line.end(),
                             [](const TextRun& a, const TextRun& b) { return a.text.size() > b.text.size(); });
            std::vector<TextRun> out;
            TextRun style;
            bool haveStyle = false;
            for (const auto& r : *shown) {
                if (r.ocr) continue;
                const bool beside = std::any_of(line.begin(), line.end(), [&](const TextRun& c) {
                    const double vo = (std::min)(r.rect.b, c.source.b) - (std::max)(r.rect.t, c.source.t);
                    const double gap = (std::max)(r.rect.l, c.source.l) - (std::min)(r.rect.r, c.source.r);
                    return vo > 0.6 * c.source.Height() && gap < 2 * c.source.Height();
                });
                if (beside) {
                    style = r, haveStyle = true;
                    break;
                }
            }
            // Text the app reports there but that is not shown yet (a line
            // just typed on): its font, weight and size.
            for (size_t i = 0; !haveStyle && i < reported->size(); ++i) {
                const TextRun& r = (*reported)[i];
                if (r.label || r.font.empty() || r.sizePx <= 0) continue;
                const bool inside = std::any_of(line.begin(), line.end(), [&](const TextRun& c) {
                    const double vo = (std::min)(r.rect.b, c.source.b) - (std::max)(r.rect.t, c.source.t);
                    return vo > 0.6 * c.source.Height() && Overlap(r.rect, c.source) > 0;
                });
                if (!inside) continue;
                style = r;
                if (v.resolver && v.resolver->Resolve(r.font, r.font, true, r.weight, r.italic).family != r.font) {
                    Substitutes::Entry sub;
                    if (!g_substitutes.Find(Substitutes::Key(r.font, r.sizePx, r.weight, r.italic), sub)) continue;
                    style.font = sub.family;
                    style.weight = sub.weight;
                }
                haveStyle = true;
            }
            // The next line of a wrapped paragraph or headline: a redrawn line
            // just above, starting at the same left edge, at a similar size.
            if (!haveStyle && !line.empty()) {
                RectD box = line.front().source;
                for (const auto& c : line)
                    box = {(std::min)(box.l, c.source.l), (std::min)(box.t, c.source.t), (std::max)(box.r, c.source.r),
                           (std::max)(box.b, c.source.b)};
                const double h = box.Height();
                for (const auto& r : *shown) {
                    if (r.icon || r.font.empty()) continue;
                    const double gap = box.t - r.rect.b;
                    if (std::fabs(r.rect.l - box.l) <= 0.5 * h && gap >= -0.3 * h && gap <= 2.6 * h &&
                        r.rect.Height() <= 1.6 * h && r.rect.Height() >= 0.8 * h) {
                        style = r, haveStyle = true;
                        break;
                    }
                }
            }
            for (auto& c : line) {
                if (!ok || !MatchOcrWord(v, *f, c, dpi, haveStyle ? &style : nullptr)) continue;
                TakeCrop(*f, c);
                if (!haveStyle) style = c, haveStyle = true;
                out.push_back(std::move(c));
            }
            if (!out.empty()) deliver(std::move(out));
            l.lock();
            --m_busy;
            if (m_next >= m_lines.size() && m_busy == 0) m_done.notify_all();
        }
        l.unlock();
        CoUninitialize();
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mu;
    std::condition_variable m_cv, m_done;
    bool m_stop = false;
    const CpuFrame* m_frame = nullptr;
    const std::vector<TextRun>* m_shown = nullptr;
    const std::vector<TextRun>* m_reported = nullptr;
    double m_dpi = 96;
    std::vector<std::vector<TextRun>> m_lines;
    size_t m_next = 0;
    int m_busy = 0;
    Deliver m_deliver;
};

// ---------------------------------------------------------------- UIA scanning

struct Scanner {
    ShellIcons* icons = nullptr;  // large icons for what controls show (nullptr: none)
    Uia uia;
    ComPtr<IUIAutomationCacheRequest> cache;
    ComPtr<IUIAutomationCondition> condition;
    std::map<HWND, double> slowUntil;  // windows whose tree took too long to read

    bool Init() {
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&uia.automation))) &&
            FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia.automation))))
            return false;
        if (FAILED(uia.automation->get_ControlViewWalker(&uia.walker)) ||
            FAILED(uia.automation->CreateCacheRequest(&cache)))
            return false;
        uia.automation->get_ReservedMixedAttributeValue(&uia.mixed);
        for (PROPERTYID id : {UIA_NamePropertyId, UIA_BoundingRectanglePropertyId, UIA_ControlTypePropertyId,
                              UIA_IsTextPatternAvailablePropertyId, UIA_ValueValuePropertyId})
            cache->AddProperty(id);

        // On-screen controls whose name is usually their visible label, plus
        // text hosts (documents, edit boxes).
        const CONTROLTYPEID types[] = {UIA_TextControlTypeId,       UIA_ButtonControlTypeId,
                                       UIA_MenuItemControlTypeId,   UIA_ListItemControlTypeId,
                                       UIA_TreeItemControlTypeId,   UIA_TabItemControlTypeId,
                                       UIA_HyperlinkControlTypeId,  UIA_CheckBoxControlTypeId,
                                       UIA_RadioButtonControlTypeId, UIA_HeaderItemControlTypeId,
                                       UIA_DataItemControlTypeId,   UIA_SplitButtonControlTypeId,
                                       UIA_TitleBarControlTypeId,   UIA_DocumentControlTypeId,
                                       UIA_EditControlTypeId};
        std::vector<IUIAutomationCondition*> ors;
        std::vector<ComPtr<IUIAutomationCondition>> keep;
        for (CONTROLTYPEID t : types) {
            VARIANT v;
            v.vt = VT_I4;
            v.lVal = t;
            ComPtr<IUIAutomationCondition> c;
            if (SUCCEEDED(uia.automation->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &c))) {
                ors.push_back(c.Get());
                keep.push_back(c);
            }
        }
        ComPtr<IUIAutomationCondition> anyType, onScreen;
        VARIANT f;
        f.vt = VT_BOOL;
        f.boolVal = VARIANT_FALSE;
        return SUCCEEDED(uia.automation->CreateOrConditionFromNativeArray(ors.data(), (int)ors.size(), &anyType)) &&
               SUCCEEDED(uia.automation->CreatePropertyCondition(UIA_IsOffscreenPropertyId, f, &onScreen)) &&
               SUCCEEDED(uia.automation->CreateAndCondition(anyType.Get(), onScreen.Get(), &condition));
    }
};

enum class Attr { Value, Mixed, Missing };

// One text attribute: a value, mixed (the range spans formats), or missing.
Attr ReadAttr(Uia& uia, IUIAutomationTextRange* r, TEXTATTRIBUTEID id, VARIANT& v) {
    VariantInit(&v);
    if (FAILED(r->GetAttributeValue(id, &v))) return Attr::Missing;
    if (v.vt == VT_UNKNOWN) {
        bool mixed = uia.mixed && v.punkVal == uia.mixed.Get();
        VariantClear(&v);
        return mixed ? Attr::Mixed : Attr::Missing;
    }
    if (v.vt == VT_EMPTY) return Attr::Missing;
    return Attr::Value;
}

// Fills font, size, weight, italic and colour. Returns false if the range
// mixes formats (so it must be split into format runs).
bool ReadFormat(Uia& uia, IUIAutomationTextRange* r, double dpi, TextRun& tr) {
    VARIANT v;
    bool uniform = true;
    Attr a = ReadAttr(uia, r, UIA_FontNameAttributeId, v);
    if (a == Attr::Value) {
        if (v.vt == VT_BSTR && v.bstrVal) tr.font = v.bstrVal;
        VariantClear(&v);
    }
    uniform &= a != Attr::Mixed;
    double pt = 0;
    a = ReadAttr(uia, r, UIA_FontSizeAttributeId, v);
    if (a == Attr::Value) {
        if (v.vt == VT_R8) pt = v.dblVal;
        VariantClear(&v);
    }
    uniform &= a != Attr::Mixed;
    tr.sizePx = pt * dpi / 72.0;  // 0 if unknown; fixed up from the box later
    a = ReadAttr(uia, r, UIA_FontWeightAttributeId, v);
    if (a == Attr::Value) {
        if (v.vt == VT_I4) tr.weight = v.lVal;
        VariantClear(&v);
    }
    uniform &= a != Attr::Mixed;
    a = ReadAttr(uia, r, UIA_IsItalicAttributeId, v);
    if (a == Attr::Value) {
        if (v.vt == VT_BOOL) tr.italic = v.boolVal != VARIANT_FALSE;
        VariantClear(&v);
    }
    uniform &= a != Attr::Mixed;
    a = ReadAttr(uia, r, UIA_ForegroundColorAttributeId, v);
    if (a == Attr::Value) {
        if (v.vt == VT_I4) {
            COLORREF c = (COLORREF)v.lVal;  // 0x00BBGGRR
            tr.foreground = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
            tr.appForeground = true;
        }
        VariantClear(&v);
    }
    uniform &= a != Attr::Mixed;
    return uniform;
}

void AddRun(TextRun&& tr, std::vector<TextRun>& runs, std::set<std::wstring>& seen) {
    bool hasText = tr.text.find_first_not_of(L" \t\u00A0") != std::wstring::npos &&
                   tr.text.find(L'\uFFFC') == std::wstring::npos;
    if (!hasText) return;
    if (tr.sizePx <= 0) tr.sizePx = tr.rect.Height() / 1.3;
    tr.source = tr.rect;
    wchar_t box[64];
    swprintf_s(box, L"|%.0f,%.0f,%.0f,%.0f", tr.rect.l, tr.rect.t, tr.rect.r, tr.rect.b);
    tr.key = tr.text + box;
    if (seen.insert(tr.key).second) runs.push_back(std::move(tr));
}

// Text runs of the lines of a text host that intersect area. Rather than
// walking down from the top of the document, it starts at the lines under a
// grid of points across the area (side-by-side columns, such as a sidebar
// and a chat, are separate stretches of the document) and walks down from
// each until it passes the area or reaches a line already read. A uniformly
// formatted line is read as one run.
void ScanDocument(Uia& uia, IUIAutomationTextPattern* tp, const RectD& area, double dpi, double deadline,
                  std::vector<TextRun>& runs, std::set<std::wstring>& seen) {
    std::set<std::wstring> linesRead;
    for (double fy : {0.0, 0.5})
        for (double fx : {0.02, 0.25, 0.5, 0.75, 0.98}) {
            if (Seconds() > deadline) return;
            POINT start = {(LONG)(area.l + area.Width() * fx), (LONG)(area.t + 2 + (area.Height() - 4) * fy)};
            ComPtr<IUIAutomationTextRange> line;
            if (FAILED(tp->RangeFromPoint(start, &line)) || !line) continue;
            for (int lines = 0; lines < kMaxLines && Seconds() < deadline; ++lines) {
                line->ExpandToEnclosingUnit(TextUnit_Line);
                RectD lineRect;
                bool hasRect = RangeRect(line.Get(), lineRect);
                if (hasRect && lineRect.t > area.b) break;  // the rest are below the area
                if (hasRect) {
                    wchar_t key[64];
                    swprintf_s(key, L"%.0f,%.0f,%.0f", lineRect.l, lineRect.t, lineRect.r);
                    if (!linesRead.insert(key).second) break;  // joined a stretch already read
                }
                if (hasRect && Intersects(lineRect, area)) {
                    TextRun whole;
                    if (ReadFormat(uia, line.Get(), dpi, whole)) {
                        whole.text = RangeText(line.Get());
                        whole.rect = lineRect;
                        AddRun(std::move(whole), runs, seen);
                    } else {
                        // Mixed formats: split the line into format runs.
                        ComPtr<IUIAutomationTextRange> run;
                        line->Clone(&run);
                        run->MoveEndpointByRange(TextPatternRangeEndpoint_End, run.Get(), TextPatternRangeEndpoint_Start);
                        for (int k = 0; k < kMaxRunsPerLine; ++k) {
                            int cmp = 0;
                            run->CompareEndpoints(TextPatternRangeEndpoint_Start, line.Get(), TextPatternRangeEndpoint_End, &cmp);
                            if (cmp >= 0) break;
                            run->ExpandToEnclosingUnit(TextUnit_Format);
                            run->CompareEndpoints(TextPatternRangeEndpoint_Start, line.Get(), TextPatternRangeEndpoint_Start, &cmp);
                            if (cmp < 0) run->MoveEndpointByRange(TextPatternRangeEndpoint_Start, line.Get(), TextPatternRangeEndpoint_Start);
                            run->CompareEndpoints(TextPatternRangeEndpoint_End, line.Get(), TextPatternRangeEndpoint_End, &cmp);
                            if (cmp > 0) run->MoveEndpointByRange(TextPatternRangeEndpoint_End, line.Get(), TextPatternRangeEndpoint_End);
                            TextRun tr;
                            if (RangeRect(run.Get(), tr.rect) && Intersects(tr.rect, area)) {
                                ReadFormat(uia, run.Get(), dpi, tr);
                                tr.text = RangeText(run.Get());
                                AddRun(std::move(tr), runs, seen);
                            }
                            run->MoveEndpointByRange(TextPatternRangeEndpoint_Start, run.Get(), TextPatternRangeEndpoint_End);
                        }
                    }
                }
                int moved = 0;
                if (FAILED(line->Move(TextUnit_Line, 1, &moved)) || moved == 0) break;
            }
        }
}

// Other readings of a control's name that may be what is actually drawn:
// "Downloads (pinned)" shows "Downloads", "Start of Quick Access - Desktop
// (pinned)" shows "Desktop", "Alex - Personal (OneDrive)" shows "Alex -
// Personal". The pixel check decides which, if any, is right.
std::vector<std::wstring> NameVariants(const std::wstring& name) {
    auto trim = [](std::wstring s) {
        while (!s.empty() && s.back() == L' ') s.pop_back();
        while (!s.empty() && s.front() == L' ') s.erase(s.begin());
        return s;
    };
    auto noParens = [&](const std::wstring& s) {
        size_t open = s.rfind(L" (");
        return (!s.empty() && s.back() == L')' && open != std::wstring::npos) ? trim(s.substr(0, open)) : s;
    };
    std::vector<std::wstring> out;
    auto add = [&](const std::wstring& v) {
        if (v.empty() || v == name || std::none_of(v.begin(), v.end(), [](wchar_t c) { return iswalnum(c); }))
            return;
        if (std::find(out.begin(), out.end(), v) == out.end() && out.size() < 5) out.push_back(v);
    };
    add(noParens(name));
    for (const wchar_t* sep : {L", ", L". "}) {
        size_t cut = name.find(sep);
        if (cut != std::wstring::npos && cut > 0) add(trim(name.substr(0, cut)));
    }
    for (size_t pos = 0, next; pos < name.size(); pos = next + 3) {
        next = name.find(L" - ", pos);
        if (next == std::wstring::npos) next = name.size();
        if (pos > 0 || next < name.size()) {
            std::wstring part = trim(name.substr(pos, next - pos));
            add(noParens(part));
            add(part);
        }
        if (next >= name.size()) break;
    }
    return out;
}

struct WindowList {
    RectD area;
    std::vector<HWND> hwnds;
};

BOOL CALLBACK CollectWindow(HWND h, LPARAM lp) {
    auto* list = reinterpret_cast<WindowList*>(lp);
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    RECT rc;
    if (!GetWindowRect(h, &rc) || rc.right - rc.left < 8 || rc.bottom - rc.top < 8) return TRUE;
    if (!Intersects({(double)rc.left, (double)rc.top, (double)rc.right, (double)rc.bottom}, list->area)) return TRUE;
    list->hwnds.push_back(h);
    return list->hwnds.size() < 8;  // top of the z-order is enough
}

// Labels of on-screen controls in the windows over area, and any text hosts.
void ScanWindows(Scanner& s, const RectD& area, double dpi, double deadline, std::vector<TextRun>& labels,
                 std::vector<ComPtr<IUIAutomationTextPattern>>& hosts) {
    WindowList list;
    list.area = area;
    EnumWindows(CollectWindow, (LPARAM)&list);
    // Shell surfaces such as the Start menu live in a separate z-order band
    // and are not listed by EnumWindows, but they are what is under the
    // pointer when they are open: add whatever windows a grid of points over
    // the area lands on (topmost first).
    std::vector<HWND> onTop;
    for (int gy = 0; gy < 4; ++gy)
        for (int gx = 0; gx < 5; ++gx) {
            POINT pt = {(LONG)(area.l + area.Width() * (gx + 0.5) / 5), (LONG)(area.t + area.Height() * (gy + 0.5) / 4)};
            HWND h = GetAncestor(WindowFromPoint(pt), GA_ROOT);
            DWORD pid = 0;
            if (h) GetWindowThreadProcessId(h, &pid);
            if (h && pid != GetCurrentProcessId() && std::find(onTop.begin(), onTop.end(), h) == onTop.end())
                onTop.push_back(h);
        }
    for (HWND h : list.hwnds)
        if (std::find(onTop.begin(), onTop.end(), h) == onTop.end()) onTop.push_back(h);
    list.hwnds = onTop;
    if (list.hwnds.size() > 8) list.hwnds.resize(8);
    for (HWND h : list.hwnds) {
        double now = Seconds();
        if (now > deadline) break;
        if (s.slowUntil.count(h) && s.slowUntil[h] > now) continue;
        ComPtr<IUIAutomationElement> root;
        ComPtr<IUIAutomationElementArray> found;
        if (FAILED(s.uia.automation->ElementFromHandle(h, &root)) || !root ||
            FAILED(root->FindAllBuildCache(TreeScope_Descendants, s.condition.Get(), s.cache.Get(), &found)) || !found)
            continue;
        if (Seconds() - now > 1.0) s.slowUntil[h] = Seconds() + 30;
        wchar_t windowClassBuf[64] = {};
        GetClassNameW(h, windowClassBuf, 64);
        const std::wstring windowClass = windowClassBuf;
        int n = 0;
        found->get_Length(&n);
        // Documents in this window (web pages, editors): their text comes
        // from the document scan, so controls inside them are skipped.
        std::vector<RectD> docBoxes;
        for (int i = 0; i < n; ++i) {
            ComPtr<IUIAutomationElement> e;
            RECT rc;
            CONTROLTYPEID type = 0;
            if (SUCCEEDED(found->GetElement(i, &e)) && SUCCEEDED(e->get_CachedControlType(&type)) &&
                type == UIA_DocumentControlTypeId && SUCCEEDED(e->get_CachedBoundingRectangle(&rc)))
                docBoxes.push_back({(double)rc.left, (double)rc.top, (double)rc.right, (double)rc.bottom});
        }
        if (g_traceScan) {
            wchar_t cls[64] = {}, title[64] = {};
            GetClassNameW(h, cls, 64);
            GetWindowTextW(h, title, 64);
            Log(L"  window %s '%s': %d elements in %.0f ms", cls, title, n, (Seconds() - now) * 1000);
        }
        for (int i = 0; i < n && labels.size() < 400; ++i) {
            ComPtr<IUIAutomationElement> e;
            if (FAILED(found->GetElement(i, &e))) continue;
            RECT rc;
            CONTROLTYPEID type = 0;
            if (FAILED(e->get_CachedBoundingRectangle(&rc)) || FAILED(e->get_CachedControlType(&type))) continue;
            RectD box = {(double)rc.left, (double)rc.top, (double)rc.right, (double)rc.bottom};
            if (g_traceScan) {
                BSTR nm = nullptr;
                e->get_CachedName(&nm);
                Log(L"    element type %d '%s' %.0f,%.0f %.0fx%.0f", type, nm ? nm : L"", box.l, box.t, box.Width(), box.Height());
                SysFreeString(nm);
            }
            if (!Intersects(box, area)) continue;
            // Only controls that are actually visible: their left, middle and
            // right must all be in this window (not under another one).
            {
                const LONG cy = (LONG)box.Center().y;
                bool visible = true;
                for (LONG px : {(LONG)box.l + 3, (LONG)box.Center().x, (LONG)box.r - 3}) {
                    POINT pt = {px, cy};
                    if (GetAncestor(WindowFromPoint(pt), GA_ROOT) != h) {
                        visible = false;
                        break;
                    }
                }
                if (!visible) {
                    if (g_traceScan) {
                        BSTR nm = nullptr;
                        if (SUCCEEDED(e->get_CachedName(&nm)) && nm) {
                            POINT cp = {(LONG)box.Center().x, (LONG)box.Center().y};
                            HWND at = GetAncestor(WindowFromPoint(cp), GA_ROOT);
                            wchar_t cls[64] = {};
                            GetClassNameW(at, cls, 64);
                            Log(L"    hidden: '%s' at %.0f,%.0f (point is in %s)", nm, box.l, box.t, cls);
                        }
                        SysFreeString(nm);
                    }
                    continue;
                }
            }
            VARIANT tpv;
            VariantInit(&tpv);
            bool hostsText = SUCCEEDED(e->GetCachedPropertyValue(UIA_IsTextPatternAvailablePropertyId, &tpv)) &&
                             tpv.vt == VT_BOOL && tpv.boolVal != VARIANT_FALSE;
            VariantClear(&tpv);
            if (hostsText) {
                if (hosts.size() < 4 && (type == UIA_DocumentControlTypeId || type == UIA_EditControlTypeId)) {
                    ComPtr<IUIAutomationTextPattern> tp;
                    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&tp))) && tp) hosts.push_back(tp);
                }
                continue;
            }
            if (type == UIA_DocumentControlTypeId) continue;
            if (type == UIA_TextControlTypeId && std::any_of(docBoxes.begin(), docBoxes.end(), [&](const RectD& d) {
                    return box.l >= d.l && box.r <= d.r && box.t >= d.t && box.b <= d.b;
                }))
                continue;
            // Read-only fields (such as Explorer's Date modified / Type / Size
            // columns) are named after their column and show their value.
            std::wstring value;
            {
                VARIANT vv;
                VariantInit(&vv);
                if (SUCCEEDED(e->GetCachedPropertyValue(UIA_ValueValuePropertyId, &vv)) && vv.vt == VT_BSTR && vv.bstrVal)
                    value.assign(vv.bstrVal, SysStringLen(vv.bstrVal));
                VariantClear(&vv);
            }
            BSTR name = nullptr;
            std::wstring raw;
            if (type == UIA_EditControlTypeId) {
                raw = value;  // an edit field shows its value, not its name
            } else {
                if (FAILED(e->get_CachedName(&name)) || !name) continue;
                raw.assign(name, SysStringLen(name));
                SysFreeString(name);
            }
            // Menu items report "Copy\tCtrl+C": the label is the part before
            // the tab. "&" marks an access key ("&&" is a literal ampersand).
            raw = raw.substr(0, raw.find(L'\t'));
            std::wstring text;
            for (size_t c = 0; c < raw.size(); ++c) {
                if (raw[c] == L'&') {
                    if (c + 1 < raw.size() && raw[c + 1] == L'&') text += L'&', ++c;
                    continue;
                }
                text += raw[c];
            }
            while (!text.empty() && text.back() == L' ') text.pop_back();
            const double maxH = 160 * dpi / 96;  // tiles hold an icon above their name
            if (text.empty() || text.size() > 120 || text.find_first_of(L"\r\n") != std::wstring::npos ||
                box.Height() < 8 || box.Height() > maxH || box.Width() < 6 ||
                std::none_of(text.begin(), text.end(), [](wchar_t c) { return iswalnum(c); }))
                continue;
            TextRun tr;
            tr.text = text;
            tr.alternatives = NameVariants(text);
            if (type != UIA_EditControlTypeId && !value.empty() && value != text && value.size() <= 120 &&
                value.find_first_of(L"\r\n") == std::wstring::npos)
                tr.alternatives.push_back(value);
            tr.rect = tr.source = box;
            tr.label = true;
            wchar_t key[64];
            swprintf_s(key, L"|L%.0f,%.0f,%.0f,%.0f", box.l, box.t, box.r, box.b);
            tr.key = text + key;
            // Items that show an icon Windows has a large copy of (files and
            // folders in Explorer and on the desktop, apps in the Start menu
            // and on the taskbar).
            if (s.icons && (type == UIA_ListItemControlTypeId || type == UIA_ButtonControlTypeId ||
                            type == UIA_MenuItemControlTypeId || type == UIA_TreeItemControlTypeId ||
                            type == UIA_DataItemControlTypeId))
                if (auto img = s.icons->Find(h, windowClass, text)) {
                    TextRun ic;
                    ic.text = text;
                    ic.icon = img;
                    ic.rect = ic.source = box;
                    ic.key = L"icon|" + tr.key;
                    labels.push_back(std::move(ic));
                }
            labels.push_back(std::move(tr));
        }
    }
}

double Overlap(const RectD& a, const RectD& b) {
    double w = (std::min)(a.r, b.r) - (std::max)(a.l, b.l), h = (std::min)(a.b, b.b) - (std::max)(a.t, b.t);
    return w > 0 && h > 0 ? w * h : 0;
}

// Phase one: control labels in the windows over the view (nearest the point
// of attention first), plus the text hosts to read in phase two.
std::vector<TextRun> ScanLabels(Scanner& s, const RectD& view, Vec2 focus,
                                std::vector<ComPtr<IUIAutomationTextPattern>>& hosts) {
    const RectD area = Expand(view, 0.25, 0.25);
    const double dpi = MonitorDpi(view.Center());
    if (auto tp = FindTextHost(s.uia, focus)) hosts.push_back(tp);
    else if (auto tp2 = FindTextHost(s.uia, view.Center())) hosts.push_back(tp2);
    std::vector<TextRun> labels;
    ScanWindows(s, area, dpi, Seconds() + 1.0, labels, hosts);

    // Duplicates (a list item and its text child): keep the smallest box.
    std::sort(labels.begin(), labels.end(), [](const TextRun& a, const TextRun& b) {
        return a.source.Width() * a.source.Height() < b.source.Width() * b.source.Height();
    });
    std::vector<TextRun> kept;
    for (auto& l : labels) {
        bool dup = std::any_of(kept.begin(), kept.end(), [&](const TextRun& k) {
            return k.text == l.text && !k.icon == !l.icon && Overlap(k.source, l.source) > 0;
        });
        if (!dup) kept.push_back(std::move(l));
    }
    std::sort(kept.begin(), kept.end(), [&](const TextRun& a, const TextRun& b) {
        auto d = [&](const TextRun& r) {
            Vec2 c = r.source.Center();
            return (c.x - focus.x) * (c.x - focus.x) + (c.y - focus.y) * (c.y - focus.y);
        };
        return d(a) < d(b);
    });
    return kept;
}

// Phase two: formatted text from the text hosts.
std::vector<TextRun> ScanDocuments(Scanner& s, const RectD& view,
                                   const std::vector<ComPtr<IUIAutomationTextPattern>>& hosts) {
    const RectD area = Expand(view, 0.25, 0.25);
    const double dpi = MonitorDpi(view.Center());
    const double deadline = Seconds() + 1.0;
    std::vector<TextRun> docs;
    std::set<std::wstring> seen;
    for (auto& tp : hosts) ScanDocument(s.uia, tp.Get(), area, dpi, deadline, docs, seen);
    return docs;
}

// ---------------------------------------------------------------- icons

// An icon scaled down to s x s by area averaging (premultiplied), as floats.
struct ScaledIcon {
    int size = 0;
    std::vector<float> r, g, b, a;
};

const ScaledIcon& Scaled(const std::shared_ptr<const IconImage>& img, int s) {
    struct Entry {
        std::weak_ptr<const IconImage> img;
        ScaledIcon scaled;
    };
    thread_local std::map<std::pair<const IconImage*, int>, Entry> cache;
    if (cache.size() > 600) cache.clear();
    Entry& e = cache[{img.get(), s}];
    if (e.scaled.size == s && e.img.lock() == img) return e.scaled;
    e.img = img;
    ScaledIcon& out = e.scaled;
    out.size = s;
    // Windows' own artwork at this size, where there is one.
    if (auto it = img->atSize.find(s); it != img->atSize.end()) {
        const IconImage& sm = *it->second;
        out.r.resize((size_t)s * s), out.g.resize((size_t)s * s), out.b.resize((size_t)s * s), out.a.resize((size_t)s * s);
        for (size_t i = 0; i < (size_t)s * s; ++i) {
            const uint32_t p = sm.pixels[i];
            out.r[i] = ((p >> 16) & 0xFF) / 255.0f, out.g[i] = ((p >> 8) & 0xFF) / 255.0f;
            out.b[i] = (p & 0xFF) / 255.0f, out.a[i] = (p >> 24) / 255.0f;
        }
        return out;
    }
    out.r.assign((size_t)s * s, 0), out.g.assign((size_t)s * s, 0), out.b.assign((size_t)s * s, 0), out.a.assign((size_t)s * s, 0);
    const double fx = (double)img->width / s, fy = (double)img->height / s;
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            const int x0 = (int)(x * fx), x1 = (std::max)(x0 + 1, (int)((x + 1) * fx));
            const int y0 = (int)(y * fy), y1 = (std::max)(y0 + 1, (int)((y + 1) * fy));
            double sr = 0, sg = 0, sb = 0, sa = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint32_t p = img->pixels[(size_t)yy * img->width + xx];
                    sr += (p >> 16) & 0xFF, sg += (p >> 8) & 0xFF, sb += p & 0xFF, sa += p >> 24;
                }
            const double n = 255.0 * (x1 - x0) * (y1 - y0);
            const size_t i = (size_t)y * s + x;
            out.r[i] = (float)(sr / n), out.g[i] = (float)(sg / n), out.b[i] = (float)(sb / n), out.a[i] = (float)(sa / n);
        }
    return out;
}

// How well a scaled icon matches the pixels at (x, y): correlation over the
// three channels, and mean absolute error. Mostly solid pixels count fully;
// see-through ones are compared over background colour bg with less weight
// (the real background may be textured, such as the taskbar's acrylic).
void IconFit(const CpuFrame& f, const ScaledIcon& m, int x, int y, const Rgb& bg, double& ncc, double& err) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, ae = 0, n = 0;
    for (int j = 0; j < m.size; ++j)
        for (int i = 0; i < m.size; ++i) {
            const size_t k = (size_t)j * m.size + i;
            const double w = m.a[k] > 0.85 ? 1.0 : 0.15;
            const Rgb p = ToRgb(f.At(x + i, y + j));
            const double ea = 1 - m.a[k];
            const double e[3] = {m.r[k] + ea * bg.r, m.g[k] + ea * bg.g, m.b[k] + ea * bg.b};
            const double q[3] = {p.r, p.g, p.b};
            for (int ch = 0; ch < 3; ++ch) {
                sa += w * e[ch], sb += w * q[ch], saa += w * e[ch] * e[ch], sbb += w * q[ch] * q[ch], sab += w * e[ch] * q[ch];
                n += w;
            }
        }
    const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    ncc = va > 1e-6 && vb > 1e-6 ? (sab - sa * sb / n) / std::sqrt(va * vb) : -1;
    // The colour error after one overall brightness factor: Windows dims
    // the taskbar, for one, while a full-screen window is over it.
    const double gain = std::clamp(saa > 1e-9 ? sab / saa : 1.0, 0.6, 1.3);
    for (int j = 0; j < m.size; ++j)
        for (int i = 0; i < m.size; ++i) {
            const size_t k = (size_t)j * m.size + i;
            const double w = m.a[k] > 0.85 ? 1.0 : 0.15;
            const Rgb p = ToRgb(f.At(x + i, y + j));
            const double ea = 1 - m.a[k];
            ae += w * (std::fabs(gain * (m.r[k] + ea * bg.r) - p.r) + std::fabs(gain * (m.g[k] + ea * bg.g) - p.g) +
                       std::fabs(gain * (m.b[k] + ea * bg.b) - p.b));
        }
    err = ae / n;
}

// The background around a box: the most common colour on its edge pixels,
// or (on a textured background such as acrylic, false is returned) their
// mean.
bool EdgeColour(const CpuFrame& f, int x0, int y0, int x1, int y1, Rgb& out) {
    std::map<uint32_t, int> counts;
    int total = 0;
    auto add = [&](int x, int y) {
        if (x < f.left || y < f.top || x >= f.left + f.width || y >= f.top + f.height) return;
        ++counts[f.At(x, y) & 0xFCFCFC], ++total;
    };
    for (int x = x0; x < x1; ++x) add(x, y0), add(x, y1 - 1);
    for (int y = y0; y < y1; ++y) add(x0, y), add(x1 - 1, y);
    if (!total) return false;
    auto best = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    // The mean of the pixels in that group (not the group's rounded value,
    // which is visibly off: white would come out as 252).
    double r = 0, g = 0, b = 0;
    int n = 0;
    auto mean = [&](int x, int y) {
        if (x < f.left || y < f.top || x >= f.left + f.width || y >= f.top + f.height) return;
        const uint32_t p = f.At(x, y);
        if ((p & 0xFCFCFC) != best->first) return;
        const Rgb c = ToRgb(p);
        r += c.r, g += c.g, b += c.b, ++n;
    };
    for (int x = x0; x < x1; ++x) mean(x, y0), mean(x, y1 - 1);
    for (int y = y0; y < y1; ++y) mean(x0, y), mean(x1 - 1, y);
    out = {r / n, g / n, b / n};
    if (best->second >= total / 3) return true;
    r = g = b = 0;
    for (const auto& [key, count] : counts) {
        const Rgb c = ToRgb(key);
        r += c.r * count, g += c.g * count, b += c.b * count;
    }
    out = {r / total, g / total, b / total};
    return false;
}

// Where in its control's box an icon is drawn, and at what size: scaled
// copies at the usual icon sizes are compared with the pixels. Only a close
// match is accepted; c becomes the icon's square. around: re-check near
// an earlier place, at the same size.
bool MatchIcon(const CpuFrame& f, TextRun& c, double dpi, const RectD* around = nullptr) {
    int bx0, by0, bx1, by1;
    const RectD area = around ? RectD{around->l - 2, around->t - 2, around->r + 2, around->b + 2} : c.source;
    if (!c.icon || !PixelBounds(f, area, bx0, by0, bx1, by1)) return false;
    const int bw = bx1 - bx0, bh = by1 - by0;
    Rgb bg;
    const bool flatBox = EdgeColour(f, bx0, by0, bx1, by1, bg);  // textured: an average is enough to match by
    struct Fit {
        double ncc = -1, err = 1;
        int x = 0, y = 0, s = 0;
    };
    std::vector<Fit> fits;  // the best place at each size
    for (int s0 : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        const int s = (int)std::lround(s0 * dpi / 96);
        if (around && std::abs(s - (int)std::lround(around->Width())) > 1) continue;
        if (s > bw || s > bh || s < 10) continue;
        const ScaledIcon& m = Scaled(c.icon, s);
        // A wide row (a list item) has its icon near the left; elsewhere the
        // whole box is searched. Coarse steps first, then the best refined.
        const int xEnd = bw > 3 * bh && !around ? (std::min)(bx1 - s, bx0 + 2 * bh) : bx1 - s;
        const int step = around ? 1 : 2;
        int cx = -1, cy = -1;
        double coarse = -1;
        for (int y = by0; y <= by1 - s; y += step)
            for (int x = bx0; x <= xEnd; x += step) {
                double ncc, err;
                IconFit(f, m, x, y, bg, ncc, err);
                if (ncc > coarse) coarse = ncc, cx = x, cy = y;
            }
        if (cx < 0) continue;
        Fit fit;
        for (int y = cy - 1; y <= cy + 1; ++y)
            for (int x = cx - 1; x <= cx + 1; ++x) {
                if (x < bx0 || y < by0 || x + s > bx1 || y + s > by1) continue;
                double ncc, err;
                IconFit(f, m, x, y, bg, ncc, err);
                if (ncc > fit.ncc) fit = {ncc, err, x, y, s};
            }
        fits.push_back(fit);
    }
    // The best fit; where a larger size fits almost as well, the larger one
    // (a smaller icon placed inside a larger one also correlates well, and
    // would leave the original's edges showing around it).
    double best = -1, bestErr = 1;
    int bestX = 0, bestY = 0, bestS = 0;
    double top = -1;
    for (const Fit& ft : fits) top = (std::max)(top, ft.ncc);
    for (const Fit& ft : fits)
        if (ft.ncc >= top - 0.03 && ft.err <= 0.12 && ft.s > bestS)
            best = ft.ncc, bestErr = ft.err, bestX = ft.x, bestY = ft.y, bestS = ft.s;
    if (best < 0.85 || bestErr > 0.12) return false;
    c.rect = {(double)bestX, (double)bestY, (double)(bestX + bestS), (double)(bestY + bestS)};
    c.match = best;
    c.label = false;
    c.background = ToHex(bg);
    // Repaint the icon's square first if its surroundings are flat (so no
    // trace of the small icon shows through the large one's soft edges).
    Rgb ring;
    c.iconFill = flatBox && EdgeColour(f, bestX - 1, bestY - 1, bestS + bestX + 1, bestS + bestY + 1, ring) &&
                 std::fabs(Luma(ring) - Luma(bg)) < 0.02;
    return true;
}

// ---------------------------------------------------------------- shared check steps
// Used by the live threads and by the --text-test replay, so both decide alike.

constexpr double kMinMatch = 0.6, kMinLabelMatch = 0.7;

// A scanner candidate against the pixels. On success c is the placed run and
// extra may hold the second line of a wrapped label.
bool CheckCandidate(Verifier& v, const CpuFrame& f, TextRun& c, double dpi, const std::wstring& fallback,
                    bool useAppFont, std::vector<TextRun>& extra) {
    if (c.icon) return MatchIcon(f, c, dpi);
    if (c.label) {
        if (!CheckLabel(v, f, c, dpi, kMinLabelMatch, extra)) return false;
        c.control = true;
        for (auto& e : extra) e.control = true;
        return true;
    }
    if (!PlainText(f, c.rect, c.foreground, c.appForeground, c.background, c.foreground, &c.bgSlope)) return false;
    auto match = [&](TextRun& t) {
        t.match = MatchRun(v, f, t, fallback, useAppFont);
        // Not convincing as a whole (usually a substitute font drifting
        // along a long line): place word by word.
        if (t.match < 0.75) {
            TextRun w = t;
            w.match = MatchWords(v, f, w, fallback, useAppFont);
            if (w.match > t.match && w.match >= 0.62) {
                w.offsetX = w.offsetY = 0;
                w.padLeft = w.padRight = 4;  // words may sit a few pixels off the box
                t = std::move(w);
            }
        }
    };
    // The app's font is not installed (web fonts; browsers may not even name
    // them): one stand-in per reported style, chosen on its first line from
    // the usual sans, a serif and another sans, at the reported weight and
    // bold, so every line of a headline or paragraph is drawn alike.
    if (useAppFont && v.resolver->Resolve(c.font, fallback, true, c.weight, c.italic).family != c.font) {
        const std::wstring key = Substitutes::Key(c.font, c.sizePx, c.weight, c.italic);
        auto tryWith = [&](const std::wstring& family, int weight) {
            TextRun t = c;
            t.font = family;
            t.weight = weight;
            match(t);
            return t;
        };
        if (Substitutes::Entry sub; g_substitutes.Find(key, sub)) {
            c = tryWith(sub.family, sub.weight);
            return c.match >= kMinMatch;
        }
        TextRun best;
        best.match = -1;
        for (const std::wstring& family : {fallback, std::wstring(L"Georgia"), std::wstring(L"Arial")})
            for (int weight : {c.weight, 700}) {
                if (weight == 700 && c.weight >= 700) continue;
                TextRun t = tryWith(family, weight);
                // The usual stand-in, at the reported weight, unless another clearly fits better.
                const double bias = family == fallback && weight == c.weight ? 0.03 : 0;
                if (t.match + bias > best.match) best = std::move(t);
            }
        if (best.match >= kMinMatch) g_substitutes.Set(key, {best.font, best.weight});
        c = std::move(best);
        return c.match >= kMinMatch;
    }
    match(c);
    return c.match >= kMinMatch;
}

// The two fonts the apps' own text uses most among candidates (installed
// ones other than the system UI font).
std::vector<std::wstring> AppFonts(Verifier& v, const std::vector<TextRun>& cands, double dpi) {
    const std::wstring ui = SystemUiFont(dpi).family;
    std::map<std::wstring, size_t> count;
    for (const auto& c : cands)
        if (!c.label && !c.font.empty() && c.font != ui) count[c.font] += c.text.size();
    std::vector<std::pair<size_t, std::wstring>> order;
    for (auto& [font, n] : count)
        if (v.resolver->Resolve(font, ui, true, 400, false).family == font) order.push_back({n, font});
    std::sort(order.rbegin(), order.rend());
    std::vector<std::wstring> out;
    for (size_t i = 0; i < order.size() && i < 2; ++i) out.push_back(order[i].second);
    return out;
}

// Two redrawn runs on one line that overlap (accepted at different moments,
// say a label and a document run for the same words): only one may be drawn.
// App document text beats a control's label, which beats OCR; then the
// closer match wins.
void ResolveCollisions(std::vector<TextRun>& runs) {
    auto rank = [](const TextRun& r) { return r.ocr ? 0 : r.control ? 1 : 2; };
    std::vector<bool> drop(runs.size(), false);
    for (size_t i = 0; i < runs.size(); ++i)
        for (size_t j = i + 1; j < runs.size() && !drop[i]; ++j) {
            if (drop[j] || runs[i].icon || runs[j].icon) continue;
            const RectD &a = runs[i].rect, &b = runs[j].rect;
            const double vo = (std::min)(a.b, b.b) - (std::max)(a.t, b.t);
            const double ho = (std::min)(a.r, b.r) - (std::max)(a.l, b.l);
            if (vo < 0.6 * (std::min)(a.Height(), b.Height()) || ho <= 2) continue;
            const int ri = rank(runs[i]), rj = rank(runs[j]);
            const bool keepI = ri != rj ? ri > rj : runs[i].match >= runs[j].match;
            drop[keepI ? j : i] = true;
        }
    size_t k = 0;
    for (size_t i = 0; i < runs.size(); ++i)
        if (!drop[i]) runs[k++] = std::move(runs[i]);
    runs.resize(k);
}

// An accepted app run: it replaces anything stale it overlaps.
void PlaceRun(std::vector<TextRun>& runs, TextRun&& c) {
    runs.erase(std::remove_if(runs.begin(), runs.end(),
                              [&](const TextRun& r) { return !r.icon == !c.icon && Overlap(r.rect, c.rect) > 0; }),
               runs.end());
    runs.push_back(std::move(c));
}

// Whether shown runs already cover an OCR word's box.
bool OcrShown(const std::vector<TextRun>& shown, const TextRun& c) {
    const double area = c.source.Width() * c.source.Height();
    return std::any_of(shown.begin(), shown.end(), [&](const TextRun& r) { return Overlap(r.rect, c.source) > 0.3 * area; });
}

// A checked OCR word, shown if its pixels are unchanged. App-reported text
// wins: a word anywhere near a shown app run is skipped.
bool PlaceOcrRun(Verifier& v, const CpuFrame& f, std::vector<TextRun>& runs, TextRun&& c) {
    const RectD around = Expand(c.rect, 0.1, 0.2);
    const double area = c.rect.Width() * c.rect.Height();
    if (std::any_of(runs.begin(), runs.end(), [&](const TextRun& r) {
            if (r.icon) return false;
            return r.ocr ? Overlap(r.rect, c.rect) > 0.15 * area : Overlap(r.rect, around) > 0;
        }))
        return false;
    if (!CropUnchanged(c, f)) return false;
    const uint64_t line = c.ocrLine;
    runs.push_back(std::move(c));
    HarmoniseOcrLine(v, f, runs, line);
    return true;
}

// OCR candidates grouped by line, the lines nearest focus first.
std::vector<std::vector<TextRun>> OcrLines(std::vector<TextRun> cands, Vec2 focus) {
    std::map<uint64_t, std::vector<TextRun>> byLine;
    for (auto& c : cands) byLine[c.ocrLine].push_back(std::move(c));
    std::vector<std::vector<TextRun>> lines;
    for (auto& [id, l] : byLine) lines.push_back(std::move(l));
    auto dist = [&](const std::vector<TextRun>& l) {
        RectD b = l.front().source;
        for (const auto& c : l)
            b = {(std::min)(b.l, c.source.l), (std::min)(b.t, c.source.t), (std::max)(b.r, c.source.r),
                 (std::max)(b.b, c.source.b)};
        const double dx = focus.x < b.l ? b.l - focus.x : focus.x > b.r ? focus.x - b.r : 0;
        const double dy = focus.y < b.t ? b.t - focus.y : focus.y > b.b ? focus.y - b.b : 0;
        return dx * dx + dy * dy;
    };
    std::sort(lines.begin(), lines.end(), [&](const auto& a, const auto& b) { return dist(a) < dist(b); });
    return lines;
}

}  // namespace

// ---------------------------------------------------------------- threads

#if MAGNIFIER_DEV_TOOLS
void TextSource::TraceOcr(bool on) { g_traceOcr = on; }

void TextSource::ProbeScan(const RectD& view, Vec2 focus) {
    Scanner scanner;
    if (!scanner.Init()) {
        Log(L"Probe: UI Automation unavailable");
        return;
    }
    g_traceScan = true;
    for (int i = 0; i < 2; ++i) {  // the second pass shows the warm cost
        double t0 = Seconds();
        std::vector<ComPtr<IUIAutomationTextPattern>> hosts;
        std::vector<TextRun> labels = ScanLabels(scanner, view, focus, hosts);
        double t1 = Seconds();
        std::vector<TextRun> docs = ScanDocuments(scanner, view, hosts);
        Log(L"Probe pass %d: %zu labels in %.0f ms, %zu document runs from %zu hosts in %.0f ms", i + 1,
            labels.size(), (t1 - t0) * 1000, docs.size(), hosts.size(), (Seconds() - t1) * 1000);
        if (i == 1) {
            // Run the real checks against a still of the screen (GDI copy;
            // no overlay is running during a probe).
            CpuFrame f;
            f.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            f.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            f.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            f.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            f.pixels.resize((size_t)f.width * f.height);
            HDC screenDc = GetDC(nullptr), mem = CreateCompatibleDC(screenDc);
            BITMAPINFO bi = {};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), f.width, -f.height, 1, 32, BI_RGB};
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(mem, bmp);
            BitBlt(mem, 0, 0, f.width, f.height, screenDc, f.left, f.top, SRCCOPY);
            memcpy(f.pixels.data(), bits, f.pixels.size() * 4);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(nullptr, screenDc);

            Verifier verifier;
            if (!verifier.Init()) return;
            const double dpi = MonitorDpi(view.Center());
            int ok = 0, total = 0;
            double t2 = Seconds();
            for (auto& c : labels) {
                ++total;
                TextRun t = c;
                std::vector<TextRun> extra;
                bool accepted = CheckLabel(verifier, f, t, dpi, 0.7, extra);
                if (!extra.empty()) t.text += L" / " + extra[0].text;
                Log(L"    %s label '%s' as '%s' match %.2f %s w%d %.1fpx", accepted ? L"OK  " : L"skip",
                    c.text.substr(0, 40).c_str(), t.text.substr(0, 30).c_str(), t.match, t.font.c_str(), t.weight,
                    t.sizePx);
            }
            for (auto& c : docs) {
                ++total;
                TextRun t = c;
                bool plain = PlainText(f, t.rect, t.foreground, t.appForeground, t.background, t.foreground);
                double mt = plain ? MatchRun(verifier, f, t, L"Segoe UI", true) : -1;
                if (plain && mt < 0.75) {
                    TextRun w = t;
                    double mw = MatchWords(verifier, f, w, L"Segoe UI", true);
                    if (mw > mt && mw >= 0.62) mt = mw;
                }
                ok += mt >= 0.6;
                Log(L"    %s doc '%s' match %.2f %s w%d %.1fpx", mt >= 0.6 ? L"OK  " : L"skip",
                    c.text.substr(0, 40).c_str(), mt, c.font.c_str(), c.weight, c.sizePx);
            }
            Log(L"Probe checks: %d of %d accepted in %.0f ms", ok, total, (Seconds() - t2) * 1000);

            OcrReader reader;
            if (reader.Init()) {
                double t3 = Seconds();
                std::vector<OcrWord> words = reader.Read(f, view);
                double t4 = Seconds();
                std::vector<TextRun> cands = OcrCandidates(words, dpi);
                int okOcr = 0;
                for (auto& c : cands) {
                    TextRun t = c;
                    bool accepted = MatchOcrWord(verifier, f, t, dpi);
                    okOcr += accepted;
                    Log(L"    %s ocr '%s' at %.0f,%.0f h%.0f match %.2f %s w%d %.1fpx", accepted ? L"OK  " : L"skip",
                        c.text.c_str(), c.source.l, c.source.t, c.source.Height(), t.match, t.font.c_str(), t.weight,
                        t.sizePx);
                }
                Log(L"Probe OCR: %zu words in %.0f ms, %d of %zu candidates accepted in %.0f ms", words.size(),
                    (t4 - t3) * 1000, okOcr, cands.size(), (Seconds() - t4) * 1000);
            }
        }
    }
    g_traceScan = false;
}
#endif  // MAGNIFIER_DEV_TOOLS

std::vector<TextRun> TextSource::ScanCandidates(const RectD& view, Vec2 focus) {
    Scanner scanner;
    if (!scanner.Init()) return {};
    std::vector<ComPtr<IUIAutomationTextPattern>> hosts;
    std::vector<TextRun> labels = ScanLabels(scanner, view, focus, hosts);
    std::vector<TextRun> docs = ScanDocuments(scanner, view, hosts);
    for (auto& l : labels) docs.push_back(std::move(l));  // documents first, as the live scan hands them over
    return docs;
}

std::vector<TextRun> TextSource::CheckStill(const CpuFrame& f, std::vector<TextRun> candidates, const RectD& view,
                                            Vec2 focus, double dpi, const std::wstring& fallback, bool useAppFont,
                                            bool ocr, StillTiming* timing) {
    const double t0 = Seconds();
    Verifier verifier;
    if (!verifier.Init()) return {};
    const std::vector<TextRun> reported = candidates;  // checking moves the candidates
    verifier.appFonts = AppFonts(verifier, candidates, dpi);
    std::vector<TextRun> runs;
    for (auto& c : candidates) {
        if (std::any_of(runs.begin(), runs.end(), [&](const TextRun& r) { return r.key == c.key; })) continue;
        std::vector<TextRun> extra;
        if (!CheckCandidate(verifier, f, c, dpi, fallback, useAppFont, extra)) continue;
        TakeCrop(f, c);
        PlaceRun(runs, std::move(c));
        for (auto& e : extra) PlaceRun(runs, std::move(e));
    }
    const double t1 = Seconds();
    double t2 = t1;
    OcrReader reader;
    if (ocr && reader.Init()) {
        std::vector<TextRun> fresh;
        const RectD area = Expand(view, 0.05, 0.05);
        const std::vector<OcrWord> read2x = reader.Read(f, area), read1x = reader.Read(f, area, 1);
        t2 = Seconds();
        for (auto& c : OcrCandidates(AgreedWords(read2x, read1x, reported), dpi))
            if (!OcrShown(runs, c)) fresh.push_back(std::move(c));
        OcrPool pool;
        pool.Start(4);
        std::mutex mu;
        std::vector<TextRun> checked;
        pool.Verify(f, OcrLines(std::move(fresh), focus), dpi, runs, reported, [&](std::vector<TextRun>&& rs) {
            std::lock_guard g(mu);
            for (auto& r : rs) checked.push_back(std::move(r));
        });
        pool.Stop();
        // Lines finish in any order on the pool; place them in reading order
        // so results repeat.
        std::sort(checked.begin(), checked.end(), [](const TextRun& a, const TextRun& b) {
            return a.ocrLine != b.ocrLine ? a.ocrLine < b.ocrLine : a.rect.l < b.rect.l;
        });
        for (auto& c : checked) PlaceOcrRun(verifier, f, runs, std::move(c));
    }
    ResolveCollisions(runs);
    if (timing) *timing = {(t1 - t0) * 1000, (t2 - t1) * 1000, (Seconds() - t2) * 1000};
    return runs;
}

void TextSource::Start() {
    m_stop = false;
    m_icons.Start();
    m_scanner = std::thread([this] { ScanLoop(); });
    m_checker = std::thread([this] { CheckLoop(); });
    m_ocr = std::thread([this] { OcrLoop(); });
}

void TextSource::Stop() {
    {
        std::lock_guard lock(m_mu);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_scanner.joinable()) m_scanner.join();
    if (m_checker.joinable()) m_checker.join();
    if (m_ocr.joinable()) m_ocr.join();
    m_icons.Stop();
}

void TextSource::SetEnabled(bool on) {
    std::lock_guard lock(m_mu);
    if (m_enabled == on) return;
    m_enabled = on;
    m_rescan = true;
    if (!on) Publish({});
    m_cv.notify_all();
}

void TextSource::SetFonts(const std::wstring& fallback, bool useAppFont) {
    std::lock_guard lock(m_mu);
    if (fallback == m_fallbackFont && useAppFont == m_useAppFont) return;
    m_fallbackFont = fallback;
    m_useAppFont = useAppFont;
    Publish({});  // runs were matched with the old font
    m_rescan = true;
    m_cv.notify_all();
}

void TextSource::SetOcr(bool on) {
    std::lock_guard lock(m_mu);
    if (m_ocrEnabled == on) return;
    m_ocrEnabled = on;
    if (!on) {
        std::vector<TextRun> keep;
        for (const auto& r : m_scene->runs)
            if (!r.ocr) keep.push_back(r);
        Publish(std::move(keep));
        m_ocrCandidates.clear();
        m_ocrFresh = true;  // empties the checker's OCR queue
    }
    m_cv.notify_all();
}

void TextSource::SetIcons(bool on) {
    std::lock_guard lock(m_mu);
    if (m_iconsEnabled == on) return;
    m_iconsEnabled = on;
    if (!on) {
        std::vector<TextRun> keep;
        for (const auto& r : m_scene->runs)
            if (!r.icon) keep.push_back(r);
        Publish(std::move(keep));
    }
    m_rescan = true;
    m_cv.notify_all();
}

void TextSource::RequestScan() {
    {
        std::lock_guard lock(m_mu);
        m_rescan = true;
    }
    m_cv.notify_all();
}

void TextSource::SetView(const RectD& view, Vec2 focus) {
    std::lock_guard lock(m_mu);
    m_view = view;
    m_focus = focus;
}

void TextSource::SubmitFrame(std::shared_ptr<const CpuFrame> frame) {
    {
        std::lock_guard lock(m_mu);
        m_frame = std::move(frame);
    }
    m_cv.notify_all();
}

void TextSource::Invalidate(const RectD* area) {
    {
        std::lock_guard lock(m_mu);
        if (!m_scene->runs.empty()) {
            std::vector<TextRun> keep;
            if (area)
                for (const auto& r : m_scene->runs)
                    if (!Intersects(r.rect, *area)) keep.push_back(r);
            if (keep.size() != m_scene->runs.size()) Publish(std::move(keep));
        }
        m_rescan = true;
    }
    m_cv.notify_all();
}

std::shared_ptr<const TextScene> TextSource::Scene() {
    std::lock_guard lock(m_mu);
    return m_scene;
}

void TextSource::Publish(std::vector<TextRun> runs) {
    auto scene = std::make_shared<TextScene>();
    scene->generation = ++m_generation;
    scene->runs = std::move(runs);
    m_scene = std::move(scene);
}

// UI Automation only: produces candidates for the checker.
void TextSource::ScanLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Scanner scanner;
    if (!scanner.Init()) Log(L"UI Automation unavailable; text re-rendering disabled");
    double lastScan = -1e9;
    RectD lastView;
    int logged = 0;

    std::unique_lock lock(m_mu);
    while (!m_stop) {
        m_cv.wait_for(lock, std::chrono::milliseconds(100));
        if (m_stop) break;
        const RectD view = m_view;
        if (!m_enabled || !m_frame || view.Width() <= 0 || !scanner.condition) continue;
        bool moved = std::fabs(view.l - lastView.l) > view.Width() * 0.2 ||
                     std::fabs(view.t - lastView.t) > view.Height() * 0.2 ||
                     std::fabs(view.Width() - lastView.Width()) > view.Width() * 0.1;
        if (!(m_rescan || moved || Seconds() - lastScan > kScanInterval)) continue;
        m_rescan = false;
        auto before = m_frame;
        Vec2 focus = m_focus;
        scanner.icons = m_iconsEnabled ? &m_icons : nullptr;
        lock.unlock();

        double t0 = Seconds();
        std::vector<ComPtr<IUIAutomationTextPattern>> hosts;
        std::vector<TextRun> labels = ScanLabels(scanner, view, focus, hosts);
        double t1 = Seconds();
        lock.lock();
        m_candidates = std::move(labels);  // the checker can start on these now
        m_candidatesFrame = before;
        m_candidatesFresh = true;
        m_candidatesNewScan = true;
        m_cv.notify_all();
        lock.unlock();

        std::vector<TextRun> docs = ScanDocuments(scanner, view, hosts);
        if (logged < 5) {
            ++logged;
            Log(L"Text scan: labels %.0f ms, %zu document runs %.0f ms", (t1 - t0) * 1000, docs.size(),
                (Seconds() - t1) * 1000);
        }
        lock.lock();
        // Documents go to the front: they are the main reading text.
        for (auto& c : m_candidates) docs.push_back(std::move(c));
        m_candidates = std::move(docs);
        m_candidatesFrame = before;
        m_candidatesFresh = true;
        lastView = view;
        lastScan = Seconds();
        m_cv.notify_all();
    }
    lock.unlock();
    CoUninitialize();
}

// Windows OCR over the view as soon as its pixels change (at most every
// 0.1 s). Words are checked on a pool of threads and each line is handed to
// the checker as soon as it is confirmed.
void TextSource::OcrLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    OcrReader reader;
    OcrPool pool;
    bool tried = false, ready = false;
    uint64_t lastPrint = 0, lastVersion = 0;
    double lastRun = -1e9, gap = kOcrMinGap;
    RectD lastView;
    std::map<std::wstring, double> rejected;  // key + pixels -> when
    int logged = 0;

    std::unique_lock lock(m_mu);
    while (!m_stop) {
        m_cv.wait_for(lock, std::chrono::milliseconds(100));
        if (m_stop) break;
        if (!m_enabled || !m_ocrEnabled || !m_frame || m_view.Width() <= 0 || (tried && !ready)) continue;
        const RectD view = m_view;
        // Straight away when the view moves; otherwise back off while
        // passes find nothing new (an animation somewhere in view).
        if (std::fabs(view.l - lastView.l) + std::fabs(view.t - lastView.t) + std::fabs(view.Width() - lastView.Width()) > 2)
            gap = kOcrMinGap;
        if (m_frame->version == lastVersion || Seconds() - lastRun < gap) continue;
        lastView = view;
        auto frame = m_frame;
        const Vec2 focus = m_focus;
        auto scene = m_scene;
        lock.unlock();

        if (!tried) {
            tried = true;
            ready = reader.Init();
            if (ready) pool.Start(std::clamp((int)std::thread::hardware_concurrency() / 2, 2, 6));
        }
        lastVersion = frame->version;
        const RectD area = Expand(view, 0.05, 0.05);
        const uint64_t print = ready ? Fingerprint(*frame, area) : 0;
        if (!ready || print == lastPrint) {
            lock.lock();
            continue;
        }
        lastPrint = print;
        lastRun = Seconds();
        const double t0 = Seconds();
        std::vector<TextRun> reported;
        {
            std::lock_guard g(m_mu);
            reported = m_candidates;
        }
        std::vector<OcrWord> words = AgreedWords(reader.Read(*frame, area), reader.Read(*frame, area, 1), reported);
        const double t1 = Seconds();
        const double dpi = MonitorDpi(view.Center());

        // Skip words already shown (app text or an earlier OCR pass) and
        // words that failed on these same pixels recently.
        std::vector<TextRun> fresh;
        int skipped = 0;
        for (auto& c : OcrCandidates(words, dpi)) {
            if (OcrShown(scene->runs, c)) {
                ++skipped;
                continue;
            }
            c.key += L"|" + std::to_wstring(Fingerprint(*frame, c.source, 1));
            auto rej = rejected.find(c.key);
            if (rej != rejected.end() && Seconds() - rej->second < 30.0) {
                ++skipped;
                continue;
            }
            fresh.push_back(std::move(c));
        }
        std::vector<std::vector<TextRun>> lines = OcrLines(std::move(fresh), focus);

        std::vector<std::wstring> keys;
        for (const auto& l : lines)
            for (const auto& c : l) keys.push_back(c.key);
        {
            std::lock_guard g(m_mu);
            m_ocrCandidates.clear();  // anything not yet taken is from older pixels
        }
        std::atomic<int> accepted = 0;
        double firstAt = 0;
        std::mutex firstMu;
        std::set<std::wstring> acceptedKeys;
        pool.Verify(*frame, std::move(lines), dpi, scene->runs, reported, [&](std::vector<TextRun>&& runs) {
            {
                std::lock_guard g(firstMu);
                if (firstAt == 0) firstAt = Seconds();
                for (const auto& r : runs) acceptedKeys.insert(r.key);
            }
            accepted += (int)runs.size();
            std::lock_guard g(m_mu);
            if (m_stop || !m_ocrEnabled) return;
            for (auto& r : runs) m_ocrCandidates.push_back(std::move(r));
            m_ocrFresh = true;
            m_cv.notify_all();
        });
        gap = accepted > 1 ? kOcrMinGap : (std::min)(gap * 2, 0.8);
        const double now = Seconds();
        for (const auto& k : keys)
            if (!acceptedKeys.count(k)) rejected[k] = now;
        if (rejected.size() > 4000) rejected.clear();
        if (logged < 12) {
            ++logged;
            Log(L"OCR: %zu words in %.0f ms; %d shown, first after %.0f ms, all checked after %.0f ms (%d skipped)",
                words.size(), (t1 - t0) * 1000, accepted.load(), firstAt ? (firstAt - t0) * 1000 : 0.0,
                (now - t0) * 1000, skipped);
        }
        lock.lock();
    }
    lock.unlock();
    if (ready) pool.Stop();
    CoUninitialize();
}

// Pixels only: keeps the published runs true to the current frame and
// verifies new candidates within a small time budget per frame.
void TextSource::CheckLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Verifier verifier;
    if (!verifier.Init()) Log(L"Text verifier unavailable; text re-rendering disabled");
    const double kRetryAfter = 20.0, kBudget = 0.010;

    std::vector<TextRun> queue;  // candidates still to check, best first
    size_t next = 0;
    std::shared_ptr<const CpuFrame> queueFrame;
    std::vector<TextRun> ocrQueue;  // OCR words, already checked by the OCR pool
    std::map<std::wstring, double> rejected;  // key -> when
    uint64_t checked = 0;
    int logged = 0, accepted = 0;
    // For the log: when the current scan's candidates arrived, when the first
    // was accepted, and the time spent checking them.
    double queueStart = 0, firstAccept = 0, checkWork = 0;

    std::unique_lock lock(m_mu);
    while (!m_stop) {
        // Candidates are worked through a slice per frame (or per 100 ms when
        // the screen is still), never in a tight loop.
        m_cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return m_stop ||
                   (m_enabled && m_frame && (m_frame->version != checked || m_candidatesFresh || m_ocrFresh));
        });
        if (m_stop) break;
        if (!m_enabled || !m_frame || !verifier.resolver) continue;
        if (m_candidatesFresh) {
            // A new scan replaces the queue; its second half (documents)
            // comes with the labels not yet taken, so it replaces it too.
            if (m_candidatesNewScan || queue.empty()) queueStart = Seconds(), firstAccept = 0, checkWork = 0;
            queue = m_candidates;  // keep a copy: the document half is merged with it
            queueFrame = m_candidatesFrame;
            next = 0;
            if (verifier.resolver) verifier.appFonts = AppFonts(verifier, queue, MonitorDpi(m_view.Center()));
            m_candidatesFresh = false;
            m_candidatesNewScan = false;
        }
        if (m_ocrFresh) {
            for (auto& r : m_ocrCandidates) ocrQueue.push_back(std::move(r));
            m_ocrCandidates.clear();
            m_ocrFresh = false;
        }
        auto frame = m_frame;
        std::vector<TextRun> runs = m_scene->runs;
        const uint64_t generation = m_scene->generation;
        const RectD view = m_view;
        const std::wstring fallback = m_fallbackFont;
        const bool useAppFont = m_useAppFont;
        lock.unlock();

        bool changed = false, dropped = false;
        const uint64_t checkedBefore = checked;
        // 1. Published runs: unchanged pixels keep them; changed pixels get one re-check.
        if (frame->version != checked) {
            const RectD keepArea = Expand(view, 1.0, 1.0);
            std::vector<TextRun> keep;
            for (auto& r : runs) {
                if (!Intersects(r.rect, keepArea)) {
                    changed = true;  // far out of view: forget it
                } else if (CropUnchanged(r, *frame)) {
                    keep.push_back(std::move(r));
                } else if (r.icon ? MatchIcon(*frame, r, MonitorDpi(view.Center()), &r.rect) && (TakeCrop(*frame, r), true)
                                  : Reverify(verifier, *frame, r, fallback, useAppFont, r.ocr ? 0.8 : kMinMatch)) {
                    keep.push_back(std::move(r));
                    changed = true;
                } else {
                    changed = dropped = true;
                }
            }
            runs = std::move(keep);
            checked = frame->version;
        }

        // 2. New candidates, a few milliseconds' worth per frame.
        std::vector<TextRun> fresh;  // accepted in this pass
        const double sliceStart = Seconds(), deadline = sliceStart + kBudget;
        const double dpi = MonitorDpi(view.Center());
        const size_t nextBefore = next;
        while (next < queue.size() && Seconds() < deadline) {
            TextRun c = std::move(queue[next++]);
            std::vector<TextRun> extra;  // a second line of a wrapped label
            if (std::any_of(runs.begin(), runs.end(), [&](const TextRun& r) { return r.key == c.key; })) continue;
            auto rej = rejected.find(c.key);
            if (rej != rejected.end() && Seconds() - rej->second < kRetryAfter) continue;
            const bool ok = queueFrame && Unchanged(*queueFrame, *frame, c.source) &&
                            CheckCandidate(verifier, *frame, c, dpi, fallback, useAppFont, extra);
            if (!ok) {
                rejected[c.key] = Seconds();
                continue;
            }
            TakeCrop(*frame, c);
            fresh.push_back(c);
            for (const auto& e : extra) fresh.push_back(e);
            PlaceRun(runs, std::move(c));
            for (auto& e : extra) PlaceRun(runs, std::move(e));
            changed = true;
            ++accepted;
            if (!firstAccept) firstAccept = Seconds();
        }
        if (next != nextBefore) checkWork += Seconds() - sliceStart;
        // 3. OCR words, checked against the frame they were read from.
        for (auto& c : ocrQueue) {
            TextRun copy = c;
            if (PlaceOcrRun(verifier, *frame, runs, std::move(c))) fresh.push_back(std::move(copy)), changed = true;
        }
        ocrQueue.clear();
        if (rejected.size() > 4000) rejected.clear();
        if (logged < 8 && next >= queue.size() && !queue.empty()) {
            ++logged;
            const double now = Seconds();
            Log(L"Text check: %d accepted from %zu candidates, %zu runs shown; first after %.0f ms, all after %.0f ms "
                L"(%.0f ms of checking)",
                accepted, queue.size(), runs.size(), firstAccept ? (firstAccept - queueStart) * 1000 : 0.0,
                (now - queueStart) * 1000, checkWork * 1000);
            accepted = 0;
            queue.clear();
            next = 0;
        }

        lock.lock();
        if (m_stop) break;
        if (m_scene->generation != generation) {
            // The published runs changed while we worked (an invalidation,
            // say a tooltip appearing), so our result is for an old set. Put
            // what this pass newly accepted on top of the current set, and
            // check that against this same frame straight away, as every run
            // is: anything the change touched is dropped then. (Discarding
            // the pass lost items that came late in the queue, such as icons,
            // until the next scan; marking the frame as checked here once
            // left stale text on screen when nothing moved afterwards.)
            checked = checkedBefore;
            if (!fresh.empty()) {
                std::vector<TextRun> merged = m_scene->runs;
                for (auto& r : fresh) {
                    if (r.ocr) {
                        if (std::none_of(merged.begin(), merged.end(),
                                         [&](const TextRun& m) { return !m.icon && Overlap(m.rect, r.rect) > 0; }))
                            merged.push_back(std::move(r));
                    } else {
                        PlaceRun(merged, std::move(r));
                    }
                }
                ResolveCollisions(merged);
                Publish(std::move(merged));
            }
        } else if (changed) {
            ResolveCollisions(runs);
            Publish(std::move(runs));
        }
        if (dropped) m_rescan = true;
    }
    lock.unlock();
    CoUninitialize();
}
