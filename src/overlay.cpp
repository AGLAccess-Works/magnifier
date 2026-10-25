#include "overlay.h"

#include "log.h"

namespace {

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool HasUIAccess() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD uiAccess = 0, size = 0;
    BOOL ok = GetTokenInformation(token, TokenUIAccess, &uiAccess, sizeof(uiAccess), &size);
    CloseHandle(token);
    return ok && uiAccess;
}

HWND CreateOverlayWindow(HINSTANCE instance, const RECT& rect, bool& excludedFromCapture) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"MagnifierOverlay";
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    // LAYERED + TRANSPARENT makes the window ignore mouse input entirely, so
    // clicks reach the real windows underneath the pointer. TOPMOST is all a
    // UIAccess process needs too: Windows puts its topmost windows in the
    // UIAccess z-order band, above the Start menu and shell flyouts.
    Log(HasUIAccess() ? L"Running with UIAccess: the overlay covers the Start menu and shell flyouts"
                      : L"Running without UIAccess: the Start menu and shell flyouts will show above the overlay");
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                          WS_EX_NOREDIRECTIONBITMAP;
    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"Magnifier", WS_POPUP, rect.left, rect.top,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        Log(L"CreateWindowEx overlay failed: %u", GetLastError());
        return nullptr;
    }
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    excludedFromCapture = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    if (!excludedFromCapture) Log(L"SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed: %u", GetLastError());
    return hwnd;
}

void RaiseOverlay(HWND overlay) {
    SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
