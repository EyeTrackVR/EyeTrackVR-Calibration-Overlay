#pragma once
#include <cstdint>

// ── Shared geometry / protocol constants ──────────────────────────────────────
//
// Ported from the original OpenVR overlay. All positions and dot sizes are
// expressed in metres on the overlay plane at OVERLAY_DIST; they scale with the
// distance to keep the same field of view.

namespace cfg {

// Distance from the HMD to the overlay plane. Increasing this reduces eye
// convergence (more relaxed gaze).
inline constexpr float OVERLAY_DIST = 3.0f;   // |z| of the overlay plane (m)

// UDP ports (identical to the OpenVR build so the Python EyeTrackVR side is
// unchanged): signals out on PORT_SIGNAL, commands in on PORT_CMD.
inline constexpr uint16_t PORT_SIGNAL = 2112;
inline constexpr uint16_t PORT_CMD    = 2113;

} // namespace cfg
