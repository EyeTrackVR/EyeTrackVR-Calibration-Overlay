#include "session.h"

#include <iostream>

Session::Session(IMode& mode, UdpNet& net, Presenter& presenter) : mode_(mode), net_(net), presenter_(presenter) {}

void Session::request_quit() { quit_ = true; }

Frame Session::step(double dt, const ViewGeometry& g) {
    t_ += dt;
    switch (state_) {
        case WARMUP:
            if (quit_) {
                presenter_.begin_outro();
                state_ = OUTRO;
                t_ = 0.0;
                break;
            }
            if (t_ >= kMinWarmup && (g.gaze.valid || t_ >= kMaxWarmup)) {
                if (!g.gaze.valid) std::cout << "[WARN] Starting on fallback geometry (headset FOV unavailable)\n";
                mode_.on_start(net_);
                state_ = RUNNING;
                t_ = 0.0;
                // The first update runs below with dt = 0 so the mode starts at t = 0.
                last_ = mode_.update(0.0, net_, g.gaze);
            }
            break;
        case RUNNING:
            last_ = mode_.update(dt, net_, g.gaze);
            if (mode_.finished() || quit_) {
                if (quit_ && !mode_.finished()) std::cout << "[INFO] Quit requested; fading out\n";
                presenter_.begin_outro();
                state_ = OUTRO;
                t_ = 0.0;
            }
            break;
        case OUTRO:
            if (presenter_.outro_done() || t_ >= kMaxOutro) state_ = DONE;
            break;
        case DONE:
            break;
    }
    return presenter_.update(last_, dt, g);
}
