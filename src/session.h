#pragma once
#include "frame.h"
#include "modes.h"
#include "net.h"
#include "presenter.h"

// Drives one calibration run independent of the rendering backend:
//
//   WARMUP  backdrop fades in; waits (briefly) for real headset geometry so the
//           first target is never placed on a fallback guess. Then on_start()
//           (interactive: the "ready" signal 255) and the mode begins.
//   RUNNING the mode produces scenes; the presenter animates them.
//   OUTRO   the mode finished (or a quit was requested): everything fades out
//           so the user is returned smoothly to whatever was underneath.
//   DONE    the caller tears the backend down.
class Session {
public:
    Session(IMode& mode, UdpNet& net, Presenter& presenter);

    Frame step(double dt, const ViewGeometry& g);

    void request_quit();
    bool done() const { return state_ == DONE; }
    bool running() const { return state_ == RUNNING; }
    bool started() const { return state_ != WARMUP; }

    // Tunables (seconds).
    static constexpr double kMinWarmup = 0.25;  // let the backdrop start fading in
    static constexpr double kMaxWarmup = 4.0;   // then proceed on fallback geometry
    static constexpr double kMaxOutro = 1.5;    // hard cap on the fade-out

private:
    enum State { WARMUP, RUNNING, OUTRO, DONE };

    IMode& mode_;
    UdpNet& net_;
    Presenter& presenter_;
    State state_ = WARMUP;
    double t_ = 0.0;
    bool quit_ = false;
    Scene last_;
};
