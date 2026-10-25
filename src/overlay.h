// The full-screen output window: topmost, click-through, never activated and
// excluded from screen capture so the magnifier never magnifies itself.
#pragma once

#include <windows.h>

// Creates the (hidden) overlay covering rect. Returns nullptr on failure.
// excludedFromCapture reports whether WDA_EXCLUDEFROMCAPTURE was accepted;
// without it the magnifier would capture its own output.
HWND CreateOverlayWindow(HINSTANCE instance, const RECT& rect, bool& excludedFromCapture);

// Puts the overlay back on top of other topmost windows (the taskbar, for one).
void RaiseOverlay(HWND overlay);

// True when this process's token has UIAccess (signed uiAccess build installed
// in a secure location such as Program Files).
bool HasUIAccess();
