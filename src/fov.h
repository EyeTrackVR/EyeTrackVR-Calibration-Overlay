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
    float center_y = -0.10f;
    float rx       = 2.10f;   // half-extent of the gaze range (m)
    float ry       = 2.10f;

    float x(float nx) const { return center_x + nx * rx; }
    float y(float ny) const { return center_y + ny * ry; }
};

struct FovConfig {
    // Fraction of the binocular FOV the outermost targets reach (--coverage).
    // Kept conservative: runtimes (and streamed transports like Virtual Desktop)
    // often report a larger FOV than is comfortably visible at the edges, so
    // leaving a generous margin keeps every target reliably reachable.
    float coverage = 0.70f;
    // Hard cap on target eccentricity in degrees (--max-deg): the human
    // oculomotor range is narrower than wide-FOV displays, and unreachable
    // targets produce mislabelled training samples. This (not coverage) bounds
    // the vertical extent on wide-FOV headsets; 30° keeps every target well
    // inside the visible field.
    float max_deg = 30.0f;
};

// Resolve the binocular gaze range from the two located eye views (poses + FOV
// half-angles, VIEW space) by intersecting each eye frustum with the plane
// z = -dist and overlapping the two boxes. Returns false (leaving `out`
// untouched) if the frusta cannot be resolved or do not overlap.
bool compute_gaze_range(const XrView views[2], float dist,
                        const FovConfig& cfg, GazeRange& out);
