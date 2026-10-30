#pragma once
#include <cstdint>

// ── Shared geometry / protocol constants ──────────────────────────────────────
//
// Ported from the original OpenVR overlay. All positions and dot sizes are
// expressed in metres on the overlay plane at OVERLAY_DIST; they scale with the
// distance to keep the same field of view.

namespace cfg {

inline constexpr const char* VERSION = "0.4.0";

// Distance from the HMD to the overlay plane. Increasing this reduces eye
// convergence (more relaxed gaze).
inline constexpr float OVERLAY_DIST = 3.0f;  // |z| of the overlay plane (m)

// Keep compositor surfaces slightly separated and depth-ordered the same way
// they are composited: backdrop furthest, then the instruction panel, then the
// target. Coplanar (or inverted) depths made some compositors draw the text
// panel over the dot. The calibration dot stays exactly at OVERLAY_DIST; only
// non-target surfaces are offset, and their x/y are scaled so they keep the
// same angular position.
inline constexpr float RING_DIST = OVERLAY_DIST + 0.004f;
inline constexpr float TRAIL_DIST = OVERLAY_DIST + 0.002f;
inline constexpr float TEXT_DIST = OVERLAY_DIST + 0.015f;
inline constexpr float BACKDROP_DIST = OVERLAY_DIST + 0.030f;

// UDP ports (identical to the OpenVR build so the Python EyeTrackVR side is
// unchanged): signals out on PORT_SIGNAL, commands in on PORT_CMD.
inline constexpr uint16_t PORT_SIGNAL = 2112;
inline constexpr uint16_t PORT_CMD = 2113;

// Default backdrop grey (sRGB 0-255). Interactive commands 130/131/132 switch
// it for the pupil-reference pass; those exact levels are part of the protocol.
inline constexpr uint8_t BACKDROP_GREY = 96;

}  // namespace cfg
