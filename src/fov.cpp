#include "fov.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <utility>

namespace {

// Intersect one eye's view frustum with the plane z = -dist (head space) and
// return the largest axis-aligned box inside the projected quad.
bool eye_plane_box(const EyeGeom& eye, float dist, float& x0, float& x1, float& y0, float& y1) {
    // Frustum corners on the z = -1 plane in eye space: TL, TR, BR, BL.
    const float cx[4] = {eye.tan_left, eye.tan_right, eye.tan_right, eye.tan_left};
    const float cy[4] = {eye.tan_up, eye.tan_up, eye.tan_down, eye.tan_down};

    const Vec3 o = eye.pose.p;
    float px[4], py[4];
    for (int i = 0; i < 4; ++i) {
        const Vec3 d = rotate(eye.pose.q, {cx[i], cy[i], -1.0f});
        if (d.z >= 0.0f) return false;
        const float s = (-dist - o.z) / d.z;
        px[i] = o.x + s * d.x;
        py[i] = o.y + s * d.y;
    }
    x0 = std::max(px[0], px[3]);  // tightest left edge
    x1 = std::min(px[1], px[2]);  // tightest right edge
    y0 = std::max(py[2], py[3]);  // tightest bottom edge
    y1 = std::min(py[0], py[1]);  // tightest top edge
    if (!(x1 > x0) || !(y1 > y0)) return false;

    // With a rotated/canted eye the projected frustum is a slanted quad.
    // Taking independent tightest X/Y edges does not guarantee that the four
    // combined rectangle corners lie inside that quad (cardinal targets remain
    // visible while diagonals clip). Axis-aligned eyes pass unchanged; canted
    // ones are shrunk only as much as needed to make the box truly inscribed.
    auto inside_quad = [&](float x, float y) {
        bool pos = false, neg = false;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) & 3;
            const float c = (px[j] - px[i]) * (y - py[i]) - (py[j] - py[i]) * (x - px[i]);
            pos |= c > 1e-5f;
            neg |= c < -1e-5f;
        }
        return !(pos && neg);
    };
    auto box_inside = [&](float ax0, float ax1, float ay0, float ay1) {
        return inside_quad(ax0, ay0) && inside_quad(ax0, ay1) && inside_quad(ax1, ay0) && inside_quad(ax1, ay1);
    };
    if (!box_inside(x0, x1, y0, y1)) {
        const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
        if (!inside_quad(mx, my)) return false;
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 24; ++i) {
            const float k = (lo + hi) * 0.5f;
            if (box_inside(mx - hx * k, mx + hx * k, my - hy * k, my + hy * k))
                lo = k;
            else
                hi = k;
        }
        x0 = mx - hx * lo;
        x1 = mx + hx * lo;
        y0 = my - hy * lo;
        y1 = my + hy * lo;
    }
    return true;
}

bool finite(float v) { return std::isfinite(v); }

bool eye_finite(const EyeGeom& e) {
    return finite(e.tan_left) && finite(e.tan_right) && finite(e.tan_up) && finite(e.tan_down) && finite(e.pose.p.x) &&
           finite(e.pose.p.y) && finite(e.pose.p.z) && finite(e.pose.q.x) && finite(e.pose.q.y) && finite(e.pose.q.z) &&
           finite(e.pose.q.w) && e.tan_left < e.tan_right && e.tan_down < e.tan_up;
}

struct Attempt {
    GazeRange g;
    float ellipse_scale = 1.0f;
    float cardinal_trim = 1.0f;  // smallest cardinal extent ratio after visibility checks
};

// One pass of the placement policy over the given eyes.
bool attempt(ViewGeometry& vg, float dist, const FovConfig& cfg, Attempt& out) {
    float lx0, lx1, ly0, ly1, rx0, rx1, ry0, ry1;
    if (!eye_plane_box(vg.eyes[0], dist, lx0, lx1, ly0, ly1) || !eye_plane_box(vg.eyes[1], dist, rx0, rx1, ry0, ry1))
        return false;

    // Binocular region — visible to both eyes.
    const float x0 = std::max(lx0, rx0), x1 = std::min(lx1, rx1);
    const float y0 = std::max(ly0, ry0), y1 = std::min(ly1, ry1);
    if (!(x1 > x0) || !(y1 > y0)) return false;

    // Coverage fraction about the binocular centre, then clamp each edge
    // independently to the eccentricity cap about straight-ahead. The cap is an
    // oculomotor limit, so it applies per direction relative to the eyes'
    // neutral axis (elevation 0), not symmetrically about the display centre:
    // on headsets whose FOV extends much further down than up (canted displays
    // like the Beyond, most wide-FOV HMDs), a symmetric cap about the display
    // centre pushes one row of targets past what the eyes can reach.
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float cap = std::tan(cfg.max_deg * kDeg2Rad) * dist;
    const float cap_up = std::tan(cfg.max_deg_up * kDeg2Rad) * dist;
    const float cap_dn = std::tan(cfg.max_deg_down * kDeg2Rad) * dist;
    const float left = std::max(cx + (x0 - cx) * cfg.coverage, -cap);
    const float right = std::min(cx + (x1 - cx) * cfg.coverage, cap);
    const float bot = std::max(cy + (y0 - cy) * cfg.coverage, -cap_dn);
    const float top = std::min(cy + (y1 - cy) * cfg.coverage, cap_up);
    if (!(right > left) || !(top > bot)) return false;

    // Normalised gaze coordinates are training/calibration labels, so a given
    // value must mean the same angular displacement in either direction. Do
    // not use the midpoint of an asymmetric HMD frustum as zero: that makes
    // (for example) +0.8 and -0.8 represent different eye rotations and
    // silently mislabels cross-HMD data. Anchor zero at straight ahead and use
    // the tighter binocular side as the symmetric reachable radius.
    GazeRange g;
    g.center_x = 0.0f;
    g.rx = std::min(right, -left);

    // Keep the optional vertical anchor. Vertical reach is intentionally
    // asymmetric: most HMDs and human gaze have substantially more usable
    // range below straight-ahead than above it. Downstream code must interpret
    // the sign-specific angular scale instead of assuming +/-ny are equal.
    const float want_cy = std::tan(cfg.center_deg * kDeg2Rad) * dist;
    g.center_y = std::clamp(want_cy, bot + 0.05f * (top - bot), top - 0.05f * (top - bot));
    g.ry_up = top - g.center_y;
    g.ry_down = g.center_y - bot;
    if (!(g.rx > 0.0f) || !(g.ry_up > 0.0f) || !(g.ry_down > 0.0f)) return false;

    // ── Visibility verification ──────────────────────────────────────────────
    // The boxes above come from rectangular frusta. Real lenses show a rounded
    // region (and some runtimes report a render frustum larger than what is
    // visible), so check the targets themselves: every point of the normalised
    // unit ellipse — which bounds every mode's target set — must be visible to
    // both eyes with a dot-sized margin. Shrink only what fails: cardinal axes
    // independently first (keeps unaffected directions' label scale intact),
    // then a uniform factor for whatever off-axis clipping remains.
    // Slightly over-sized margin: the boundary is sampled, not solved exactly.
    const float m = kTargetVisibleMargin * 1.05f;
    if (!vg.disc_visible(g.center_x, g.center_y, m)) return false;

    auto reach = [&](float dx, float dy, float len) {
        auto ok = [&](float t) { return vg.disc_visible(g.center_x + dx * t, g.center_y + dy * t, m); };
        if (ok(len)) return len;
        float lo = 0.0f, hi = len;
        for (int i = 0; i < 22; ++i) {
            const float mid = 0.5f * (lo + hi);
            if (ok(mid))
                lo = mid;
            else
                hi = mid;
        }
        return lo;
    };
    const float rx_pre = g.rx, ru_pre = g.ry_up, rd_pre = g.ry_down;
    g.rx = std::min({g.rx, reach(1.0f, 0.0f, g.rx), reach(-1.0f, 0.0f, g.rx)});
    g.ry_up = reach(0.0f, 1.0f, g.ry_up);
    g.ry_down = reach(0.0f, -1.0f, g.ry_down);
    out.cardinal_trim = std::min({g.rx / rx_pre, g.ry_up / ru_pre, g.ry_down / rd_pre});

    float s_min = 1.0f;
    constexpr int kSamples = 256;
    for (int i = 0; i < kSamples; ++i) {
        const float a = 2.0f * kPi * (float)i / kSamples;
        const float nx = std::cos(a), ny = std::sin(a);
        const float px = g.rx * nx;
        const float py = ny * (ny >= 0.0f ? g.ry_up : g.ry_down);
        auto ok = [&](float s) { return vg.disc_visible(g.center_x + px * s, g.center_y + py * s, m); };
        if (ok(s_min)) continue;
        float lo = 0.0f, hi = s_min;
        for (int k = 0; k < 20; ++k) {
            const float mid = 0.5f * (lo + hi);
            if (ok(mid))
                lo = mid;
            else
                hi = mid;
        }
        s_min = lo;
    }
    g.rx *= s_min;
    g.ry_up *= s_min;
    g.ry_down *= s_min;
    out.ellipse_scale = s_min;
    if (!(g.rx > 0.02f) || !(g.ry_up > 0.02f) || !(g.ry_down > 0.02f)) return false;

    g.vis_left = x0;
    g.vis_right = x1;
    g.vis_bottom = y0;
    g.vis_top = y1;
    g.valid = true;
    out.g = g;
    return true;
}

}  // namespace

// ── Polygon helpers ───────────────────────────────────────────────────────────

bool point_in_polygon(const std::vector<Vec2>& poly, float x, float y) {
    bool in = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[j];
        if ((a.y > y) != (b.y > y)) {
            const float xi = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x < xi) in = !in;
        }
    }
    return in;
}

float polygon_area(const std::vector<Vec2>& poly) {
    double a = 0.0;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) a += (double)poly[j].x * poly[i].y - (double)poly[i].x * poly[j].y;
    return (float)std::fabs(a * 0.5);
}

bool sanitize_polygon(EyeGeom& eye, std::string* why) {
    auto reject = [&](const char* reason) {
        if (why) *why = reason;
        eye.visible.clear();
        return false;
    };
    if (eye.visible.empty()) return reject("none provided");
    if (eye.visible.size() < 3) return reject("fewer than 3 vertices");
    for (const Vec2& v : eye.visible)
        if (!finite(v.x) || !finite(v.y)) return reject("non-finite vertex");
    const float rect = (eye.tan_right - eye.tan_left) * (eye.tan_up - eye.tan_down);
    const float area = polygon_area(eye.visible);
    if (!(rect > 0.0f) || area < 0.15f * rect) return reject("area implausibly small");
    if (area > 4.0f * rect) return reject("area implausibly large");
    if (!point_in_polygon(eye.visible, 0.0f, 0.0f)) return reject("does not contain the eye axis");
    return true;
}

// ── EyeGeom / ViewGeometry ────────────────────────────────────────────────────

bool EyeGeom::contains_tangent(float tx, float ty) const {
    if (tx < tan_left || tx > tan_right || ty < tan_down || ty > tan_up) return false;
    return visible.empty() || point_in_polygon(visible, tx, ty);
}

bool EyeGeom::sees(Vec3 p) const {
    const Vec3 d = rotate(conj(pose.q), p - pose.p);
    if (d.z > -1e-4f) return false;
    return contains_tangent(d.x / -d.z, d.y / -d.z);
}

bool ViewGeometry::visible(float x, float y, float z) const {
    return eyes[0].sees({x, y, z}) && eyes[1].sees({x, y, z});
}

bool ViewGeometry::visible(float x, float y) const { return visible(x, y, -cfg::OVERLAY_DIST); }

bool ViewGeometry::disc_visible(float x, float y, float r) const {
    if (!visible(x, y)) return false;
    for (int i = 0; i < 8; ++i) {
        const float a = (float)i * (kPi / 4.0f);
        if (!visible(x + r * std::cos(a), y + r * std::sin(a))) return false;
    }
    return true;
}

bool ViewGeometry::rect_visible(float x0, float y0, float x1, float y1, float z) const {
    constexpr int kSteps = 6;
    for (int i = 0; i <= kSteps; ++i) {
        const float t = (float)i / kSteps;
        const float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
        if (!visible(x, y0, z) || !visible(x, y1, z) || !visible(x0, y, z) || !visible(x1, y, z)) return false;
    }
    return true;
}

// ── Policy ────────────────────────────────────────────────────────────────────

bool compute_view_geometry(const EyeGeom eyes[2], float dist, const FovConfig& cfg, ViewGeometry& out,
                           std::string* notes) {
    std::ostringstream log;
    ViewGeometry vg;
    vg.synthetic = false;
    for (int e = 0; e < 2; ++e) {
        vg.eyes[e] = eyes[e];
        if (!eye_finite(vg.eyes[e])) {
            log << "eye " << e << " frustum invalid; ";
            if (notes) *notes = log.str();
            return false;
        }
        if (!vg.eyes[e].visible.empty()) {
            std::string why;
            if (!sanitize_polygon(vg.eyes[e], &why))
                log << (e ? "R" : "L") << " lens polygon ignored (" << why << "); ";
        }
    }
    const bool had_polys = !vg.eyes[0].visible.empty() || !vg.eyes[1].visible.empty();

    Attempt a;
    bool ok = attempt(vg, dist, cfg, a);
    // A polygon that collapses the range is far more likely a runtime quirk
    // (wrong space/orientation) than real optics: retry on the frusta alone.
    if (had_polys && (!ok || a.ellipse_scale < 0.6f || a.cardinal_trim < 0.6f)) {
        log << "lens polygons rejected (they would shrink the range to "
            << (ok ? std::min(a.ellipse_scale, a.cardinal_trim) : 0.0f) << "x); ";
        for (auto& e : vg.eyes) e.visible.clear();
        ok = attempt(vg, dist, cfg, a);
    }
    if (!ok) {
        log << "frusta do not yield a usable binocular range; ";
        if (notes) *notes = log.str();
        return false;
    }
    vg.gaze = a.g;
    vg.used_polygons = !vg.eyes[0].visible.empty() || !vg.eyes[1].visible.empty();
    vg.ellipse_scale = a.ellipse_scale;
    if (a.cardinal_trim < 0.999f) log << "cardinal extents trimmed to stay visible (min " << a.cardinal_trim << "x); ";
    if (a.ellipse_scale < 0.999f) log << "range scaled " << a.ellipse_scale << "x so diagonal targets stay visible; ";
    if (notes) *notes = log.str();
    out = std::move(vg);
    return true;
}

ViewGeometry fallback_view_geometry(const FovConfig& cfg, float dist) {
    EyeGeom eyes[2];
    for (int e = 0; e < 2; ++e) {
        eyes[e].pose.p = {e ? 0.0315f : -0.0315f, 0.0f, 0.0f};
        eyes[e].tan_left = -std::tan(47.0f * kDeg2Rad);
        eyes[e].tan_right = std::tan(47.0f * kDeg2Rad);
        eyes[e].tan_up = std::tan(42.0f * kDeg2Rad);
        eyes[e].tan_down = -std::tan(47.0f * kDeg2Rad);
    }
    ViewGeometry vg;
    if (!compute_view_geometry(eyes, dist, cfg, vg)) {
        // Only reachable with absurd flags; keep the historical constants.
        vg = ViewGeometry{};
        for (int e = 0; e < 2; ++e) vg.eyes[e] = eyes[e];
    }
    vg.synthetic = true;
    vg.gaze.valid = false;
    return vg;
}

void log_eye(const char* tag, const EyeGeom& e) {
    const Vec3 fwd = rotate(e.pose.q, {0.0f, 0.0f, -1.0f});
    std::cout << "[INFO] Eye " << tag << ": fov L/R " << std::atan(e.tan_left) * kRad2Deg << "/"
              << std::atan(e.tan_right) * kRad2Deg << "° up/down " << std::atan(e.tan_up) * kRad2Deg << "/"
              << std::atan(e.tan_down) * kRad2Deg << "°, cant pitch/yaw "
              << std::asin(std::clamp(fwd.y, -1.0f, 1.0f)) * kRad2Deg << "/" << std::atan2(-fwd.x, -fwd.z) * kRad2Deg
              << "°, pos (" << e.pose.p.x << ", " << e.pose.p.y << ", " << e.pose.p.z << "), lens polygon "
              << (e.visible.empty() ? std::string("none") : std::to_string(e.visible.size()) + " verts") << "\n";
}

void log_view_geometry(const ViewGeometry& vg, const FovConfig& cfg, float dist) {
    const GazeRange& g = vg.gaze;
    auto deg = [&](float m) { return std::atan(m / dist) * kRad2Deg; };
    std::cout << "[INFO] Binocular FOV at " << dist << " m: visible box x " << deg(g.vis_left) << "°.."
              << deg(g.vis_right) << "°, y " << deg(g.vis_bottom) << "°.." << deg(g.vis_top) << "°\n";
    std::cout << "[INFO] Gaze range" << (vg.synthetic ? " (FALLBACK - no headset geometry)" : "") << ": centre=("
              << g.center_x << ", " << g.center_y << ") m (" << deg(g.center_y) << "° elev)  ±" << g.rx << " m (±"
              << deg(g.rx) << "°) x +" << g.ry_up << "/-" << g.ry_down << " m (" << deg(g.center_y + g.ry_up) << "°.."
              << deg(g.center_y - g.ry_down) << "° elev)  coverage=" << cfg.coverage << "  cap=" << cfg.max_deg
              << "° lat/" << cfg.max_deg_up << "° up/" << cfg.max_deg_down
              << "° down  lens-polygons=" << (vg.used_polygons ? "yes" : "no") << "  ellipse-scale=" << vg.ellipse_scale
              << "\n";
}
