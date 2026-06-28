#include "fov.h"

#include <algorithm>
#include <cmath>

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
    return x1 > x0 && y1 > y0;
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

    out.center_x = (x0 + x1) * 0.5f;
    out.center_y = (y0 + y1) * 0.5f;

    const float cap = std::tan(cfg.max_deg * kPi / 180.0f) * dist;
    out.rx = std::min((x1 - x0) * 0.5f * cfg.coverage, cap);
    out.ry = std::min((y1 - y0) * 0.5f * cfg.coverage, cap);
    return true;
}
