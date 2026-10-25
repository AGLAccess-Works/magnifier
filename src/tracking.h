// Text caret and keyboard focus tracking.
//
// WinEvent hooks (out of context, on the UI thread's message loop) only record
// what changed. A worker thread then asks for the geometry, so a slow or hung
// application can never stall input handling or rendering.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "viewport.h"

struct TrackedRect {
    RectD rect;           // physical desktop pixels
    uint64_t serial = 0;  // increases with every update; 0 = never seen
};

class Tracker {
public:
    // Must be called on a thread that runs a message loop.
    bool Start();
    void Stop();

    TrackedRect Caret();
    TrackedRect Focus();

    // Called on the UI thread when the foreground window changes or a window
    // appears, either of which may put another topmost window above ours.
    std::function<void()> onZOrderChange;
    // Called on the UI thread when the foreground window changes.
    std::function<void()> onForeground;
    // Called on the UI thread when a top-level window is shown.
    std::function<void(HWND)> onWindowShown;

private:
    struct Request {
        HWND hwnd = nullptr;
        LONG idObject = 0, idChild = 0;
        bool pending = false;
    };

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                      DWORD);
    void Worker();
    void Publish(TrackedRect& slot, const RectD& r);

    HWINEVENTHOOK m_hooks[4] = {};
    std::thread m_worker;
    std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_stop = false;
    Request m_caretReq, m_focusReq;
    TrackedRect m_caret, m_focus;
    uint64_t m_serial = 0;
};
