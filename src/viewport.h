// Viewport geometry. Everything here is in physical desktop pixels, kept as
// doubles so fractional origins survive (no rounding until rasterization).
//
//   q = output.topLeft + zoom * (p - origin)      source p -> output q
//   p = origin + (q - output.topLeft) / zoom      output q -> source p
#pragma once

struct Vec2 {
    double x = 0, y = 0;
};

struct RectD {
    double l = 0, t = 0, r = 0, b = 0;
    double Width() const { return r - l; }
    double Height() const { return b - t; }
    Vec2 Center() const { return {(l + r) * 0.5, (t + b) * 0.5}; }
};

struct Viewport {
    RectD desktop;   // source area that may be magnified (one monitor for now)
    RectD output;    // where the magnified image is shown
    double zoom = 2.0;
    Vec2 origin;     // top-left of the visible source region

    Vec2 SourceSize() const { return {output.Width() / zoom, output.Height() / zoom}; }
    Vec2 ToOutput(Vec2 p) const {
        return {output.l + zoom * (p.x - origin.x), output.t + zoom * (p.y - origin.y)};
    }
    Vec2 ToSource(Vec2 q) const {
        return {origin.x + (q.x - output.l) / zoom, origin.y + (q.y - output.t) / zoom};
    }
};

// Keep the visible source region inside the desktop. If the region is larger
// than the desktop on an axis (zoom below 1) it is centred on that axis.
Vec2 ClampOrigin(const Viewport& v, Vec2 origin);

// Smallest origin change on one axis that puts [lo, hi] inside the band
// [o + margin*S, o + (1-margin)*S]. margin is a fraction in [0, 0.5].
double EdgeAxis(double current, double lo, double hi, double extent, double margin);

// Edge tracking for a target rectangle, then clamped to the desktop.
Vec2 EdgeTrack(const Viewport& v, const RectD& target, double margin);

// Origin that centres a source point, clamped to the desktop.
Vec2 CenterOn(const Viewport& v, Vec2 point);

// Origin that keeps source point p at the same output position when the zoom
// changes to newZoom. Returned origin is clamped for newZoom.
Vec2 ZoomAbout(const Viewport& v, double newZoom, Vec2 p);

// Frame-rate independent exponential approach towards goal.
// tau: time constant in seconds (0 = jump). maxSpeed: source px/s (0 = no limit).
Vec2 SmoothApproach(Vec2 current, Vec2 goal, double dt, double tau, double maxSpeed);

// Next step on the zoom ladder (dir +1 in, -1 out), clamped to the ladder ends.
double NextZoom(double zoom, int dir);
