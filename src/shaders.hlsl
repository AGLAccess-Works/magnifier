// Magnifier shaders. Compiled at startup, one pixel shader per filter.
//
// Coordinates: texel n covers [n, n+1) and its centre is n + 0.5. For an output
// pixel centre q (SV_Position) the continuous source coordinate is
//     t = srcOrigin + (q - outOffset) / zoom
// and the array-index coordinate used for interpolation is u = t - 0.5.
// Colour work is done on the encoded (sRGB) values, consistently everywhere.

cbuffer Params : register(b0)
{
    float2 srcOrigin;    // continuous texel coordinate shown at outOffset
    float  invZoom;
    float  zoom;
    float2 srcSize;      // texture size in texels
    float2 outOffset;    // backbuffer position of the output origin
    int    colorMode;
    float  edgeStrength;
    float  edgeWidth;
    float  inkThreshold; // Smooth: coverage at which a pixel counts as ink (lower = bolder)
    float3 fgColour;
    float  brightness;
    float3 bgColour;
    float  contrast;
    float  edgeRoundness; // Smooth: 0 = Catmull-Rom outlines, 1 = rounder B-spline outlines
    float  graphicsCrisp; // Smooth: crisping of sharp two-colour edges in icons and graphics
    float2 pad2;
};

cbuffer CursorParams : register(b1)
{
    float4 cursorRect;   // x, y, width, height in backbuffer pixels
    float2 targetSize;   // backbuffer size
    float2 pad1;
};

Texture2D<float4> src       : register(t0);
Texture2D<float4> cursorTex : register(t1);
Texture2D<float>  invertTex : register(t2);
Texture2D<float4> statsLo   : register(t3);  // Smooth pre-pass: rgb = min, a = mean luma
Texture2D<float4> statsHi   : register(t4);  // Smooth pre-pass: rgb = max, a = mean luma squared
Texture2D<float4> statsHLo  : register(t5);  // pre-pass intermediates (horizontal half)
Texture2D<float4> statsHHi  : register(t6);
SamplerState      linearClamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut FullscreenVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float3 Texel(int2 i)
{
    i = clamp(i, int2(0, 0), int2(srcSize) - 1);
    return src.Load(int3(i, 0)).rgb;
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Continuous source texel coordinate for a backbuffer pixel centre.
float2 SourcePos(float2 pos) { return srcOrigin + (pos - outOffset) * invZoom; }

// Catmull-Rom: interpolating, so the field passes through every texel value.
float CubicWeight(float x)
{
    x = abs(x);
    if (x < 1) return (1.5 * x - 2.5) * x * x + 1;
    if (x < 2) return ((-0.5 * x + 2.5) * x - 4) * x + 2;
    return 0;
}

// Cubic B-spline: smooth but not interpolating; its level curves are rounder.
float BSplineWeight(float x)
{
    x = abs(x);
    if (x < 1) return (4 - 6 * x * x + 3 * x * x * x) / 6;
    if (x < 2) return (2 - x) * (2 - x) * (2 - x) / 6;
    return 0;
}

float LanczosWeight(float x)
{
    x = abs(x);
    if (x >= 3) return 0;
    if (x < 1e-5) return 1;
    float px = 3.14159265 * x;
    return 3 * sin(px) * sin(px / 3) / (px * px);
}

// Lanczos-2 on a distance (radial use).
float Lanczos2(float r)
{
    if (r >= 2) return 0;
    if (r < 1e-5) return 1;
    float pr = 3.14159265 * r;
    return 2 * sin(pr) * sin(pr / 2) / (pr * pr);
}

struct Neighbourhood
{
    float3 field;   // bicubic interpolation at u
    float3 lo, hi;  // local per-channel darkest and brightest (continuous in u)
    float  spread;  // how far local colours stray from the lo-hi line (continuous)
};

// Bicubic field plus local statistics. The statistics are computed per texel
// over its 3x3 neighbours for the four texels around u, then interpolated
// bilinearly, so they change smoothly as u moves. (Taking them straight from
// the 4x4 window would make them jump at every texel boundary, which shows up
// as blocks in letter stems.)
Neighbourhood Bicubic(float2 u)
{
    int2 base = (int2)floor(u);
    float2 f = u - base;
    float wx[4], wy[4];
    [unroll] for (int k = 0; k < 4; ++k)
    {
        wx[k] = CubicWeight(f.x - (k - 1));
        wy[k] = CubicWeight(f.y - (k - 1));
    }

    Neighbourhood n;
    float3 c[16];
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 4; ++j)
    {
        [unroll] for (int i = 0; i < 4; ++i)
        {
            float3 s = Texel(base + int2(i - 1, j - 1));
            c[j * 4 + i] = s;
            float w = wx[i] * wy[j];
            acc += s * w;
            wsum += w;
        }
    }
    n.field = acc / wsum;

    // Stats for the central texels (1,1) (2,1) (1,2) (2,2) of the 4x4 block.
    float3 mn[4], mx[4];
    float sp[4];
    [unroll] for (int q = 0; q < 4; ++q)
    {
        int cx = 1 + (q & 1), cy = 1 + (q >> 1);
        mn[q] = 1e9; mx[q] = -1e9;
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                float3 s = c[(cy + y) * 4 + cx + x];
                mn[q] = min(mn[q], s);
                mx[q] = max(mx[q], s);
            }
        float3 d = mx[q] - mn[q];
        float dd = max(dot(d, d), 1e-6);
        sp[q] = 0;
        [unroll] for (int y2 = -1; y2 <= 1; ++y2)
            [unroll] for (int x2 = -1; x2 <= 1; ++x2)
            {
                float3 s = c[(cy + y2) * 4 + cx + x2];
                float t = saturate(dot(s - mn[q], d) / dd);
                sp[q] = max(sp[q], length(s - (mn[q] + t * d)));
            }
    }
    n.lo = lerp(lerp(mn[0], mn[1], f.x), lerp(mn[2], mn[3], f.x), f.y);
    n.hi = lerp(lerp(mx[0], mx[1], f.x), lerp(mx[2], mx[3], f.x), f.y);
    n.spread = lerp(lerp(sp[0], sp[1], f.x), lerp(sp[2], sp[3], f.x), f.y);
    return n;
}

float3 ApplyColour(float3 c)
{
    float y = Luma(c);
    if (colorMode == 1)      c = 1 - c;
    else if (colorMode == 2) c = saturate(c + (1 - 2 * y));   // invert lightness, keep hue
    else if (colorMode == 3) c = y;
    else if (colorMode == 4) c = 1 - y;
    else if (colorMode == 5) c = lerp(fgColour, bgColour, smoothstep(0.4, 0.6, y));
    return saturate((c - 0.5) * contrast + 0.5 + brightness);
}

float4 NearestPS(VSOut i) : SV_Target
{
    float2 t = SourcePos(i.pos.xy);
    return float4(ApplyColour(Texel((int2)floor(t))), 1);
}

float4 BilinearPS(VSOut i) : SV_Target
{
    float2 t = SourcePos(i.pos.xy);
    return float4(ApplyColour(src.SampleLevel(linearClamp, t / srcSize, 0).rgb), 1);
}

float4 BicubicPS(VSOut i) : SV_Target
{
    float2 t = SourcePos(i.pos.xy);
    return float4(ApplyColour(saturate(Bicubic(t - 0.5).field)), 1);
}

float4 LanczosPS(VSOut i) : SV_Target
{
    float2 t = SourcePos(i.pos.xy);
    float2 u = t - 0.5;
    int2 base = (int2)floor(u);
    float2 f = u - base;
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int j = -2; j <= 3; ++j)
    {
        float wy = LanczosWeight(f.y - j);
        [unroll] for (int k = -2; k <= 3; ++k)
        {
            float w = LanczosWeight(f.x - k) * wy;
            acc += Texel(base + int2(k, j)) * w;
            wsum += w;
        }
    }
    return float4(ApplyColour(saturate(acc / wsum)), 1);
}

// ---------------------------------------------------------------- Smooth
//
// Crisp, smooth edges for text and line art at high zoom, built from
// standard image-processing parts:
//
//  1. A pre-pass (StatsH/StatsV, run once per captured frame) measures, over
//     9x9 source pixels, the darkest and lightest colours and the mean and
//     mean-square brightness. That gives each word one text colour, tells
//     dark-on-light from light-on-dark, and tells two-level content (text,
//     line art) from photos and gradients.
//  2. Brightness is reconstructed with Catmull-Rom, which passes through every
//     texel, so one-pixel strokes keep their full darkness.
//  3. A local structure tensor gives the edge direction and how clearly there
//     is one; the brightness is averaged along that direction, which irons out
//     pixel-grid wobble on diagonals and curves without blurring across edges.
//  4. A pixel is ink where that brightness is at least inkThreshold of the way
//     from the background towards the text level, so faint, partly covered
//     strokes still render solid and stroke weight is consistent. The edge
//     gets an anti-aliasing ramp edgeWidth output pixels wide (via fwidth),
//     as when rendering a distance field.
//  5. Ink takes the text colour from the pre-pass; background takes the colour
//     of the immediate neighbourhood, so coloured buttons stay coloured.
//     ClearType fringes disappear because neither colour comes from them.
//
// Gates fall back to plain bicubic where there is no edge, where the content
// is not two-level, and at low zoom. All statistics vary smoothly with
// position, so nothing jumps at texel boundaries.

static const int kStatsRadius = 4;

struct StatsOut
{
    float4 lo : SV_Target0;
    float4 hi : SV_Target1;
};

// Horizontal half of the pre-pass, drawn at source resolution.
StatsOut StatsHPS(VSOut i)
{
    int2 p = (int2)i.pos.xy;
    float3 mn = 1e9, mx = -1e9;
    float sy = 0, syy = 0;
    [unroll] for (int k = -kStatsRadius; k <= kStatsRadius; ++k)
    {
        float3 c = Texel(p + int2(k, 0));
        float y = Luma(c);
        mn = min(mn, c);
        mx = max(mx, c);
        sy += y;
        syy += y * y;
    }
    StatsOut o;
    o.lo = float4(mn, sy);
    o.hi = float4(mx, syy);
    return o;
}

// Vertical half: combines the horizontal results into 9x9 statistics.
StatsOut StatsVPS(VSOut i)
{
    int2 p = (int2)i.pos.xy;
    int2 last = int2(srcSize) - 1;
    float3 mn = 1e9, mx = -1e9;
    float sy = 0, syy = 0;
    [unroll] for (int k = -kStatsRadius; k <= kStatsRadius; ++k)
    {
        int2 q = clamp(p + int2(0, k), int2(0, 0), last);
        float4 lo = statsHLo.Load(int3(q, 0)), hi = statsHHi.Load(int3(q, 0));
        mn = min(mn, lo.rgb);
        mx = max(mx, hi.rgb);
        sy += lo.a;
        syy += hi.a;
    }
    const float n = (2 * kStatsRadius + 1) * (2 * kStatsRadius + 1);
    StatsOut o;
    o.lo = float4(mn, sy / n);
    o.hi = float4(mx, syy / n);
    return o;
}

// How far a pixel is from being a mix of the paper and ink colours, mixed
// per subpixel (ClearType) or per pixel. Each channel's ink coverage must lie
// in [0, 1]; neighbouring subpixels' coverage may differ only gradually,
// since ClearType filters across subpixels (an orange fringe on a black stem
// is about 0.13, 0.46, 0.84), while a coloured icon jumps (Windows red is
// 0.1, 1.0, 0.95). A channel where paper and ink are alike must stay at the
// paper's value.
float SubpixelOff(float3 v, float3 paper, float3 ink)
{
    float3 d = ink - paper;
    float3 known = step(0.15, abs(d));
    float3 a = (v - paper) / (known > 0 ? d : 1);
    float3 outside = max(0, max(-a - 0.1, a - 1.1)) * known
                   + max(0, abs(v - paper) - 0.12) * (1 - known);
    float off = max(outside.r, max(outside.g, outside.b));
    off = max(off, max(0, abs(a.r - a.g) - 0.5) * known.r * known.g);
    off = max(off, max(0, abs(a.g - a.b) - 0.5) * known.g * known.b);
    return off;
}

float4 SmoothPS(VSOut i) : SV_Target
{
    float2 t = SourcePos(i.pos.xy);
    float2 u = t - 0.5;
    int2 base = (int2)floor(u);
    float2 f = u - base;

    // 6x6 window starting two texels before base; u sits at (2,2) + f.
    float3 c[36];
    float y[36];
    [unroll] for (int j = 0; j < 6; ++j)
        [unroll] for (int k = 0; k < 6; ++k)
        {
            c[j * 6 + k] = Texel(base + int2(k - 2, j - 2));
            y[j * 6 + k] = Luma(c[j * 6 + k]);
        }
    const float2 p = float2(2, 2) + f;

    // Structure tensor from central-difference gradients of the inner 4x4.
    float3 J = 0;
    [unroll] for (int jj = 1; jj <= 4; ++jj)
        [unroll] for (int ii = 1; ii <= 4; ++ii)
        {
            float gx = 0.5 * (y[jj * 6 + ii + 1] - y[jj * 6 + ii - 1]);
            float gy = 0.5 * (y[(jj + 1) * 6 + ii] - y[(jj - 1) * 6 + ii]);
            float2 d = float2(ii, jj) - p;
            float w = exp(-dot(d, d) * 0.7);
            J += w * float3(gx * gx, gx * gy, gy * gy);
        }
    float tr = J.x + J.z;
    float root = sqrt((J.x - J.z) * (J.x - J.z) + 4 * J.y * J.y);
    float l1 = 0.5 * (tr + root);
    float coherence = root / (tr + 1e-6);
    coherence *= coherence;
    float2 n = J.x >= J.z ? float2(l1 - J.z, J.y) : float2(J.y, l1 - J.x);
    n = dot(n, n) > 1e-12 ? normalize(n) : float2(1, 0);
    float2 tangent = float2(-n.y, n.x);

    // Pictures, icons and other graphics: edge-directed resampling. A
    // Lanczos-2 kernel over the 6x6 texels, stretched along the local edge
    // and narrowed across it as the edge gets clearer, so diagonals and
    // curves come out smooth instead of stepped while edges stay sharp; then
    // clamped to the range of the four nearest texels so the kernel's lobes
    // cannot ring (no halos). Where there is no clear edge it is a plain,
    // sharp Lanczos-2.
    float3 aniso = 0;
    float anisoW = 0;
    {
        const float stretch = 1 + 0.9 * coherence, squeeze = 1 - 0.3 * coherence;
        [unroll] for (int aj = 0; aj < 6; ++aj)
            [unroll] for (int ai = 0; ai < 6; ++ai)
            {
                float2 d = float2(ai, aj) - p;
                float2 e = float2(dot(d, tangent) / stretch, dot(d, n) / squeeze);
                float w = Lanczos2(length(e));
                aniso += c[aj * 6 + ai] * w;
                anisoW += w;
            }
        aniso /= max(anisoW, 1e-4);
        float3 near4Min = min(min(c[14], c[15]), min(c[20], c[21]));
        float3 near4Max = max(max(c[14], c[15]), max(c[20], c[21]));
        aniso = clamp(aniso, near4Min, near4Max);
    }

    // Brightness field (Catmull-Rom) at five points along the edge.
    float along = 0.35 * coherence;  // source pixels between samples
    float ys = 0, wsum = 0;
    [unroll] for (int m = -2; m <= 2; ++m)
    {
        float2 q = p + tangent * (along * m);
        float wx[6], wy[6];
        [unroll] for (int a = 0; a < 6; ++a)
        {
            wx[a] = lerp(CubicWeight(q.x - a), BSplineWeight(q.x - a), edgeRoundness);
            wy[a] = lerp(CubicWeight(q.y - a), BSplineWeight(q.y - a), edgeRoundness);
        }
        float acc = 0, wq = 0;
        [unroll] for (int b = 0; b < 6; ++b)
            [unroll] for (int a2 = 0; a2 < 6; ++a2)
            {
                float w = wx[a2] * wy[b];
                acc += y[b * 6 + a2] * w;
                wq += w;
            }
        float gw = exp(-0.5 * m * m);
        ys += gw * acc / wq;
        wsum += gw;
    }
    ys /= wsum;

    // Wide statistics from the pre-pass (bilinear, so they vary smoothly).
    float4 lo = statsLo.SampleLevel(linearClamp, t / srcSize, 0);
    float4 hi = statsHi.SampleLevel(linearClamp, t / srcSize, 0);
    float yd = Luma(lo.rgb), yl = Luma(hi.rgb), range = yl - yd;
    float mean = lo.a, variance = max(hi.a - mean * mean, 0);
    // Wider 27x27 mean (nine 9x9 means), for deciding which level is ink
    // where dense bold text is about half ink locally. Dark text on a light
    // page is favoured when unsure.
    float wideMean = 0;
    [unroll] for (int oy = -1; oy <= 1; ++oy)
        [unroll] for (int ox = -1; ox <= 1; ++ox)
            wideMean += statsLo.SampleLevel(linearClamp, (t + float2(ox, oy) * (2 * kStatsRadius + 1)) / srcSize, 0).a;
    wideMean /= 9;
    // Two-level content has variance close to (mean-dark)(light-mean); a
    // spread of tones (photo, gradient) has much less.
    float twoLevel = variance / max((mean - yd) * (yl - mean), 1e-5);
    // Use the local 9x9 answer when it is clear-cut (mostly paper or mostly
    // ink), and the 27x27 one only when the local area is about half ink.
    float pos9 = (mean - yd) / max(range, 1e-4);
    float localDark = smoothstep(0.35, 0.65, pos9);
    float wideDark = smoothstep(0.25, 0.5, (wideMean - yd) / max(range, 1e-4));
    float inkIsDark = lerp(wideDark, localDark, smoothstep(0.12, 0.25, abs(pos9 - 0.5)));
    float T = lerp(yd + inkThreshold * range, yl - inkThreshold * range, inkIsDark);

    // Background colour from the immediate neighbours (per-channel extremes of
    // each core texel's 3x3, interpolated).
    // Also, per core texel, whether its 3x3 is a sharp edge between two flat
    // colours (for graphics): colours close to the min-max line, and
    // brightness bunched at the two ends rather than spread (a gradient).
    const float3 widePaper = lerp(lo.rgb, hi.rgb, inkIsDark), wideInk = lerp(hi.rgb, lo.rgb, inkIsDark);
    float3 nMin[4], nMax[4];
    float nOff[4], nTwo[4], nTextOff[4], nSubOff[4];
    float3 nHMin[4], nHMax[4];
    [unroll] for (int qd = 0; qd < 4; ++qd)
    {
        int cx = 2 + (qd & 1), cy = 2 + (qd >> 1);
        nMin[qd] = 1e9;
        nMax[qd] = -1e9;
        float ySum = 0, yySum = 0, yLo = 1e9, yHi = -1e9;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                float3 v = c[(cy + dy) * 6 + cx + dx];
                nMin[qd] = min(nMin[qd], v);
                nMax[qd] = max(nMax[qd], v);
                float yv = y[(cy + dy) * 6 + cx + dx];
                ySum += yv;
                yySum += yv * yv;
                yLo = min(yLo, yv);
                yHi = max(yHi, yv);
            }
        float3 d = nMax[qd] - nMin[qd];
        float dd = max(dot(d, d), 1e-6);
        nOff[qd] = 0;
        nSubOff[qd] = 0;
        [unroll] for (int dy2 = -1; dy2 <= 1; ++dy2)
            [unroll] for (int dx2 = -1; dx2 <= 1; ++dx2)
            {
                float3 v = c[(cy + dy2) * 6 + cx + dx2];
                float tt = saturate(dot(v - nMin[qd], d) / dd);
                nOff[qd] = max(nOff[qd], length(v - (nMin[qd] + tt * d)));
                nSubOff[qd] = max(nSubOff[qd], SubpixelOff(v, widePaper, wideInk));
            }
        float m9 = ySum / 9, var9 = max(yySum / 9 - m9 * m9, 0);
        nTwo[qd] = var9 / max((m9 - yLo) * (yHi - m9), 1e-5);

        // The same off-line measure on colours averaged with their horizontal
        // neighbours, which merges ClearType subpixel fringes: text (any
        // colour, ClearType or not) stays on one line between paper and ink,
        // multi-colour graphics do not.
        float3 hMin = 1e9, hMax = -1e9, h[9];
        [unroll] for (int dy3 = -1; dy3 <= 1; ++dy3)
            [unroll] for (int dx3 = -1; dx3 <= 1; ++dx3)
            {
                int idx = (cy + dy3) * 6 + cx + dx3;
                float3 av = (c[idx - 1] + c[idx] + c[idx + 1]) / 3;
                h[(dy3 + 1) * 3 + dx3 + 1] = av;
                hMin = min(hMin, av);
                hMax = max(hMax, av);
            }
        nHMin[qd] = hMin;
        nHMax[qd] = hMax;
        float3 hd = hMax - hMin;
        float hdd = max(dot(hd, hd), 1e-6);
        nTextOff[qd] = 0;
        [unroll] for (int k3 = 0; k3 < 9; ++k3)
        {
            float tt = saturate(dot(h[k3] - hMin, hd) / hdd);
            nTextOff[qd] = max(nTextOff[qd], length(h[k3] - (hMin + tt * hd)));
        }
    }
    float3 localMin = lerp(lerp(nMin[0], nMin[1], f.x), lerp(nMin[2], nMin[3], f.x), f.y);
    float3 localMax = lerp(lerp(nMax[0], nMax[1], f.x), lerp(nMax[2], nMax[3], f.x), f.y);
    float localOff = lerp(lerp(nOff[0], nOff[1], f.x), lerp(nOff[2], nOff[3], f.x), f.y);
    float localTwo = lerp(lerp(nTwo[0], nTwo[1], f.x), lerp(nTwo[2], nTwo[3], f.x), f.y);
    float textOff = lerp(lerp(nTextOff[0], nTextOff[1], f.x), lerp(nTextOff[2], nTextOff[3], f.x), f.y);
    float subOff = lerp(lerp(nSubOff[0], nSubOff[1], f.x), lerp(nSubOff[2], nSubOff[3], f.x), f.y);
    // The local colours (subpixel-merged) must also lie on the line between
    // the wide paper and ink colours: true for text, false at an icon edge
    // whose 9x9 area contains other colours too.
    float3 hLocalMin = lerp(lerp(nHMin[0], nHMin[1], f.x), lerp(nHMin[2], nHMin[3], f.x), f.y);
    float3 hLocalMax = lerp(lerp(nHMax[0], nHMax[1], f.x), lerp(nHMax[2], nHMax[3], f.x), f.y);
    float3 segD = lo.rgb - hi.rgb;
    float segDD = max(dot(segD, segD), 1e-6);
    float3 pMin = hi.rgb + saturate(dot(hLocalMin - hi.rgb, segD) / segDD) * segD;
    float3 pMax = hi.rgb + saturate(dot(hLocalMax - hi.rgb, segD) / segDD) * segD;
    float wideOff = max(length(hLocalMin - pMin), length(hLocalMax - pMax));
    float3 inkColour = lerp(hi.rgb, lo.rgb, inkIsDark);
    float3 paperColour = lerp(localMin, localMax, inkIsDark);

    // ClearType draws neutral text with coloured subpixel fringes, so a stem
    // can lack a dark value in one channel and "darkest per channel" turns
    // coloured. Averaging each pixel with its horizontal neighbours (as the
    // eye merges subpixels) tells the cases apart: if the darkest (or, for
    // light ink, lightest) averaged colour is grey, the ink is grey, drawn at
    // the most extreme level present. Genuinely coloured text stays coloured.
    float yBest = 1e9, yWorst = -1e9;
    float3 darkAvg = 0, lightAvg = 0;
    [unroll] for (int sj = 1; sj <= 4; ++sj)
        [unroll] for (int si = 1; si <= 4; ++si)
        {
            float3 av = (c[sj * 6 + si - 1] + c[sj * 6 + si] + c[sj * 6 + si + 1]) / 3;
            float ly = Luma(av);
            if (ly < yBest) { yBest = ly; darkAvg = av; }
            if (ly > yWorst) { yWorst = ly; lightAvg = av; }
        }
    float3 inkAvg = lerp(lightAvg, darkAvg, inkIsDark);
    float inkSat = max(inkAvg.r, max(inkAvg.g, inkAvg.b)) - min(inkAvg.r, min(inkAvg.g, inkAvg.b));
    float paperSat = max(paperColour.r, max(paperColour.g, paperColour.b)) - min(paperColour.r, min(paperColour.g, paperColour.b));
    float neutral = (1 - smoothstep(0.08, 0.2, inkSat)) * (1 - smoothstep(0.15, 0.35, paperSat));
    float darkest = min(lo.r, min(lo.g, lo.b)), lightest = max(hi.r, max(hi.g, hi.b));
    inkColour = lerp(inkColour, lerp(lightest, darkest, inkIsDark).xxx, neutral);

    float w = max(fwidth(ys) * edgeWidth, 1e-5);
    float ink = lerp(saturate((ys - T) / w + 0.5), saturate((T - ys) / w + 0.5), inkIsDark);
    float3 crisp = lerp(paperColour, inkColour, ink);

    const float zoomGate = saturate((zoom - 1.2) / 0.6);
    // Two colours (not an icon) that match the wider area: by the merged-
    // colour tests, or by every pixel being paper and ink mixed per subpixel
    // (strong ClearType fringes on bold text defeat the merged colours).
    float twoColours = max((1 - smoothstep(0.1, 0.22, textOff)) * (1 - smoothstep(0.1, 0.25, wideOff)),
                           1 - smoothstep(0.06, 0.18, subOff));
    float gate = edgeStrength
               * smoothstep(0.08, 0.25, range)            // there is contrast nearby
               * smoothstep(0.45, 0.7, twoLevel)          // and it is two-level
               * twoColours
               * zoomGate;                                // and we are zoomed in
    // Where it is text, the fallback loses its ClearType fringes too.
    float3 picture = lerp(aniso, Luma(aniso).xxx, neutral * gate);
    float3 result = lerp(picture, crisp, gate);

    // Graphics (icons, symbols): where the immediate neighbourhood is a
    // sharp edge between two flat colours, re-threshold halfway between them
    // with a slightly wider ramp than text. One step along the line between
    // the two colours, not one per channel, so the colours cannot part into
    // fringes. Gradients and photos fail the test and keep the resampling.
    float3 gd = localMax - localMin;
    float gt = saturate(dot(aniso - localMin, gd) / max(dot(gd, gd), 1e-6));
    float gw = max(fwidth(gt) * edgeWidth * 1.25, 1e-5);
    float3 crispGraphic = lerp(localMin, localMax, saturate((gt - 0.5) / gw + 0.5));
    float gRange = max(localMax.r - localMin.r, max(localMax.g - localMin.g, localMax.b - localMin.b));
    float graphicGate = graphicsCrisp
                      * smoothstep(0.12, 0.3, gRange)         // a real edge
                      * (1 - smoothstep(0.06, 0.16, localOff)) // between two colours
                      * smoothstep(0.55, 0.8, localTwo)        // not a gradient
                      * zoomGate;
    result = lerp(result, crispGraphic, graphicGate * (1 - gate));
    return float4(ApplyColour(saturate(result)), 1);
}

VSOut CursorVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2(id & 1, id >> 1);
    float2 px = cursorRect.xy + uv * cursorRect.zw;
    o.pos = float4(px / targetSize * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 CursorPS(VSOut i) : SV_Target
{
    return cursorTex.Sample(linearClamp, i.uv);
}

float4 CursorInvertPS(VSOut i) : SV_Target
{
    float m = invertTex.Sample(linearClamp, i.uv);
    return float4(m, m, m, m);
}
