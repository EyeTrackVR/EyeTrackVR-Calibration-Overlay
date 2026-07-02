#pragma once
#include <string>

// What a calibration mode wants drawn this frame. Positions are in metres on the
// overlay plane (head-locked x/y at z = -OVERLAY_DIST); dot_size is the quad
// width in metres. Text content is diffed against the previous frame so the
// texture is only re-rasterised when it changes.
//
// Backend-agnostic: produced by the modes (modes.cpp) and consumed by whichever
// rendering backend is active (OpenXR scene session or OpenVR overlay).
struct Scene {
    bool        dot_visible  = false;
    float       dot_x        = 0.0f;
    float       dot_y        = 0.0f;
    float       dot_size     = 0.1f;

    bool        text_visible = false;
    std::string text;
};
