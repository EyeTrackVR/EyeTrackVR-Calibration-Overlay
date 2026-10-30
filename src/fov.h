#pragma once
#include "math3d.h"

#include <string>
#include <vector>

// ── Gaze-range geometry ───────────────────────────────────────────────────────
//
// The gaze range is the region of the overlay plane visible to BOTH eyes,
// anchored on straight-ahead. Targets are placed in normalised coordinates
// (±1 = edge of the gaze range) and mapped to metres via x()/y(). Both backends
// (and the headless simulator) describe the headset as two EyeGeoms and feed
// them through compute_view_geometry(), so every runtime shares one policy.

struct GazeRange {
    // Fallbacks approximate a 90° symmetric headset at 3 m until the runtime's
    // real frusta are resolved (see compute_view_geometry).
    float center_x = 0.0f;
    float center_y = 0.0f;
    float rx = 2.10f;  // horizontal half-extent of the gaze range (m)
    // Vertical extents are independent because binocular visibility and useful
    // eye rotation are strongly asymmetric above/below straight-ahead.
    float ry_up = 2.10f;
    float ry_down = 2.10f;

    // Binocular box visible to both eyes on the overlay plane (m), before the
    // coverage/caps are applied. Used for UI layout, never for target labels.
    float vis_left = -2.10f, vis_right = 2.10f, vis_bottom = -2.10f, vis_top = 2.10f;

    // False while this is a synthetic fallback (no headset geometry yet).
    bool valid = false;

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
    float max_deg = 30.0f;       // lateral (--max-deg)
    float max_deg_up = 15.0f;    // upward  (--max-deg-up)
    float max_deg_down = 40.0f;  // downward (--max-deg-down)
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

// One eye of the headset, in head (VIEW) space: x right, y up, -z forward.
struct EyeGeom {
    // Eye position + orientation. The orientation carries any display cant;
    // the tangents and polygon below are relative to that canted axis.
    Pose pose;
    // Visible frustum as Y-up half-angle tangents (left/down negative).
    float tan_left = -1.0f, tan_right = 1.0f, tan_up = 1.0f, tan_down = -1.0f;
    // Optional lens-visible region (OpenVR hidden-area line loop, OpenXR
    // visibility mask) as a polygon in this eye's tangent space. Empty = the
    // rectangle above is the whole story.
    std::vector<Vec2> visible;

    bool contains_tangent(float tx, float ty) const;
    // True if the head-space point is inside this eye's visible region.
    bool sees(Vec3 head_point) const;
};

struct ViewGeometry {
    GazeRange gaze;
    EyeGeom eyes[2];
    bool synthetic = true;       // eyes are a generic stand-in, not the headset
    bool used_polygons = false;  // lens-visibility polygons constrained the range
    float ellipse_scale = 1.0f;  // uniform shrink applied so the target ellipse is visible

    // Point on the plane z (default: the overlay plane) visible to both eyes.
    bool visible(float x, float y, float z) const;
    bool visible(float x, float y) const;
    // A disc of radius r (centre + rim samples) visible to both eyes.
    bool disc_visible(float x, float y, float r) const;
    // Axis-aligned rectangle (edges sampled) visible to both eyes, plane z.
    bool rect_visible(float x0, float y0, float x1, float y1, float z) const;
};

// Margin around each target that must stay visible (m on the overlay plane):
// the fixation dot's radius plus a little slack.
inline constexpr float kTargetVisibleMargin = 0.05f;

// Resolve the binocular gaze range from the two eyes. Applies coverage and the
// per-direction eccentricity caps, then (always) verifies that the whole
// normalised target ellipse is actually visible to both eyes — through the lens
// polygons when the runtime provides them — shrinking the range only as much as
// needed. Invalid polygons are discarded (with a note) rather than trusted.
// Returns false (leaving `out` untouched) if the frusta cannot be resolved.
bool compute_view_geometry(const EyeGeom eyes[2], float dist, const FovConfig& cfg, ViewGeometry& out,
                           std::string* notes = nullptr);

// Generic ~95° headset run through the same policy (caps/coverage honoured).
// Used until/unless the runtime's real geometry is known. gaze.valid == false.
ViewGeometry fallback_view_geometry(const FovConfig& cfg, float dist);

// Validate an eye's lens polygon; clears it (returning false, with reason) if
// it is degenerate, non-finite, tiny, or does not contain the eye's own axis.
bool sanitize_polygon(EyeGeom& eye, std::string* why = nullptr);

bool point_in_polygon(const std::vector<Vec2>& poly, float x, float y);
float polygon_area(const std::vector<Vec2>& poly);

// Print the resolved gaze range (metres + degrees) — shared by every backend so
// eyetrackvr_overlay.log always records what geometry a run actually used.
void log_view_geometry(const ViewGeometry& g, const FovConfig& cfg, float dist);
void log_eye(const char* tag, const EyeGeom& e);
