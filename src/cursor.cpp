#include "cursor.h"

#include <shellscalingapi.h>

#include <algorithm>

#include "cursor_hide.h"

namespace {

bool ReadBits(HBITMAP bmp, int w, int h, std::vector<uint32_t>& out) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign((size_t)w * h, 0);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, bmp, 0, h, out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == h;
}

struct IconBits {
    int w = 0, h = 0;
    POINT hotspot{};
    std::vector<uint32_t> colour;  // empty for monochrome pointers
    std::vector<uint32_t> mask;    // AND mask (and XOR below it when monochrome)
};

bool ReadIcon(HCURSOR cursor, IconBits& out) {
    ICONINFO ii = {};
    if (!GetIconInfo(cursor, &ii)) return false;
    bool ok = false;
    BITMAP bm = {};
    if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm)) {
        out.hotspot = {(LONG)ii.xHotspot, (LONG)ii.yHotspot};
        out.w = bm.bmWidth;
        if (ii.hbmColor) {
            out.h = bm.bmHeight;
            ok = ReadBits(ii.hbmColor, out.w, out.h, out.colour) && ReadBits(ii.hbmMask, out.w, out.h, out.mask);
        } else {
            out.h = bm.bmHeight / 2;  // AND mask on top, XOR mask below
            ok = ReadBits(ii.hbmMask, out.w, out.h * 2, out.mask);
        }
    }
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return ok && out.w > 0 && out.h > 0;
}

}  // namespace

double PointerSizeOnScreen(HMONITOR monitor) {
    DWORD base = 32, bytes = sizeof(base);  // 32 is Windows' default pointer size
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", RRF_RT_REG_DWORD, nullptr, &base,
                     &bytes) != ERROR_SUCCESS ||
        base < 16 || base > 512)
        base = 32;
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX < 48) dpiX = 96;
    return base * dpiX / 96.0;
}

bool ExtractCursor(HCURSOR cursor, double shownSize, double scale, CursorImage& out) {
    // While the real pointer is hidden its handle shows a blank image, so read
    // the saved original instead.
    IconBits native;
    HCURSOR original = cursorhide::CopyOriginal(cursor);
    const bool read = original && ReadIcon(original, native);
    if (original) DestroyCursor(original);
    if (!read) return false;
    // Windows enlarges every pointer image to the size it shows pointers at.
    const double shown = shownSize > 0 ? shownSize : native.w;
    int desiredSize = (std::min)((int)(shown * scale + 0.5), 256);

    // Try for a sharper copy at the size we will draw it.
    IconBits bits = native;
    if (desiredSize > native.w) {
        if (HCURSOR big = cursorhide::LoadLarge(cursor, desiredSize)) {
            IconBits large;
            if (ReadIcon(big, large) && large.w == desiredSize && large.h == desiredSize) bits = std::move(large);
            DestroyCursor(big);
        }
    }

    out = CursorImage{};
    out.handle = cursor;
    out.requestedScale = scale;
    out.width = (UINT)bits.w;
    out.height = (UINT)bits.h;
    out.sourcePixelsPerImagePixel = shown / bits.w;
    out.hotspotX = bits.hotspot.x + 0.5;  // the hotspot is the centre of its pixel
    out.hotspotY = bits.hotspot.y + 0.5;
    const size_t n = (size_t)bits.w * bits.h;
    out.colour.assign(n, 0);
    out.invert.assign(n, 0);

    if (!bits.colour.empty()) {
        bool hasAlpha = false;
        for (uint32_t c : bits.colour)
            if (c >> 24) { hasAlpha = true; break; }
        for (size_t i = 0; i < n; ++i) {
            uint32_t c = bits.colour[i];
            bool andBit = (bits.mask[i] & 0xFFFFFF) != 0;
            if (hasAlpha) {
                // Straight alpha in the bitmap; the renderer wants premultiplied.
                uint32_t a = c >> 24;
                uint32_t b = (c & 0xFF) * a / 255, g = ((c >> 8) & 0xFF) * a / 255, r = ((c >> 16) & 0xFF) * a / 255;
                out.colour[i] = (a << 24) | (r << 16) | (g << 8) | b;
            } else if (!andBit) {
                out.colour[i] = 0xFF000000 | (c & 0xFFFFFF);
            } else if (c & 0xFFFFFF) {
                // Masked pixel with colour: Windows XORs it with the screen.
                out.invert[i] = 255;
                out.hasInvert = true;
            }
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            bool andBit = (bits.mask[i] & 0xFFFFFF) != 0;
            bool xorBit = (bits.mask[i + n] & 0xFFFFFF) != 0;
            if (!andBit) {
                out.colour[i] = xorBit ? 0xFFFFFFFF : 0xFF000000;
            } else if (xorBit) {
                out.invert[i] = 255;
                out.hasInvert = true;
            }
        }
    }
    return true;
}
