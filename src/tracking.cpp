#include "tracking.h"

#include <oleacc.h>

#include "log.h"

namespace {

Tracker* g_tracker = nullptr;

// Native caret from GetGUIThreadInfo. rcCaret is in the caret window's client
// coordinates in that window's own DPI context, so convert in that context and
// then map to physical pixels.
bool CaretFromThread(HWND eventHwnd, RectD& out) {
    DWORD thread = GetWindowThreadProcessId(eventHwnd, nullptr);
    GUITHREADINFO gti = {sizeof(gti)};
    if (!thread || !GetGUIThreadInfo(thread, &gti) || !gti.hwndCaret) return false;
    RECT rc = gti.rcCaret;
    if (rc.right <= rc.left && rc.bottom <= rc.top) return false;

    POINT tl = {rc.left, rc.top}, br = {rc.right, rc.bottom};
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(gti.hwndCaret));
    ClientToScreen(gti.hwndCaret, &tl);
    ClientToScreen(gti.hwndCaret, &br);
    SetThreadDpiAwarenessContext(old);
    LogicalToPhysicalPointForPerMonitorDPI(gti.hwndCaret, &tl);
    LogicalToPhysicalPointForPerMonitorDPI(gti.hwndCaret, &br);
    out = {(double)tl.x, (double)tl.y, (double)(std::max)(br.x, tl.x + 1), (double)(std::max)(br.y, tl.y + 1)};
    return true;
}

// Location of the object an event refers to, through MSAA. Covers carets that
// are not Win32 carets (Chromium, many custom editors) and focused controls.
// Called from a PMv2 thread, so the result is taken as physical pixels.
bool LocationFromMsaa(const HWND hwnd, LONG idObject, LONG idChild, RectD& out) {
    IAccessible* acc = nullptr;
    VARIANT child;
    VariantInit(&child);
    if (FAILED(AccessibleObjectFromEvent(hwnd, idObject, idChild, &acc, &child)) || !acc) return false;
    long l = 0, t = 0, w = 0, h = 0;
    HRESULT hr = acc->accLocation(&l, &t, &w, &h, child);
    acc->Release();
    VariantClear(&child);
    if (FAILED(hr) || (w <= 0 && h <= 0)) return false;
    out = {(double)l, (double)t, (double)(l + (std::max)(w, 1L)), (double)(t + (std::max)(h, 1L))};
    return true;
}

}  // namespace

bool Tracker::Start() {
    g_tracker = this;
    m_stop = false;
    m_worker = std::thread([this] { Worker(); });
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    m_hooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, flags);
    m_hooks[1] = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, WinEventProc, 0, 0, flags);
    m_hooks[2] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc, 0,
                                 0, flags);
    m_hooks[3] = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, WinEventProc, 0, 0, flags);
    bool ok = m_hooks[0] && m_hooks[1] && m_hooks[2] && m_hooks[3];
    if (!ok) Log(L"SetWinEventHook failed");
    return ok;
}

void Tracker::Stop() {
    for (auto& h : m_hooks) {
        if (h) UnhookWinEvent(h);
        h = nullptr;
    }
    {
        std::lock_guard lock(m_mu);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    g_tracker = nullptr;
}

TrackedRect Tracker::Caret() {
    std::lock_guard lock(m_mu);
    return m_caret;
}

TrackedRect Tracker::Focus() {
    std::lock_guard lock(m_mu);
    return m_focus;
}

void CALLBACK Tracker::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                    DWORD) {
    Tracker* t = g_tracker;
    if (!t) return;
    if (event == EVENT_SYSTEM_FOREGROUND && t->onForeground) t->onForeground();
    if (event == EVENT_OBJECT_SHOW && idObject == OBJID_WINDOW && idChild == CHILDID_SELF && t->onWindowShown)
        t->onWindowShown(hwnd);
    if (event == EVENT_SYSTEM_FOREGROUND || (event == EVENT_OBJECT_SHOW && idObject == OBJID_WINDOW)) {
        if (t->onZOrderChange) t->onZOrderChange();
        return;
    }
    Request* req = nullptr;
    if (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_CARET) req = &t->m_caretReq;
    else if (event == EVENT_OBJECT_FOCUS) req = &t->m_focusReq;
    if (!req) return;
    {
        std::lock_guard lock(t->m_mu);
        *req = {hwnd, idObject, idChild, true};
    }
    t->m_cv.notify_one();
}

void Tracker::Publish(TrackedRect& slot, const RectD& r) {
    std::lock_guard lock(m_mu);
    slot.rect = r;
    slot.serial = ++m_serial;
}

void Tracker::Worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    for (;;) {
        Request caret, focus;
        {
            std::unique_lock lock(m_mu);
            m_cv.wait(lock, [&] { return m_stop || m_caretReq.pending || m_focusReq.pending; });
            if (m_stop) break;
            caret = m_caretReq;
            focus = m_focusReq;
            m_caretReq.pending = m_focusReq.pending = false;
        }
        RectD r;
        // Focus first: a focus change is usually followed by a caret update
        // that should win.
        if (focus.pending && LocationFromMsaa(focus.hwnd, focus.idObject, focus.idChild, r)) Publish(m_focus, r);
        if (caret.pending &&
            (CaretFromThread(caret.hwnd, r) || LocationFromMsaa(caret.hwnd, caret.idObject, caret.idChild, r)))
            Publish(m_caret, r);
    }
    CoUninitialize();
}
