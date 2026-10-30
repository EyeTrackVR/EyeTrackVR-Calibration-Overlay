#include "sim_app.h"
#include "config.h"
#include "sim_hmd.h"
#include "sprites.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <thread>

#include <stb_image_write.h>

namespace {

double wall_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Rgbf {
    float r, g, b;
};

// Bilinear sample of a straight-alpha image, returned premultiplied.
void sample(const Image& img, float u, float v, float& r, float& g, float& b, float& a) {
    const float x = u * img.w - 0.5f, y = v * img.h - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    r = g = b = a = 0.0f;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int xi = std::clamp(x0 + i, 0, img.w - 1), yi = std::clamp(y0 + j, 0, img.h - 1);
            const float wgt = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
            const uint8_t* p = &img.rgba[((size_t)yi * img.w + xi) * 4];
            const float pa = p[3] / 255.0f * wgt;
            r += p[0] * pa;
            g += p[1] * pa;
            b += p[2] * pa;
            a += pa;
        }
}

void composite(Rgbf& dst, const Quad& q, const Image* img, Vec3 o, Vec3 d) {
    if (!q.visible || !img || img->w <= 0 || q.alpha <= 0.0f) return;
    if (std::fabs(d.z) < 1e-6f) return;
    const float t = (q.z - o.z) / d.z;
    if (t <= 0.0f) return;
    // Hit point in the quad's own (unrotated) frame.
    const float dx = o.x + d.x * t - q.x, dy = o.y + d.y * t - q.y;
    const float cr = std::cos(q.rot), sr = std::sin(q.rot);
    const float lx = cr * dx + sr * dy, ly = -sr * dx + cr * dy;
    const float u = (lx + q.w * 0.5f) / q.w;
    const float v = (q.h * 0.5f - ly) / q.h;
    if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) return;
    float r, g, b, a;
    sample(*img, u, v, r, g, b, a);
    r *= q.alpha;
    g *= q.alpha;
    b *= q.alpha;
    a *= q.alpha;
    dst.r = r + dst.r * (1.0f - a);
    dst.g = g + dst.g * (1.0f - a);
    dst.b = b + dst.b * (1.0f - a);
}

}  // namespace

bool SimApp::init(const FovConfig& fov_cfg) {
    fov_cfg_ = fov_cfg;
    EyeGeom eyes[2];
    if (!sim_hmd(opt_.hmd, eyes)) {
        std::cout << "[ERROR] Unknown --sim-hmd=" << opt_.hmd << " (have:";
        for (const auto& n : sim_hmd_names()) std::cout << " " << n;
        std::cout << ")\n";
        return false;
    }
    std::string notes;
    if (!compute_view_geometry(eyes, cfg::OVERLAY_DIST, fov_cfg_, geom_, &notes)) {
        std::cout << "[ERROR] Simulated headset yields no gaze range: " << notes << "\n";
        return false;
    }
    for (int e = 0; e < 2; ++e) log_eye(e ? "R" : "L", geom_.eyes[e]);
    if (!notes.empty()) std::cout << "[INFO] Geometry: " << notes << "\n";
    log_view_geometry(geom_, fov_cfg_, cfg::OVERLAY_DIST);
    if (!opt_.dump_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opt_.dump_dir, ec);
    }
    std::cout << "[INFO] Simulator backend ready (headset: " << opt_.hmd
              << (opt_.fast ? ", fast clock" : ", real-time clock") << ")\n";
    last_wall_ = wall_s();
    return true;
}

void SimApp::shutdown() {}

bool SimApp::poll_events() { return !(opt_.max_seconds > 0.0 && t_ >= opt_.max_seconds); }

bool SimApp::frame(const std::function<Frame(double, const ViewGeometry&)>& update_cb) {
    constexpr double kStep = 1.0 / 90.0;
    double dt = kStep;
    if (!opt_.fast) {
        const double next = last_wall_ + kStep;
        const double now = wall_s();
        if (next > now) std::this_thread::sleep_for(std::chrono::duration<double>(next - now));
        const double w = wall_s();
        dt = std::clamp(w - last_wall_, 0.0, 0.1);
        last_wall_ = w;
    }
    if (frame_ == 0) dt = 0.0;
    t_ += dt;

    const Frame f = update_cb(dt, geom_);

    if (opt_.dump_every > 0 && !opt_.dump_dir.empty() && frame_ % opt_.dump_every == 0) {
        std::vector<uint8_t> rgb;
        int w = 0, h = 0;
        render(f, geom_, opt_.eye_px, opt_.debug, rgb, w, h);
        char name[64];
        std::snprintf(name, sizeof(name), "frame_%06ld_t%07.3f.png", frame_, t_);
        const std::string path = (std::filesystem::path(opt_.dump_dir) / name).string();
        if (!stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3))
            std::cout << "[WARN] Could not write " << path << "\n";
    }
    ++frame_;
    return true;
}

void SimApp::render(const Frame& f, const ViewGeometry& g, int eye_px, bool debug, std::vector<uint8_t>& rgb, int& w,
                    int& h) {
    // Common vertical scale so both eyes share a height.
    float span_y = 0.0f;
    for (const auto& e : g.eyes) span_y = std::max(span_y, e.tan_up - e.tan_down);
    float span_x = 0.0f;
    for (const auto& e : g.eyes) span_x = std::max(span_x, e.tan_right - e.tan_left);
    const int eh = (int)std::lround(eye_px * span_y / span_x);
    w = eye_px * 2 + 8;
    h = eh;
    rgb.assign((size_t)w * h * 3, 20);

    const GazeRange& gz = g.gaze;
    for (int e = 0; e < 2; ++e) {
        const EyeGeom& eye = g.eyes[e];
        const int ox = e * (eye_px + 8);
        for (int py = 0; py < eh; ++py) {
            for (int px = 0; px < eye_px; ++px) {
                const float tx = eye.tan_left + (px + 0.5f) / eye_px * (eye.tan_right - eye.tan_left);
                const float ty = eye.tan_up - (py + 0.5f) / eh * span_y;
                const Vec3 d = rotate(eye.pose.q, {tx, ty, -1.0f});
                const Vec3 o = eye.pose.p;

                // "Game" behind the backdrop: a dim blue grid.
                const float az = std::atan2(d.x, -d.z) * kRad2Deg,
                            el = std::atan2(d.y, std::hypot(d.x, d.z)) * kRad2Deg;
                const bool line =
                    std::fmod(std::fabs(az) + 0.4f, 10.0f) < 0.8f || std::fmod(std::fabs(el) + 0.4f, 10.0f) < 0.8f;
                Rgbf c = line ? Rgbf{70, 110, 160} : Rgbf{24, 40, 66};
                const float ba = std::clamp(f.backdrop_alpha, 0.0f, 1.0f);
                c.r += (f.backdrop_gray - c.r) * ba;
                c.g += (f.backdrop_gray - c.g) * ba;
                c.b += (f.backdrop_gray - c.b) * ba;

                composite(c, f.panel, f.panel_image, o, d);
                composite(c, f.ring, &sprites::ring(), o, d);
                composite(c, f.trail, &sprites::trail(), o, d);
                composite(c, f.dot, &sprites::dot(), o, d);

                if (debug && std::fabs(d.z) > 1e-6f) {
                    const float t = (-cfg::OVERLAY_DIST - o.z) / d.z;
                    const float x = o.x + d.x * t, y = o.y + d.y * t;
                    const float nx = (x - gz.center_x) / gz.rx;
                    const float ny = (y - gz.center_y) / (y >= gz.center_y ? gz.ry_up : gz.ry_down);
                    const float r = std::hypot(nx, ny);
                    if (std::fabs(r - 1.0f) < 0.006f || std::fabs(r - 0.88f) < 0.004f) c = {0, 200, 220};
                }
                // Outside this eye's lens-visible region: show it dimmed.
                if (!eye.contains_tangent(tx, ty)) {
                    c.r *= 0.25f;
                    c.g *= 0.25f;
                    c.b *= 0.25f;
                }

                uint8_t* p = &rgb[((size_t)py * w + ox + px) * 3];
                p[0] = (uint8_t)std::clamp(c.r, 0.0f, 255.0f);
                p[1] = (uint8_t)std::clamp(c.g, 0.0f, 255.0f);
                p[2] = (uint8_t)std::clamp(c.b, 0.0f, 255.0f);
            }
        }
    }
}
