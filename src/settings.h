// User settings, stored in %APPDATA%\Magnifier\settings.ini.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum class Filter : int { Nearest, Bilinear, Bicubic, Lanczos, Smooth, Count };
enum class ColorMode : int { Normal, Invert, InvertLightness, Greyscale, GreyscaleInvert, TwoColour, Count };

const wchar_t* FilterName(Filter f);
const wchar_t* ColorModeName(ColorMode c);

enum class Action : int {
    ZoomIn, ZoomOut, Toggle, Invert, CycleColour, CycleFilter, ToggleText, Snapshot, Quit, Settings, Count
};

struct Hotkey {
    UINT modifiers = 0;  // MOD_CONTROL | MOD_ALT | ...
    UINT vk = 0;
};

struct Settings {
    bool enabled = true;
    double zoom = 2.0;

    Filter filter = Filter::Smooth;
    double edgeStrength = 1.0;  // Smooth filter: how strongly edges are crisped, 0..1
    double edgeWidth = 1.0;     // Smooth filter: anti-aliasing width in output pixels
    double textBoldness = 0.35; // Smooth filter: 0 = faithful stroke weight, 1 = bold
    double edgeRoundness = 0.5; // Smooth filter: 0 = sharper corners, 1 = rounder curves
    double graphicsSharpness = 0.8; // Smooth filter: crisp edges in icons and graphics, 0..1

    ColorMode color = ColorMode::Normal;
    uint32_t foreground = 0xFFFF00;  // two-colour mode text colour, 0xRRGGBB
    uint32_t background = 0x000000;  // two-colour mode background colour
    double brightness = 0.0;         // -1..1 added after colour mode
    double contrast = 1.0;           // multiplier around mid grey

    bool trackMouse = true;
    bool trackCaret = true;
    bool trackFocus = true;
    double mouseMargin = 0.10;  // fraction of the view kept as an edge band
    double caretMargin = 0.15;
    double panTime = 0.08;      // seconds, time constant for caret/focus panning

    bool textEnabled = true;         // re-render text from the font where the app reports it
    std::wstring textFont = L"Segoe UI";  // used when the app's own font is not installed
    bool textUseAppFont = true;      // false: always use textFont (e.g. a high-legibility font)
    double textMinZoom = 1.5;        // below this zoom, leave text to the scaling filter
    bool textOcr = true;             // also read text that exists only as pixels (Windows OCR)
    bool iconsEnabled = true;        // draw icons from Windows' large copies

    // The pointer is Windows' own (its shape, size and colour come from
    // Settings > Accessibility > Mouse pointer), magnified with the view up
    // to this many times its size.
    double cursorMaxSize = 8.0;

    std::vector<Hotkey> hotkeys[(int)Action::Count];  // several keys may do the same thing
    UINT wheelZoomModifiers = MOD_CONTROL | MOD_ALT;   // hold these and turn the mouse wheel to zoom

    Settings();
};

std::wstring SettingsPath();
Settings LoadSettings();
void SaveSettings(const Settings& s);

// "Ctrl+Alt+Equals" <-> Hotkey. Returns false if the text is not understood.
bool ParseHotkey(const std::wstring& text, Hotkey& out);
std::wstring FormatHotkey(const Hotkey& h);
// "Ctrl+Alt" <-> MOD_* flags. Returns false if the text is not understood.
bool ParseModifiers(const std::wstring& text, UINT& out);
std::wstring FormatModifiers(UINT mods);
const wchar_t* ActionName(Action a);
