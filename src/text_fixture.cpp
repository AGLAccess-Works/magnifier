#include "text_fixture.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "image_io.h"
#include "log.h"
#include "ocr.h"
#include "renderer.h"
#include "text_draw.h"
#include "text_source.h"

namespace {

constexpr double kZoom = 3.0;

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool WriteText(const std::wstring& path, const std::wstring& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    std::string u = Utf8(text);
    fwrite(u.data(), 1, u.size(), f);
    fclose(f);
    return true;
}

bool ReadText(const std::wstring& path, std::wstring& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    std::string u;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) u.append(buf, n);
    fclose(f);
    text = Wide(u);
    return true;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(a, i - a));
            a = i + 1;
        }
    return out;
}

// Tabs, newlines and backslashes in text fields.
std::wstring Escape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\t') o += L"\\t";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') o += L"\\r";
        else o += c;
    }
    return o;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 == s.size()) {
            o += s[i];
            continue;
        }
        wchar_t c = s[++i];
        o += c == L't' ? L'\t' : c == L'n' ? L'\n' : c == L'r' ? L'\r' : c;
    }
    return o;
}

std::wstring Num(double v) {
    wchar_t b[32];
    swprintf_s(b, L"%.2f", v);
    return b;
}

std::wstring RectText(const RectD& r) { return Num(r.l) + L"\t" + Num(r.t) + L"\t" + Num(r.r) + L"\t" + Num(r.b); }

// Candidates, one per line: label, text, source, rect, font, size, weight,
// italic, foreground, background, appForeground, key, alternatives.
std::wstring RunsToText(const std::vector<TextRun>& runs) {
    std::wstring out;
    for (const auto& r : runs) {
        std::wstring alts;
        for (size_t i = 0; i < r.alternatives.size(); ++i) alts += (i ? L"\x1F" : L"") + Escape(r.alternatives[i]);
        wchar_t colours[64];
        swprintf_s(colours, L"%06X\t%06X\t%d", r.foreground, r.background, r.appForeground ? 1 : 0);
        out += std::wstring(r.label ? L"1" : L"0") + L"\t" + Escape(r.text) + L"\t" + RectText(r.source) + L"\t" +
               RectText(r.rect) + L"\t" + Escape(r.font) + L"\t" + Num(r.sizePx) + L"\t" + std::to_wstring(r.weight) +
               L"\t" + (r.italic ? L"1" : L"0") + L"\t" + colours + L"\t" + Escape(r.key) + L"\t" + alts + L"\n";
    }
    return out;
}

std::vector<TextRun> RunsFromText(const std::wstring& text) {
    std::vector<TextRun> runs;
    for (const auto& line : Split(text, L'\n')) {
        auto f = Split(line, L'\t');
        if (f.size() < 19) continue;
        TextRun r;
        r.label = f[0] == L"1";
        r.text = Unescape(f[1]);
        r.source = {_wtof(f[2].c_str()), _wtof(f[3].c_str()), _wtof(f[4].c_str()), _wtof(f[5].c_str())};
        r.rect = {_wtof(f[6].c_str()), _wtof(f[7].c_str()), _wtof(f[8].c_str()), _wtof(f[9].c_str())};
        r.font = Unescape(f[10]);
        r.sizePx = _wtof(f[11].c_str());
        r.weight = _wtoi(f[12].c_str());
        r.italic = f[13] == L"1";
        r.foreground = wcstoul(f[14].c_str(), nullptr, 16);
        r.background = wcstoul(f[15].c_str(), nullptr, 16);
        r.appForeground = f[16] == L"1";
        r.key = Unescape(f[17]);
        if (!f[18].empty())
            for (const auto& a : Split(f[18], L'\x1F')) r.alternatives.push_back(Unescape(a));
        runs.push_back(std::move(r));
    }
    return runs;
}

std::map<std::wstring, std::wstring> ReadMeta(const std::wstring& path) {
    std::map<std::wstring, std::wstring> m;
    std::wstring text;
    if (!ReadText(path, text)) return m;
    for (const auto& line : Split(text, L'\n')) {
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

double Overlap(const RectD& a, const RectD& b) {
    double w = (std::min)(a.r, b.r) - (std::max)(a.l, b.l), h = (std::min)(a.b, b.b) - (std::max)(a.t, b.t);
    return w > 0 && h > 0 ? w * h : 0;
}

const wchar_t* Kind(const TextRun& r) { return r.ocr ? L"ocr" : r.control ? L"label" : L"app"; }

std::wstring Describe(const TextRun& r) {
    wchar_t b[160];
    swprintf_s(b, L"\"%s\" %s w%d %.1fpx", r.text.substr(0, 40).c_str(), r.font.c_str(), r.weight, r.sizePx);
    return b;
}

// Runs drawn on one line beside each other (a sentence split into runs or
// words): they should share a style.
bool SameLine(const TextRun& a, const TextRun& b) {
    const double vo = (std::min)(a.rect.b, b.rect.b) - (std::max)(a.rect.t, b.rect.t);
    if (vo < 0.6 * (std::min)(a.rect.Height(), b.rect.Height())) return false;
    const double gap = (std::max)(a.rect.l, b.rect.l) - (std::min)(a.rect.r, b.rect.r);
    return gap < 1.2 * (std::max)(a.sizePx, b.sizePx);
}

bool SameStyle(const TextRun& a, const TextRun& b) {
    return a.font == b.font && a.weight == b.weight && a.italic == b.italic &&
           std::fabs(a.sizePx - b.sizePx) <= 0.12 * (std::max)(a.sizePx, b.sizePx);
}

// Words OCR sees in view (2x reading, letters or digits, two or more
// characters) that no redrawn run covers: what is still left blurry.
std::vector<OcrWord> Uncovered(const CpuFrame& f, const RectD& view, const std::vector<TextRun>& runs) {
    std::vector<OcrWord> out;
    OcrReader reader;
    if (!reader.Init()) return out;
    for (const auto& w : reader.Read(f, view)) {
        if (w.text.size() < 2 || std::none_of(w.text.begin(), w.text.end(), [](wchar_t c) { return iswalnum(c); }))
            continue;
        const double area = w.box.Width() * w.box.Height();
        if (std::none_of(runs.begin(), runs.end(), [&](const TextRun& r) { return Overlap(r.rect, w.box) > 0.5 * area; }))
            out.push_back(w);
    }
    return out;
}

std::wstring Report(std::vector<TextRun> runs, const std::vector<OcrWord>& uncovered) {
    std::sort(runs.begin(), runs.end(), [](const TextRun& a, const TextRun& b) {
        const double ta = std::round(a.rect.t / 4), tb = std::round(b.rect.t / 4);
        return ta != tb ? ta < tb : a.rect.l < b.rect.l;
    });
    int nApp = 0, nLabel = 0, nOcr = 0;
    for (const auto& r : runs) (r.ocr ? nOcr : r.control ? nLabel : nApp)++;

    std::vector<std::wstring> collisions, mixed;
    for (size_t i = 0; i < runs.size(); ++i)
        for (size_t j = i + 1; j < runs.size(); ++j) {
            const TextRun &a = runs[i], &b = runs[j];
            if (SameLine(a, b) && (std::min)(a.rect.r, b.rect.r) - (std::max)(a.rect.l, b.rect.l) > 2.0)
                collisions.push_back(std::wstring(Kind(a)) + L" " + Describe(a) + L"  x  " + Kind(b) + L" " + Describe(b));
            else if (SameLine(a, b) && !SameStyle(a, b))
                mixed.push_back(std::wstring(Kind(a)) + L" " + Describe(a) + L"  |  " + Kind(b) + L" " + Describe(b));
        }

    std::wstring out;
    out += L"# runs " + std::to_wstring(runs.size()) + L" (app " + std::to_wstring(nApp) + L", label " +
           std::to_wstring(nLabel) + L", ocr " + std::to_wstring(nOcr) + L")\n";
    out += L"# collisions " + std::to_wstring(collisions.size()) + L"\n";
    for (const auto& c : collisions) out += L"#   " + c + L"\n";
    out += L"# mixed-style neighbours " + std::to_wstring(mixed.size()) + L"\n";
    for (const auto& m : mixed) out += L"#   " + m + L"\n";
    out += L"# not redrawn " + std::to_wstring(uncovered.size()) + L"\n";
    for (const auto& w : uncovered) {
        wchar_t b[64];
        swprintf_s(b, L" @ %.0f,%.0f h%.0f", w.box.l, w.box.t, w.box.Height());
        out += L"#   \"" + w.text + L"\"" + b + L"\n";
    }
    for (const auto& r : runs) {
        wchar_t b[256];
        swprintf_s(b, L"\t%.0f,%.0f\t%.0fx%.0f\t%s w%d %.1fpx\t%.2f", r.rect.l, r.rect.t, r.rect.Width(),
                   r.rect.Height(), r.font.c_str(), r.weight, r.sizePx, r.match);
        out += std::wstring(Kind(r)) + L"\t" + Escape(r.text) + b + L"\n";
    }
    return out;
}

// ------------------------------------------------------------ scoring
//
// truth.tsv lists every word really on screen in the scene: text, then its
// box (x, y, width, height, desktop pixels), tab-separated; # starts a comment.
// Each redrawn run is cut into words, and each word's box estimated (from
// where the checker placed it, or by its share of the run's characters).
// Then every true word is:
//   correct  a redrawn word over it has the same text;
//   wrong    a redrawn word over it has different text (letters or digits,
//            or punctuation only);
//   partial  only part of it is redrawn, the rest left to the filter (it
//            reads right, but is only partly sharp);
//   missed   nothing redrawn over it (left to the scaling filter).
// A redrawn word over no true word is a phantom (icons or marks read as
// text), counted as wrong.

struct Word {
    std::wstring text;
    RectD box;
    bool used = false;
};

// "# area x y w h" limits the scoring to that part of the screen (the truth
// covers only it); redrawn words elsewhere are not counted. "# ignore x y w h"
// marks text the truth cannot list (a word wrapped onto two lines, a word cut
// off at the edge): redrawn words over it are not counted either.
std::vector<Word> ReadTruth(const std::wstring& path, RectD& area, std::vector<RectD>& ignore) {
    std::vector<Word> out;
    std::wstring text;
    area = {-1e9, -1e9, 1e9, 1e9};
    if (!ReadText(path, text)) return out;
    for (auto line : Split(text, L'\n')) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        double ax, ay, aw, ah;
        if (swscanf_s(line.c_str(), L"# area %lf %lf %lf %lf", &ax, &ay, &aw, &ah) == 4) area = {ax, ay, ax + aw, ay + ah};
        if (swscanf_s(line.c_str(), L"# ignore %lf %lf %lf %lf", &ax, &ay, &aw, &ah) == 4)
            ignore.push_back({ax, ay, ax + aw, ay + ah});
        if (line.empty() || line[0] == L'#') continue;
        auto f = Split(line, L'\t');
        if (f.size() < 5) continue;
        const double x = _wtof(f[1].c_str()), y = _wtof(f[2].c_str());
        out.push_back({Unescape(f[0]), {x, y, x + _wtof(f[3].c_str()), y + _wtof(f[4].c_str())}});
    }
    return out;
}

std::vector<Word> DrawnWords(const std::vector<TextRun>& runs) {
    std::vector<Word> out;
    for (const auto& r : runs) {
        if (r.icon) continue;
        const double l = r.rect.l + r.offsetX, t = r.rect.t + r.offsetY, w = r.rect.Width(), h = r.rect.Height();
        if (!r.words.empty()) {
            for (size_t i = 0; i < r.words.size(); ++i) {
                const WordPlace& p = r.words[i];
                const double right = i + 1 < r.words.size() ? r.words[i + 1].x : l + w;
                for (const auto& piece : Split(r.text.substr(p.start, p.length), L' '))
                    if (!piece.empty()) out.push_back({piece, {p.x, t + p.dy, (std::max)(right - 2, p.x + 1), t + p.dy + h}});
            }
            continue;
        }
        const double n = (double)(std::max<size_t>)(r.text.size(), 1);
        size_t a = 0;
        for (size_t i = 0; i <= r.text.size(); ++i)
            if (i == r.text.size() || r.text[i] == L' ') {
                if (i > a) out.push_back({r.text.substr(a, i - a), {l + w * a / n, t, l + w * i / n, t + h}});
                a = i + 1;
            }
    }
    return out;
}

// Most of the shorter box's height in common, and most of the narrower box's
// width or the drawn word's middle within the true word (a drawn word's box
// is only an estimate; short words can sit a few pixels off).
bool Over(const RectD& truth, const RectD& drawn) {
    const double ow = (std::min)(truth.r, drawn.r) - (std::max)(truth.l, drawn.l);
    const double oh = (std::min)(truth.b, drawn.b) - (std::max)(truth.t, drawn.t);
    if (oh <= 0.5 * (std::min)(truth.Height(), drawn.Height())) return false;
    const double slack = 0.3 * truth.Height(), cx = drawn.Center().x;
    return ow > 0.5 * (std::min)(truth.Width(), drawn.Width()) || (cx > truth.l - slack && cx < truth.r + slack);
}

bool HasLetterOrDigit(const std::wstring& s) {
    return std::any_of(s.begin(), s.end(), [](wchar_t c) { return iswalnum(c) != 0; });
}

bool OnlyPunctuation(const std::wstring& a, const std::wstring& b) {
    auto core = [](const std::wstring& s) {
        std::wstring c;
        for (wchar_t ch : s)
            if (iswalnum(ch)) c += ch;
        return c;
    };
    return core(a) == core(b);
}

std::wstring Score(const std::vector<Word>& truthIn, const RectD& area, const std::vector<RectD>& ignore,
                   const std::vector<TextRun>& runs, const TextSource::StillTiming& tm) {
    std::vector<Word> truth = truthIn, drawn;
    // Drawn words wholly in the scored area (as true words must be wholly in
    // view), with a letter or digit, and not over text the truth cannot list.
    for (auto& d : DrawnWords(runs))
        if (d.box.l >= area.l - 1 && d.box.r <= area.r + 1 && d.box.t >= area.t - 1 && d.box.b <= area.b + 1 &&
            HasLetterOrDigit(d.text) &&
            std::none_of(ignore.begin(), ignore.end(), [&](const RectD& g) { return Overlap(g, d.box) > 0; }))
            drawn.push_back(std::move(d));
    int correct = 0, wrong = 0, punct = 0, partial = 0, missed = 0, phantom = 0;
    std::wstring detail;
    auto at = [](const RectD& r) {
        wchar_t b[48];
        swprintf_s(b, L" @ %.0f,%.0f h%.0f", r.l, r.t, r.Height());
        return std::wstring(b);
    };
    for (auto& t : truth) {
        std::vector<Word*> over;
        for (auto& d : drawn)
            if (Over(t.box, d.box)) over.push_back(&d);
        if (over.empty()) {
            ++missed;
            detail += L"missed\t\"" + t.text + L"\"" + at(t.box) + L"\n";
            continue;
        }
        Word* same = nullptr;
        for (Word* d : over)
            if (d->text == t.text) same = d;
        for (Word* d : over) d->used = true;
        if (same) {
            ++correct;
            continue;
        }
        std::wstring shown, joined;
        for (Word* d : over) shown += (shown.empty() ? L"" : L" ") + d->text, joined += d->text;
        // Part of the word redrawn, the rest left to the filter: it still
        // reads right, but is only partly sharp.
        if (t.text.find(joined) != std::wstring::npos) {
            ++partial;
            detail += L"partial\t\"" + t.text + L"\" drawn as \"" + shown + L"\"" + at(t.box) + L"\n";
            continue;
        }
        const bool p = OnlyPunctuation(t.text, shown);
        ++(p ? punct : wrong);
        detail += std::wstring(p ? L"punct" : L"WRONG") + L"\t\"" + t.text + L"\" drawn as \"" + shown + L"\"" + at(t.box) + L"\n";
    }
    for (const auto& d : drawn)
        if (!d.used && std::none_of(truth.begin(), truth.end(), [&](const Word& t) { return Over(t.box, d.box); })) {
            ++phantom;
            detail += L"PHANTOM\t\"" + d.text + L"\"" + at(d.box) + L"\n";
        }
    wchar_t head[400];
    swprintf_s(head, L"# score words=%zu correct=%d wrong=%d punct=%d partial=%d missed=%d phantom=%d drawn=%zu\n",
               truth.size(), correct, wrong, punct, partial, missed, phantom, drawn.size());
    wchar_t time[200];
    swprintf_s(time, L"# timing app_ms=%.0f ocr_read_ms=%.0f ocr_verify_ms=%.0f\n", tm.appText, tm.ocrRead, tm.ocrVerify);
    return truth.empty() ? std::wstring(time) : head + std::wstring(time) + detail;
}

bool RenderFixture(const CpuFrame& f, const RectD& view, const std::vector<TextRun>& runs, const std::wstring& path) {
    const UINT outW = (UINT)std::lround(view.Width() * kZoom), outH = (UINT)std::lround(view.Height() * kZoom);
    Renderer r;
    if (!r.Init(nullptr, outW, outH)) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = f.width;
    desc.Height = f.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {f.pixels.data(), (UINT)f.width * 4, 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(r.Device()->CreateTexture2D(&desc, &data, &tex)) ||
        FAILED(r.Device()->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return false;

    Viewport vp;
    vp.zoom = kZoom;
    vp.origin = {view.l, view.t};
    vp.output = {0, 0, (double)outW, (double)outH};
    std::vector<TextDraw> draws;
    for (const auto& run : runs) draws.push_back(MakeTextDraw(run, vp, 0, 0));

    RenderParams rp;  // default settings, so results do not depend on the user's
    rp.srcOriginX = view.l - f.left;
    rp.srcOriginY = view.t - f.top;
    rp.zoom = kZoom;
    rp.sourceVersion = 1;
    rp.text = &draws;
    r.Render(srv.Get(), f.width, f.height, rp, path.c_str());
    r.Shutdown();
    return true;
}

}  // namespace

int CaptureTextFixture(const std::wstring& dir, POINT pt) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CreateDirectoryW(dir.c_str(), nullptr);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& m = mi.rcMonitor;
    const double w = (m.right - m.left) / kZoom, h = (m.bottom - m.top) / kZoom;
    const double vl = std::clamp(pt.x - w / 2, (double)m.left, m.right - w);
    const double vt = std::clamp(pt.y - h / 2, (double)m.top, m.bottom - h);
    const RectD view = {vl, vt, vl + w, vt + h};
    const int cl = (std::max)((int)m.left, (int)std::floor(vl - w * 0.25));
    const int ct = (std::max)((int)m.top, (int)std::floor(vt - h * 0.25));
    const int cr = (std::min)((int)m.right, (int)std::ceil(vl + w * 1.25));
    const int cb = (std::min)((int)m.bottom, (int)std::ceil(vt + h * 1.25));
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);

    // The pixels first, as the live checker's frame is from before the scan.
    std::vector<uint32_t> px((size_t)(cr - cl) * (cb - ct));
    HDC screenDc = GetDC(nullptr), mem = CreateCompatibleDC(screenDc);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), cr - cl, -(cb - ct), 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, cr - cl, cb - ct, screenDc, cl, ct, SRCCOPY);
    memcpy(px.data(), bits, px.size() * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screenDc);
    if (!SavePngBGRA((dir + L"\\screen.png").c_str(), cr - cl, cb - ct, px.data(), (cr - cl) * 4)) return 2;

    std::vector<TextRun> cands = TextSource::ScanCandidates(view, {(double)pt.x, (double)pt.y});
    WriteText(dir + L"\\runs.tsv", RunsToText(cands));
    wchar_t meta[512];
    swprintf_s(meta, L"left=%d\ntop=%d\nview=%.2f %.2f %.2f %.2f\nfocus=%ld %ld\ndpi=%u\n", cl, ct, view.l, view.t,
               view.r, view.b, pt.x, pt.y, dpiX);
    WriteText(dir + L"\\meta.txt", meta);
    Log(L"Fixture %s: %dx%d at %d,%d, %zu candidates", dir.c_str(), cr - cl, cb - ct, cl, ct, cands.size());
    return 0;
}

int RunTextTest(const std::wstring& dir, const std::wstring& outDir) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto meta = ReadMeta(dir + L"\\meta.txt");
    std::wstring runsText;
    CpuFrame f;
    UINT w = 0, h = 0;
    if (meta.empty() || !ReadText(dir + L"\\runs.tsv", runsText) ||
        !LoadImageBGRA((dir + L"\\screen.png").c_str(), w, h, f.pixels)) {
        Log(L"Text test %s: missing files", dir.c_str());
        return 2;
    }
    f.left = _wtoi(meta[L"left"].c_str());
    f.top = _wtoi(meta[L"top"].c_str());
    f.width = (int)w;
    f.height = (int)h;
    f.version = 1;
    RectD view;
    swscanf_s(meta[L"view"].c_str(), L"%lf %lf %lf %lf", &view.l, &view.t, &view.r, &view.b);
    Vec2 focus;
    swscanf_s(meta[L"focus"].c_str(), L"%lf %lf", &focus.x, &focus.y);
    const double dpi = _wtof(meta[L"dpi"].c_str());

    TextSource::StillTiming timing;
    TextSource::TraceOcr(GetEnvironmentVariableW(L"MAGNIFIER_TRACE_OCR", nullptr, 0) > 0);
    std::vector<TextRun> runs =
        TextSource::CheckStill(f, RunsFromText(runsText), view, focus, dpi, L"Segoe UI", true, true, &timing);
    CreateDirectoryW(outDir.c_str(), nullptr);
    WriteText(outDir + L"\\result.txt", Report(runs, Uncovered(f, view, runs)));
    // Against the words really on screen, where the scene lists them.
    // Only words wholly in view count, and only redrawn words over the truth's area.
    RectD area;
    std::vector<RectD> ignore;
    std::vector<Word> truth;
    for (auto& t : ReadTruth(dir + L"\\truth.tsv", area, ignore))
        if (t.box.l >= view.l && t.box.r <= view.r && t.box.t >= view.t && t.box.b <= view.b) truth.push_back(std::move(t));
    area = {(std::max)(area.l, view.l), (std::max)(area.t, view.t), (std::min)(area.r, view.r), (std::min)(area.b, view.b)};
    WriteText(outDir + L"\\score.txt", Score(truth, area, ignore, runs, timing));
    // MAGNIFIER_OCR_SCALES: also write what OCR reads at 1x to 4x.
    if (GetEnvironmentVariableW(L"MAGNIFIER_OCR_SCALES", nullptr, 0)) {
        OcrReader reader;
        std::wstring dump;
        if (reader.Init())
            for (int sc = 1; sc <= 4; ++sc)
                for (const auto& wd : reader.Read(f, view, sc)) {
                    wchar_t b[96];
                    swprintf_s(b, L"%d\t%.0f,%.0f\t", sc, wd.box.l, wd.box.t);
                    dump += b + wd.text + L"\n";
                }
        WriteText(outDir + L"\\ocr_scales.txt", dump);
    }
    if (!RenderFixture(f, view, runs, outDir + L"\\render.png")) Log(L"Text test %s: render failed", dir.c_str());
    return 0;
}
