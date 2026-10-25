// The settings window: the everyday options as ordinary, keyboard-accessible
// controls in a large font. Every change applies at once (there is no OK
// button); the full list of options stays in settings.ini.
#pragma once

#include <windows.h>

#include <functional>

#include "settings.h"

// Opens the window, or brings it forward if it is open. apply is called on
// the UI thread with the changed settings after each change.
void ShowSettingsWindow(HINSTANCE instance, const Settings& current, std::function<void(const Settings&)> apply);
// Updates the window's controls after settings changed elsewhere (hotkeys).
void RefreshSettingsWindow(const Settings& current);
// For the message loop: keyboard navigation (Tab, arrows, Alt+letter).
bool SettingsWindowMessage(MSG* msg);

// "Start Magnifier when I sign in": a shortcut to this program in the
// user's Startup folder (it goes through the shell, so a UIAccess build
// starts too).
bool StartsAtSignIn();
bool SetStartAtSignIn(bool on);
