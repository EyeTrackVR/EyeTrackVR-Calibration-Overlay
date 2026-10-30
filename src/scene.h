#pragma once
#include "config.h"

#include <cstdint>
#include <string>

// What a calibration mode wants shown this frame. Positions are in metres on the
// overlay plane (head-locked x/y at z = -OVERLAY_DIST); sizes are diameters in
// metres. Produced by the modes (modes.cpp) and turned into animated, laid-out
// compositor quads by the Presenter (presenter.cpp); backends never see it.
//
// Modes own *where* the target is and *when* signals fire (that is the
// protocol); everything purely cosmetic — fades, pop-in, ring opacity, where the
// instruction panel goes so it never covers a target — lives in the Presenter.

enum class Tone : uint8_t { Normal, Success };

struct Scene {
    uint8_t backdrop_gray = cfg::BACKDROP_GREY;

    // Fixation dot. dot_size is its diameter.
    bool dot_visible = false;
    float dot_x = 0.0f;
    float dot_y = 0.0f;
    float dot_size = 0.075f;
    // The dot jumped here instead of moving continuously (a new target without
    // a glide): the presenter must not draw a motion trail across the jump.
    bool dot_cut = false;

    // Attention ring converging on the dot (the original build's large
    // shrinking disc). ring_size <= dot_size means no ring. Centred on the
    // target's destination so it appears there while the dot glides over.
    float ring_size = 0.0f;
    float ring_x = 0.0f;
    float ring_y = 0.0f;

    // True while samples are being captured for the current target: the
    // presenter keeps everything around it still (no panel appearing/moving).
    bool capture_hold = false;

    // Instruction panel. First line is the title, further lines the subtitle.
    bool text_visible = false;
    std::string text;
    Tone tone = Tone::Normal;
    // Progress: pips when progress_total > 0 (done = filled, the next one
    // highlighted), otherwise a bar when progress_frac >= 0.
    int progress_total = 0;
    int progress_done = 0;
    float progress_frac = -1.0f;
};
