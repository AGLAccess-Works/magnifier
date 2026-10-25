// Self-checks for the viewport maths. Exit code 0 means every check passed.
#include "viewport.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

static int g_failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static Viewport MakeView(double zoom) {
    Viewport v;
    v.desktop = {0, 0, 1920, 1080};
    v.output = v.desktop;
    v.zoom = zoom;
    return v;
}

int main() {
    // Forward/inverse round trip below 1e-6 px.
    for (double z : {1.0, 1.5, 2.0, 3.7, 8.0, 16.0}) {
        Viewport v = MakeView(z);
        v.origin = {123.456, 78.9};
        for (double x = -50; x < 2000; x += 97.3)
            for (double y = -50; y < 1200; y += 61.7) {
                Vec2 p = v.ToSource(v.ToOutput({x, y}));
                CHECK(Near(p.x, x) && Near(p.y, y));
            }
    }

    // Clamp keeps the view inside the desktop, and centres it below 1x.
    {
        Viewport v = MakeView(2.0);
        Vec2 o = ClampOrigin(v, {-100, 5000});
        CHECK(Near(o.x, 0) && Near(o.y, 540));
        Viewport small = MakeView(0.5);
        Vec2 c = ClampOrigin(small, {0, 0});
        CHECK(Near(c.x, -960) && Near(c.y, -540));
    }

    // Edge tracking: no movement inside the band, minimal movement outside.
    {
        Viewport v = MakeView(4.0);  // source extent 480 x 270
        v.origin = {400, 300};
        double m = 0.1;               // band is [448, 832] horizontally
        Vec2 o = EdgeTrack(v, {500, 400, 501, 401}, m);
        CHECK(Near(o.x, 400) && Near(o.y, 300));
        o = EdgeTrack(v, {900, 400, 901, 401}, m);  // right edge must be 832
        CHECK(Near(o.x, 901 - 0.9 * 480));
        o = EdgeTrack(v, {420, 400, 421, 401}, m);  // left edge must be 448
        CHECK(Near(o.x, 420 - 0.1 * 480));
        // Oversized target is centred.
        o = EdgeTrack(v, {0, 400, 1000, 401}, m);
        CHECK(Near(o.x, 500 - 240));
    }

    // Edge axis with margin 0.5 centres a point target.
    CHECK(Near(EdgeAxis(0, 300, 300, 200, 0.5), 200));

    // Zooming about a point keeps that point fixed on the output.
    {
        Viewport v = MakeView(2.0);
        v.origin = {300, 200};
        Vec2 p = {700, 450};
        Vec2 q0 = v.ToOutput(p);
        Viewport n = v;
        n.zoom = 3.0;
        n.origin = ZoomAbout(v, 3.0, p);
        Vec2 q1 = n.ToOutput(p);
        CHECK(Near(q0.x, q1.x) && Near(q0.y, q1.y));
    }

    // Smoothing converges, is rate independent, and respects the speed cap.
    {
        Vec2 a = {0, 0}, b = {0, 0}, goal = {100, 0};
        for (int i = 0; i < 60; ++i) a = SmoothApproach(a, goal, 1.0 / 60, 0.1, 0);
        for (int i = 0; i < 144; ++i) b = SmoothApproach(b, goal, 1.0 / 144, 0.1, 0);
        CHECK(Near(a.x, b.x, 1e-3));
        Vec2 c = SmoothApproach({0, 0}, {1000, 0}, 0.1, 0.01, 500);
        CHECK(c.x <= 50.0 + 1e-9);
        CHECK(Near(SmoothApproach({0, 0}, goal, 0.016, 0, 0).x, 100));
    }

    // Zoom ladder.
    CHECK(Near(NextZoom(2.0, +1), 2.5));
    CHECK(Near(NextZoom(2.2, -1), 2.0));
    CHECK(Near(NextZoom(16.0, +1), 16.0));
    CHECK(Near(NextZoom(1.0, -1), 1.0));

    if (g_failures == 0) std::printf("all viewport checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
