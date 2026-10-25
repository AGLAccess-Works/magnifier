#include "viewport.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

double ClampAxis(double o, double lo, double hi, double extent) {
    double room = (hi - lo) - extent;
    if (room <= 0) return lo + room * 0.5;  // view wider than desktop: centre
    return std::clamp(o, lo, hi - extent);
}

constexpr double kZoomLadder[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 3.5, 4.0,
                                  5.0, 6.0,  7.0, 8.0,  10.0, 12.0, 14.0, 16.0};

}  // namespace

Vec2 ClampOrigin(const Viewport& v, Vec2 o) {
    Vec2 s = v.SourceSize();
    return {ClampAxis(o.x, v.desktop.l, v.desktop.r, s.x),
            ClampAxis(o.y, v.desktop.t, v.desktop.b, s.y)};
}

double EdgeAxis(double current, double lo, double hi, double extent, double margin) {
    margin = std::clamp(margin, 0.0, 0.5);
    double low = hi - (1.0 - margin) * extent;
    double high = lo - margin * extent;
    if (low <= high) return std::clamp(current, low, high);
    // Target does not fit inside the margin band. If it fits in the view at
    // all, do the least movement that shows it whole; otherwise centre it.
    if (hi - lo <= extent) return std::clamp(current, hi - extent, lo);
    return (lo + hi) * 0.5 - extent * 0.5;
}

Vec2 EdgeTrack(const Viewport& v, const RectD& target, double margin) {
    Vec2 s = v.SourceSize();
    Vec2 o = {EdgeAxis(v.origin.x, target.l, target.r, s.x, margin),
              EdgeAxis(v.origin.y, target.t, target.b, s.y, margin)};
    return ClampOrigin(v, o);
}

Vec2 CenterOn(const Viewport& v, Vec2 p) {
    Vec2 s = v.SourceSize();
    return ClampOrigin(v, {p.x - s.x * 0.5, p.y - s.y * 0.5});
}

Vec2 ZoomAbout(const Viewport& v, double newZoom, Vec2 p) {
    Vec2 q = v.ToOutput(p);
    Viewport n = v;
    n.zoom = newZoom;
    n.origin = {p.x - (q.x - v.output.l) / newZoom, p.y - (q.y - v.output.t) / newZoom};
    return ClampOrigin(n, n.origin);
}

Vec2 SmoothApproach(Vec2 cur, Vec2 goal, double dt, double tau, double maxSpeed) {
    double dx = goal.x - cur.x, dy = goal.y - cur.y;
    if (tau <= 0 || dt <= 0) return goal;
    double a = 1.0 - std::exp(-dt / tau);
    double sx = dx * a, sy = dy * a;
    if (maxSpeed > 0) {
        double len = std::hypot(sx, sy), lim = maxSpeed * dt;
        if (len > lim) { sx *= lim / len; sy *= lim / len; }
    }
    Vec2 next = {cur.x + sx, cur.y + sy};
    // Finish the last fraction of a pixel instead of creeping forever.
    if (std::hypot(goal.x - next.x, goal.y - next.y) < 0.05) return goal;
    return next;
}

double NextZoom(double zoom, int dir) {
    if (dir > 0) {
        for (double z : kZoomLadder)
            if (z > zoom + 1e-9) return z;
        return kZoomLadder[std::size(kZoomLadder) - 1];
    }
    for (auto it = std::rbegin(kZoomLadder); it != std::rend(kZoomLadder); ++it)
        if (*it < zoom - 1e-9) return *it;
    return kZoomLadder[0];
}
