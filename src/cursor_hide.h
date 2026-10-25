// Hiding the real pointer.
//
// Windows offers no supported way for an ordinary app to hide the pointer
// everywhere (MagShowSystemCursor is ignored without UIAccess), so the
// standard system pointers are swapped for blank ones while the magnifier is
// visible, and the user's scheme is restored afterwards. Copies of the
// originals are kept so the magnifier can still draw the right pointer.
//
// With UIAccess the Magnification API also hides the pointer outright, which
// covers apps' own pointers (web pages, Office, design tools) as well.
//
// Windows reloads the user's scheme on its own now and then (Remote Desktop
// reconnect, display scaling or pointer settings changes), so Reassert checks
// for that and hides the pointers again.
//
// Safety: a watchdog process restores the pointers if this process dies in
// any way, and startup always restores them first in case of an earlier crash.
#pragma once

#include <windows.h>

#include <cstdint>

namespace cursorhide {

// Restores any leftover blank pointers and snapshots the originals. Call once
// on the UI thread before anything reads Original().
void Init();

// Call once after Init when the process has UIAccess.
void UseMagnificationApi();

void Hide();
void Show();  // reloads the user's pointer scheme
// While hidden: if Windows has put the pointers back, hide them again (taking
// fresh copies of the originals, which may have changed). Cheap; call about
// once a second and on pointer, display and session changes.
void Reassert();

// A copy of what cursor looks like: for a blanked system pointer the saved
// original, otherwise cursor itself. The caller destroys it with DestroyCursor.
// Safe to call from any thread.
HCURSOR CopyOriginal(HCURSOR cursor);

// Changes whenever fresh copies of the originals are taken (Windows reloaded
// the pointers, for example after a change to the pointer size or colour).
uint64_t Generation();

// A fresh, sharp copy of a system pointer drawn at size x size pixels, loaded
// from the user's pointer scheme. nullptr if cursor is not a system pointer or
// no image of that size can be made. The caller destroys it with DestroyCursor.
HCURSOR LoadLarge(HCURSOR cursor, int size);

// Starts "<this exe> --watchdog <pid>".
void StartWatchdog();
// The watchdog itself: waits for pid to exit, then restores the pointers.
int RunWatchdog(DWORD pid);

}  // namespace cursorhide
