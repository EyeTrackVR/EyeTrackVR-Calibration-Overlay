#include "sim_hmd.h"

#include <cmath>

namespace {

struct Profile {
    const char* name;
    // Left eye, degrees (right eye is mirrored).
    float left, right, up, down;
    float cant_yaw;    // outward yaw of each display (deg)
    float cant_pitch;  // downward pitch (deg, negative = down)
    float lens_n;      // superellipse exponent of the lens outline (0 = no polygon)
    float ipd;         // metres
};

const Profile kProfiles[] = {
    {"beyond2", -47.0f, 43.0f, 38.9f, -50.5f, 0.0f, 0.0f, 2.4f, 0.064f},
    {"index", -54.0f, 45.0f, 55.0f, -55.0f, 0.0f, 0.0f, 2.2f, 0.064f},
    {"quest3", -52.0f, 43.0f, 45.0f, -52.0f, 0.0f, 0.0f, 2.8f, 0.064f},
    {"pimax", -65.0f, 48.0f, 52.0f, -58.0f, 10.0f, 0.0f, 2.4f, 0.064f},
    {"vive", -46.0f, 43.0f, 50.0f, -50.0f, 0.0f, 0.0f, 2.0f, 0.064f},
    {"canted", -50.0f, 44.0f, 42.0f, -48.0f, 6.2f, -5.3f, 2.4f, 0.064f},
    {"narrow", -40.0f, 37.0f, 36.0f, -40.0f, 0.0f, 0.0f, 2.0f, 0.064f},
    {"generic", -47.0f, 47.0f, 42.0f, -47.0f, 0.0f, 0.0f, 0.0f, 0.063f},
};

// Rounded-rectangle lens outline in tangent space, per-quadrant extents.
std::vector<Vec2> lens(float l, float r, float u, float d, float n) {
    std::vector<Vec2> poly;
    if (n <= 0.0f) return poly;
    constexpr int kN = 96;
    for (int i = 0; i < kN; ++i) {
        const float a = 2.0f * kPi * (float)i / kN;
        const float c = std::cos(a), s = std::sin(a);
        const float ex = 2.0f / n;
        const float x = std::copysign(std::pow(std::fabs(c), ex), c) * (c >= 0 ? r : -l);
        const float y = std::copysign(std::pow(std::fabs(s), ex), s) * (s >= 0 ? u : -d);
        poly.push_back({x * 0.995f, y * 0.995f});
    }
    return poly;
}

}  // namespace

std::vector<std::string> sim_hmd_names() {
    std::vector<std::string> out;
    for (const auto& p : kProfiles) out.push_back(p.name);
    return out;
}

bool sim_hmd(const std::string& name, EyeGeom out[2]) {
    for (const auto& p : kProfiles) {
        if (name != p.name) continue;
        for (int e = 0; e < 2; ++e) {
            const float sgn = e ? 1.0f : -1.0f;  // -1 left eye, +1 right eye
            EyeGeom& g = out[e];
            // Mirror horizontally for the right eye.
            const float l = e ? -p.right : p.left;
            const float r = e ? -p.left : p.right;
            g.tan_left = std::tan(l * kDeg2Rad);
            g.tan_right = std::tan(r * kDeg2Rad);
            g.tan_up = std::tan(p.up * kDeg2Rad);
            g.tan_down = std::tan(p.down * kDeg2Rad);
            g.pose.p = {sgn * p.ipd * 0.5f, 0.0f, 0.0f};
            // Outward yaw: left eye turns left (+yaw about Y), right eye right.
            const Quat yaw = quat_axis_angle({0, 1, 0}, -sgn * p.cant_yaw * kDeg2Rad);
            const Quat pitch = quat_axis_angle({1, 0, 0}, p.cant_pitch * kDeg2Rad);
            g.pose.q = yaw * pitch;
            g.visible = lens(g.tan_left, g.tan_right, g.tan_up, g.tan_down, p.lens_n);
        }
        return true;
    }
    return false;
}
