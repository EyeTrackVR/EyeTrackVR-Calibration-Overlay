#pragma once
#include <cmath>

// Minimal vector/quaternion types shared by the geometry, presenter and
// backends. Kept free of OpenXR/OpenVR headers so the core logic (and its unit
// tests) builds without any VR SDK.

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};  // xyz imaginary, w real
struct Pose {
    Quat q;
    Vec3 p;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

inline Quat conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

// Rotate v by the unit quaternion q.
inline Vec3 rotate(const Quat& q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

inline Quat quat_axis_angle(Vec3 axis, float rad) {
    const float n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (n <= 0.0f) return {};
    const float s = std::sin(rad * 0.5f) / n;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(rad * 0.5f)};
}

inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDeg2Rad = kPi / 180.0f;
inline constexpr float kRad2Deg = 180.0f / kPi;
