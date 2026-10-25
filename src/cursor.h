// Converts the current Windows pointer into images the renderer can draw.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

struct CursorImage {
    HCURSOR handle = nullptr;
    double requestedScale = 0;

    UINT width = 0, height = 0;
    double hotspotX = 0, hotspotY = 0;   // in image pixels
    double sourcePixelsPerImagePixel = 1; // screen pixels per image pixel, as Windows shows it
    std::vector<uint32_t> colour;         // premultiplied BGRA
    std::vector<uint8_t> invert;          // 255 where the screen underneath is inverted
    bool hasInvert = false;
};

// The width in screen pixels Windows draws pointers at on a monitor: the
// pointer size from Settings > Accessibility > Mouse pointer (96-DPI pixels),
// scaled for the monitor's display scaling. Windows hands out the pointer
// images at a smaller size, so this is what the magnified pointer is sized by.
double PointerSizeOnScreen(HMONITOR monitor);

// Reads the pointer image. shownSize is the width Windows draws it at on the
// real screen (PointerSizeOnScreen); scale is the zoom: each of those screen
// pixels covers scale output pixels. Windows is asked for a sharp copy drawn
// at the final size where it can make one.
bool ExtractCursor(HCURSOR cursor, double shownSize, double scale, CursorImage& out);
