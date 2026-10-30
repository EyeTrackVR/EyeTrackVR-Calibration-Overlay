#include "modes.h"
#include "i18n.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>

// ── UDP protocol ──────────────────────────────────────────────────────────────
//
// Big-endian int32 signals OUT to 127.0.0.1:2112, commands IN on 2113
// (interactive mode only). Byte-compatible with the original OpenVR build.
//
// Signals (overlay → app)
//   0..8      9point / datacollect phase 1 / interactive pass: point i captured
//   9         9point/center/ellipse done; datacollect phase 1 done
//   10..18    datacollect phase 2 point i-10;  10 also = interactive "jitter phase"
//   19        datacollect done; interactive pass ended (after 119 / 121)
//   20        interactive jittered point captured (after 111)
//   21        interactive pin ack (after 113)
//   22        interactive backdrop-level ack (after 130/131/132)
//   0 (ellipse) start sampling;  0..10 then 100 finetune points / done
//   255       interactive overlay ready
//
// Commands (app → overlay), interactive mode
//   50 <utf8>   show text                98   hide text
//   99          hide centre dot          100..106  start pass (9-point sweep, then jitter)
//   110 / 112   start jitter-only pass (normal / squint)
//   111 <f32 x> <f32 y>  animate one jittered point (normalised), signal 20
//   113 <f32 x> <f32 y>  pin a persistent dot, ack 21     114  unpin
//   119         end pass, signal 19       120  centre dot + text   121  end it, signal 19
//   130/131/132 dim / bright / neutral backdrop, ack 22
//   200         quit

namespace {

// ── Shared capture constants (ported from the OpenVR build) ───────────────────
// Per point: appear (dot glides in; ring appears at the target) → shrink (ring
// converges; signal at its end) → hold (capture window). Timings are protocol:
// the app starts sampling on the signal and relies on the 0.5 s hold after it.
constexpr float DC_DOT_LARGE = 0.63f;   // attention ring start diameter (m)
constexpr float DC_DOT_SMALL = 0.075f;  // fixation dot diameter (m)
constexpr double DC_APPEAR_S = 0.150;
constexpr double DC_SHRINK_S = 0.500;
constexpr double DC_HOLD_S = 0.500;
constexpr double DC_SQUINT_WAIT_S = 5.500;
constexpr double INTRO_S = 1.500;  // instruction card before the first dot

// Capture grid in normalised gaze coordinates (±1 = edge of the gaze range).
// Pushed out (v6 data collection, 2026-07-04): cardinals 0.786→0.85, corners
// 0.607→0.60, so the fixed 9-point ring reaches the same eccentricity as the
// jittered grid instead of sitting more central. The trainer's ring labels are
// version-gated to match (dataset.py _RING_COORDS_V6). ±1 = edge of the gaze
// range, whose extent is set by the --coverage/--max-deg flags at launch.
const float DC_POS[9][2] = {
    {-0.60f, 0.60f}, {0.0f, 0.85f},    {0.60f, 0.60f}, {0.85f, 0.0f}, {0.60f, -0.60f},
    {0.0f, -0.85f},  {-0.60f, -0.60f}, {-0.85f, 0.0f}, {0.0f, 0.0f},
};

// Classic 3×3 grid (top row → bottom, right → left) at 57.1% of the range.
constexpr float GRID_FRAC = 0.571f;
const float GRID[9][2] = {
    {1.0f, 1.0f},  {0.0f, 1.0f},  {-1.0f, 1.0f}, {1.0f, 0.0f},   {0.0f, 0.0f},
    {-1.0f, 0.0f}, {1.0f, -1.0f}, {0.0f, -1.0f}, {-1.0f, -1.0f},
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
    {0.000000f, 0.800000f},    // 0 outer top
    {0.760845f, 0.247214f},    // 1 outer upper-right
    {0.470228f, -0.647214f},   // 2 outer lower-right
    {-0.470228f, -0.647214f},  // 3 outer lower-left
    {-0.760845f, 0.247214f},   // 4 outer upper-left
    {0.246870f, 0.339787f},    // 5 inner upper-right
    {0.399444f, -0.129787f},   // 6 inner lower-right
    {0.000000f, -0.420000f},   // 7 inner bottom
    {-0.399444f, -0.129787f},  // 8 inner lower-left
    {-0.246870f, 0.339787f},   // 9 inner upper-left
    {0.000000f, 0.000000f},    // 10 centre (last)
};

using i18n::Str;
using i18n::tr;
using i18n::tr2;

float ease_in_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}
// Ring convergence: lingers large long enough to be noticed, then closes in and
// settles onto the dot right as the capture signal fires.
float converge(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

struct Cmd {
    int32_t code = 0;
    bool has_xy = false;
    float x = 0, y = 0;
    std::string str;
};

// Read one datagram (non-blocking). Returns false if none/too short.
bool poll_cmd(UdpNet& net, Cmd& out) {
    unsigned char buf[1024];
    for (;;) {
        const int len = net.recv(buf, (int)sizeof(buf));
        if (len <= 0) return false;
        if (len < 4) {
            std::cout << "[WARN] Ignoring short command datagram (" << len << " bytes)\n";
            continue;
        }
        out = Cmd{};
        out.code = (int32_t)be32(buf);
        if ((out.code == 111 || out.code == 113) && len >= 12) {
            const uint32_t xi = be32(buf + 4), yi = be32(buf + 8);
            std::memcpy(&out.x, &xi, 4);
            std::memcpy(&out.y, &yi, 4);
            out.has_xy = std::isfinite(out.x) && std::isfinite(out.y);
        } else if (out.code == 50 && len > 4) {
            out.str.assign((const char*)buf + 4, (size_t)len - 4);
        }
        return true;
    }
}

// ── Reusable point sweep: per point appear → shrink → (signal) → hold ─────────
// Reproduces run_dc_phase(): emits base+index at the shrink→hold boundary.
struct PointSequence {
    const float (*pts)[2] = nullptr;
    int count = 0;
    int base = 0;
    float scale = 1.0f;
    float large = DC_DOT_LARGE, small = DC_DOT_SMALL;
    double appear_s = DC_APPEAR_S, shrink_s = DC_SHRINK_S, hold_s = DC_HOLD_S;
    bool signal_points = true;

    int idx = 0, sub = 0;  // sub: 0 appear, 1 shrink, 2 hold
    double t = 0;
    bool done = false;

    // Where the dot is currently shown (metres): the next point glides from
    // here during its appear phase instead of popping in.
    bool has_from = false;
    float from_x = 0.0f, from_y = 0.0f;

    // glide_from_current: continue from the dot already on screen.
    void start(const float (*p)[2], int n, int base_idx, bool glide_from_current = false) {
        pts = p;
        count = n;
        base = base_idx;
        idx = 0;
        sub = 0;
        t = 0;
        done = false;
        if (!glide_from_current) has_from = false;
    }

    void update(double dt, UdpNet& net, const GazeRange& g, Scene& s) {
        if (done) return;
        t += dt;
        const float tx = g.x(pts[idx][0] * scale);
        const float ty = g.y(pts[idx][1] * scale);
        float x = tx, y = ty;
        s.dot_visible = true;
        s.dot_size = small;
        s.ring_x = tx;
        s.ring_y = ty;
        s.progress_total = count;
        s.progress_done = idx;

        if (sub == 0) {  // appear
            s.ring_size = large;
            if (has_from && appear_s > 0.0) {
                const float f = ease_in_out((float)(t / appear_s));
                x = from_x + (tx - from_x) * f;
                y = from_y + (ty - from_y) * f;
            } else {
                s.dot_cut = true;
            }
            if (t >= appear_s) {
                t = 0;
                sub = 1;
            }
        } else if (sub == 1) {  // shrink
            const float f = (float)std::min(1.0, t / shrink_s);
            s.ring_size = large + (small - large) * converge(f);
            if (t >= shrink_s) {
                t = 0;
                sub = 2;
                if (signal_points) net.send_int32(base + idx);
            }
        } else {  // hold
            s.ring_size = 0.0f;
            s.capture_hold = true;
            if (t >= hold_s) {
                t = 0;
                sub = 0;
                from_x = tx;
                from_y = ty;
                has_from = true;
                if (++idx >= count) done = true;
            }
        }
        s.dot_x = x;
        s.dot_y = y;
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
            seq_.small = 0.045f;
            if (center_) {
                seq_.large = 3.75f;
                seq_.shrink_s = 0.25;
                seq_.hold_s = 0.0;
                seq_.signal_points = false;
                seq_.start(CENTER_PT, 1, 0);
            } else {
                seq_.large = 1.5f;
                seq_.shrink_s = 0.10;
                seq_.hold_s = 0.05;
                seq_.scale = GRID_FRAC;
                seq_.start(GRID, 9, 0);
            }
        }
        if (!seq_.done) {
            seq_.update(dt, net, g, s);
            if (seq_.done) {
                net.send_int32(9);
                finished_ = true;
            }
        }
        s.progress_total = 0;  // no instruction card in the quick modes
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
        total_ += dt;
        s.progress_frac = (float)std::min(1.0, total_ / kTotalS);
        switch (phase_) {
            case INTRO:
                s.text = tr2(Str::FollowTitle, Str::FollowSub);
                s.dot_visible = true;
                s.dot_size = DOT_LARGE;
                s.dot_x = g.x(-1.0f);
                s.dot_y = g.y(0.0f);
                if (t_ >= 1.2) {
                    net.send_int32(0);
                    next(ORBIT);
                }
                break;
            case ORBIT: {
                s.text = tr2(Str::FollowTitle, Str::FollowSub);
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
                s.text = (SPIRAL_S - t_ <= 5.0) ? tr2(Str::AlmostDone, Str::KeepFollowing)
                                                : tr2(Str::FollowTitle, Str::FollowSub);
                if (t_ >= SPIRAL_S) next(CENTER);
                break;
            }
            case CENTER:
                s.text = tr2(Str::HoldTitle, Str::HoldSub);
                place(s, g, 0.0f, 0.0f, DOT_SMALL);
                s.capture_hold = true;
                if (t_ >= 1.5) {
                    net.send_int32(9);
                    next(COMPLETE);
                }
                break;
            case COMPLETE:
                s.text = tr(Str::CalibrationComplete);
                s.tone = Tone::Success;
                s.progress_frac = 1.0f;
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
    static constexpr float DOT_LARGE = 0.18f, DOT_SMALL = 0.06f;
    static constexpr double ORBIT_ROTS = 1.0, ORBIT_S = 9.0;
    static constexpr double SPIRAL_ROTS = 3.0, SPIRAL_S = 30.0;
    static constexpr double kTotalS = 1.2 + ORBIT_S + SPIRAL_S + 1.5;

    void next(Phase p) {
        phase_ = p;
        t_ = 0;
    }

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
            prev_x = x;
            prev_y = y;
        }
        const double target = frac * cumulative[N];
        int hi = 1;
        while (hi < N && cumulative[hi] < target) ++hi;
        const double seg = cumulative[hi] - cumulative[hi - 1];
        const double f = seg > 0.0 ? (target - cumulative[hi - 1]) / seg : 0.0;
        return kPi + 2.0 * kPi * ((double)(hi - 1) + f) / N;
    }

    static void place(Scene& s, const GazeRange& g, float nx, float ny, float sz) {
        s.dot_visible = true;
        s.dot_size = sz;
        s.dot_x = g.x(nx);
        s.dot_y = g.y(ny);
    }

    Phase phase_ = INTRO;
    double t_ = 0, total_ = 0;
    bool finished_ = false;
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
                s.text = tr2(Str::FollowTitle, Str::FollowSub);
                s.progress_total = 9;
                if (t_ >= INTRO_S) {
                    seq_.start(DC_POS, 9, 0);
                    next(PHASE1);
                }
                break;
            case PHASE1:
                s.text = tr2(Str::FollowTitle, Str::FollowSub);
                seq_.update(dt, net, g, s);
                if (seq_.done) {
                    net.send_int32(9);
                    next(ANNOUNCE);
                }
                break;
            case ANNOUNCE:
                s.text = tr2(Str::SquintTitle, Str::SquintSub);
                s.progress_total = 9;
                if (t_ >= DC_SQUINT_WAIT_S) {
                    seq_.start(DC_POS, 9, 10);
                    next(PHASE2);
                }
                break;
            case PHASE2:
                s.text = tr2(Str::SquintTitle, Str::SquintSub);
                seq_.update(dt, net, g, s);
                if (seq_.done) {
                    net.send_int32(19);
                    next(COMPLETE);
                }
                break;
            case COMPLETE:
                s.text = tr(Str::CollectionComplete);
                s.tone = Tone::Success;
                s.progress_total = 9;
                s.progress_done = 9;
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
    void next(Phase p) {
        phase_ = p;
        t_ = 0;
    }
    Phase phase_ = INTRO;
    double t_ = 0;
    bool finished_ = false;
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
        s.text = tr2(Str::FollowTitle, Str::EachDotCenter);
        t_ += dt;
        switch (phase_) {
            case INTRO:
                s.progress_total = 11;
                if (t_ >= INTRO_S) {
                    seq_.start(FINETUNE_POS, 11, 0);
                    phase_ = RUN;
                    t_ = 0;
                }
                break;
            case RUN:
                seq_.update(dt, net, g, s);  // signals point index 0..10 as each is captured
                if (seq_.done) {
                    net.send_int32(100);
                    phase_ = COMPLETE;
                    t_ = 0;
                }
                break;
            case COMPLETE:
                s.text = tr(Str::FinetuneComplete);
                s.tone = Tone::Success;
                s.progress_total = 11;
                s.progress_done = 11;
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
    Phase phase_ = INTRO;
    double t_ = 0;
    bool finished_ = false;
    PointSequence seq_;
};

// ── interactive (driven by the EyeTrackVR Python app over UDP) ────────────────
//
// Commands are queued and applied in order. Commands that only make sense
// between passes (pass starts, pin, centre dot) are applied immediately when
// idle; if one arrives while a pass is still running, the app has given up on
// that pass (e.g. after an error), so the stale pass is abandoned rather than
// the command being dropped — the old build dropped it and the app could then
// wait forever for signals. Global commands (text, backdrop, unpin, quit)
// apply in any state.
class InteractiveMode : public IMode {
    enum State { IDLE, PASS_TEXT, PASS_DC, RANDOM_WAIT, RANDOM_ANIM, FINISHED };

public:
    bool needs_recv() const override { return true; }
    void on_start(UdpNet& net) override { net.send_int32(255); }  // overlay ready

    Scene update(double dt, UdpNet& net, const GazeRange& g) override {
        // Pull everything that arrived since the last frame.
        Cmd c;
        while (queue_.size() < kMaxQueue && poll_cmd(net, c)) queue_.push_back(c);
        process_queue(net);

        Scene s;
        s.backdrop_gray = backdrop_gray_;

        switch (state_) {
            case IDLE:
                if (pin_dot_) {
                    // Pinned dot (cmd 113): converge once, then hold indefinitely
                    // until re-pinned or hidden (cmd 114). Used by the blink-at-dot
                    // data-collection pass, where the user must keep a stable
                    // fixation target across several seconds of blinking.
                    pin_t_ += dt;
                    show_converging(s, g.x(pin_x_), g.y(pin_y_), pin_t_);
                } else if (center_dot_) {
                    center_t_ += dt;
                    show_converging(s, g.x(0.0f), g.y(0.0f), center_t_);
                }
                break;

            case PASS_TEXT:
                t_ += dt;
                if (t_ >= 0.4) {
                    if (include_dc_) {
                        seq_.start(DC_POS, 9, 0);
                        state_ = PASS_DC;
                    } else {
                        net.send_int32(10);
                        state_ = RANDOM_WAIT;
                    }
                }
                break;

            case PASS_DC:
                seq_.update(dt, net, g, s);
                if (seq_.done) {
                    net.send_int32(10);
                    state_ = RANDOM_WAIT;
                }
                break;

            case RANDOM_WAIT:
                // Keep the last captured dot up while the app saves its frames and
                // picks the next point: the user keeps fixating instead of the
                // target blinking out.
                if (dot_on_) {
                    s.dot_visible = true;
                    s.dot_x = dot_x_;
                    s.dot_y = dot_y_;
                    s.dot_size = DC_DOT_SMALL;
                }
                break;

            case RANDOM_ANIM:
                anim_point(dt, net, g, s);
                break;

            case FINISHED:
                break;
        }
        // Remember what is on screen so the next target can glide from it.
        dot_on_ = s.dot_visible;
        if (dot_on_) {
            dot_x_ = s.dot_x;
            dot_y_ = s.dot_y;
        }

        s.text_visible = show_text_;
        s.text = text_;
        s.progress_total = 0;  // the app owns pacing; no progress pips here
        return s;
    }
    bool finished() const override { return state_ == FINISHED; }

private:
    static constexpr size_t kMaxQueue = 256;

    static bool is_global(int32_t c) {
        return c == 50 || c == 98 || c == 99 || c == 114 || (c >= 130 && c <= 132) || c == 200;
    }
    static bool is_between_passes(int32_t c) {
        return (c >= 100 && c <= 106) || c == 110 || c == 112 || c == 113 || c == 120 || c == 121;
    }

    void process_queue(UdpNet& net) {
        while (!queue_.empty() && state_ != FINISHED) {
            const Cmd& c = queue_.front();
            if (is_global(c.code)) {
                apply_global(c, net);
            } else if (is_between_passes(c.code)) {
                if (state_ == RANDOM_ANIM) break;  // let the point finish first
                if (state_ != IDLE) {
                    std::cout << "[WARN] Command " << c.code << " during a running pass; abandoning that pass\n";
                    end_pass();
                }
                apply_idle(c, net);
            } else if (c.code == 111) {
                if (state_ == RANDOM_ANIM || state_ == PASS_TEXT || state_ == PASS_DC) break;
                if (state_ != RANDOM_WAIT) {
                    std::cout << "[WARN] Ignoring 111 outside a jitter phase\n";
                } else if (!c.has_xy) {
                    std::cout << "[WARN] Ignoring 111 with missing/non-finite coordinates\n";
                } else {
                    nx_ = std::clamp(c.x, -1.0f, 1.0f);
                    ny_ = std::clamp(c.y, -1.0f, 1.0f);
                    t_ = 0;
                    rsub_ = 0;
                    begin_glide();
                    state_ = RANDOM_ANIM;
                }
            } else if (c.code == 119) {
                if (state_ == RANDOM_ANIM) break;  // normal: ends after the hold
                if (state_ != RANDOM_WAIT && state_ != IDLE)
                    std::cout << "[WARN] 119 during a running pass; ending it early\n";
                net.send_int32(19);
                end_pass();
            } else {
                std::cout << "[WARN] Unknown command " << c.code << " ignored\n";
            }
            queue_.pop_front();
        }
    }

    void apply_global(const Cmd& c, UdpNet& net) {
        switch (c.code) {
            case 50:
                text_ = c.str.substr(0, 400);
                show_text_ = true;
                break;
            case 98:
                show_text_ = false;
                text_.clear();
                break;
            case 99:
                center_dot_ = false;
                break;
            case 114:
                pin_dot_ = false;
                break;
            case 130:
            case 131:
            case 132:
                // Pupil-reference illumination: dim, bright, or neutral.
                backdrop_gray_ = c.code == 130 ? 24 : (c.code == 131 ? 208 : 96);
                net.send_int32(22);
                break;
            case 200:
                state_ = FINISHED;
                break;
            default:
                break;
        }
    }

    void apply_idle(const Cmd& c, UdpNet& net) {
        if (c.code >= 100 && c.code <= 106) {
            static const Str kPassTitle[] = {
                Str::FollowTitle, Str::SquintTitle,   Str::WidenTitle,    Str::BrowsUpFull,
                Str::BrowsUpHalf, Str::BrowsDownFull, Str::BrowsDownHalf,
            };
            const Str title = kPassTitle[c.code - 100];
            show_default_text(c.code == 100 ? std::string(tr(title)) : tr2(title, Str::FollowLower));
            include_dc_ = true;
            center_dot_ = false;
            t_ = 0;
            state_ = PASS_TEXT;
        } else if (c.code == 110 || c.code == 112) {
            show_default_text(c.code == 112 ? tr2(Str::SquintTitle, Str::EachDotAppears)
                                            : std::string(tr(Str::EachDotAppears)));
            include_dc_ = false;
            center_dot_ = false;
            t_ = 0;
            state_ = PASS_TEXT;
        } else if (c.code == 113) {
            if (!c.has_xy) {
                std::cout << "[WARN] Ignoring 113 with missing/non-finite coordinates\n";
                return;
            }
            // Pin a persistent dot at (x, y); ack with 21 so the app knows
            // the dot is up (and can distinguish a new overlay from an old
            // exe that ignores this command).
            pin_x_ = std::clamp(c.x, -1.0f, 1.0f);
            pin_y_ = std::clamp(c.y, -1.0f, 1.0f);
            pin_t_ = 0;
            pin_dot_ = true;
            begin_glide();
            net.send_int32(21);
        } else if (c.code == 120) {
            show_default_text(tr2(Str::CenterTitle, Str::ShiftHeadset));
            if (!center_dot_) {
                center_t_ = 0;
                begin_glide();
            }
            center_dot_ = true;
        } else if (c.code == 121) {
            center_dot_ = false;
            show_text_ = false;
            text_.clear();
            net.send_int32(19);
        }
    }

    // Instruction for a pass/target the overlay starts. If the app already put
    // its own (translated) text up for this step with 50 - it sends it before
    // the voice prompt so the text never lags the speech - keep that instead of
    // swapping in the built-in wording mid-sentence.
    void show_default_text(const std::string& t) {
        if (!show_text_ || text_.empty()) text_ = t;
        show_text_ = true;
    }

    // The next target glides in from the dot currently on screen (if any).
    void begin_glide() {
        glide_ = dot_on_;
        glide_x_ = dot_x_;
        glide_y_ = dot_y_;
    }

    // Position along the glide from the remembered origin to (tx, ty).
    void glide(double t, float tx, float ty, float& x, float& y) const {
        x = tx;
        y = ty;
        if (glide_ && t < DC_APPEAR_S) {
            const float k = ease_in_out((float)(t / DC_APPEAR_S));
            x = glide_x_ + (tx - glide_x_) * k;
            y = glide_y_ + (ty - glide_y_) * k;
        }
    }

    // A dot at (x, y) that glides in from wherever the previous one was and
    // has its attention ring converge once.
    void show_converging(Scene& s, float tx, float ty, double t) {
        s.dot_visible = true;
        s.dot_size = DC_DOT_SMALL;
        s.ring_x = tx;
        s.ring_y = ty;
        const float f = (float)std::min(1.0, t / DC_SHRINK_S);
        s.ring_size = f < 1.0f ? DC_DOT_LARGE + (DC_DOT_SMALL - DC_DOT_LARGE) * converge(f) : 0.0f;
        glide(t, tx, ty, s.dot_x, s.dot_y);
        s.dot_cut = !glide_;
    }

    // One jittered-position sweep: appear → shrink (signal 20) → hold → back to WAIT.
    void anim_point(double dt, UdpNet& net, const GazeRange& g, Scene& s) {
        t_ += dt;
        const float tx = g.x(nx_), ty = g.y(ny_);
        float x = tx, y = ty;
        s.dot_visible = true;
        s.dot_size = DC_DOT_SMALL;
        s.ring_x = tx;
        s.ring_y = ty;
        if (rsub_ == 0) {
            s.ring_size = DC_DOT_LARGE;
            glide(t_, tx, ty, x, y);
            if (t_ >= DC_APPEAR_S) {
                t_ = 0;
                rsub_ = 1;
            }
        } else if (rsub_ == 1) {
            const float f = (float)std::min(1.0, t_ / DC_SHRINK_S);
            s.ring_size = DC_DOT_LARGE + (DC_DOT_SMALL - DC_DOT_LARGE) * converge(f);
            if (t_ >= DC_SHRINK_S) {
                t_ = 0;
                rsub_ = 2;
                net.send_int32(20);
            }
        } else {
            s.capture_hold = true;
            if (t_ >= DC_HOLD_S) {
                state_ = RANDOM_WAIT;
            }
        }
        s.dot_x = x;
        s.dot_y = y;
        s.dot_cut = !glide_;
    }

    void end_pass() {
        state_ = IDLE;
        show_text_ = false;
        text_.clear();
        center_dot_ = false;
        dot_on_ = false;
    }

    State state_ = IDLE;
    std::deque<Cmd> queue_;
    bool show_text_ = false, center_dot_ = false, include_dc_ = false;
    bool pin_dot_ = false;
    uint8_t backdrop_gray_ = cfg::BACKDROP_GREY;
    float pin_x_ = 0, pin_y_ = 0;
    double pin_t_ = 0, center_t_ = 0;
    std::string text_;
    double t_ = 0;
    int rsub_ = 0;
    float nx_ = 0, ny_ = 0;
    // The dot currently on screen (metres), for glides and RANDOM_WAIT.
    bool dot_on_ = false;
    float dot_x_ = 0, dot_y_ = 0;
    // Origin of the current glide (captured when the command arrived).
    bool glide_ = false;
    float glide_x_ = 0, glide_y_ = 0;
    PointSequence seq_;
};

}  // namespace

bool is_mode_name(const std::string& n) {
    return n == "center" || n == "9point" || n == "ellipse" || n == "datacollect" || n == "finetune" ||
           n == "interactive";
}

std::unique_ptr<IMode> make_mode(const std::string& name) {
    if (name == "center") return std::make_unique<ClassicMode>(true);
    if (name == "9point") return std::make_unique<ClassicMode>(false);
    if (name == "ellipse") return std::make_unique<EllipseMode>();
    if (name == "datacollect") return std::make_unique<DataCollectMode>();
    if (name == "finetune") return std::make_unique<FineTuneMode>();
    if (name == "interactive") return std::make_unique<InteractiveMode>();
    return nullptr;
}
