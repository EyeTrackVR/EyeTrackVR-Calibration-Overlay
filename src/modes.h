#pragma once
#include "scene.h"
#include "net.h"
#include "fov.h"

#include <memory>
#include <string>

// Each calibration mode is a per-frame state machine: update(dt, net, gaze)
// advances time, emits the same UDP signals the OpenVR build did, and returns
// the Scene to display. finished() flips true once the mode is complete.
class IMode {
public:
    virtual ~IMode() = default;
    virtual Scene update(double dt, UdpNet& net, const GazeRange& g) = 0;
    virtual bool finished() const = 0;
    // Called once rendering is up and the headset geometry is known (e.g. to
    // emit the "ready" signal).
    virtual void on_start(UdpNet&) {}
    // True if this mode receives commands (binds the command port).
    virtual bool needs_recv() const { return false; }
};

// name: "center" | "9point" | "ellipse" | "datacollect" | "finetune" | "interactive".
// Returns nullptr for an unknown name.
std::unique_ptr<IMode> make_mode(const std::string& name);
bool is_mode_name(const std::string& name);
