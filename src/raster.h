#pragma once
#include "frame.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Tiny software-raster helpers shared by the procedural sprites, the
// instruction panel and the headless simulator. Colours are straight-alpha
// sRGB bytes; shapes are anti-aliased analytically from signed distances.

namespace raster {

struct Rgba {
    float r, g, b, a;
};  // 0..255 colour, 0..1 alpha

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float smoothstep(float e0, float e1, float x) {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
constexpr Rgba hex(uint32_t rgb, float a = 1.0f) {
    return {(float)((rgb >> 16) & 0xFF), (float)((rgb >> 8) & 0xFF), (float)(rgb & 0xFF), a};
}
inline Rgba mix(Rgba a, Rgba b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// Coverage of a shape edge at signed distance d (pixels; negative = inside).
inline float coverage(float d) { return clamp01(0.5f - d); }

// Porter-Duff "over" of colour c (with coverage cov) onto a straight-alpha pixel.
inline void blend(uint8_t* dst, Rgba c, float cov) {
    const float sa = clamp01(c.a * cov);
    if (sa <= 0.0f) return;
    const float da = dst[3] / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0f) return;
    const float k = da * (1.0f - sa);
    dst[0] = (uint8_t)std::lround((c.r * sa + dst[0] * k) / oa);
    dst[1] = (uint8_t)std::lround((c.g * sa + dst[1] * k) / oa);
    dst[2] = (uint8_t)std::lround((c.b * sa + dst[2] * k) / oa);
    dst[3] = (uint8_t)std::lround(oa * 255.0f);
}

inline void blend_at(Image& img, int x, int y, Rgba c, float cov) {
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) return;
    blend(&img.rgba[((size_t)y * img.w + x) * 4], c, cov);
}

// Signed distance to a rounded rectangle centred at (cx, cy).
inline float sd_round_rect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::fabs(px - cx) - (hw - r);
    const float qy = std::fabs(py - cy) - (hh - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// Signed distance to a capsule (segment a-b with radius r).
inline float sd_capsule(float px, float py, float ax, float ay, float bx, float by, float r) {
    const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    const float h = clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay));
    const float dx = pax - bax * h, dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy) - r;
}

inline void fill_disc(Image& img, float cx, float cy, float r, Rgba c) {
    const int x0 = std::max(0, (int)std::floor(cx - r - 1)), x1 = std::min(img.w - 1, (int)std::ceil(cx + r + 1));
    const int y0 = std::max(0, (int)std::floor(cy - r - 1)), y1 = std::min(img.h - 1, (int)std::ceil(cy + r + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) - r;
            const float cov = coverage(d);
            if (cov > 0.0f) blend_at(img, x, y, c, cov);
        }
}

inline void stroke_circle(Image& img, float cx, float cy, float r, float half_w, Rgba c) {
    const float ro = r + half_w + 1.0f;
    const int x0 = std::max(0, (int)std::floor(cx - ro)), x1 = std::min(img.w - 1, (int)std::ceil(cx + ro));
    const int y0 = std::max(0, (int)std::floor(cy - ro)), y1 = std::min(img.h - 1, (int)std::ceil(cy + ro));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float d = std::fabs(std::hypot(x + 0.5f - cx, y + 0.5f - cy) - r) - half_w;
            const float cov = coverage(d);
            if (cov > 0.0f) blend_at(img, x, y, c, cov);
        }
}

// Give fully transparent pixels the colour c. Straight-alpha textures are
// filtered without regard to alpha, so a transparent-black surround darkens
// anti-aliased edges into a dark fringe; matching the edge colour avoids it.
inline void bleed(Image& img, Rgba c) {
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4)
        if (img.rgba[i + 3] == 0) {
            img.rgba[i + 0] = (uint8_t)std::lround(c.r);
            img.rgba[i + 1] = (uint8_t)std::lround(c.g);
            img.rgba[i + 2] = (uint8_t)std::lround(c.b);
        }
}

inline void alloc(Image& img, int w, int h) {
    img.w = w;
    img.h = h;
    img.rgba.assign((size_t)w * h * 4, 0);
    ++img.version;
}

}  // namespace raster
