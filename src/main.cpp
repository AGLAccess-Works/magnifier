// Magnifier: full-screen magnifier for Windows.
//
// Threads:
//   UI thread      windows, tray icon, hotkeys, WinEvent hooks, settings
//   render thread  capture polling, viewport/tracking, D3D drawing, present
//   tracker worker caret/focus geometry queries (see tracking.h)
//
// Input stays correct because the overlay is click-through and the real
// pointer is hidden: we draw the pointer at the magnified position of its
// real location, so what is under the drawn pointer is what gets clicked.
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>

#include <winrt/base.h>

#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "image_io.h"
#include "cursor.h"
#include "cursor_hide.h"
#include "log.h"
#include "overlay.h"
#include "readback.h"
#include "renderer.h"
#include "settings.h"
#include "settings_window.h"
#include "text_draw.h"
#include "text_source.h"
#if MAGNIFIER_DEV_TOOLS
#include "text_fixture.h"
#endif
#include "tracking.h"
#include "viewport.h"

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_FIRSTFRAME = WM_APP + 2;
constexpr UINT WM_APP_RAISE = WM_APP + 3;
constexpr UINT WM_APP_RENDERFAILED = WM_APP + 4;
constexpr UINT WM_APP_WHEELZOOM = WM_APP + 5;  // wParam: +1 in, -1 out
constexpr UINT WM_APP_SETTINGS = WM_APP + 6;   // another start with --settings: open the settings window
constexpr UINT WM_APP_REBUILD = WM_APP + 7;    // the display may have changed: re-read it and start rendering afresh
constexpr UINT WM_APP_MONITOR = WM_APP + 8;    // magnify the monitor at (wParam, lParam) instead
constexpr int kMaxKeysPerAction = 16;

enum TrayCommand : UINT { kCmdToggle = 1, kCmdInvert, kCmdOpenSettings, kCmdReload, kCmdExit, kCmdSettings };

// State shared between the UI and render threads.
struct Shared {
    std::mutex mu;
    Settings settings;        // guarded by mu
    HMONITOR monitor = nullptr;  // guarded by mu
    RECT monitorRect = {};       // guarded by mu
    std::wstring message;        // on-screen message, guarded by mu
    double messageTime = -1e9;   // when it was set (Now()), guarded by mu
    std::atomic<uint64_t> messageGen{0};
    std::atomic<uint64_t> settingsGen{1};
    std::atomic<uint64_t> topologyGen{1};
    std::atomic<bool> running{true};
};

Shared g;

std::wstring g_snapshotPath;     // pending snapshot, guarded by g.mu
// Test options (developer builds): --snapshot <file.png> saves the output
// after a moment of running; --exit-after <seconds> quits automatically.
double g_snapshotDelay = 1.0;  // --snapshot-delay <seconds>
double g_exitAfter = 0;

HINSTANCE g_instance;
HHOOK g_mouseHook;
std::atomic<UINT> g_wheelMods{MOD_CONTROL | MOD_ALT};
int g_wheelAccum = 0;
HWND g_control;   // hidden window: hotkeys, tray, display changes
HWND g_overlay;   // the magnified output
Tracker g_tracker;
TextSource g_textSource;
bool g_overlayReady = false;  // a frame has been rendered into the overlay
UINT g_taskbarCreated;

// ---------------------------------------------------------------- UI thread

Settings SnapshotSettings() {
    std::lock_guard lock(g.mu);
    return g.settings;
}

double Now() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / freq.QuadPart;
}

constexpr UINT_PTR kExitTimer = 1, kPointerTimer = 2;

void SetSystemCursorHidden(bool hidden) {
    if (hidden) cursorhide::Hide();
    else cursorhide::Show();
}

// Shows a short message in large text on the magnified screen.
void ShowMessage(const std::wstring& text) {
    {
        std::lock_guard lock(g.mu);
        g.message = text;
        g.messageTime = Now();
    }
    g.messageGen++;
}

std::wstring ZoomLabel(double z) {
    wchar_t buf[32];
    swprintf_s(buf, L"Zoom %g\u00D7", z);
    return buf;
}

void UpdateTray(DWORD message) {
    Settings s = SnapshotSettings();
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_control;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    swprintf_s(nid.szTip, L"Magnifier %s, %gx, %s, %s", s.enabled ? L"on" : L"off", s.zoom, FilterName(s.filter),
               ColorModeName(s.color));
    Shell_NotifyIconW(message, &nid);
}

// Shows or hides the overlay to match the enabled setting.
void ApplyEnabled() {
    bool on = SnapshotSettings().enabled && g_overlayReady;
    if (on) {
        ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
        RaiseOverlay(g_overlay);
    } else {
        ShowWindow(g_overlay, SW_HIDE);
    }
    SetSystemCursorHidden(on);
}

void ChangeSettings(void (*change)(Settings&)) {
    Settings copy;
    {
        std::lock_guard lock(g.mu);
        change(g.settings);
        copy = g.settings;
    }
    g.settingsGen++;
    SaveSettings(copy);
    UpdateTray(NIM_MODIFY);
    RefreshSettingsWindow(copy);
    Log(L"Settings: %s zoom %g filter %s colour %s", copy.enabled ? L"on" : L"off", copy.zoom, FilterName(copy.filter),
        ColorModeName(copy.color));
}

// The settings window changed something.
void ApplySettings(const Settings& s) {
    bool wasEnabled;
    {
        std::lock_guard lock(g.mu);
        wasEnabled = g.settings.enabled;
        g.settings = s;
    }
    g.settingsGen++;
    SaveSettings(s);
    if (s.enabled != wasEnabled) ApplyEnabled();
    UpdateTray(NIM_MODIFY);
    Log(L"Settings: %s zoom %g filter %s colour %s (settings window)", s.enabled ? L"on" : L"off", s.zoom,
        FilterName(s.filter), ColorModeName(s.color));
}

void OpenSettingsWindow() {
    Log(L"Opening the settings window");
    ShowSettingsWindow(g_instance, SnapshotSettings(), ApplySettings);
}

void RunAction(Action a) {
    switch (a) {
        case Action::ZoomIn:
            ChangeSettings([](Settings& s) { s.zoom = NextZoom(s.zoom, +1); });
            ShowMessage(ZoomLabel(SnapshotSettings().zoom));
            break;
        case Action::ZoomOut:
            ChangeSettings([](Settings& s) { s.zoom = NextZoom(s.zoom, -1); });
            ShowMessage(ZoomLabel(SnapshotSettings().zoom));
            break;
        case Action::Toggle:
            ChangeSettings([](Settings& s) { s.enabled = !s.enabled; });
            ApplyEnabled();
            break;
        case Action::Invert:
            ChangeSettings([](Settings& s) {
                s.color = s.color == ColorMode::Normal ? ColorMode::Invert : ColorMode::Normal;
            });
            ShowMessage(std::wstring(L"Colours: ") + ColorModeName(SnapshotSettings().color));
            break;
        case Action::CycleColour:
            ChangeSettings([](Settings& s) { s.color = (ColorMode)(((int)s.color + 1) % (int)ColorMode::Count); });
            ShowMessage(std::wstring(L"Colours: ") + ColorModeName(SnapshotSettings().color));
            break;
        case Action::CycleFilter:
            ChangeSettings([](Settings& s) { s.filter = (Filter)(((int)s.filter + 1) % (int)Filter::Count); });
            ShowMessage(std::wstring(L"Smoothing: ") + FilterName(SnapshotSettings().filter));
            break;
        case Action::ToggleText:
            ChangeSettings([](Settings& s) { s.textEnabled = !s.textEnabled; });
            ShowMessage(SnapshotSettings().textEnabled ? L"Text re-rendering on" : L"Text re-rendering off");
            break;
        case Action::Snapshot: {
            // Saves exactly what the magnifier shows; ordinary screenshots
            // cannot, because the output is excluded from capture.
            PWSTR pictures = nullptr;
            std::wstring dir;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) dir = pictures;
            CoTaskMemFree(pictures);
            dir += L"\\Magnifier";
            CreateDirectoryW(dir.c_str(), nullptr);
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t name[64];
            swprintf_s(name, L"\\magnifier-%04d%02d%02d-%02d%02d%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour,
                       t.wMinute, t.wSecond);
            std::lock_guard lock(g.mu);
            g_snapshotPath = dir + name;
            break;
        }
        case Action::Quit: PostQuitMessage(0); break;
        case Action::Settings: OpenSettingsWindow(); break;
        default: break;
    }
}

// Hotkey id = action * kMaxKeysPerAction + index + 1.
void RegisterHotkeys(const Settings& s) {
    for (int i = 0; i < (int)Action::Count; ++i) {
        for (int k = 0; k < kMaxKeysPerAction; ++k) UnregisterHotKey(g_control, i * kMaxKeysPerAction + k + 1);
        for (int k = 0; k < (int)s.hotkeys[i].size() && k < kMaxKeysPerAction; ++k) {
            const Hotkey& h = s.hotkeys[i][k];
            if (h.vk && !RegisterHotKey(g_control, i * kMaxKeysPerAction + k + 1, h.modifiers, h.vk))
                Log(L"Hotkey %s (%s) is taken by another program", ActionName((Action)i), FormatHotkey(h).c_str());
        }
    }
    g_wheelMods = s.wheelZoomModifiers;
}

bool ModifiersHeld(UINT mods) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (!mods) return false;
    return (!(mods & MOD_CONTROL) || down(VK_CONTROL)) && (!(mods & MOD_ALT) || down(VK_MENU)) &&
           (!(mods & MOD_SHIFT) || down(VK_SHIFT)) && (!(mods & MOD_WIN) || down(VK_LWIN) || down(VK_RWIN));
}

// Modifier + mouse wheel zooms. Kept tiny: low-level hooks delay all mouse
// input if they are slow.
LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && ModifiersHeld(g_wheelMods)) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        PostMessageW(g_control, WM_APP_WHEELZOOM, (WPARAM)(short)HIWORD(ms->mouseData), 0);
        return 1;  // swallow it so the window underneath does not scroll
    }
    // Scrolling moves text: stop drawing it at its old place straight away.
    // (Other changes are caught by the per-frame pixel check.)
    if (code == HC_ACTION && (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL)) g_textSource.Invalidate(nullptr);
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Magnifies the monitor at a point: by default the one the pointer is on.
void UpdateMonitor(const POINT* at = nullptr) {
    POINT pt = {0, 0};
    if (at) pt = *at;
    else GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    {
        std::lock_guard lock(g.mu);
        if (mon == g.monitor && EqualRect(&mi.rcMonitor, &g.monitorRect)) return;
        g.monitor = mon;
        g.monitorRect = mi.rcMonitor;
    }
    const RECT& r = mi.rcMonitor;
    Log(L"Monitor %ld,%ld %ldx%ld", r.left, r.top, r.right - r.left, r.bottom - r.top);
    if (g_overlay)
        SetWindowPos(g_overlay, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    g.topologyGen++;
}

void ShowTrayMenu() {
    Settings s = SnapshotSettings();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"Settings...");
    SetMenuDefaultItem(menu, kCmdSettings, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.enabled ? MF_CHECKED : 0), kCmdToggle, L"Magnifier on");
    AppendMenuW(menu, MF_STRING | (s.color == ColorMode::Invert ? MF_CHECKED : 0), kCmdInvert, L"Invert colours");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdOpenSettings, L"Edit all settings (file)");
    AppendMenuW(menu, MF_STRING, kCmdReload, L"Reload settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_control);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_control, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case kCmdSettings: OpenSettingsWindow(); break;
        case kCmdToggle: RunAction(Action::Toggle); break;
        case kCmdInvert: RunAction(Action::Invert); break;
        case kCmdOpenSettings: ShellExecuteW(nullptr, L"open", SettingsPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case kCmdReload: {
            Settings fresh = LoadSettings();
            {
                std::lock_guard lock(g.mu);
                g.settings = fresh;
            }
            g.settingsGen++;
            RegisterHotkeys(fresh);
            ApplyEnabled();
            UpdateTray(NIM_MODIFY);
            RefreshSettingsWindow(fresh);
            break;
        }
        case kCmdExit: PostQuitMessage(0); break;
    }
}

LRESULT CALLBACK ControlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_HOTKEY:
            if (wp >= 1 && wp <= (WPARAM)Action::Count * kMaxKeysPerAction)
                RunAction((Action)((wp - 1) / kMaxKeysPerAction));
            return 0;
        case WM_APP_WHEELZOOM:
            // Precision wheels send small deltas; step once per notch's worth.
            g_wheelAccum += (int)(short)wp;
            while (g_wheelAccum >= WHEEL_DELTA) {
                g_wheelAccum -= WHEEL_DELTA;
                RunAction(Action::ZoomIn);
            }
            while (g_wheelAccum <= -WHEEL_DELTA) {
                g_wheelAccum += WHEEL_DELTA;
                RunAction(Action::ZoomOut);
            }
            return 0;
        case WM_APP_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) ShowTrayMenu();
            else if (lp == WM_LBUTTONDBLCLK) OpenSettingsWindow();
            return 0;
        case WM_APP_SETTINGS:
            OpenSettingsWindow();
            return 0;
        case WM_APP_FIRSTFRAME:
            g_overlayReady = true;
            ApplyEnabled();
            return 0;
        case WM_APP_RENDERFAILED:
            // Never leave a stale overlay over the desktop.
            g_overlayReady = false;
            ApplyEnabled();
            return 0;
        case WM_APP_RAISE:
            if (IsWindowVisible(g_overlay) && GetTopWindow(nullptr) != g_overlay) RaiseOverlay(g_overlay);
            return 0;
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
        case WM_TIMER:
            if (wp == kExitTimer) PostQuitMessage(0);
            else if (wp == kPointerTimer) cursorhide::Reassert();
            return 0;
        case WM_DISPLAYCHANGE:
            UpdateMonitor();
            cursorhide::Reassert();
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) UpdateMonitor();
            if (wp == SPI_SETCURSORS) cursorhide::Reassert();
            break;
        case WM_WTSSESSION_CHANGE:
            // Back from a Remote Desktop disconnect, a console switch or the
            // lock screen: the display, the graphics device and the pointer
            // shapes may all have changed. Start rendering afresh.
            if (wp == WTS_REMOTE_CONNECT || wp == WTS_CONSOLE_CONNECT || wp == WTS_SESSION_UNLOCK) {
                Log(L"Session change %u: rebuilding the display", (unsigned)wp);
                PostMessageW(hwnd, WM_APP_REBUILD, 1, 0);
            }
            cursorhide::Reassert();
            return 0;
        case WM_APP_MONITOR: {
            const POINT at = {(LONG)(INT_PTR)wp, (LONG)lp};
            UpdateMonitor(&at);  // starts rendering afresh there if it is another monitor
            g_textSource.Invalidate(nullptr);
            return 0;
        }
        case WM_APP_REBUILD:
            UpdateMonitor();
            if (wp) g.topologyGen++;  // wp: rebuild even if the monitor looks the same
            g_textSource.Invalidate(nullptr);
            cursorhide::Reassert();
            return 0;
        case WM_ENDSESSION:
            if (wp) {
                cursorhide::Show();
                g.running = false;
            }
            return 0;
        default:
            if (msg == g_taskbarCreated) {
                UpdateTray(NIM_ADD);
                return 0;
            }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------ render thread

enum class Owner { Mouse, Caret, Focus };
enum class LoopExit { Stop, Topology, DeviceLost };

bool Inside(const RectD& outer, const RectD& r) {
    return r.r > outer.l && r.l < outer.r && r.b > outer.t && r.t < outer.b;
}

LoopExit RenderLoop(Renderer& renderer, MonitorCapture& capture, const RECT& mon, uint64_t topology) {
    const RectD desktop = {(double)mon.left, (double)mon.top, (double)mon.right, (double)mon.bottom};
    bool monitorAsked = false;  // asked once to move to another monitor
    RectD caretWindow;         // across the window with the caret, at the caret's height
    const double kMouseQuiet = 0.4;  // caret/focus may take over this long after the mouse stops
    const double kZoomTime = 0.06;   // time constant of the zoom glide, seconds
    const double kMessageShow = 1.2, kMessageFade = 0.4;
    float lastMessageOpacity = 0;

    Settings s = SnapshotSettings();
    uint64_t settingsGen = g.settingsGen;

    POINT pt = {};
    GetCursorPos(&pt);
    Viewport view;
    view.desktop = desktop;
    view.output = desktop;
    view.zoom = s.zoom;
    view.origin = CenterOn(view, {pt.x + 0.5, pt.y + 0.5});
    Vec2 goal = view.origin;

    Owner owner = Owner::Mouse;
    POINT lastPt = pt;
    double lastMouseMove = 0;
    uint64_t caretSerial = g_tracker.Caret().serial, focusSerial = g_tracker.Focus().serial;
    RectD targetRect;

    CursorImage cursorImg;
    uint64_t cursorGeneration = cursorhide::Generation();
    double pointerSize = 0;  // how wide Windows draws pointers on this monitor (screen pixels)
    uint64_t sourceVersion = 0;
    FrameReadback readback;
    uint64_t readbackVersion = ~0ull;
    double lastReadback = -1e9, lastCaretChange = -1e9;
    std::map<std::wstring, double> textFirstSeen;  // run key -> when it appeared, for the fade-in
    std::shared_ptr<const TextScene> shownScene;   // the scene those times belong to
    const double kTextFade = 0.15;
    uint64_t textGeneration = 0;
    std::vector<TextDraw> textDraws;
    bool cursorLogged = false;
    bool firstFrame = true;
    bool dirty = true;
    Vec2 lastOrigin = {NAN, NAN};
    Vec2 lastCursorOut = {NAN, NAN};
    double prev = Now();
    const double startTime = prev;

    while (g.running && g.topologyGen == topology) {
        if (FAILED(DwmFlush())) Sleep(8);  // pace to the compositor
        double now = Now();
        double dt = (std::min)(now - prev, 0.1);
        prev = now;

        if (g.settingsGen != settingsGen) {
            settingsGen = g.settingsGen;
            s = SnapshotSettings();
            g_textSource.SetEnabled(s.textEnabled);
            g_textSource.SetFonts(s.textFont, s.textUseAppFont);
            g_textSource.SetOcr(s.textOcr);
            g_textSource.SetIcons(s.iconsEnabled);
            dirty = true;
        }
        if (!s.enabled) {
            view.zoom = s.zoom;  // no animation while hidden
            continue;
        }

        // ---- zoom: glide to the new level, keeping the pointer (or the
        // caret/focus being followed) fixed on screen.
        if (view.zoom != s.zoom) {
            GetCursorPos(&pt);
            Vec2 anchor = owner == Owner::Mouse ? Vec2{pt.x + 0.5, pt.y + 0.5}
                          : Inside(view.desktop, targetRect) ? targetRect.Center()
                                                             : view.ToSource(view.output.Center());
            double a = 1.0 - std::exp(-dt / kZoomTime);
            double z = std::exp(std::log(view.zoom) + (std::log(s.zoom) - std::log(view.zoom)) * a);
            if (std::fabs(z - s.zoom) < 0.002 * s.zoom) z = s.zoom;
            view.origin = ZoomAbout(view, z, anchor);
            view.zoom = z;
            goal = view.origin;
        }

        // ---- on-screen message: shown for a moment, then faded out.
        std::wstring message;
        float messageOpacity = 0;
        {
            std::lock_guard lock(g.mu);
            double age = now - g.messageTime;
            if (age < kMessageShow + kMessageFade) {
                message = g.message;
                messageOpacity = (float)(std::min)(1.0, (kMessageShow + kMessageFade - age) / kMessageFade);
            }
        }
        if (!message.empty() || messageOpacity != lastMessageOpacity) dirty = true;
        lastMessageOpacity = messageOpacity;

        bool newFrame = capture.Poll(renderer.Context());
        if (newFrame) ++sourceVersion;

        // CPU copies of the screen, a few times a second, for the text checks.
        const bool textOn = s.textEnabled && view.zoom >= s.textMinZoom;
        // Whenever the screen changes while text is shown, so a change is
        // caught at once (plus a slow heartbeat). The copy is collected a
        // frame later and never stalls the GPU.
        if (textOn) {
            bool start = sourceVersion != readbackVersion || now - lastReadback > 0.5;
            if (start) readbackVersion = sourceVersion, lastReadback = now;
            if (auto f = readback.Poll(renderer.Device(), renderer.Context(), capture.Texture(), mon.left, mon.top,
                                       start)) {
                g_textSource.SubmitFrame(std::move(f));
            }
        }

        // ---- tracking arbitration
        GetCursorPos(&pt);
        bool mouseMoved = pt.x != lastPt.x || pt.y != lastPt.y;
        lastPt = pt;
        if (mouseMoved) {
            lastMouseMove = now;
            if (s.trackMouse) owner = Owner::Mouse;
        }
        Vec2 p = {pt.x + 0.5, pt.y + 0.5};
        bool mouseQuiet = now - lastMouseMove > kMouseQuiet;
        // The pointer went onto another monitor: magnify that one.
        if (mouseMoved && s.trackMouse && !monitorAsked && !Inside(desktop, {p.x, p.y, p.x, p.y})) {
            PostMessageW(g_control, WM_APP_MONITOR, (WPARAM)(INT_PTR)pt.x, (LPARAM)pt.y);
            monitorAsked = true;
        }

        TrackedRect caret = g_tracker.Caret(), focus = g_tracker.Focus();
        bool caretNew = caret.serial != caretSerial, focusNew = focus.serial != focusSerial;
        caretSerial = caret.serial;
        focusSerial = focus.serial;
        // Typing on another monitor (mouse still): magnify that one.
        if (caretNew && mouseQuiet && s.trackCaret && !monitorAsked && caret.rect.Width() >= 0 &&
            caret.rect.Height() > 0 && !Inside(desktop, caret.rect)) {
            const Vec2 c = caret.rect.Center();
            PostMessageW(g_control, WM_APP_MONITOR, (WPARAM)(INT_PTR)(LONG)c.x, (LPARAM)(LONG)c.y);
            monitorAsked = true;
        }
        if (caretNew) {
            // The window being typed in (its line is left to the filter).
            caretWindow = {caret.rect.l - 3, caret.rect.t, caret.rect.r + 3, caret.rect.b};
            const POINT cp = {(LONG)caret.rect.Center().x, (LONG)caret.rect.Center().y};
            RECT wr;
            if (HWND w = GetAncestor(WindowFromPoint(cp), GA_ROOT); w && GetWindowRect(w, &wr))
                caretWindow.l = (std::min)(caretWindow.l, (double)wr.left), caretWindow.r = (std::max)(caretWindow.r, (double)wr.right);
            // Typing or moving the caret changes that line: drop its text now.
            lastCaretChange = now;
            RectD band = {view.desktop.l, caret.rect.t - 2, view.desktop.r, caret.rect.b + 2};
            g_textSource.Invalidate(&band);
        }
        Vec2 src = view.SourceSize();
        // Ignore focus on whole windows or documents; that would just recentre.
        bool focusUseful = focus.rect.Width() < src.x * 0.9 && focus.rect.Height() < src.y * 0.9;
        if (mouseQuiet && caretNew && s.trackCaret && Inside(desktop, caret.rect) &&
            !(focusNew && focus.serial > caret.serial)) {
            owner = Owner::Caret;
            targetRect = caret.rect;
            Viewport at = view;
            at.origin = goal;
            goal = EdgeTrack(at, targetRect, s.caretMargin);
        } else if (mouseQuiet && focusNew && s.trackFocus && focusUseful && Inside(desktop, focus.rect)) {
            owner = Owner::Focus;
            targetRect = focus.rect;
            Viewport at = view;
            at.origin = goal;
            goal = EdgeTrack(at, targetRect, 0.05);
        }

        double tau = s.panTime;
        if (owner == Owner::Mouse && s.trackMouse) {
            goal = EdgeTrack(view, {p.x, p.y, p.x, p.y}, s.mouseMargin);
            // Follow the pointer immediately unless the view has to travel a
            // long way (back from the caret, say), which is eased.
            bool longWay = std::fabs(goal.x - view.origin.x) > src.x * 0.25 ||
                       std::fabs(goal.y - view.origin.y) > src.y * 0.25;
            tau = longWay ? s.panTime * 0.75 : 0;
        }
        view.origin = SmoothApproach(view.origin, goal, dt, tau, 0);

        // ---- pointer image: Windows' own pointer (its size and colour
        // settings included), magnified with everything else.
        CURSORINFO ci = {sizeof(ci)};
        GetCursorInfo(&ci);
        if (!cursorLogged) {
            Log(L"GetCursorInfo flags 0x%X handle %p", ci.flags, ci.hCursor);
            cursorLogged = true;
        }
        double cursorScale = (std::min)(view.zoom, s.cursorMaxSize);
        // Apps hide the pointer while you type; follow that.
        bool cursorVisible = ci.hCursor && (ci.flags & CURSOR_SHOWING) && !(ci.flags & CURSOR_SUPPRESSED);
        // Windows reloaded its pointers (a new size or colour, say): take them afresh.
        const bool pointersChanged = cursorhide::Generation() != cursorGeneration;
        if (cursorVisible && (pointersChanged || ci.hCursor != cursorImg.handle ||
                              std::fabs(cursorScale - cursorImg.requestedScale) > cursorImg.requestedScale * 0.1)) {
            if (pointersChanged || pointerSize <= 0) pointerSize = PointerSizeOnScreen(MonitorFromRect(&mon, MONITOR_DEFAULTTONEAREST));
            cursorGeneration = cursorhide::Generation();
            if (ExtractCursor(ci.hCursor, pointerSize, cursorScale, cursorImg)) renderer.SetCursor(cursorImg);
            else cursorImg = CursorImage{};
            dirty = true;
        }
        // ---- re-rendered text
        {
            Vec2 viewSize = view.SourceSize();
            RectD viewRect = {view.origin.x, view.origin.y, view.origin.x + viewSize.x, view.origin.y + viewSize.y};
            Vec2 focusPoint = owner == Owner::Mouse ? p : targetRect.Center();
            g_textSource.SetView(viewRect, focusPoint);
            auto scene = g_textSource.Scene();
            if (scene->generation != textGeneration) {
                textGeneration = scene->generation;
                dirty = true;
                std::map<std::wstring, double> seen;
                for (const TextRun& r : scene->runs) {
                    auto it = textFirstSeen.find(r.key);
                    double at = it != textFirstSeen.end() ? it->second : now;
                    // A run taking over from text already redrawn there (app
                    // text replacing an OCR word, a re-check after a hover)
                    // carries on from it instead of fading in again, which
                    // would briefly show the blurry pixels.
                    if (it == textFirstSeen.end() && shownScene)
                        for (const TextRun& o : shownScene->runs) {
                            auto oi = textFirstSeen.find(o.key);
                            if (oi != textFirstSeen.end() && o.rect.r > r.rect.l && o.rect.l < r.rect.r &&
                                o.rect.b > r.rect.t && o.rect.t < r.rect.b)
                                at = (std::min)(at, oi->second);
                        }
                    seen[r.key] = at;
                }
                textFirstSeen.swap(seen);
                shownScene = scene;
            }
            textDraws.clear();
            // Leave the line with a recently active caret to the scaling
            // filter, so the app's own blinking caret stays visible.
            bool caretActive = now - lastCaretChange < 30;
            // The whole line, across the window being typed in, so it is not
            // left half redrawn (OCR words beside the caret).
            RectD caretZone = {caretWindow.l, caret.rect.t, caretWindow.r, caret.rect.b};
            for (const TextRun& r : textOn ? scene->runs : std::vector<TextRun>{}) {
                if (!(r.rect.r > viewRect.l && r.rect.l < viewRect.r && r.rect.b > viewRect.t && r.rect.t < viewRect.b))
                    continue;
                // On the caret's line: most of the run's height level with the caret.
                const bool onCaretLine =
                    caretActive && r.rect.r > caretZone.l && r.rect.l < caretZone.r &&
                    (std::min)(r.rect.b, caretZone.b) - (std::max)(r.rect.t, caretZone.t) >
                        0.5 * (std::min)(r.rect.Height(), caretZone.Height());
                if (onCaretLine) continue;
                TextDraw t = MakeTextDraw(r, view, mon.left, mon.top);
                auto seenAt = textFirstSeen.find(r.key);
                double age = seenAt != textFirstSeen.end() ? now - seenAt->second : kTextFade;
                t.opacity = (float)std::clamp(age / kTextFade, 0.0, 1.0);
                if (t.opacity < 1) dirty = true;  // keep drawing until the fade completes
                textDraws.push_back(std::move(t));
            }
        }

        Vec2 cursorOut = view.ToOutput(p);
        cursorOut.x -= mon.left;
        cursorOut.y -= mon.top;

        // ---- draw only when something changed
        bool snapshotDue;
        {
            std::lock_guard lock(g.mu);
            snapshotDue = !g_snapshotPath.empty() && now - startTime > g_snapshotDelay;
        }
        if (snapshotDue) dirty = true;
        dirty |= newFrame || view.origin.x != lastOrigin.x || view.origin.y != lastOrigin.y ||
                 cursorOut.x != lastCursorOut.x || cursorOut.y != lastCursorOut.y;
        if (!dirty || !capture.HasFrame()) continue;

        RenderParams rp;
        rp.srcOriginX = view.origin.x - mon.left;
        rp.srcOriginY = view.origin.y - mon.top;
        rp.zoom = view.zoom;
        rp.filter = s.filter;
        rp.edgeStrength = s.edgeStrength;
        rp.edgeWidth = s.edgeWidth;
        rp.textBoldness = s.textBoldness;
        rp.edgeRoundness = s.edgeRoundness;
        rp.graphicsSharpness = s.graphicsSharpness;
        rp.color = s.color;
        rp.foreground = s.foreground;
        rp.background = s.background;
        rp.brightness = s.brightness;
        rp.contrast = s.contrast;
        rp.drawCursor = cursorVisible && cursorImg.width > 0;
        rp.cursorX = cursorOut.x;
        rp.cursorY = cursorOut.y;
        rp.cursorScale = cursorScale;
        rp.sourceVersion = sourceVersion;
        rp.text = &textDraws;
        rp.fallbackFont = s.textFont;
        rp.useAppFont = s.textUseAppFont;
        rp.message = message.empty() ? nullptr : message.c_str();
        rp.messageOpacity = messageOpacity;

        // Save a snapshot once things have settled (about a second in).
        std::wstring snapshot;
        if (snapshotDue) {
            std::lock_guard lock(g.mu);
            snapshot.swap(g_snapshotPath);
        }

        WaitForSingleObjectEx(renderer.FrameWaitable(), 100, TRUE);
        HRESULT hr = renderer.Render(capture.View(), capture.Width(), capture.Height(), rp,
                                     snapshot.empty() ? nullptr : snapshot.c_str());
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            Log(L"Device lost: 0x%08X", (unsigned)hr);
            return LoopExit::DeviceLost;
        }
        if (!snapshot.empty()) ShowMessage(L"Snapshot saved");
        dirty = false;
        lastOrigin = view.origin;
        lastCursorOut = cursorOut;
        if (firstFrame) {
            PostMessageW(g_control, WM_APP_FIRSTFRAME, 0, 0);
            firstFrame = false;
        }
    }
    return g.running ? LoopExit::Topology : LoopExit::Stop;
}

void RenderThread() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    int failures = 0;
    while (g.running) {
        HMONITOR mon;
        RECT rect;
        uint64_t topology = g.topologyGen;
        {
            std::lock_guard lock(g.mu);
            mon = g.monitor;
            rect = g.monitorRect;
        }
        Renderer renderer;
        MonitorCapture capture;
        bool rendererOk = renderer.Init(g_overlay, rect.right - rect.left, rect.bottom - rect.top);
        bool captureOk = rendererOk && capture.Start(renderer.Device(), mon);
        // No display to capture (a disconnected remote session, the secure
        // desktop): keep the renderer and retry the capture every 2 seconds.
        if (rendererOk && !captureOk) {
            Log(L"Screen capture unavailable; waiting for the display");
            PostMessageW(g_control, WM_APP_RENDERFAILED, 0, 0);
            while (g.running && g.topologyGen == topology && !captureOk) {
                for (int i = 0; i < 20 && g.running; ++i) Sleep(100);
                // The monitor may have been replaced without a display-change
                // message (a Remote Desktop reconnect): have it re-read.
                POINT cursor = {0, 0};
                GetCursorPos(&cursor);
                HMONITOR now = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {sizeof(mi)};
                if (now != mon || (GetMonitorInfoW(now, &mi) && !EqualRect(&mi.rcMonitor, &rect))) {
                    PostMessageW(g_control, WM_APP_REBUILD, 0, 0);
                    break;
                }
                captureOk = g.running && capture.Start(renderer.Device(), mon);
            }
            if (captureOk) Log(L"Display is back");
        }
        LoopExit exit = g.running ? LoopExit::Topology : LoopExit::Stop;
        if (captureOk) {
            failures = 0;
            exit = RenderLoop(renderer, capture, rect, topology);
        } else if (!rendererOk) {
            exit = LoopExit::DeviceLost;
        }
        capture.Stop();
        renderer.Shutdown();
        if (exit == LoopExit::Stop) break;
        if (exit == LoopExit::DeviceLost) {
            // Never give up (a disconnected session can refuse a device for
            // as long as it lasts); just retry less often, up to every 5 s.
            PostMessageW(g_control, WM_APP_RENDERFAILED, 0, 0);
            ++failures;
            if (failures == 1 || failures % 20 == 0) Log(L"Rendering unavailable (%d tries); retrying", failures);
            const int wait = (std::min)(500 * failures, 5000);
            for (int i = 0; i < wait / 100 && g.running && g.topologyGen == topology; ++i) Sleep(100);
        }
    }
    winrt::uninit_apartment();
}

#if MAGNIFIER_DEV_TOOLS
// --render <in> <out> <zoom> [filter]: runs the real shaders on an image file
// instead of the screen, for tuning filters against fixed test images.
int OfflineRender(const wchar_t* in, const wchar_t* out, double zoom, const wchar_t* filter) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    UINT w = 0, h = 0;
    std::vector<uint32_t> px;
    if (!LoadImageBGRA(in, w, h, px)) return 2;

    Settings s = g.settings;
    if (filter)
        for (int i = 0; i < (int)Filter::Count; ++i)
            if (!_wcsicmp(filter, FilterName((Filter)i))) s.filter = (Filter)i;

    UINT outW = (UINT)std::ceil(w * zoom), outH = (UINT)std::ceil(h * zoom);
    Renderer r;
    if (!r.Init(nullptr, outW, outH)) return 3;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {px.data(), w * 4, 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(r.Device()->CreateTexture2D(&desc, &data, &tex)) ||
        FAILED(r.Device()->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return 4;

    RenderParams rp;
    rp.zoom = zoom;
    rp.filter = s.filter;
    rp.edgeStrength = s.edgeStrength;
    rp.edgeWidth = s.edgeWidth;
    rp.textBoldness = s.textBoldness;
    rp.edgeRoundness = s.edgeRoundness;
    rp.graphicsSharpness = s.graphicsSharpness;
    rp.color = s.color;
    rp.foreground = s.foreground;
    rp.background = s.background;
    rp.brightness = s.brightness;
    rp.contrast = s.contrast;
    rp.sourceVersion = 1;
    r.Render(srv.Get(), w, h, rp, out);

    // MAGNIFIER_BENCH=N: time N more frames (new source each time, so the
    // Smooth pre-pass is included) and log the average GPU+CPU frame time.
    wchar_t bench[16] = {};
    if (GetEnvironmentVariableW(L"MAGNIFIER_BENCH", bench, 16)) {
        int frames = (std::max)(_wtoi(bench), 1);
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        Microsoft::WRL::ComPtr<ID3D11Query> done;
        r.Device()->CreateQuery(&qd, &done);
        double start = Now();
        for (int i = 0; i < frames; ++i) {
            rp.sourceVersion = 2 + i;
            r.Render(srv.Get(), w, h, rp);
        }
        r.Context()->End(done.Get());
        while (r.Context()->GetData(done.Get(), nullptr, 0, 0) == S_FALSE) Sleep(0);
        Log(L"Bench: %s %ux%u -> %ux%u, %.2f ms per frame", FilterName(s.filter), w, h, outW, outH,
            (Now() - start) * 1000.0 / frames);
    }
    r.Shutdown();
    return 0;
}

// One text scan around the pointer (or x y) at 3x, timings to scan-log.txt.
int ScanProbe(int argc, wchar_t** argv) {
    LogFileName() = L"scan-log.txt";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    POINT pt;
    GetCursorPos(&pt);
    if (argc >= 4) pt = {_wtoi(argv[2]), _wtoi(argv[3])};  // --scan-probe x y
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    double w = (mi.rcMonitor.right - mi.rcMonitor.left) / 3.0, h = (mi.rcMonitor.bottom - mi.rcMonitor.top) / 3.0;
    RectD view = {pt.x - w / 2, pt.y - h / 2, pt.x + w / 2, pt.y + h / 2};
    Log(L"Probe around %ld,%ld", pt.x, pt.y);
    TextSource::ProbeScan(view, {(double)pt.x, (double)pt.y});
    return 0;
}

// The test and tuning modes. Returns true (and the exit code) if argv asked
// for one.
bool RunDevTool(int argc, wchar_t** argv, int& rc) {
    if (argc >= 2 && !wcscmp(argv[1], L"--scan-probe")) {
        rc = ScanProbe(argc, argv);
        return true;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"--capture-fixture")) {
        LogFileName() = L"test-log.txt";
        POINT pt;
        GetCursorPos(&pt);
        if (argc >= 5) pt = {_wtoi(argv[3]), _wtoi(argv[4])};
        rc = CaptureTextFixture(argv[2], pt);
        return true;
    }
    if (argc >= 4 && !wcscmp(argv[1], L"--text-test")) {
        LogFileName() = L"test-log.txt";
        rc = RunTextTest(argv[2], argv[3]);
        return true;
    }
    if (argc >= 5 && !wcscmp(argv[1], L"--render")) {
        LogFileName() = L"render-log.txt";
        g.settings = LoadSettings();
        rc = OfflineRender(argv[2], argv[3], _wtof(argv[4]), argc >= 6 ? argv[5] : nullptr);
        return true;
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], L"--snapshot")) g_snapshotPath = argv[++i];
        else if (!wcscmp(argv[i], L"--exit-after")) g_exitAfter = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--snapshot-delay")) g_snapshotDelay = _wtof(argv[++i]);
    }
    return false;
}
#endif  // MAGNIFIER_DEV_TOOLS

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    // Whatever happens, give the user their pointer back.
    cursorhide::Show();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    // Command line, parsed by the C runtime.
    const int argc = __argc;
    wchar_t** argv = __wargv;
    if (argc >= 3 && !wcscmp(argv[1], L"--watchdog")) return cursorhide::RunWatchdog((DWORD)_wtoi(argv[2]));
#if MAGNIFIER_DEV_TOOLS
    if (int rc = 0; RunDevTool(argc, argv, rc)) return rc;
#endif
    bool openSettings = false;
    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--settings")) openSettings = true;

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MagnifierSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: --settings asks that copy to open its window.
        if (openSettings)
            if (HWND other = FindWindowW(L"MagnifierControl", nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, WM_APP_SETTINGS, 0, 0);
            }
        return 0;
    }
    SetUnhandledExceptionFilter(CrashFilter);

    g.settings = LoadSettings();
    SaveSettings(g.settings);  // write the file on first run so it can be edited
    Log(L"Magnifier starting, settings at %s", SettingsPath().c_str());

    cursorhide::Init();  // also undoes any blank pointers left by a crash
    if (HasUIAccess()) cursorhide::UseMagnificationApi();
    cursorhide::StartWatchdog();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ControlProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"MagnifierControl";
    RegisterClassExW(&wc);
    g_control = CreateWindowExW(0, wc.lpszClassName, L"Magnifier", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                instance, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_control, WM_APP_SETTINGS, MSGFLT_ALLOW, nullptr);  // from a non-UIAccess start

    UpdateMonitor();
    bool excluded = false;
    g_overlay = CreateOverlayWindow(instance, g.monitorRect, excluded);
    if (!g_overlay || !excluded) {
        MessageBoxW(nullptr,
                    g_overlay ? L"Windows refused to exclude the magnifier window from screen capture, so it would "
                                L"magnify itself. Windows 10 version 2004 or later is required."
                              : L"Could not create the magnifier window.",
                    L"Magnifier", MB_ICONERROR);
        return 1;
    }

    RegisterHotkeys(g.settings);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, instance, 0);
    if (!g_mouseHook) Log(L"Mouse wheel zoom unavailable: %u", GetLastError());
    UpdateTray(NIM_ADD);
    g_tracker.onZOrderChange = [] {
        PostMessageW(g_control, WM_APP_RAISE, 0, 0);
        g_textSource.RequestScan();  // a menu or other window appeared
    };
    g_tracker.onForeground = [] { g_textSource.Invalidate(nullptr); };
    // A window appearing (menu, tooltip, the Start menu) covers what was under
    // it: stop drawing re-rendered text there at once.
    g_tracker.onWindowShown = [](HWND h) {
        RECT rc;
        if (h && GetAncestor(h, GA_ROOT) == h && GetWindowRect(h, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
            RectD area = {(double)rc.left, (double)rc.top, (double)rc.right, (double)rc.bottom};
            g_textSource.Invalidate(&area);
        }
    };
    g_tracker.Start();
    g_textSource.SetEnabled(g.settings.textEnabled);
    g_textSource.SetFonts(g.settings.textFont, g.settings.textUseAppFont);
    g_textSource.SetOcr(g.settings.textOcr);
    g_textSource.SetIcons(g.settings.iconsEnabled);
    g_textSource.Start();

    std::thread render(RenderThread);
    if (g_exitAfter > 0) SetTimer(g_control, kExitTimer, (UINT)(g_exitAfter * 1000), nullptr);
    SetTimer(g_control, kPointerTimer, 1000, nullptr);
    WTSRegisterSessionNotification(g_control, NOTIFY_FOR_THIS_SESSION);
    if (openSettings) OpenSettingsWindow();

    MSG msg;
    while (g.running && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (SettingsWindowMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g.running = false;
    render.join();
    g_textSource.Stop();
    g_tracker.Stop();
    ShowWindow(g_overlay, SW_HIDE);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    cursorhide::Show();
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_control;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(g_overlay);
    DestroyWindow(g_control);
    Log(L"Magnifier stopped");
    ReleaseMutex(single);
    CloseHandle(single);
    return 0;
}
