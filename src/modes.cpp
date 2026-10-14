#include "modes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

constexpr float  kPi = 3.14159265358979323846f;

// ── Shared capture constants (ported from the OpenVR build) ───────────────────
constexpr float  DC_DOT_LARGE = 0.63f;
constexpr float  DC_DOT_SMALL = 0.075f;
constexpr double DC_APPEAR_S  = 0.150;
constexpr double DC_SHRINK_S  = 0.500;
constexpr double DC_HOLD_S    = 0.500;
constexpr double DC_SQUINT_WAIT_S = 5.500;

// Capture grid in normalised gaze coordinates (±1 = edge of the gaze range).
// Pushed out (v6 data collection, 2026-07-04): cardinals 0.786→0.85, corners
// 0.607→0.60, so the fixed 9-point ring reaches the same eccentricity as the
// jittered grid instead of sitting more central. The trainer's ring labels are
// version-gated to match (dataset.py _RING_COORDS_V6). ±1 = edge of the gaze
// range, whose extent is set by the --coverage/--max-deg flags at launch.
const float DC_POS[9][2] = {
    {-0.60f,  0.60f}, { 0.0f,  0.85f}, { 0.60f,  0.60f},
    { 0.85f,  0.0f }, { 0.60f, -0.60f}, { 0.0f, -0.85f},
    {-0.60f, -0.60f}, {-0.85f, 0.0f }, { 0.0f,   0.0f  },
};

// Classic 3×3 grid (top row → bottom, right → left) at 57.1% of the range.
constexpr float GRID_FRAC = 0.571f;
const float GRID[9][2] = {
    { 1.0f,  1.0f}, { 0.0f,  1.0f}, {-1.0f,  1.0f},
    { 1.0f,  0.0f}, { 0.0f,  0.0f}, {-1.0f,  0.0f},
    { 1.0f, -1.0f}, { 0.0f, -1.0f}, {-1.0f, -1.0f},
};
const float CENTER_PT[1][2] = {{0.0f, 0.0f}};

// Fine-tune grid: five outer points, five inner points, then centre. The inner
// pentagon is rotated by 36 degrees (half the 72-degree point spacing), so the
// two rings jointly sample ten interleaved directions instead of duplicating
// the same five rays. This gives the fit substantially better mid-field spatial
// coverage while retaining a comfortable outer reach.
// Order must match NEXT_SMARTCAL_DOTS downstream.
// Normalised gaze coordinates (±1 = edge of the gaze range).
const float FINETUNE_POS[11][2] = {
    { 0.000000f,  0.800000f},   // 0 outer top
    { 0.760845f,  0.247214f},   // 1 outer upper-right
    { 0.470228f, -0.647214f},   // 2 outer lower-right
    {-0.470228f, -0.647214f},   // 3 outer lower-left
    {-0.760845f,  0.247214f},   // 4 outer upper-left
    { 0.246870f,  0.339787f},   // 5 inner upper-right
    { 0.399444f, -0.129787f},   // 6 inner lower-right
    { 0.000000f, -0.420000f},   // 7 inner bottom
    {-0.399444f, -0.129787f},   // 8 inner lower-left
    {-0.246870f,  0.339787f},   // 9 inner upper-left
    { 0.000000f,  0.000000f},   // 10 centre (last)
};

uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

struct Cmd {
    int32_t     code = 0;
    bool        has_xy = false;
    float       x = 0, y = 0;
    std::string str;
};

// Read one datagram (non-blocking). Returns false if none/too short.
bool poll_cmd(UdpNet& net, Cmd& out) {
    unsigned char buf[1024];
    int len = net.recv(buf, (int)sizeof(buf) - 1);
    if (len < 4) return false;
    out = Cmd{};
    out.code = (int32_t)be32(buf);
    if ((out.code == 111 || out.code == 113) && len >= 12) {
        uint32_t xi = be32(buf + 4), yi = be32(buf + 8);
        std::memcpy(&out.x, &xi, 4);
        std::memcpy(&out.y, &yi, 4);
        out.has_xy = true;
    } else if (out.code == 50 && len > 4) {
        out.str.assign((const char*)buf + 4, (size_t)len - 4);
    }
    return true;
}

// ── Reusable point sweep: per point appear → shrink → (signal) → hold ─────────
// Reproduces run_dc_phase(): emits base+index at the shrink→hold boundary.
struct PointSequence {
    const float (*pts)[2] = nullptr;
    int    count = 0;
    int    base  = 0;
    float  scale = 1.0f;
    float  large = DC_DOT_LARGE, small = DC_DOT_SMALL;
    double appear_s = DC_APPEAR_S, shrink_s = DC_SHRINK_S, hold_s = DC_HOLD_S;
    bool   signal_points = true;

    int    idx = 0, sub = 0;   // sub: 0 appear, 1 shrink, 2 hold
    double t = 0;
    bool   done = false;

    void start(const float (*p)[2], int n, int base_idx) {
        pts = p; count = n; base = base_idx;
        idx = 0; sub = 0; t = 0; done = false;
    }

    void update(double dt, UdpNet& net, const GazeRange& g, Scene& s) {
        if (done) return;
        t += dt;
        s.dot_visible = true;
        s.dot_x = g.x(pts[idx][0] * scale);
        s.dot_y = g.y(pts[idx][1] * scale);

        if (sub == 0) {                          // appear
            s.dot_size = large;
            if (t >= appear_s) { t = 0; sub = 1; }
        } else if (sub == 1) {                   // shrink
            float f = (float)std::min(1.0, t / shrink_s);
            s.dot_size = large + (small - large) * f;
            if (t >= shrink_s) {
                t = 0; sub = 2;
                if (signal_points) net.send_int32(base + idx);
            }
        } else {                                 // hold
            s.dot_size = small;
            if (t >= hold_s) {
                t = 0; sub = 0;
                if (++idx >= count) done = true;
            }
        }
    }
};

// ── classic / center ──────────────────────────────────────────────────────────
class ClassicMode : public IMode {
public:
    explicit ClassicMode(bool center_only) : center_(center_only) {}

    Scene update(double dt, UdpNet& net, const GazeRange& g) override {
        Scene s;
        if (!started_) {
            started_ = true;
            seq_.appear_s = 0.0;
            seq_.small    = 0.045f;
            if (center_) {
                seq_.large = 3.75f; seq_.shrink_s = 0.25; seq_.hold_s = 0.0;
                seq_.signal_points = false;
                seq_.start(CENTER_PT, 1, 0);
            } else {
                seq_.large = 1.5f; seq_.shrink_s = 0.10; seq_.hold_s = 0.05;
                seq_.scale = GRID_FRAC;
                seq_.start(GRID, 9, 0);
            }
        }
        if (!seq_.done) {
            seq_.update(dt, net, g, s);
            if (seq_.done) { net.send_int32(9); finished_ = true; }
        }
        return s;
    }
    bool finished() const override { return finished_; }

private:
    bool center_, started_ = false, finished_ = false;
    PointSequence seq_;
};

// ── ellipse ─────────────────────────────────────────────────────────────────
class EllipseMode : public IMode {
    enum Phase { INTRO, ORBIT, SPIRAL, CENTER, COMPLETE, DONE };
public:
    Scene update(double dt, UdpNet& net, const GazeRange& g) override {
        Scene s;
        s.text_visible = true;
        t_ += dt;
        switch (phase_) {
        case INTRO:
            s.text = "Follow the dot -- keep your gaze on it";
            s.dot_visible = true; s.dot_size = DOT_LARGE;
            s.dot_x = g.x(-1.0f); s.dot_y = g.y(0.0f);
            if (t_ >= 1.2) { net.send_int32(0); next(ORBIT); }
            break;
        case ORBIT: {
            s.text = "Follow the dot -- keep your gaze on it";
            double frac = std::min(1.0, t_ / ORBIT_S);
            // Parameter angle moves unevenly around a stretched/asymmetric
            // ellipse. Convert elapsed fraction to arc-length fraction so the
            // dot has constant visible speed on every HMD.
            double a = arc_angle(g, std::fmod(ORBIT_ROTS * frac, 1.0));
            float nx, ny;
            path_coords(g, a, nx, ny);
            place(s, g, nx, ny, DOT_LARGE);
            if (t_ >= ORBIT_S) next(SPIRAL);
            break;
        }
        case SPIRAL: {
            double frac = std::min(1.0, t_ / SPIRAL_S);
            double shrink = 1.0 - frac;
            // Each spiral revolution uses the same constant-arc-speed mapping.
            // Its overall radius still shrinks linearly as intended.
            double a = arc_angle(g, std::fmod(SPIRAL_ROTS * frac, 1.0));
            float nx, ny;
            path_coords(g, a, nx, ny);
            float sz = DOT_LARGE + (DOT_SMALL - DOT_LARGE) * (float)frac;
            place(s, g, (float)(shrink * nx), (float)(shrink * ny), sz);
            int secs = (int)(SPIRAL_S - t_) + 1;
            s.text = (secs <= 5) ? ("Almost done... " + std::to_string(secs) + "s")
                                 : std::string("Follow the dot -- keep your gaze on it");
            if (t_ >= SPIRAL_S) next(CENTER);
            break;
        }
        case CENTER:
            s.text = "Hold -- look at center";
            place(s, g, 0.0f, 0.0f, DOT_SMALL);
            if (t_ >= 1.5) { net.send_int32(9); next(COMPLETE); }
            break;
        case COMPLETE:
            s.text = "Calibration complete!";
            if (t_ >= 1.0) phase_ = DONE;
            break;
        case DONE:
            finished_ = true;
            break;
        }
        return s;
    }
    bool finished() const override { return finished_; }

private:
    static constexpr float  DOT_LARGE = 0.18f, DOT_SMALL = 0.06f;
    static constexpr double ORBIT_ROTS = 1.0, ORBIT_S = 9.0;
    static constexpr double SPIRAL_ROTS = 3.0, SPIRAL_S = 30.0;

    void next(Phase p) { phase_ = p; t_ = 0; }

    // The upper gaze extent can be much shorter than the horizontal extent
    // (e.g. 15 vs 28 degrees on the Beyond). A plain parametric ellipse then
    // has a visibly flat upper cap. Raising positive sin(a) to the radius-ratio
    // squared gives the top approximately circular curvature while preserving
    // the exact side and top endpoints. The lower half remains unchanged so it
    // can use the intentionally deeper downward range.
    static void path_coords(const GazeRange& g, double a, float& nx, float& ny) {
        nx = (float)std::cos(a);
        const double sy = std::sin(a);
        if (sy > 0.0) {
            const double ratio = g.ry_up > 0.0f ? (double)g.rx / g.ry_up : 1.0;
            const double exponent = std::clamp(ratio * ratio, 1.0, 4.0);
            ny = (float)std::pow(sy, exponent);
        } else {
            ny = (float)sy;
        }
    }

    // Map a 0..1 traversal fraction (starting at the leftmost point) to the
    // parameter angle whose polyline arc length has advanced by that fraction.
    // GazeRange::y is piecewise because upper/lower reach differs, so numerical
    // integration is clearer and less error-prone than ellipse approximations.
    static double arc_angle(const GazeRange& g, double frac) {
        constexpr int N = 256;
        frac = std::clamp(frac, 0.0, 1.0);
        double cumulative[N + 1]{};
        double prev_a = kPi;
        float pnx, pny;
        path_coords(g, prev_a, pnx, pny);
        double prev_x = g.x(pnx);
        double prev_y = g.y(pny);
        for (int i = 1; i <= N; ++i) {
            const double a = kPi + 2.0 * kPi * (double)i / N;
            float nx, ny;
            path_coords(g, a, nx, ny);
            const double x = g.x(nx);
            const double y = g.y(ny);
            cumulative[i] = cumulative[i - 1] + std::hypot(x - prev_x, y - prev_y);
            prev_x = x; prev_y = y;
        }
        const double target = frac * cumulative[N];
        int hi = 1;
        while (hi < N && cumulative[hi] < target) ++hi;
        const double seg = cumulative[hi] - cumulative[hi - 1];
        const double f = seg > 0.0 ? (target - cumulative[hi - 1]) / seg : 0.0;
        return kPi + 2.0 * kPi * ((double)(hi - 1) + f) / N;
    }

    static void place(Scene& s, const GazeRange& g, float nx, float ny, float sz) {
        s.dot_visible = true; s.dot_size = sz;
        s.dot_x = g.x(nx); s.dot_y = g.y(ny);
    }

    Phase  phase_ = INTRO;
    double t_ = 0;
    bool   finished_ = false;
};

// ── datacollect ───────────────────────────────────────────────────────────────
class DataCollectMode : public IMode {
    enum Phase { INTRO, PHASE1, ANNOUNCE, PHASE2, COMPLETE, DONE };
public:
    Scene update(double dt, UdpNet& net, const GazeRange& g) override {
        Scene s;
        s.text_visible = true;
        t_ += dt;
        switch (phase_) {
        case INTRO:
            s.text = "Follow the purple dot";
            if (t_ >= 1.0) { seq_.start(DC_POS, 9, 0); next(PHASE1); }
            break;
        case PHASE1:
            s.text = "Follow the purple dot";
            seq_.update(dt, net, g, s);
            if (seq_.done) { net.send_int32(9); next(ANNOUNCE); }
            break;
        case ANNOUNCE:
            s.text = "Squint while following the dot";
            if (t_ >= DC_SQUINT_WAIT_S) { seq_.start(DC_POS, 9, 10); next(PHASE2); }
            break;
        case PHASE2:
            s.text = "Squint while following the dot";
            seq_.update(dt, net, g, s);
            if (seq_.done) { net.send_int32(19); next(COMPLETE); }
            break;
        case COMPLETE:
            s.text = "Collection complete!";
            if (t_ >= 1.2) phase_ = DONE;
            break;
        case DONE:
            finished_ = true;
            break;
        }
        return s;
    }
    bool finished() const override { return finished_; }

private:
    void next(Phase p) { phase_ = p; t_ = 0; }
    Phase  phase_ = INTRO;
    double t_ = 0;
    bool   finished_ = false;
    PointSequence seq_;
};

// ── finetune (two rings around a circle, then centre) ────────────────────────
// Emits point index 0..10 as each dot is captured, then 100 = done. The done
// sentinel is 100 (not 9) because dot indices now reach 10 and would collide
// with the classic 9=done value; the app's finetune handler matches on 100.
class FineTuneMode : public IMode {
    enum Phase { INTRO, RUN, COMPLETE, DONE };
public:
    Scene update(double dt, UdpNet& net, const GazeRange& g) override {
        Scene s;
        s.text_visible = true;
        t_ += dt;
        switch (phase_) {
        case INTRO:
            s.text = "Follow the purple dot";
            if (t_ >= 1.0) { seq_.start(FINETUNE_POS, 11, 0); phase_ = RUN; t_ = 0; }
            break;
        case RUN:
            s.text = "Follow the purple dot";
            seq_.update(dt, net, g, s);   // signals point index 0..10 as each is captured
            if (seq_.done) { net.send_int32(100); phase_ = COMPLETE; t_ = 0; }
            break;
        case COMPLETE:
            s.text = "Fine-tune complete!";
            if (t_ >= 1.2) phase_ = DONE;
            break;
        case DONE:
            finished_ = true;
            break;
        }
        return s;
    }
    bool finished() const override { return finished_; }

private:
    Phase  phase_ = INTRO;
    double t_ = 0;
    bool   finished_ = false;
    PointSequence seq_;
};

// ── interactive (driven by the EyeTrackVR Python app over UDP) ────────────────
class InteractiveMode : public IMode {
    enum State { IDLE, PASS_TEXT, PASS_DC, RANDOM_WAIT, RANDOM_ANIM, FINISHED };
public:
    bool needs_recv() const override { return true; }
    void on_start(UdpNet& net) override { net.send_int32(255); }   // overlay ready

    Scene update(double dt, UdpNet& net, const GazeRange& g) override {
        Scene s;
        s.backdrop_gray = backdrop_gray_;
        s.text_visible = show_text_;
        s.text         = text_;

        switch (state_) {
        case IDLE:
            handle_idle(net, s);
            if (pin_dot_) {
                // Pinned dot (cmd 113): appear large and shrink once, then hold
                // indefinitely until re-pinned or hidden (cmd 114). Used by the
                // blink-at-dot data-collection pass, where the user must keep a
                // stable fixation target across several seconds of blinking.
                pin_t_ += dt;
                float f = (float)std::min(1.0, pin_t_ / DC_SHRINK_S);
                s.dot_visible = true;
                s.dot_size = DC_DOT_LARGE + (DC_DOT_SMALL - DC_DOT_LARGE) * f;
                s.dot_x = g.x(pin_x_); s.dot_y = g.y(pin_y_);
            } else if (center_dot_) {
                s.dot_visible = true; s.dot_size = DC_DOT_SMALL;
                s.dot_x = g.x(0.0f); s.dot_y = g.y(0.0f);
            }
            break;

        case PASS_TEXT:
            s.text_visible = true; s.text = text_;
            t_ += dt;
            if (t_ >= 0.4) {
                if (include_dc_) { seq_.start(DC_POS, 9, 0); state_ = PASS_DC; }
                else             { net.send_int32(10); state_ = RANDOM_WAIT; }
            }
            break;

        case PASS_DC:
            s.text_visible = true; s.text = text_;
            seq_.update(dt, net, g, s);
            if (seq_.done) { net.send_int32(10); state_ = RANDOM_WAIT; }
            break;

        case RANDOM_WAIT: {
            s.text_visible = true; s.text = text_;
            Cmd c;
            if (poll_cmd(net, c)) {
                if (c.code == 111 && c.has_xy) {
                    nx_ = std::clamp(c.x, -1.0f, 1.0f);
                    ny_ = std::clamp(c.y, -1.0f, 1.0f);
                    t_ = 0; rsub_ = 0;
                    state_ = RANDOM_ANIM;
                } else if (c.code == 119) {
                    net.send_int32(19);
                    end_pass();
                } else if (c.code == 200) {
                    finished_ = true; state_ = FINISHED;
                }
            }
            break;
        }

        case RANDOM_ANIM:
            s.text_visible = true; s.text = text_;
            anim_point(dt, net, g, s);
            break;

        case FINISHED:
            finished_ = true;
            break;
        }
        s.backdrop_gray = backdrop_gray_;
        return s;
    }
    bool finished() const override { return finished_; }

private:
    void handle_idle(UdpNet& net, Scene& s) {
        // Drain all pending commands; stop once a pass starts.
        Cmd c;
        while (state_ == IDLE && poll_cmd(net, c)) {
            if (c.code == 50) {
                text_ = c.str; show_text_ = true;
            } else if (c.code == 98) {
                show_text_ = false; text_.clear();
            } else if (c.code == 99) {
                center_dot_ = false;
            } else if (c.code >= 100 && c.code <= 106) {
                static const char* kPassText[] = {
                    "Follow the dot",
                    "Squint\nfollow the dot",
                    "Widen eyes\nfollow the dot",
                    "Raise eyebrows fully\nfollow the dot",
                    "Raise eyebrows halfway\nfollow the dot",
                    "Lower eyebrows fully\nfollow the dot",
                    "Lower eyebrows halfway\nfollow the dot",
                };
                text_ = kPassText[c.code - 100]; show_text_ = true;
                include_dc_ = true; center_dot_ = false;
                t_ = 0; state_ = PASS_TEXT;
            } else if (c.code == 110 || c.code == 112) {
                text_ = (c.code == 112) ? "Squint\nlook at each dot as it appears"
                                        : "Look at each dot\nas it appears";
                show_text_ = true; include_dc_ = false; center_dot_ = false;
                t_ = 0; state_ = PASS_TEXT;
            } else if (c.code == 113 && c.has_xy) {
                // Pin a persistent dot at (x, y); ack with 21 so the app knows
                // the dot is up (and can distinguish a new overlay from an old
                // exe that ignores this command).
                pin_x_ = std::clamp(c.x, -1.0f, 1.0f);
                pin_y_ = std::clamp(c.y, -1.0f, 1.0f);
                pin_t_ = 0; pin_dot_ = true;
                net.send_int32(21);
            } else if (c.code == 114) {
                pin_dot_ = false;
            } else if (c.code == 120) {
                text_ = "Look at center\nshift your headset around";
                show_text_ = true; center_dot_ = true;
            } else if (c.code == 121) {
                center_dot_ = false; show_text_ = false; text_.clear();
                net.send_int32(19);
            } else if (c.code == 130 || c.code == 131 || c.code == 132) {
                // Pupil-reference illumination: dim, bright, or neutral.
                backdrop_gray_ = c.code == 130 ? 24 : (c.code == 131 ? 208 : 96);
                net.send_int32(22);
            } else if (c.code == 200) {
                finished_ = true; state_ = FINISHED;
            }
        }
        s.text_visible = show_text_; s.text = text_;
    }

    // One jittered-position sweep: appear → shrink (signal 20) → hold → back to WAIT.
    void anim_point(double dt, UdpNet& net, const GazeRange& g, Scene& s) {
        t_ += dt;
        s.dot_visible = true;
        s.dot_x = g.x(nx_); s.dot_y = g.y(ny_);
        if (rsub_ == 0) {
            s.dot_size = DC_DOT_LARGE;
            if (t_ >= DC_APPEAR_S) { t_ = 0; rsub_ = 1; }
        } else if (rsub_ == 1) {
            float f = (float)std::min(1.0, t_ / DC_SHRINK_S);
            s.dot_size = DC_DOT_LARGE + (DC_DOT_SMALL - DC_DOT_LARGE) * f;
            if (t_ >= DC_SHRINK_S) { t_ = 0; rsub_ = 2; net.send_int32(20); }
        } else {
            s.dot_size = DC_DOT_SMALL;
            if (t_ >= DC_HOLD_S) { state_ = RANDOM_WAIT; }
        }
    }

    void end_pass() {
        state_ = IDLE; show_text_ = false; text_.clear(); center_dot_ = false;
    }

    State  state_ = IDLE;
    bool   finished_ = false;
    bool   show_text_ = false, center_dot_ = false, include_dc_ = false;
    bool   pin_dot_ = false;
    uint8_t backdrop_gray_ = 96;
    float  pin_x_ = 0, pin_y_ = 0;
    double pin_t_ = 0;
    std::string text_;
    double t_ = 0;
    int    rsub_ = 0;
    float  nx_ = 0, ny_ = 0;
    PointSequence seq_;
};

} // namespace

std::unique_ptr<IMode> make_mode(const std::string& name) {
    if (name == "center")       return std::make_unique<ClassicMode>(true);
    if (name == "ellipse")      return std::make_unique<EllipseMode>();
    if (name == "datacollect")  return std::make_unique<DataCollectMode>();
    if (name == "finetune")     return std::make_unique<FineTuneMode>();
    if (name == "interactive")  return std::make_unique<InteractiveMode>();
    return std::make_unique<ClassicMode>(false);   // "9point" default
}
