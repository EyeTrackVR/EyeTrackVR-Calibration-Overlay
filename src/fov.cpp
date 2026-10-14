#include "fov.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Rotate a vector by a unit quaternion (q.xyz = imaginary, q.w = real).
XrVector3f rotate(const XrQuaternionf& q, XrVector3f v) {
    const XrVector3f u{q.x, q.y, q.z};
    auto cross = [](XrVector3f a, XrVector3f b) {
        return XrVector3f{a.y * b.z - a.z * b.y,
                          a.z * b.x - a.x * b.z,
                          a.x * b.y - a.y * b.x};
    };
    auto dot = [](XrVector3f a, XrVector3f b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    XrVector3f t = cross(u, v);
    t = {2.0f * t.x, 2.0f * t.y, 2.0f * t.z};
    XrVector3f c = cross(u, t);
    return {v.x + q.w * t.x + c.x,
            v.y + q.w * t.y + c.y,
            v.z + q.w * t.z + c.z};
    (void)dot;
}

// Intersect one eye's view frustum with the plane z = -dist (VIEW space) and
// return the largest axis-aligned box inside the projected quad. Mirrors the
// OpenVR eye_plane_box(), but uses the unambiguous Y-up XrFovf half-angles
// instead of GetProjectionRaw's Y-down tangents.
bool eye_plane_box(const XrView& view, float dist,
                   float& x0, float& x1, float& y0, float& y1) {
    const float tanL = std::tan(view.fov.angleLeft);   // negative
    const float tanR = std::tan(view.fov.angleRight);  // positive
    const float tanU = std::tan(view.fov.angleUp);     // positive
    const float tanD = std::tan(view.fov.angleDown);   // negative

    // Frustum corners on the z = -1 plane in eye space (x right, y up, -z fwd):
    // TL, TR, BR, BL.
    const float cx[4] = {tanL, tanR, tanR, tanL};
    const float cy[4] = {tanU, tanU, tanD, tanD};

    const XrVector3f o = view.pose.position;
    float px[4], py[4];
    for (int i = 0; i < 4; ++i) {
        XrVector3f d = rotate(view.pose.orientation, {cx[i], cy[i], -1.0f});
        if (d.z >= 0.0f) return false;
        const float s = (-dist - o.z) / d.z;
        px[i] = o.x + s * d.x;
        py[i] = o.y + s * d.y;
    }
    x0 = std::max(px[0], px[3]);   // tightest left edge
    x1 = std::min(px[1], px[2]);   // tightest right edge
    y0 = std::max(py[2], py[3]);   // tightest bottom edge
    y1 = std::min(py[0], py[1]);   // tightest top edge
    if (x1 <= x0 || y1 <= y0) return false;

    // With a rotated/canted XrView the projected frustum is a slanted quad.
    // Taking independent tightest X/Y edges does not guarantee that the four
    // combined rectangle corners lie inside that quad (cardinal targets remain
    // visible while diagonals clip). OpenVR views on conventional headsets are
    // axis-aligned and pass unchanged; canted OpenXR views are shrunk only as
    // much as needed to make the whole axis-aligned box genuinely inscribed.
    auto inside_quad = [&](float x, float y) {
        bool pos = false, neg = false;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) & 3;
            const float cross = (px[j] - px[i]) * (y - py[i])
                              - (py[j] - py[i]) * (x - px[i]);
            pos |= cross > 1e-5f;
            neg |= cross < -1e-5f;
        }
        return !(pos && neg);
    };
    auto box_inside = [&](float ax0, float ax1, float ay0, float ay1) {
        return inside_quad(ax0, ay0) && inside_quad(ax0, ay1)
            && inside_quad(ax1, ay0) && inside_quad(ax1, ay1);
    };
    if (!box_inside(x0, x1, y0, y1)) {
        const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
        if (!inside_quad(mx, my)) return false;
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 24; ++i) {
            const float k = (lo + hi) * 0.5f;
            if (box_inside(mx - hx * k, mx + hx * k,
                           my - hy * k, my + hy * k)) lo = k;
            else hi = k;
        }
        x0 = mx - hx * lo; x1 = mx + hx * lo;
        y0 = my - hy * lo; y1 = my + hy * lo;
    }
    return true;
}

} // namespace

bool compute_gaze_range(const XrView views[2], float dist,
                        const FovConfig& cfg, GazeRange& out) {
    float lx0, lx1, ly0, ly1, rx0, rx1, ry0, ry1;
    if (!eye_plane_box(views[0], dist, lx0, lx1, ly0, ly1) ||
        !eye_plane_box(views[1], dist, rx0, rx1, ry0, ry1))
        return false;

    // Binocular region — visible to both eyes.
    const float x0 = std::max(lx0, rx0), x1 = std::min(lx1, rx1);
    const float y0 = std::max(ly0, ry0), y1 = std::min(ly1, ry1);
    if (x1 <= x0 || y1 <= y0) return false;

    // Coverage fraction about the binocular centre, then clamp each edge
    // independently to the eccentricity cap about straight-ahead. The cap is an
    // oculomotor limit, so it applies per direction relative to the eyes'
    // neutral axis (elevation 0), not symmetrically about the display centre:
    // on headsets whose FOV extends much further down than up (canted displays
    // like the Beyond, most wide-FOV HMDs), a symmetric cap about the display
    // centre pushes one row of targets past what the eyes can reach.
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float cap    = std::tan(cfg.max_deg * kPi / 180.0f) * dist;
    const float cap_up = std::tan(cfg.max_deg_up * kPi / 180.0f) * dist;
    const float cap_dn = std::tan(cfg.max_deg_down * kPi / 180.0f) * dist;
    const float left  = std::max(cx + (x0 - cx) * cfg.coverage, -cap);
    const float right = std::min(cx + (x1 - cx) * cfg.coverage,  cap);
    const float bot   = std::max(cy + (y0 - cy) * cfg.coverage, -cap_dn);
    const float top   = std::min(cy + (y1 - cy) * cfg.coverage,  cap_up);
    if (right <= left || top <= bot) return false;

    // Normalised gaze coordinates are training/calibration labels, so a given
    // value must mean the same angular displacement in either direction.  Do
    // not use the midpoint of an asymmetric HMD frustum as zero: that makes
    // (for example) +0.8 and -0.8 represent different eye rotations and
    // silently mislabels cross-HMD data.  Anchor zero at straight ahead and use
    // the tighter binocular side as the symmetric reachable radius.
    out.center_x = 0.0f;
    out.rx = std::min(right, -left);

    // Keep the optional vertical anchor. Vertical reach is intentionally
    // asymmetric: most HMDs and human gaze have substantially more usable
    // range below straight-ahead than above it. Downstream code must interpret
    // the sign-specific angular scale instead of assuming +/-ny are equal.
    const float want_cy = std::tan(cfg.center_deg * kPi / 180.0f) * dist;
    out.center_y = std::clamp(want_cy, bot + 0.05f * (top - bot),
                                       top - 0.05f * (top - bot));
    out.ry_up = top - out.center_y;
    out.ry_down = out.center_y - bot;
    if (out.rx <= 0.0f || out.ry_up <= 0.0f || out.ry_down <= 0.0f) return false;
    return true;
}

void log_gaze_range(const GazeRange& g, const FovConfig& cfg, float dist) {
    auto deg = [&](float m) { return std::atan(m / dist) * 180.0f / kPi; };
    std::cout << "[INFO] Binocular FOV at " << dist << " m: centre=("
              << g.center_x << ", " << g.center_y << ") m ("
              << deg(g.center_y) << "° elev)  gaze range: ±"
              << g.rx << " m (±" << deg(g.rx) << "°) x +" << g.ry_up
              << "/-" << g.ry_down << " m (" << deg(g.center_y + g.ry_up)
              << "°.." << deg(g.center_y - g.ry_down) << "° elev)  coverage="
              << cfg.coverage << "  cap=" << cfg.max_deg << "° lat/"
              << cfg.max_deg_up << "° up/" << cfg.max_deg_down << "° down\n";
}
