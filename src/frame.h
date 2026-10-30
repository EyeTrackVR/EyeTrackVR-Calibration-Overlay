#pragma once
#include "config.h"

#include <cstdint>
#include <vector>

// ── Compositor-facing frame description ───────────────────────────────────────
//
// The Presenter's output and the only thing backends render: a backdrop plus
// four head-locked quads, each facing the viewer. Positions/sizes are final
// head-space metres (x right, y up, z negative forward); rot spins a quad about
// the view axis (radians, counter-clockwise, so +x turns towards +y); alpha
// multiplies the texture's own (straight, non-premultiplied) alpha.

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // straight alpha, sRGB, row-major, w*h*4
    uint64_t version = 0;       // bumped whenever the pixels change
};

struct Quad {
    bool visible = false;
    float x = 0.0f, y = 0.0f, z = -cfg::OVERLAY_DIST;
    float w = 0.0f, h = 0.0f;
    float alpha = 1.0f;
    float rot = 0.0f;
};

struct Frame {
    uint8_t backdrop_gray = cfg::BACKDROP_GREY;
    float backdrop_alpha = 1.0f;         // 0 = scene/game fully visible
    Quad panel, ring, trail, dot;        // composited in this order
    const Image* panel_image = nullptr;  // owned by the Presenter; valid until next update
};
