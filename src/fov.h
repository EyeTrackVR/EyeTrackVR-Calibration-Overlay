#pragma once
#include <openxr/openxr.h>

// ── Gaze-range geometry ───────────────────────────────────────────────────────
//
// Replaces init_fov()/eye_plane_box()/gaze_x()/gaze_y() from the OpenVR build.
// The gaze range is the region of the overlay plane visible to BOTH eyes,
// centred on the true binocular optical axis. Targets are placed in normalised
// coordinates (±1 = edge of the gaze range) and mapped to metres via x()/y().

struct GazeRange {
    // Fallbacks approximate a 90° symmetric headset at 3 m until the runtime's
    // real frusta are resolved (see compute_gaze_range).
    float center_x = 0.0f;
    float center_y = 0.0f;
    float rx       = 2.10f;   // horizontal half-extent of the gaze range (m)
    // Vertical extents are independent because binocular visibility and useful
    // eye rotation are strongly asymmetric above/below straight-ahead.
    float ry_up   = 2.10f;
    float ry_down = 2.10f;

    float x(float nx) const { return center_x + nx * rx; }
    float y(float ny) const { return center_y + ny * (ny >= 0.0f ? ry_up : ry_down); }
};

struct FovConfig {
    // Fraction of the binocular FOV the outermost targets reach (--coverage).
    // Kept conservative: runtimes (and streamed transports like Virtual Desktop)
    // often report a larger FOV than is comfortably visible at the edges, so
    // leaving a generous margin keeps every target reliably reachable.
    float coverage = 0.70f;
    // Hard caps on target eccentricity in degrees: the human oculomotor range
    // is narrower than wide-FOV displays, and unreachable targets produce
    // mislabelled training samples. These (not coverage) bound the extents on
    // wide-FOV headsets. Vertical comfort is asymmetric — upward eye rotation
    // is markedly smaller than lateral, downward markedly larger, and resting
    // gaze sits below the horizon. Directional options remain available for
    // experiments. Downstream labels/calibration must use the matching cap for
    // the sign of Y.
    float max_deg      = 30.0f;   // lateral (--max-deg)
    float max_deg_up   = 15.0f;   // upward  (--max-deg-up)
    float max_deg_down = 40.0f;   // downward (--max-deg-down)
    // Elevation of the centre (ny=0) target in degrees, negative = below the
    // horizon (--center-deg). The centre dot is the calibration's straight-ahead
    // fixation point — the neutral gaze the tracker maps to zero. Kept at 0 so
    // the grid is anchored on the true forward axis (= what VRChat treats as
    // neutral gaze). The up/down extents may differ, but both start from the
    // same physical zero. A non-zero anchor desyncs eye rotation from world
    // elevation and leaks in as a small vertical bias downstream.
    // Resting gaze may sit a hair off the optical axis, but that constant offset
    // is absorbed by the per-user calibration (centre dot → 0) and headset
    // recenter, so anchoring at forward is the cleaner reference.
    float center_deg = 0.0f;
};

// Resolve the binocular gaze range from the two located eye views (poses + FOV
// half-angles, VIEW space) by intersecting each eye frustum with the plane
// z = -dist and overlapping the two boxes. Returns false (leaving `out`
// untouched) if the frusta cannot be resolved or do not overlap.
bool compute_gaze_range(const XrView views[2], float dist,
                        const FovConfig& cfg, GazeRange& out);

// Print the resolved gaze range (metres + degrees) — shared by both backends so
// eyetrackvr_overlay.log always records what geometry a run actually used.
void log_gaze_range(const GazeRange& g, const FovConfig& cfg, float dist);
