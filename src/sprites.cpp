#include "sprites.h"
#include "raster.h"

#include <cmath>

namespace sprites {
namespace {

using raster::Rgba;

// Flat palette shared with the instruction panel (text.cpp kAccent / card).
constexpr Rgba kViolet = raster::hex(0xB58CFF);
constexpr Rgba kInk = raster::hex(0x10121A);
constexpr Rgba kWhite = raster::hex(0xFFFFFF);
constexpr Rgba kRing = raster::hex(0xE6DAFF);

Image make_dot() {
    constexpr int S = 128;
    Image img;
    raster::alloc(img, S, S);
    const float c = S * 0.5f, half = S * 0.5f;
    const float r_body = half * kSpriteEdge;  // violet disc
    const float r_ring = half * 0.215f;       // dark centre ring
    const float r_pip = half * 0.100f;        // white fixation pip

    // Flat, like the app UI: one solid accent disc, no gradient or shadow. The
    // ink ring + white pip keep the exact fixation point high-contrast on the
    // dim, neutral and bright backdrops alike.
    raster::fill_disc(img, c, c, r_body, kViolet);
    raster::fill_disc(img, c, c, r_ring, kInk);
    raster::fill_disc(img, c, c, r_pip, kWhite);
    raster::bleed(img, kViolet);
    return img;
}

Image make_ring() {
    constexpr int S = 256;
    Image img;
    raster::alloc(img, S, S);
    const float c = S * 0.5f, half = S * 0.5f;
    raster::stroke_circle(img, c, c, half * kSpriteEdge, half * 0.032f, kRing);
    raster::bleed(img, kRing);
    return img;
}

Image make_trail() {
    constexpr int W = 256, H = 64;
    Image img;
    raster::alloc(img, W, H);
    const float cy = H * 0.5f, max_hw = H * 0.5f - 1.5f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float u = (x + 0.5f) / W;  // 0 tail .. 1 head
            const float hw = max_hw * (0.15f + 0.85f * std::pow(u, 0.6f));
            const float cov = raster::coverage(std::fabs(y + 0.5f - cy) - hw);
            const float a = std::pow(u, 1.5f);
            if (cov > 0.0f) raster::blend_at(img, x, y, kViolet, cov * a);
        }
    raster::bleed(img, kViolet);
    return img;
}

}  // namespace

const Image& dot() {
    static const Image img = make_dot();
    return img;
}

const Image& ring() {
    static const Image img = make_ring();
    return img;
}

const Image& trail() {
    static const Image img = make_trail();
    return img;
}

}  // namespace sprites
