#include "settings_window.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cmath>
#include <iterator>
#include <set>
#include <string>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {

enum Id : int {
    // Checkboxes and lists that change a setting: kOn..kFollowFocus.
    kOn = 101, kZoom, kFilter, kColour, kText, kOcr, kFont, kAppFont, kIcons, kFollowMouse, kFollowCaret, kFollowFocus,
    kPointerSettings, kStartup, kEditAll,
    kClose = IDCANCEL,  // Esc
};

constexpr double kZooms[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 7.0, 8.0, 10.0, 12.0, 14.0, 16.0};
constexpr const wchar_t* kFilterLabels[] = {L"Pixels (no smoothing)", L"Bilinear", L"Bicubic", L"Lanczos",
                                            L"Smooth (sharp text and edges)"};
constexpr const wchar_t* kColourLabels[] = {L"Normal", L"Inverted", L"Inverted brightness", L"Greyscale",
                                            L"Greyscale, inverted", L"Two colours"};
static_assert(std::size(kFilterLabels) == (size_t)Filter::Count);
static_assert(std::size(kColourLabels) == (size_t)ColorMode::Count);

HWND g_wnd = nullptr;
HFONT g_font = nullptr, g_heading = nullptr;
Settings g_current;
std::function<void(const Settings&)> g_apply;
bool g_filling = false;

int Nearest(const double* values, size_t n, double v) {
    size_t best = 0;
    for (size_t i = 1; i < n; ++i)
        if (std::fabs(values[i] - v) < std::fabs(values[best] - v)) best = i;
    return (int)best;
}

void Check(int id, bool on) { CheckDlgButton(g_wnd, id, on ? BST_CHECKED : BST_UNCHECKED); }
bool Checked(int id) { return IsDlgButtonChecked(g_wnd, id) == BST_CHECKED; }
int Selected(int id) { return (int)SendDlgItemMessageW(g_wnd, id, CB_GETCURSEL, 0, 0); }
void Select(int id, int index) { SendDlgItemMessageW(g_wnd, id, CB_SETCURSEL, index, 0); }

void Fill(const Settings& s) {
    g_filling = true;
    Check(kOn, s.enabled);
    Select(kZoom, Nearest(kZooms, std::size(kZooms), s.zoom));
    Select(kFilter, (int)s.filter);
    Select(kColour, (int)s.color);
    Check(kText, s.textEnabled);
    Check(kOcr, s.textOcr);
    SetDlgItemTextW(g_wnd, kFont, s.textFont.c_str());
    Check(kAppFont, s.textUseAppFont);
    Check(kIcons, s.iconsEnabled);
    Check(kFollowMouse, s.trackMouse);
    Check(kFollowCaret, s.trackCaret);
    Check(kFollowFocus, s.trackFocus);
    Check(kStartup, StartsAtSignIn());
    EnableWindow(GetDlgItem(g_wnd, kOcr), s.textEnabled);
    EnableWindow(GetDlgItem(g_wnd, kFont), s.textEnabled);
    EnableWindow(GetDlgItem(g_wnd, kAppFont), s.textEnabled);
    g_filling = false;
}

// font: the font box's text when it is not yet in the edit field (a list
// selection is reported before the edit field changes).
void ApplyFromControls(const std::wstring* font = nullptr) {
    if (g_filling) return;
    Settings s = g_current;
    s.enabled = Checked(kOn);
    if (int i = Selected(kZoom); i >= 0) s.zoom = kZooms[i];
    if (int i = Selected(kFilter); i >= 0) s.filter = (Filter)i;
    if (int i = Selected(kColour); i >= 0) s.color = (ColorMode)i;
    s.textEnabled = Checked(kText);
    s.textOcr = Checked(kOcr);
    std::wstring f;
    if (font) {
        f = *font;
    } else {
        wchar_t buf[LF_FACESIZE * 2] = {};
        GetDlgItemTextW(g_wnd, kFont, buf, (int)std::size(buf));
        f = buf;
    }
    while (!f.empty() && iswspace(f.back())) f.pop_back();
    while (!f.empty() && iswspace(f.front())) f.erase(f.begin());
    if (!f.empty()) s.textFont = f;
    s.textUseAppFont = Checked(kAppFont);
    s.iconsEnabled = Checked(kIcons);
    s.trackMouse = Checked(kFollowMouse);
    s.trackCaret = Checked(kFollowCaret);
    s.trackFocus = Checked(kFollowFocus);
    g_current = s;
    Fill(s);  // enable or disable dependent options
    if (g_apply) g_apply(s);
}

int CALLBACK AddFontFamily(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM param) {
    if (lf->lfFaceName[0] != L'@') reinterpret_cast<std::set<std::wstring>*>(param)->insert(lf->lfFaceName);
    return 1;
}

// Builds the controls: a column of labelled rows under small headings.
void CreateControls(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 5 / 4;  // a little larger than usual
    g_font = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 6 / 5;
    lf.lfWeight = FW_SEMIBOLD;
    g_heading = CreateFontIndirectW(&lf);

    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, g_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    const int em = tm.tmHeight;
    const int margin = em, rowH = em * 2, labelW = em * 8, ctrlW = em * 15, width = labelW + ctrlW;
    int y = em / 2, colX = 0;  // two columns, so the window fits on the screen at large text sizes

    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int w, int h, HFONT font) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, margin + colX + x, y, w, h, hwnd,
                                 (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
        return c;
    };
    auto heading = [&](const wchar_t* text) {
        y += em / 2;
        add(L"STATIC", text, 0, -1, 0, width, rowH, g_heading);
        y += rowH;
    };
    auto check = [&](int id, const wchar_t* text) {
        add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, 0, width, rowH, g_font);
        y += rowH;
    };
    auto combo = [&](int id, const wchar_t* label, DWORD style) {
        // The label first (Alt+letter moves to the next control), then
        // sized to the box's own height so their text lines up.
        HWND l = add(L"STATIC", label, SS_CENTERIMAGE, -1, 0, labelW, rowH, g_font);
        HWND c = add(L"COMBOBOX", L"", style | WS_TABSTOP | WS_VSCROLL, id, labelW, ctrlW, rowH * 10, g_font);
        RECT cr;
        GetWindowRect(c, &cr);
        SetWindowPos(l, nullptr, 0, 0, labelW, cr.bottom - cr.top, SWP_NOMOVE | SWP_NOZORDER);
        y += (std::max)(rowH, (int)(cr.bottom - cr.top)) + em / 3;
        return c;
    };

    heading(L"Magnifier");
    check(kOn, L"&Magnifier on");
    HWND zoom = combo(kZoom, L"&Zoom:", CBS_DROPDOWNLIST);
    for (double z : kZooms) {
        wchar_t b[32];
        swprintf_s(b, L"%g×", z);
        SendMessageW(zoom, CB_ADDSTRING, 0, (LPARAM)b);
    }
    HWND filter = combo(kFilter, L"Smoot&hing:", CBS_DROPDOWNLIST);
    for (auto l : kFilterLabels) SendMessageW(filter, CB_ADDSTRING, 0, (LPARAM)l);
    HWND colour = combo(kColour, L"&Colours:", CBS_DROPDOWNLIST);
    for (auto l : kColourLabels) SendMessageW(colour, CB_ADDSTRING, 0, (LPARAM)l);

    heading(L"Text");
    check(kText, L"&Redraw text sharply from its font");
    check(kOcr, L"Also read text in pictures (&OCR)");
    HWND font = combo(kFont, L"&Font:", CBS_DROPDOWN | CBS_SORT | CBS_AUTOHSCROLL);
    {
        std::set<std::wstring> families;
        LOGFONTW all = {};
        all.lfCharSet = DEFAULT_CHARSET;
        HDC sdc = GetDC(nullptr);
        EnumFontFamiliesExW(sdc, &all, AddFontFamily, (LPARAM)&families, 0);
        ReleaseDC(nullptr, sdc);
        for (const auto& f : families) SendMessageW(font, CB_ADDSTRING, 0, (LPARAM)f.c_str());
    }
    check(kAppFont, L"Use the app's own font when i&nstalled");
    check(kIcons, L"Draw &icons sharp from Windows' large icons");
    const int leftBottom = y;

    // Second column.
    y = em / 2;
    colX = width + margin * 2;
    heading(L"Follow");
    check(kFollowMouse, L"The mo&use pointer");
    check(kFollowCaret, L"The text cursor (c&aret)");
    check(kFollowFocus, L"The &keyboard focus");

    heading(L"Pointer");
    add(L"STATIC", L"The magnifier shows Windows' own mouse pointer. Change its size and colour in Windows "
                   L"accessibility settings.",
        0, -1, 0, width, rowH * 2, g_font);
    y += rowH * 2;
    add(L"BUTTON", L"Mouse &pointer settings...", BS_PUSHBUTTON | WS_TABSTOP, kPointerSettings, 0, em * 12, rowH,
        g_font);
    y += rowH + em / 3;

    heading(L"Start-up");
    check(kStartup, L"&Start Magnifier when I sign in");

    // Buttons along the bottom, under both columns.
    y = (std::max)(y, leftBottom) + em / 2;
    colX = 0;
    const int bw = em * 9, totalW = width * 2 + margin * 2;
    add(L"BUTTON", L"&Edit all settings...", BS_PUSHBUTTON | WS_TABSTOP, kEditAll, 0, bw, rowH, g_font);
    add(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, kClose, totalW - bw, bw, rowH, g_font);
    y += rowH + em;

    RECT r = {0, 0, totalW + 2 * margin, y};
    AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE), dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);

    // Keep it all on its monitor's work area.
    RECT wr;
    MONITORINFO mi = {sizeof(mi)};
    if (GetWindowRect(hwnd, &wr) && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& work = mi.rcWork;
        const LONG x = (std::max)(work.left, (std::min)(wr.left, work.right - (wr.right - wr.left)));
        const LONG top = (std::max)(work.top, (std::min)(wr.top, work.bottom - (wr.bottom - wr.top)));
        if (x != wr.left || top != wr.top) SetWindowPos(hwnd, nullptr, x, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_wnd = hwnd;
            CreateControls(hwnd);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kClose) {
                DestroyWindow(hwnd);
            } else if (id == kEditAll && code == BN_CLICKED) {
                ShellExecuteW(hwnd, L"open", SettingsPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (id == kPointerSettings && code == BN_CLICKED) {
                // Settings > Accessibility > Mouse pointer and touch.
                ShellExecuteW(hwnd, L"open", L"ms-settings:easeofaccess-mousepointer", nullptr, nullptr, SW_SHOWNORMAL);
            } else if (id == kStartup && code == BN_CLICKED) {
                if (!g_filling && !SetStartAtSignIn(Checked(kStartup))) {
                    Check(kStartup, StartsAtSignIn());
                    MessageBoxW(hwnd, L"The Startup folder could not be changed.", L"Magnifier", MB_ICONWARNING);
                }
            } else if (code == BN_CLICKED && id >= kOn && id <= kFollowFocus) {
                ApplyFromControls();
            } else if (code == CBN_SELCHANGE && id == kFont) {
                wchar_t buf[LF_FACESIZE * 2] = {};
                const int i = Selected(kFont);
                if (i >= 0 && SendDlgItemMessageW(hwnd, kFont, CB_GETLBTEXTLEN, i, 0) < (LRESULT)std::size(buf)) {
                    SendDlgItemMessageW(hwnd, kFont, CB_GETLBTEXT, i, (LPARAM)buf);
                    const std::wstring f = buf;
                    ApplyFromControls(&f);
                }
            } else if (code == CBN_SELCHANGE) {
                ApplyFromControls();
            } else if (code == CBN_KILLFOCUS && id == kFont) {
                ApplyFromControls();
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_DPICHANGED: {
            // Rebuild at the new size on the new monitor.
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(hwnd, GW_CHILD)) DestroyWindow(c);
            DeleteObject(g_font);
            DeleteObject(g_heading);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            CreateControls(hwnd);
            Fill(g_current);
            return 0;
        }
        case WM_DESTROY:
            g_wnd = nullptr;
            DeleteObject(g_font);
            DeleteObject(g_heading);
            g_font = g_heading = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring StartupLink() {
    PWSTR p = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p))) path = std::wstring(p) + L"\\Magnifier.lnk";
    CoTaskMemFree(p);
    return path;
}

}  // namespace

void ShowSettingsWindow(HINSTANCE instance, const Settings& current, std::function<void(const Settings&)> apply) {
    g_current = current;
    g_apply = std::move(apply);
    if (!g_wnd) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc = {sizeof(wc)};
            wc.lpfnWndProc = Proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
            wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wc.lpszClassName = L"MagnifierSettings";
            RegisterClassExW(&wc);
            registered = true;
        }
        CreateWindowExW(0, L"MagnifierSettings", L"Magnifier settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, 400, 400, nullptr, nullptr, instance, nullptr);
        if (!g_wnd) {
            Log(L"Settings window could not be created: %u", GetLastError());
            return;
        }
        Fill(g_current);
        ShowWindow(g_wnd, SW_SHOWNORMAL);
        SetFocus(GetDlgItem(g_wnd, kOn));
    } else {
        Fill(g_current);
        if (IsIconic(g_wnd)) ShowWindow(g_wnd, SW_RESTORE);
    }
    SetForegroundWindow(g_wnd);
}

void RefreshSettingsWindow(const Settings& current) {
    g_current = current;
    if (g_wnd) Fill(current);
}

bool SettingsWindowMessage(MSG* msg) { return g_wnd && IsDialogMessageW(g_wnd, msg); }

bool StartsAtSignIn() {
    const std::wstring link = StartupLink();
    return !link.empty() && GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SetStartAtSignIn(bool on) {
    const std::wstring link = StartupLink();
    if (link.empty()) return false;
    if (!on) return DeleteFileW(link.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.rfind(L'\\'));
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IShellLinkW> sl;
        ComPtr<IPersistFile> pf;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
            sl->SetPath(exe);
            sl->SetWorkingDirectory(dir.c_str());
            sl->SetDescription(L"Full-screen magnifier");
            ok = SUCCEEDED(sl.As(&pf)) && SUCCEEDED(pf->Save(link.c_str(), TRUE));
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}
