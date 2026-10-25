// Picking the installed font to draw a run with, shared by the renderer and
// the text verifier so both lay text out identically.
#pragma once

#include <dwrite.h>

#include <map>
#include <string>

struct ResolvedFont {
    std::wstring family;
    float ascentShare = 0.8f;  // ascent / (ascent + descent)
};

class FontResolver {
public:
    explicit FontResolver(IDWriteFontCollection* fonts) : m_fonts(fonts) {}

    // The app's family if installed and allowed, else fallback, else Segoe UI.
    ResolvedFont Resolve(const std::wstring& family, const std::wstring& fallback, bool useAppFont, int weight,
                         bool italic);

private:
    bool Lookup(const std::wstring& name, int weight, bool italic, float& share);

    IDWriteFontCollection* m_fonts;
    std::map<std::wstring, std::pair<bool, float>> m_cache;
};

// Where to put the text origin so it sits in an app's line box the way the
// renderer and verifier both expect: the font's ascent+descent box centred
// vertically, and a horizontal stretch so the run spans exactly boxWidth.
struct TextPlacement {
    float originY;  // top of the layout
    float scaleX;
};
TextPlacement PlaceText(float boxTop, float boxHeight, float boxWidth, float layoutWidth, float layoutBaseline,
                        float ascentShare);
