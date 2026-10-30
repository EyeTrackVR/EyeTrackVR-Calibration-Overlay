// Unit tests for the VR-independent core: geometry/visibility, mode timing and
// UDP protocol, interactive command handling, presenter layout invariants
// (the instruction panel never covers a target), text rendering and the
// session lifecycle. Runs headless; no VR runtime or hardware needed.

#include "config.h"
#include "fov.h"
#include "i18n.h"
#include "modes.h"
#include "net.h"
#include "presenter.h"
#include "session.h"
#include "sim_hmd.h"
#include "sprites.h"
#include "text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

#ifndef ETVR_FONT
#define ETVR_FONT "external/font/OpenSans-Regular.ttf"
#endif

static int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        ++g_checks;                                                    \
        if (!(c)) {                                                    \
            ++g_fail;                                                  \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                              \
    } while (0)
#define CHECKM(c, ...)                                                   \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(c)) {                                                      \
            ++g_fail;                                                    \
            std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #c); \
            std::printf(__VA_ARGS__);                                    \
            std::printf("\n");                                           \
        }                                                                \
    } while (0)

// ── Test doubles ──────────────────────────────────────────────────────────────

struct FakeNet : UdpNet {
    double now = 0.0;
    std::vector<std::pair<double, int32_t>> sent;
    std::deque<std::vector<unsigned char>> inbox;

    void send_int32(int32_t v) override { sent.push_back({now, v}); }
    int recv(void* buf, int len) override {
        if (inbox.empty()) return 0;
        auto d = inbox.front();
        inbox.pop_front();
        const int n = std::min<int>(len, (int)d.size());
        std::memcpy(buf, d.data(), (size_t)n);
        return n;
    }
    static void be(std::vector<unsigned char>& v, uint32_t x) {
        v.push_back((unsigned char)(x >> 24));
        v.push_back((unsigned char)(x >> 16));
        v.push_back((unsigned char)(x >> 8));
        v.push_back((unsigned char)x);
    }
    void cmd(int32_t c) {
        std::vector<unsigned char> v;
        be(v, (uint32_t)c);
        inbox.push_back(v);
    }
    void cmd_xy(int32_t c, float x, float y) {
        std::vector<unsigned char> v;
        be(v, (uint32_t)c);
        uint32_t xi, yi;
        std::memcpy(&xi, &x, 4);
        std::memcpy(&yi, &y, 4);
        be(v, xi);
        be(v, yi);
        inbox.push_back(v);
    }
    void text(const std::string& s) {
        std::vector<unsigned char> v;
        be(v, 50u);
        v.insert(v.end(), s.begin(), s.end());
        inbox.push_back(v);
    }
    std::vector<int32_t> values() const {
        std::vector<int32_t> out;
        out.reserve(sent.size());
        for (auto& p : sent) out.push_back(p.second);
        return out;
    }
};

struct Config {
    const char* name;
    FovConfig f;
};
static std::vector<Config> configs() {
    std::vector<Config> c;
    c.push_back({"default", FovConfig{}});
    FovConfig v6;
    v6.coverage = 0.80f;
    v6.max_deg = 36;
    v6.max_deg_up = 20;
    v6.max_deg_down = 40;
    c.push_back({"v6", v6});
    FovConfig wide;
    wide.coverage = 1.0f;
    wide.max_deg = 70;
    wide.max_deg_up = 70;
    wide.max_deg_down = 70;
    c.push_back({"wide", wide});
    FovConfig low;
    low.center_deg = -8.0f;
    low.max_deg_down = 35;
    c.push_back({"center-8", low});
    return c;
}

static ViewGeometry geometry(const std::string& hmd, const FovConfig& f) {
    EyeGeom eyes[2];
    sim_hmd(hmd, eyes);
    ViewGeometry g;
    compute_view_geometry(eyes, cfg::OVERLAY_DIST, f, g);
    return g;
}

// ── Text ──────────────────────────────────────────────────────────────────────

static void test_text() {
    std::printf("text\n");
    const std::u32string a = decode_utf8("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    CHECK(a.size() == 4 && a[0] == U'A' && a[1] == 0xE9 && a[2] == 0x20AC && a[3] == 0x1F600);
    CHECK(decode_utf8("\xC3") == std::u32string(1, 0xFFFD));
    CHECK(decode_utf8("\xC0\xAF").find(0x2F) == std::u32string::npos);  // overlong rejected
    CHECK(decode_utf8("\xED\xA0\x80")[0] == 0xFFFD);                    // surrogate rejected
    CHECK(normalize_instruction("  squint \r\n follow\tthe dot\n\n\x01") == "Squint\nFollow the dot");
    CHECK(normalize_instruction("\n\n").empty());

    TextRenderer tr;
    CHECK(tr.load(ETVR_FONT));
    CHECK(tr.ready());
    Image img;
    PanelContent pc;
    pc.text = "Follow the dot\nKeep your head still";
    pc.pips_total = 11;
    pc.pips_done = 3;
    tr.render_panel(pc, 990, img);
    CHECK(img.w > 2 * TextRenderer::kShadowPad && img.h > 2 * TextRenderer::kShadowPad);
    CHECK(img.w <= TextRenderer::kMaxWidth && img.h <= TextRenderer::kMaxHeight);
    size_t opaque = 0;
    for (size_t i = 3; i < img.rgba.size(); i += 4) opaque += img.rgba[i] > 200;
    CHECK(opaque > (size_t)(img.w * img.h / 3));
    const uint64_t v0 = img.version;
    pc.text = std::string(2000, 'W') + "\n" + std::string(3000, 'x') + " a b c";
    pc.tone = Tone::Success;
    tr.render_panel(pc, 990, img);  // must clamp, not overflow
    CHECK(img.version != v0);
    CHECK(img.w <= TextRenderer::kMaxWidth && img.h <= TextRenderer::kMaxHeight);
    pc.text =
        "\xE6\xA0\xA1\xE5\x87\x86 "
        "\xD0\x9A\xD0\xB0\xD0\xBB\xD0\xB8\xD0\xB1\xD1\x80\xD0\xBE\xD0\xB2\xD0\xBA\xD0\xB0\n\xFF\xFE";
    pc.pips_total = 0;
    pc.bar = 0.4f;
    tr.render_panel(pc, 400, img);  // CJK/Cyrillic/garbage: no crash
    CHECK(img.w >= 320);
    CHECK(sprites::dot().w == 128 && sprites::ring().w == 256);
}

// ── Geometry ──────────────────────────────────────────────────────────────────

static void test_geometry() {
    std::printf("geometry\n");
    for (const auto& hmd : sim_hmd_names()) {
        for (const auto& c : configs()) {
            EyeGeom eyes[2];
            CHECK(sim_hmd(hmd, eyes));
            ViewGeometry g;
            std::string notes;
            const bool ok = compute_view_geometry(eyes, cfg::OVERLAY_DIST, c.f, g, &notes);
            CHECKM(ok, "%s/%s: %s", hmd.c_str(), c.name, notes.c_str());
            if (!ok) continue;
            const GazeRange& r = g.gaze;
            CHECK(r.valid && !g.synthetic);
            const float D = cfg::OVERLAY_DIST;
            CHECKM(r.rx <= std::tan(c.f.max_deg * kDeg2Rad) * D + 1e-3f, "%s/%s rx %f", hmd.c_str(), c.name, r.rx);
            CHECK(r.center_y + r.ry_up <= std::tan(c.f.max_deg_up * kDeg2Rad) * D + 1e-3f);
            CHECK(r.center_y - r.ry_down >= -std::tan(c.f.max_deg_down * kDeg2Rad) * D - 1e-3f);
            // Every point of the unit target ellipse is visible to both eyes.
            int bad = 0;
            for (int i = 0; i < 360; ++i) {
                const float a = i * kDeg2Rad;
                for (float rad : {0.3f, 0.6f, 0.88f, 1.0f})
                    if (!g.disc_visible(r.x(rad * std::cos(a)), r.y(rad * std::sin(a)), kTargetVisibleMargin * 0.98f))
                        ++bad;
            }
            CHECKM(bad == 0, "%s/%s: %d invisible ellipse samples (scale %.3f)", hmd.c_str(), c.name, bad,
                   g.ellipse_scale);
            // Lens polygons from the profiles are plausible and get used.
            if (hmd != "generic")
                CHECKM(g.used_polygons, "%s/%s polygons not used: %s", hmd.c_str(), c.name, notes.c_str());
        }
    }

    // Symmetric headset, no polygon, default caps: matches the historical policy.
    {
        ViewGeometry g = geometry("generic", FovConfig{});
        CHECK(std::fabs(g.gaze.rx - std::tan(30.0f * kDeg2Rad) * 3.0f) < 1e-3f);
        CHECK(std::fabs(g.gaze.center_y) < 1e-6f);
        CHECK(std::fabs(g.gaze.ry_up - std::tan(15.0f * kDeg2Rad) * 3.0f) < 1e-3f);
        CHECK(g.ellipse_scale == 1.0f);
    }
    // Beyond-like down-heavy FOV with the app's v6 caps: caps bind, no shrink.
    {
        ViewGeometry g = geometry("beyond2", configs()[1].f);
        CHECK(std::fabs(g.gaze.rx - std::tan(36.0f * kDeg2Rad) * 3.0f) < 1e-3f);
        CHECK(std::fabs(g.gaze.ry_down - std::tan(40.0f * kDeg2Rad) * 3.0f) < 1e-3f);
        CHECK(g.ellipse_scale > 0.999f);
    }
    // Polygon sanitising.
    {
        EyeGeom eyes[2];
        sim_hmd("index", eyes);
        EyeGeom e = eyes[0];
        e.visible = {{0.2f, 0.2f}, {0.5f, 0.2f}, {0.5f, 0.5f}};  // misses the axis
        CHECK(!sanitize_polygon(e) && e.visible.empty());
        e.visible = {{-0.1f, -0.1f}, {0.1f, -0.1f}, {0.1f, 0.1f}, {-0.1f, 0.1f}};  // tiny
        CHECK(!sanitize_polygon(e));
        e.visible = {{0, 0}, {std::numeric_limits<float>::quiet_NaN(), 1}, {1, 1}};
        CHECK(!sanitize_polygon(e));
        // A polygon that would collapse the range is rejected, not obeyed.
        EyeGeom bad[2] = {eyes[0], eyes[1]};
        for (auto& b : bad) {
            b.visible.clear();
            for (int i = 0; i < 32; ++i) {
                const float a = 2 * kPi * i / 32;
                b.visible.push_back({1.05f * std::cos(a), 0.9f * std::sin(a)});
            }
        }
        FovConfig wide = configs()[2].f;
        ViewGeometry g;
        std::string notes;
        CHECK(compute_view_geometry(bad, 3.0f, wide, g, &notes));
        // Area check keeps it; it only trims modestly (> 0.6x) so it is used.
        CHECK(g.used_polygons || notes.find("rejected") != std::string::npos);
        for (auto& b : bad)
            for (auto& v : b.visible) {
                v.x *= 0.45f;
                v.y *= 0.55f;
            }
        CHECK(compute_view_geometry(bad, 3.0f, wide, g, &notes));
        CHECKM(!g.used_polygons, "collapsing polygon was obeyed: %s", notes.c_str());
    }
    // Garbage frusta fail cleanly; fallback honours caps.
    {
        EyeGeom eyes[2];
        sim_hmd("generic", eyes);
        eyes[1].tan_left = std::numeric_limits<float>::quiet_NaN();
        ViewGeometry g;
        CHECK(!compute_view_geometry(eyes, 3.0f, FovConfig{}, g));
        ViewGeometry fb = fallback_view_geometry(FovConfig{}, 3.0f);
        CHECK(!fb.gaze.valid && fb.synthetic);
        CHECK(fb.gaze.rx <= std::tan(30.0f * kDeg2Rad) * 3.0f + 1e-3f);
    }
}

// ── Mode timing / protocol ────────────────────────────────────────────────────

struct Run {
    std::vector<std::pair<double, int32_t>> signals;
    std::vector<double> t;
    std::vector<Scene> scenes;
    bool finished = false;
};

static Run run_mode(const std::string& name, const GazeRange& g, double dt_lo, double dt_hi, double limit_s,
                    unsigned seed = 1, const std::function<void(FakeNet&, double, const Scene&)>& driver = {}) {
    Run r;
    FakeNet net;
    auto mode = make_mode(name);
    mode->on_start(net);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(dt_lo, dt_hi);
    double t = 0.0;
    Scene s = mode->update(0.0, net, g);
    while (t < limit_s && !mode->finished()) {
        if (driver) driver(net, t, s);
        const double dt = dist(rng);
        t += dt;
        net.now = t;
        s = mode->update(dt, net, g);
        r.t.push_back(t);
        r.scenes.push_back(s);
    }
    r.signals = net.sent;
    r.finished = mode->finished();
    return r;
}

static void check_holds(const Run& r, const char* what) {
    // Whenever capture_hold is set the dot must be stationary and every capture
    // signal must be followed by >= 0.5 s - one frame of stationary dot.
    for (const auto& sig : r.signals) {
        if (sig.second == 9 || sig.second == 19 || sig.second == 100) continue;  // "done" sentinels
        size_t i = 0;
        while (i < r.t.size() && r.t[i] < sig.first - 1e-9) ++i;
        if (i >= r.scenes.size()) continue;
        const Scene& at = r.scenes[i];
        if (!at.dot_visible) continue;
        double held = 0.0;
        size_t j = i;
        while (j + 1 < r.scenes.size() && r.scenes[j + 1].dot_visible &&
               std::fabs(r.scenes[j + 1].dot_x - at.dot_x) < 1e-6f &&
               std::fabs(r.scenes[j + 1].dot_y - at.dot_y) < 1e-6f) {
            held = r.t[j + 1] - r.t[i];
            ++j;
        }
        if (j + 1 < r.scenes.size() || !r.finished)
            CHECKM(held >= 0.5 - 0.035, "%s: signal %d held only %.3f s", what, sig.second, held);
    }
}

static void test_modes(const GazeRange& g) {
    std::printf("modes\n");
    for (auto dts : {std::pair<double, double>{1.0 / 90, 1.0 / 90}, {1.0 / 144, 1.0 / 50}}) {
        Run ft = run_mode("finetune", g, dts.first, dts.second, 60.0, 3);
        std::vector<int32_t> want;
        for (int i = 0; i <= 10; ++i) want.push_back(i);
        want.push_back(100);
        std::vector<int32_t> got;
        got.reserve(ft.signals.size());
        for (auto& s : ft.signals) got.push_back(s.second);
        CHECK(got == want);
        CHECK(ft.finished);
        for (size_t i = 1; i + 1 < ft.signals.size(); ++i) {
            const double gap = ft.signals[i].first - ft.signals[i - 1].first;
            CHECKM(std::fabs(gap - 1.15) < 0.07, "finetune gap %.3f", gap);
        }
        check_holds(ft, "finetune");

        Run dc = run_mode("datacollect", g, dts.first, dts.second, 80.0, 4);
        want.clear();
        for (int i = 0; i <= 8; ++i) want.push_back(i);
        want.push_back(9);
        for (int i = 10; i <= 18; ++i) want.push_back(i);
        want.push_back(19);
        got.clear();
        for (auto& s : dc.signals) got.push_back(s.second);
        CHECK(got == want);
        check_holds(dc, "datacollect");

        Run p9 = run_mode("9point", g, dts.first, dts.second, 10.0, 5);
        got.clear();
        for (auto& s : p9.signals) got.push_back(s.second);
        CHECK((got == std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        CHECK(p9.finished && p9.t.back() < 2.0);

        Run c = run_mode("center", g, dts.first, dts.second, 10.0, 6);
        CHECK(c.signals.size() == 1 && c.signals[0].second == 9);

        Run el = run_mode("ellipse", g, dts.first, dts.second, 60.0, 7);
        got.clear();
        for (auto& s : el.signals) got.push_back(s.second);
        CHECK((got == std::vector<int32_t>{0, 9}));
        CHECK(el.finished);
    }

    // Finetune dot positions at capture equal the protocol coordinates.
    const float FT[11][2] = {{0.0f, 0.8f},
                             {0.760845f, 0.247214f},
                             {0.470228f, -0.647214f},
                             {-0.470228f, -0.647214f},
                             {-0.760845f, 0.247214f},
                             {0.24687f, 0.339787f},
                             {0.399444f, -0.129787f},
                             {0.0f, -0.42f},
                             {-0.399444f, -0.129787f},
                             {-0.24687f, 0.339787f},
                             {0.0f, 0.0f}};
    Run ft = run_mode("finetune", g, 1.0 / 90, 1.0 / 90, 60.0);
    for (const auto& sig : ft.signals) {
        if (sig.second > 10) continue;
        size_t i = 0;
        while (i < ft.t.size() && ft.t[i] < sig.first - 1e-9) ++i;
        const Scene& s = ft.scenes[i];
        CHECK(s.dot_visible);
        CHECKM(
            std::fabs(s.dot_x - g.x(FT[sig.second][0])) < 1e-4f && std::fabs(s.dot_y - g.y(FT[sig.second][1])) < 1e-4f,
            "finetune point %d misplaced", sig.second);
    }
}

// ── Interactive protocol ──────────────────────────────────────────────────────

struct Interactive {
    FakeNet net;
    std::unique_ptr<IMode> mode = make_mode("interactive");
    GazeRange g;
    double t = 0.0;
    Scene last;
    explicit Interactive(const GazeRange& gr) : g(gr) { mode->on_start(net); }
    void step(double secs) {
        const double dt = 1.0 / 90;
        for (int i = 0; i * dt < secs - 1e-9 && !mode->finished(); ++i) {
            t += dt;
            net.now = t;
            last = mode->update(dt, net, g);
        }
    }
    size_t count(int32_t v) const {
        size_t n = 0;
        for (auto& s : net.sent) n += s.second == v;
        return n;
    }
};

static void test_interactive(const GazeRange& g) {
    std::printf("interactive\n");
    Interactive it(g);
    CHECK(it.net.sent.size() == 1 && it.net.sent[0].second == 255);

    // Pass with the fixed 9-point sweep, then the jitter phase.
    it.net.cmd(100);
    it.step(0.1);
    CHECK(it.last.text_visible && it.last.text.find("Follow") != std::string::npos);
    it.step(12.0);
    auto v = it.net.values();
    CHECK((std::vector<int32_t>(v.begin() + 1, v.end()) == std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 10}));
    // Waiting for the next point: the last dot stays up.
    CHECK(it.last.dot_visible);

    it.net.cmd_xy(111, 0.5f, -0.3f);
    it.step(0.1);
    CHECK(it.last.dot_visible);
    it.step(0.6);
    CHECK(it.count(20) == 1);
    CHECK(std::fabs(it.last.dot_x - g.x(0.5f)) < 1e-4f && std::fabs(it.last.dot_y - g.y(-0.3f)) < 1e-4f);
    // 119 arriving during the hold (normal flow) is deferred until it ends.
    it.net.cmd(119);
    it.step(0.1);
    CHECK(it.count(19) == 0);
    it.step(0.6);
    CHECK(it.count(19) == 1);
    CHECK(!it.last.dot_visible && !it.last.text_visible);

    // Two queued 111s run back to back, each fully.
    it.net.cmd(110);
    it.step(0.6);
    CHECK(it.count(10) == 2);
    it.net.cmd_xy(111, -0.4f, 0.4f);
    it.net.cmd_xy(111, 0.2f, 0.1f);
    it.step(3.0);
    CHECK(it.count(20) == 3);
    // Non-finite coordinates are ignored (no capture of an invisible dot).
    it.net.cmd_xy(111, std::numeric_limits<float>::quiet_NaN(), 0.0f);
    it.step(1.5);
    CHECK(it.count(20) == 3);
    it.net.cmd(119);
    it.step(0.2);
    CHECK(it.count(19) == 2);

    // Pin / unpin, backdrop levels, centre dot.
    it.net.text("Blink repeatedly\nlook at the dot \xE2\x80\x94 ok");
    it.net.cmd_xy(113, 0.45f, -0.35f);
    it.step(0.8);
    CHECK(it.count(21) == 1);
    CHECK(it.last.dot_visible && std::fabs(it.last.dot_x - g.x(0.45f)) < 1e-4f);
    CHECK(it.last.text.find("Blink") == 0);
    it.net.cmd_xy(113, -0.45f, 0.35f);
    it.step(0.05);
    CHECK(it.count(21) == 2);
    CHECK(it.last.dot_x > g.x(-0.45f) && it.last.dot_x < g.x(0.45f));  // gliding between pins
    it.step(0.8);
    it.net.cmd(114);
    it.net.cmd(98);
    it.step(0.1);
    CHECK(!it.last.dot_visible && !it.last.text_visible);
    it.net.cmd(130);
    it.step(0.05);
    CHECK(it.count(22) == 1 && it.last.backdrop_gray == 24);
    it.net.cmd(131);
    it.step(0.05);
    CHECK(it.last.backdrop_gray == 208);
    it.net.cmd(132);
    it.step(0.05);
    CHECK(it.last.backdrop_gray == 96 && it.count(22) == 3);
    it.net.cmd(120);
    it.step(0.7);
    CHECK(it.last.dot_visible && std::fabs(it.last.dot_x - g.x(0.0f)) < 1e-4f);
    it.net.cmd(121);
    it.step(0.05);
    CHECK(it.count(19) == 3 && !it.last.dot_visible);

    // Recovery: the app gives up on a pass mid-sweep and starts the next one.
    it.net.cmd(101);
    it.step(2.0);
    const size_t zeros = it.count(0);
    it.net.cmd(102);
    it.step(0.1);
    CHECK(it.last.text.find("Widen") == 0);
    it.step(1.5);
    CHECK(it.count(0) == zeros + 1);  // new sweep started from point 0
    // Text the app put up before its voice prompt survives the pass start; with
    // none, the pass shows its own (localised) instruction.
    it.net.cmd(119);
    it.step(0.05);
    it.net.text("Sigue el punto");
    it.step(0.3);
    CHECK(it.last.text_visible && it.last.text == "Sigue el punto");
    it.net.cmd(100);
    it.step(0.1);
    CHECK(it.last.text == "Sigue el punto");
    it.net.cmd(119);
    it.step(0.05);
    CHECK(!it.last.text_visible);
    it.net.cmd(100);
    it.step(0.1);
    CHECK(it.last.text == i18n::tr(i18n::Str::FollowTitle));
    // Malformed / unknown datagrams are ignored.
    it.net.inbox.push_back({0, 0});
    it.net.cmd(4242);
    it.step(0.1);
    // Quit works even mid-pass.
    it.net.cmd(200);
    it.step(0.1);
    CHECK(it.mode->finished());
}

// ── Presenter layout invariants ───────────────────────────────────────────────

struct LayoutStats {
    int frames = 0, text_frames = 0, panel_frames = 0, overlap = 0, hold_fadein = 0, invisible_panel = 0,
        invisible_dot = 0;
    int trail_frames = 0, trail_at_hold = 0, trail_detached = 0, trail_too_wide = 0, trail_jump = 0;
};

static void layout_run(const std::string& mode_name, const ViewGeometry& vg, TextRenderer& tr, LayoutStats& st,
                       const std::string& tag, unsigned seed) {
    FakeNet net;
    auto mode = make_mode(mode_name);
    Presenter pr(&tr);
    mode->on_start(net);
    const double dt = 1.0 / 90;
    double t = 0.0;
    float prev_alpha = 0.0f, px = 0, py = 0;
    bool have_prev = false;

    // Interactive script mirroring data_collection.py.
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> jit(-0.12f, 0.12f);
    const float base[22][2] = {{-0.60f, -0.60f}, {-0.85f, -0.26f}, {-0.43f, -0.46f}, {-0.36f, 0.00f}, {-0.85f, 0.33f},
                               {-0.43f, 0.46f},  {-0.60f, 0.60f},  {0.00f, 0.85f},   {0.00f, 0.39f},  {-0.26f, 0.16f},
                               {-0.17f, -0.23f}, {0.00f, -0.39f},  {0.00f, -0.85f},  {0.43f, -0.46f}, {0.26f, -0.16f},
                               {0.36f, 0.00f},   {0.17f, 0.23f},   {0.43f, 0.46f},   {0.60f, 0.60f},  {0.85f, 0.33f},
                               {0.85f, -0.26f},  {0.60f, -0.60f}};
    int step = 0, jitter_i = 0, blink_i = 0;
    double wait_until = 0.0;
    size_t seen = 0;
    auto got = [&](int32_t v) {
        for (size_t i = seen; i < net.sent.size(); ++i)
            if (net.sent[i].second == v) {
                seen = i + 1;
                return true;
            }
        return false;
    };

    Scene s = mode->update(0.0, net, vg.gaze);
    while (t < 400.0 && !mode->finished()) {
        if (mode_name == "interactive" && t >= wait_until) {
            switch (step) {
                case 0:
                    net.text("Close your eyes and look in random direction");
                    wait_until = t + 3;
                    ++step;
                    break;
                case 1:
                    net.cmd(98);
                    net.cmd(100);
                    ++step;
                    break;
                case 2:
                    if (got(10)) {
                        ++step;
                    }
                    break;
                case 3: {
                    float x = base[jitter_i][0] + jit(rng), y = base[jitter_i][1] + jit(rng);
                    const float r = std::hypot(x, y);
                    if (r > 0.879f) {
                        x *= 0.879f / r;
                        y *= 0.879f / r;
                    }
                    net.cmd_xy(111, x, y);
                    ++step;
                    break;
                }
                case 4:
                    if (got(20)) {
                        wait_until = t + 0.05;
                        ++jitter_i;
                        step = jitter_i < 22 ? 3 : 5;
                    }
                    break;
                case 5:
                    net.cmd(119);
                    ++step;
                    break;
                case 6:
                    if (got(19)) {
                        net.text("Blink repeatedly\nlook at the dot");
                        ++step;
                    }
                    break;
                case 7: {
                    const float bx[4] = {-0.45f, 0.45f, 0.45f, -0.45f}, by[4] = {0.35f, 0.35f, -0.35f, -0.35f};
                    net.cmd_xy(113, bx[blink_i] + jit(rng), by[blink_i] + jit(rng));
                    wait_until = t + 3.5;
                    ++blink_i;
                    if (blink_i == 4) ++step;
                    break;
                }
                case 8:
                    net.cmd(114);
                    net.cmd(98);
                    net.cmd(120);
                    wait_until = t + 4;
                    ++step;
                    break;
                case 9:
                    net.cmd(121);
                    net.cmd(200);
                    ++step;
                    break;
                default:
                    break;
            }
        }
        t += dt;
        net.now = t;
        s = mode->update(dt, net, vg.gaze);
        const Frame f = pr.update(s, dt, vg);
        ++st.frames;
        if (s.text_visible) ++st.text_frames;

        Presenter::Rect body;
        const bool panel = pr.panel_body(body) && f.panel.alpha > 0.02f;
        if (panel) ++st.panel_frames;
        const float speed = have_prev ? std::hypot(f.dot.x - px, f.dot.y - py) / (float)dt : 0.0f;
        if (f.dot.visible) {
            px = f.dot.x;
            py = f.dot.y;
            have_prev = true;
        } else
            have_prev = false;

        if (panel && f.dot.visible && f.dot.alpha > 0.02f && speed < 1.5f) {
            const float r = f.dot.w * 0.5f * sprites::kSpriteEdge;
            const float qx = std::clamp(f.dot.x, body.x0, body.x1), qy = std::clamp(f.dot.y, body.y0, body.y1);
            if (std::hypot(f.dot.x - qx, f.dot.y - qy) < r) {
                if (st.overlap++ < 3)
                    std::printf("  overlap %s t=%.2f dot(%.2f,%.2f) panel[%.2f..%.2f]x[%.2f..%.2f]\n", tag.c_str(), t,
                                f.dot.x, f.dot.y, body.x0, body.x1, body.y0, body.y1);
            }
        }
        const float palpha = f.panel.visible ? f.panel.alpha : 0.0f;
        if (s.capture_hold && palpha > prev_alpha + 1e-4f) ++st.hold_fadein;
        prev_alpha = palpha;
        if (panel) {
            const float k = cfg::TEXT_DIST / cfg::OVERLAY_DIST;
            if (!vg.rect_visible(body.x0 * k, body.y0 * k, body.x1 * k, body.y1 * k, -cfg::TEXT_DIST))
                ++st.invisible_panel;
        }
        if (s.capture_hold && s.dot_visible &&
            !vg.disc_visible(s.dot_x, s.dot_y, s.dot_size * 0.5f * sprites::kSpriteEdge))
            ++st.invisible_dot;

        // Motion trail: only behind a moving dot, its head under the dot, never
        // wider than the dot, never across a jump, gone while capturing a
        // stationary target (the ellipse samples a moving one).
        if (f.trail.visible) {
            ++st.trail_frames;
            if (s.capture_hold && mode_name != "ellipse") ++st.trail_at_hold;
            const float k = cfg::TRAIL_DIST / cfg::OVERLAY_DIST;
            const float hx = (f.trail.x + 0.5f * f.trail.w * std::cos(f.trail.rot)) / k;
            const float hy = (f.trail.y + 0.5f * f.trail.w * std::sin(f.trail.rot)) / k;
            if (!f.dot.visible || std::hypot(hx - f.dot.x, hy - f.dot.y) > 1e-3f) ++st.trail_detached;
            if (f.trail.h / k > f.dot.w * sprites::kSpriteEdge) ++st.trail_too_wide;
            if (s.dot_cut) ++st.trail_jump;
        }
    }
    CHECKM(mode->finished(), "%s did not finish", tag.c_str());
}

// Runs every shard-th headset/config/mode combination starting at `shard`.
static void test_layout(int shard, int shards) {
    std::printf("layout %d/%d\n", shard, shards);
    TextRenderer tr;
    CHECK(tr.load(ETVR_FONT));
    int index = 0;
    for (const auto& hmd : sim_hmd_names()) {
        for (const auto& c : configs()) {
            const ViewGeometry vg = geometry(hmd, c.f);
            for (const char* m : {"finetune", "datacollect", "ellipse", "interactive", "9point", "center"}) {
                if (index++ % shards != shard) continue;
                LayoutStats st;
                const std::string tag = hmd + "/" + c.name + "/" + m;
                layout_run(m, vg, tr, st, tag, 11);
                CHECKM(st.overlap == 0, "%s: panel covered the dot on %d frames", tag.c_str(), st.overlap);
                CHECKM(st.hold_fadein == 0, "%s: panel appeared during capture on %d frames", tag.c_str(),
                       st.hold_fadein);
                CHECKM(st.invisible_panel == 0, "%s: panel outside the visible FOV on %d frames", tag.c_str(),
                       st.invisible_panel);
                CHECKM(st.invisible_dot == 0, "%s: target not fully visible on %d capture frames", tag.c_str(),
                       st.invisible_dot);
                CHECKM(st.trail_at_hold == 0, "%s: trail visible during capture on %d frames", tag.c_str(),
                       st.trail_at_hold);
                CHECKM(st.trail_detached == 0, "%s: trail head off the dot on %d frames", tag.c_str(),
                       st.trail_detached);
                CHECKM(st.trail_too_wide == 0, "%s: trail wider than the dot on %d frames", tag.c_str(),
                       st.trail_too_wide);
                CHECKM(st.trail_jump == 0, "%s: trail drawn across a jump on %d frames", tag.c_str(), st.trail_jump);
                const bool moves = std::string(m) != "9point" && std::string(m) != "center";
                CHECKM(moves == (st.trail_frames > 0), "%s: trail shown on %d frames", tag.c_str(), st.trail_frames);
                if (st.text_frames > 200)
                    CHECKM(st.panel_frames > st.text_frames / 2, "%s: panel shown only %d of %d text frames",
                           tag.c_str(), st.panel_frames, st.text_frames);
            }
        }
    }
}

// ── Session lifecycle ─────────────────────────────────────────────────────────

static void test_session() {
    std::printf("session\n");
    TextRenderer tr;
    tr.load(ETVR_FONT);
    // Valid geometry: starts after the minimum warm-up and sends "ready".
    {
        FakeNet net;
        auto mode = make_mode("interactive");
        Presenter pr(&tr);
        Session s(*mode, net, pr);
        const ViewGeometry vg = geometry("quest3", FovConfig{});
        double t = 0;
        while (!s.started() && t < 10) {
            t += 1.0 / 90;
            net.now = t;
            s.step(1.0 / 90, vg);
        }
        CHECK(t >= Session::kMinWarmup && t < Session::kMinWarmup + 0.05);
        CHECK(net.sent.size() == 1 && net.sent[0].second == 255);
        s.request_quit();
        double q = 0;
        Frame f;
        while (!s.done() && q < 5) {
            q += 1.0 / 90;
            f = s.step(1.0 / 90, vg);
        }
        CHECK(s.done() && q <= Session::kMaxOutro + 0.05);
        CHECK(f.backdrop_alpha < 0.05f);
    }
    // Fallback geometry: waits, then starts anyway.
    {
        FakeNet net;
        auto mode = make_mode("finetune");
        Presenter pr(&tr);
        Session s(*mode, net, pr);
        const ViewGeometry vg = fallback_view_geometry(FovConfig{}, 3.0f);
        double t = 0;
        while (!s.started() && t < 10) {
            t += 1.0 / 90;
            s.step(1.0 / 90, vg);
        }
        CHECK(t >= Session::kMaxWarmup - 0.02 && t < Session::kMaxWarmup + 0.05);
        // And runs to completion with the outro.
        while (!s.done() && t < 60) {
            t += 1.0 / 90;
            net.now = t;
            s.step(1.0 / 90, vg);
        }
        CHECK(s.done());
        CHECK(!net.sent.empty() && net.sent.back().second == 100);
    }
}

// ── Localisation ──────────────────────────────────────────────────────────────

static void test_i18n() {
    std::printf("i18n\n");
    const auto langs = i18n::languages();
    CHECK(!langs.empty() && langs[0] == "en");
    for (const char* want : {"en", "de", "es", "ja", "zh"})
        CHECKM(std::find(langs.begin(), langs.end(), want) != langs.end(), "missing language %s", want);
    for (const auto& l : langs) {
        CHECK(i18n::set_language(l) == l);
        for (int i = 0; i < (int)i18n::Str::Count; ++i) {
            const char* s = i18n::tr((i18n::Str)i);
            CHECKM(s && *s, "%s: empty string %d", l.c_str(), i);
            const std::u32string u = decode_utf8(s);
            CHECKM(std::find(u.begin(), u.end(), (char32_t)0xFFFD) == u.end(), "%s: bad UTF-8 in %d", l.c_str(), i);
        }
        if (l != "en")
            CHECKM(std::string(i18n::tr(i18n::Str::FollowTitle)) != "Follow the dot", "%s untranslated", l.c_str());
    }
    CHECK(i18n::set_language("zh-CN") == "zh");
    CHECK(i18n::set_language("DE_de") == "de");
    CHECK(i18n::set_language("fr") == "en");
    CHECK(i18n::set_language("") == "en");
    CHECK(std::string(i18n::tr(i18n::Str::FollowTitle)) == "Follow the dot");
}

// Parses "shard/shards" for the layout suite.
static bool parse_shard(const char* s, int& shard, int& shards) {
    const char* end = s + std::strlen(s);
    auto r = std::from_chars(s, end, shard);
    if (r.ec != std::errc() || r.ptr == end || *r.ptr != '/') return false;
    r = std::from_chars(r.ptr + 1, end, shards);
    return r.ec == std::errc() && r.ptr == end && shards >= 1 && shard >= 0 && shard < shards;
}

// etvr_tests [suite [shard/shards]]: no suite runs everything; CTest runs one
// suite (or one layout shard) per test so they spread over the cores.
int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    int shard = 0, shards = 1;
    if (argc > 2 && !parse_shard(argv[2], shard, shards)) {
        std::printf("bad shard '%s', expected i/n\n", argv[2]);
        return 2;
    }
    bool ran = false;
    auto want = [&](const char* name) {
        const bool w = only.empty() || only == name;
        ran |= w;
        return w;
    };
    const GazeRange g = geometry("beyond2", configs()[1].f).gaze;
    if (want("i18n")) test_i18n();
    if (want("text")) test_text();
    if (want("geometry")) test_geometry();
    if (want("modes")) test_modes(g);
    if (want("interactive")) test_interactive(g);
    if (want("layout")) test_layout(shard, shards);
    if (want("session")) test_session();
    if (!ran) {
        std::printf("unknown suite '%s'\n", only.c_str());
        return 2;
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
