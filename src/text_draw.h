// Turns a verified text run (source desktop pixels) into what the renderer
// draws (backbuffer pixels). Shared by the live render loop and the text
// tests so both draw alike.
#pragma once

#include <algorithm>

#include "renderer.h"
#include "text_source.h"
#include "viewport.h"

// outLeft/outTop: desktop position of backbuffer pixel (0,0).
inline TextDraw MakeTextDraw(const TextRun& r, const Viewport& view, double outLeft, double outTop) {
    TextDraw t;
    // Draw where the pixels say the text is (the verifier's nudge), and
    // repaint the original area plus a pixel of fringe.
    Vec2 a = view.ToOutput({r.rect.l + r.offsetX, r.rect.t + r.offsetY});
    Vec2 b = view.ToOutput({r.rect.r + r.offsetX, r.rect.b + r.offsetY});
    double boxT = r.rect.t - 1 + (std::min)(r.offsetY, 0.0), boxB = r.rect.b + 1 + (std::max)(r.offsetY, 0.0);
    if (r.inkBottom > r.inkTop) {
        // Only around the app's own glyphs: a line's full font height can
        // reach into the lines above and below.
        boxT = (std::max)(boxT, r.inkTop - 2);
        boxB = (std::min)(boxB, r.inkBottom + 2);
    }
    const double boxL = r.rect.l - 1 - r.padLeft + (std::min)(r.offsetX, 0.0);
    const double boxR = r.rect.r + 1 + r.padRight + (std::max)(r.offsetX, 0.0);
    Vec2 ba = view.ToOutput({boxL, boxT});
    Vec2 bb = view.ToOutput({boxR, boxB});
    t.left = (float)(a.x - outLeft);
    t.top = (float)(a.y - outTop);
    t.width = (float)(b.x - a.x);
    t.height = (float)(b.y - a.y);
    t.boxLeft = (float)(ba.x - outLeft);
    t.boxTop = (float)(ba.y - outTop);
    t.boxRight = (float)(bb.x - outLeft);
    t.boxBottom = (float)(bb.y - outTop);
    t.text = r.text;
    t.family = r.font;
    t.emPx = (float)(r.sizePx * view.zoom);
    t.weight = r.weight;
    t.italic = r.italic;
    t.foreground = r.foreground;
    t.background = r.background;
    t.icon = r.icon;
    t.iconFill = r.iconFill;
    if (!r.bgSlope.Flat()) {
        // The background at the corners of the repainted box (source pixels).
        const double cx = (r.rect.l + r.rect.r) / 2, cy = (r.rect.t + r.rect.b) / 2;
        const double xs[4] = {boxL, boxR, boxL, boxR}, ys[4] = {boxT, boxT, boxB, boxB};
        for (int k = 0; k < 4; ++k) {
            uint32_t c = 0;
            for (int ch = 0; ch < 3; ++ch) {
                const int shift = 16 - 8 * ch;
                double v = ((r.background >> shift) & 0xFF) / 255.0 + r.bgSlope.dx[ch] * (xs[k] - cx) +
                           r.bgSlope.dy[ch] * (ys[k] - cy);
                c |= (uint32_t)std::lround(std::clamp(v, 0.0, 1.0) * 255) << shift;
            }
            t.bgCorners[k] = c;
        }
        t.bgGradient = true;
    }
    for (const WordPlace& w : r.words)
        t.words.push_back({r.text.substr(w.start, w.length), (float)(view.ToOutput({w.x, 0}).x - outLeft),
                           (float)(w.dy * view.zoom)});
    return t;
}
