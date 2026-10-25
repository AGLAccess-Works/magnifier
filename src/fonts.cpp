#include "fonts.h"

#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

bool FontResolver::Lookup(const std::wstring& name, int weight, bool italic, float& share) {
    std::wstring key = name + L"|" + std::to_wstring(weight) + (italic ? L"|i" : L"|n");
    auto it = m_cache.find(key);
    if (it == m_cache.end()) {
        bool found = false;
        float s = 0.8f;
        UINT32 index = 0;
        BOOL exists = FALSE;
        ComPtr<IDWriteFontFamily> fam;
        ComPtr<IDWriteFont> font;
        if (m_fonts && !name.empty() && SUCCEEDED(m_fonts->FindFamilyName(name.c_str(), &index, &exists)) && exists &&
            SUCCEEDED(m_fonts->GetFontFamily(index, &fam)) &&
            SUCCEEDED(fam->GetFirstMatchingFont((DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STRETCH_NORMAL,
                                                italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font))) {
            DWRITE_FONT_METRICS m;
            font->GetMetrics(&m);
            if (m.ascent + m.descent > 0) s = (float)m.ascent / (m.ascent + m.descent);
            found = true;
        }
        it = m_cache.emplace(key, std::make_pair(found, s)).first;
    }
    share = it->second.second;
    return it->second.first;
}

ResolvedFont FontResolver::Resolve(const std::wstring& family, const std::wstring& fallback, bool useAppFont,
                                   int weight, bool italic) {
    ResolvedFont r;
    if (useAppFont && Lookup(family, weight, italic, r.ascentShare)) r.family = family;
    else if (Lookup(fallback, weight, italic, r.ascentShare)) r.family = fallback;
    else {
        Lookup(L"Segoe UI", weight, italic, r.ascentShare);
        r.family = L"Segoe UI";
    }
    return r;
}

TextPlacement PlaceText(float boxTop, float boxHeight, float boxWidth, float layoutWidth, float layoutBaseline,
                        float ascentShare) {
    float fontBox = layoutBaseline / ascentShare;  // ascent / share = ascent + descent
    float baseline = boxTop + (boxHeight - fontBox) * 0.5f + fontBox * ascentShare;
    TextPlacement p;
    p.originY = baseline - layoutBaseline;
    p.scaleX = layoutWidth > 0 ? std::clamp(boxWidth / layoutWidth, 0.75f, 1.33f) : 1.0f;
    return p;
}
