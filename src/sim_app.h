#pragma once
#include "backend.h"

#include <string>

struct SimOptions {
    std::string hmd = "beyond2";  // see sim_hmd_names()
    std::string dump_dir;         // write stereo PNG frames here (empty = none)
    int dump_every = 0;           // dump every Nth frame (0 = never)
    bool fast = false;            // fixed 90 Hz steps without sleeping (deterministic)
    double max_seconds = 0;       // stop after this much simulated time (0 = no limit)
    int eye_px = 480;             // per-eye image width
    bool debug = false;           // draw the gaze-range ellipse and lens outline
};

// Headless backend: a synthetic headset and a CPU compositor. Runs the full
// session (modes, presenter, UDP protocol) without VR hardware, and can dump
// what each eye would see — used by the tests and for previewing layouts.
class SimApp : public IOverlayBackend {
public:
    explicit SimApp(SimOptions opt) : opt_(std::move(opt)) {}

    bool init(const FovConfig& fov_cfg) override;
    void shutdown() override;
    bool poll_events() override;
    bool frame(const std::function<Frame(double dt, const ViewGeometry&)>& update_cb) override;

    // Render both eyes of `f` side by side into an RGB buffer (w x h).
    static void render(const Frame& f, const ViewGeometry& g, int eye_px, bool debug, std::vector<uint8_t>& rgb, int& w,
                       int& h);

private:
    SimOptions opt_;
    FovConfig fov_cfg_;
    ViewGeometry geom_;
    double t_ = 0.0;
    double last_wall_ = 0.0;
    long frame_ = 0;
};
