#include "settings.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <sstream>
#include <vector>

namespace {

constexpr const wchar_t* kFilterNames[] = {L"nearest", L"bilinear", L"bicubic", L"lanczos", L"smooth"};
constexpr const wchar_t* kColorNames[] = {L"normal",    L"invert",          L"invert-lightness",
                                          L"greyscale", L"greyscale-invert", L"two-colour"};
constexpr const wchar_t* kActionNames[] = {L"ZoomIn",      L"ZoomOut",     L"Toggle",     L"Invert",
                                           L"CycleColour", L"CycleFilter", L"ToggleText", L"Snapshot",
                                           L"Quit",        L"Settings"};
static_assert(std::size(kActionNames) == (size_t)Action::Count);
static_assert(std::size(kFilterNames) == (size_t)Filter::Count);
static_assert(std::size(kColorNames) == (size_t)ColorMode::Count);

struct KeyName {
    const wchar_t* name;
    UINT vk;
};
constexpr KeyName kKeyNames[] = {
    {L"Equals", VK_OEM_PLUS}, {L"Plus", VK_OEM_PLUS},   {L"Minus", VK_OEM_MINUS},
    {L"Up", VK_UP},           {L"Down", VK_DOWN},       {L"Left", VK_LEFT},
    {L"Right", VK_RIGHT},     {L"PageUp", VK_PRIOR},    {L"PageDown", VK_NEXT},
    {L"Home", VK_HOME},       {L"End", VK_END},         {L"Insert", VK_INSERT},
    {L"Delete", VK_DELETE},   {L"Space", VK_SPACE},     {L"Enter", VK_RETURN},
    {L"NumPlus", VK_ADD},     {L"NumMinus", VK_SUBTRACT}, {L"Escape", VK_ESCAPE},
};

bool IEquals(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::wstring ReadStr(const wchar_t* section, const wchar_t* key, const std::wstring& path) {
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 256, path.c_str());
    return Trim(buf);
}

double ReadDouble(const wchar_t* sec, const wchar_t* key, double def, double lo, double hi,
                  const std::wstring& path) {
    std::wstring s = ReadStr(sec, key, path);
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v)) return def;  // "nan" would pass the clamp
    return std::clamp(v, lo, hi);
}

bool ReadBool(const wchar_t* sec, const wchar_t* key, bool def, const std::wstring& path) {
    std::wstring s = ReadStr(sec, key, path);
    if (s.empty()) return def;
    return IEquals(s, L"true") || IEquals(s, L"yes") || IEquals(s, L"on") || s == L"1";
}

uint32_t ReadColour(const wchar_t* sec, const wchar_t* key, uint32_t def, const std::wstring& path) {
    std::wstring s = ReadStr(sec, key, path);
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6 || !std::all_of(s.begin(), s.end(), [](wchar_t ch) { return std::iswxdigit(ch) != 0; }))
        return def;
    return (uint32_t)wcstoul(s.c_str(), nullptr, 16);
}

template <typename E, size_t N>
E ReadEnum(const wchar_t* sec, const wchar_t* key, E def, const wchar_t* const (&names)[N],
           const std::wstring& path) {
    std::wstring s = ReadStr(sec, key, path);
    for (size_t i = 0; i < N; ++i)
        if (IEquals(s, names[i])) return (E)i;
    return def;
}

void Write(const wchar_t* sec, const wchar_t* key, const std::wstring& v, const std::wstring& path) {
    WritePrivateProfileStringW(sec, key, v.c_str(), path.c_str());
}

std::wstring Num(double v) {
    std::wostringstream o;
    o << v;
    return o.str();
}

std::wstring Hex(uint32_t c) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%06X", c & 0xFFFFFF);
    return buf;
}

}  // namespace

const wchar_t* FilterName(Filter f) { return kFilterNames[(int)f]; }
const wchar_t* ColorModeName(ColorMode c) { return kColorNames[(int)c]; }
const wchar_t* ActionName(Action a) { return kActionNames[(int)a]; }

// Bump when default hotkeys change, so older settings files pick them up.
constexpr int kSettingsVersion = 5;

Settings::Settings() {
    const UINT ca = MOD_CONTROL | MOD_ALT;
    // Shift+= is "+" on most keyboards, so accept it with or without Shift.
    hotkeys[(int)Action::ZoomIn] = {{ca, VK_OEM_PLUS}, {ca | MOD_SHIFT, VK_OEM_PLUS}, {ca, VK_UP}, {ca, VK_ADD}};
    hotkeys[(int)Action::ZoomOut] = {{ca, VK_OEM_MINUS}, {ca, VK_DOWN}, {ca, VK_SUBTRACT}};
    hotkeys[(int)Action::Toggle] = {{ca, 'M'}};
    hotkeys[(int)Action::Invert] = {{ca, 'I'}};
    hotkeys[(int)Action::CycleColour] = {{ca, 'C'}};
    hotkeys[(int)Action::CycleFilter] = {{ca, 'F'}};
    hotkeys[(int)Action::ToggleText] = {{ca, 'T'}};
    hotkeys[(int)Action::Snapshot] = {{ca, 'S'}};
    hotkeys[(int)Action::Quit] = {{ca | MOD_SHIFT, 'Q'}};
    hotkeys[(int)Action::Settings] = {{ca, 'O'}};
}

bool ParseModifiers(const std::wstring& text, UINT& out) {
    UINT mods = 0;
    std::wstringstream ss(text);
    std::wstring tok;
    while (std::getline(ss, tok, L'+')) {
        tok = Trim(tok);
        if (IEquals(tok, L"Ctrl") || IEquals(tok, L"Control")) mods |= MOD_CONTROL;
        else if (IEquals(tok, L"Alt")) mods |= MOD_ALT;
        else if (IEquals(tok, L"Shift")) mods |= MOD_SHIFT;
        else if (IEquals(tok, L"Win")) mods |= MOD_WIN;
        else if (!tok.empty()) return false;
    }
    out = mods;
    return true;
}

std::wstring FormatModifiers(UINT mods) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";
    if (!s.empty()) s.pop_back();
    return s;
}

std::wstring SettingsPath() {
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) dir = appdata;
    CoTaskMemFree(appdata);
    dir += L"\\Magnifier";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

bool ParseHotkey(const std::wstring& text, Hotkey& out) {
    Hotkey h;
    std::wstringstream ss(text);
    std::wstring tok;
    std::vector<std::wstring> parts;
    while (std::getline(ss, tok, L'+')) parts.push_back(Trim(tok));
    if (parts.empty()) return false;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const auto& p = parts[i];
        if (IEquals(p, L"Ctrl") || IEquals(p, L"Control")) h.modifiers |= MOD_CONTROL;
        else if (IEquals(p, L"Alt")) h.modifiers |= MOD_ALT;
        else if (IEquals(p, L"Shift")) h.modifiers |= MOD_SHIFT;
        else if (IEquals(p, L"Win")) h.modifiers |= MOD_WIN;
        else return false;
    }
    const std::wstring& key = parts.back();
    if (key.size() == 1 && (std::iswalnum(key[0]))) {
        h.vk = (UINT)std::towupper(key[0]);
    } else if (key.size() >= 2 && (key[0] == L'F' || key[0] == L'f') && std::iswdigit(key[1])) {
        int n = _wtoi(key.c_str() + 1);
        if (n < 1 || n > 24) return false;
        h.vk = VK_F1 + n - 1;
    } else {
        for (const auto& k : kKeyNames)
            if (IEquals(key, k.name)) h.vk = k.vk;
    }
    if (!h.vk) return false;
    out = h;
    return true;
}

std::wstring FormatHotkey(const Hotkey& h) {
    std::wstring s;
    if (h.modifiers & MOD_CONTROL) s += L"Ctrl+";
    if (h.modifiers & MOD_ALT) s += L"Alt+";
    if (h.modifiers & MOD_SHIFT) s += L"Shift+";
    if (h.modifiers & MOD_WIN) s += L"Win+";
    if ((h.vk >= 'A' && h.vk <= 'Z') || (h.vk >= '0' && h.vk <= '9')) return s + (wchar_t)h.vk;
    if (h.vk >= VK_F1 && h.vk <= VK_F24) return s + L"F" + std::to_wstring(h.vk - VK_F1 + 1);
    for (const auto& k : kKeyNames)
        if (k.vk == h.vk) return s + k.name;
    return s + L"?";
}

Settings LoadSettings() {
    Settings s;
    const std::wstring path = SettingsPath();
    const wchar_t* m = L"Magnifier";
    s.enabled = ReadBool(m, L"enabled", s.enabled, path);
    s.zoom = ReadDouble(m, L"zoom", s.zoom, 1.0, 16.0, path);

    const wchar_t* r = L"Rendering";
    s.filter = ReadEnum(r, L"filter", s.filter, kFilterNames, path);
    s.edgeStrength = ReadDouble(r, L"edge_strength", s.edgeStrength, 0.0, 1.0, path);
    s.edgeWidth = ReadDouble(r, L"edge_width", s.edgeWidth, 0.25, 4.0, path);
    s.textBoldness = ReadDouble(r, L"text_boldness", s.textBoldness, 0.0, 1.0, path);
    s.edgeRoundness = ReadDouble(r, L"edge_roundness", s.edgeRoundness, 0.0, 1.0, path);
    s.graphicsSharpness = ReadDouble(r, L"graphics_sharpness", s.graphicsSharpness, 0.0, 1.0, path);

    const wchar_t* c = L"Colour";
    s.color = ReadEnum(c, L"mode", s.color, kColorNames, path);
    s.foreground = ReadColour(c, L"foreground", s.foreground, path);
    s.background = ReadColour(c, L"background", s.background, path);
    s.brightness = ReadDouble(c, L"brightness", s.brightness, -1.0, 1.0, path);
    s.contrast = ReadDouble(c, L"contrast", s.contrast, 0.0, 4.0, path);

    const wchar_t* t = L"Tracking";
    s.trackMouse = ReadBool(t, L"mouse", s.trackMouse, path);
    s.trackCaret = ReadBool(t, L"caret", s.trackCaret, path);
    s.trackFocus = ReadBool(t, L"focus", s.trackFocus, path);
    s.mouseMargin = ReadDouble(t, L"mouse_margin", s.mouseMargin, 0.0, 0.5, path);
    s.caretMargin = ReadDouble(t, L"caret_margin", s.caretMargin, 0.0, 0.5, path);
    s.panTime = ReadDouble(t, L"pan_time", s.panTime, 0.0, 2.0, path);

    const wchar_t* x = L"Text";
    s.textEnabled = ReadBool(x, L"enabled", s.textEnabled, path);
    std::wstring font = ReadStr(x, L"font", path);
    if (!font.empty()) s.textFont = font;
    s.textUseAppFont = ReadBool(x, L"use_app_font", s.textUseAppFont, path);
    s.textMinZoom = ReadDouble(x, L"min_zoom", s.textMinZoom, 1.0, 16.0, path);
    s.textOcr = ReadBool(x, L"ocr", s.textOcr, path);
    s.iconsEnabled = ReadBool(L"Icons", L"enabled", s.iconsEnabled, path);

    s.cursorMaxSize = ReadDouble(L"Pointer", L"max_size", s.cursorMaxSize, 1.0, 16.0, path);

    // Hotkeys from an older file are replaced by the current defaults.
    const bool current = _wtoi(ReadStr(m, L"version", path).c_str()) >= kSettingsVersion;
    for (int i = 0; current && i < (int)Action::Count; ++i) {
        std::wstring txt = ReadStr(L"Hotkeys", kActionNames[i], path);
        if (txt.empty()) continue;
        std::vector<Hotkey> keys;
        std::wstringstream ss(txt);
        std::wstring one;
        while (std::getline(ss, one, L',')) {
            Hotkey h;
            if (ParseHotkey(Trim(one), h)) keys.push_back(h);
        }
        if (!keys.empty()) s.hotkeys[i] = keys;
    }
    UINT mods;
    std::wstring wheel = ReadStr(L"Hotkeys", L"WheelZoom", path);
    if (current && !wheel.empty() && ParseModifiers(wheel, mods)) s.wheelZoomModifiers = mods;
    return s;
}

void SaveSettings(const Settings& s) {
    const std::wstring path = SettingsPath();
    const wchar_t* m = L"Magnifier";
    Write(m, L"enabled", s.enabled ? L"true" : L"false", path);
    Write(m, L"zoom", Num(s.zoom), path);

    const wchar_t* r = L"Rendering";
    Write(r, L"filter", FilterName(s.filter), path);
    Write(r, L"edge_strength", Num(s.edgeStrength), path);
    Write(r, L"edge_width", Num(s.edgeWidth), path);
    Write(r, L"text_boldness", Num(s.textBoldness), path);
    Write(r, L"edge_roundness", Num(s.edgeRoundness), path);
    Write(r, L"graphics_sharpness", Num(s.graphicsSharpness), path);

    const wchar_t* c = L"Colour";
    Write(c, L"mode", ColorModeName(s.color), path);
    Write(c, L"foreground", Hex(s.foreground), path);
    Write(c, L"background", Hex(s.background), path);
    Write(c, L"brightness", Num(s.brightness), path);
    Write(c, L"contrast", Num(s.contrast), path);

    const wchar_t* t = L"Tracking";
    Write(t, L"mouse", s.trackMouse ? L"true" : L"false", path);
    Write(t, L"caret", s.trackCaret ? L"true" : L"false", path);
    Write(t, L"focus", s.trackFocus ? L"true" : L"false", path);
    Write(t, L"mouse_margin", Num(s.mouseMargin), path);
    Write(t, L"caret_margin", Num(s.caretMargin), path);
    Write(t, L"pan_time", Num(s.panTime), path);

    const wchar_t* x = L"Text";
    Write(x, L"enabled", s.textEnabled ? L"true" : L"false", path);
    Write(x, L"font", s.textFont, path);
    Write(x, L"use_app_font", s.textUseAppFont ? L"true" : L"false", path);
    Write(x, L"min_zoom", Num(s.textMinZoom), path);
    Write(x, L"ocr", s.textOcr ? L"true" : L"false", path);
    Write(L"Icons", L"enabled", s.iconsEnabled ? L"true" : L"false", path);

    Write(L"Pointer", L"max_size", Num(s.cursorMaxSize), path);
    // Options earlier versions had.
    for (const wchar_t* key : {L"scale", L"ring", L"crosshair", L"locator_colour"})
        WritePrivateProfileStringW(L"Pointer", key, nullptr, path.c_str());
    for (const wchar_t* key : {L"ReadPointer", L"ReadFocus", L"StopSpeech"})
        WritePrivateProfileStringW(L"Hotkeys", key, nullptr, path.c_str());
    for (const wchar_t* section : {L"Caret", L"Speech"})
        WritePrivateProfileStringW(section, nullptr, nullptr, path.c_str());

    Write(m, L"version", std::to_wstring(kSettingsVersion), path);
    for (int i = 0; i < (int)Action::Count; ++i) {
        std::wstring list;
        for (const auto& h : s.hotkeys[i]) list += (list.empty() ? L"" : L", ") + FormatHotkey(h);
        Write(L"Hotkeys", kActionNames[i], list, path);
    }
    Write(L"Hotkeys", L"WheelZoom", FormatModifiers(s.wheelZoomModifiers), path);
}
