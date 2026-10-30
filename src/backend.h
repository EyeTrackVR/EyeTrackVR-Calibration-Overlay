#pragma once
#include "fov.h"
#include "frame.h"

#include <functional>

// Common interface for the rendering backends.
//
//  * XrApp  — a focused OpenXR/Vulkan scene session. Works on any PC OpenXR
//             runtime, but on SteamVR a scene session evicts the running game
//             (SteamVR allows only one scene app). Used when SteamVR is not
//             driving the session.
//  * OvrApp — an OpenVR overlay (IVROverlay) app. Composites the dot/text/grey
//             backdrop on TOP of whatever scene application is already running,
//             so calibrating never closes the current game. Used on SteamVR.
//  * SimApp — headless software renderer with a synthetic headset, for tests,
//             CI and previewing layouts without hardware (--backend=sim).
//
// All are driven identically by main(): init -> {poll_events, frame}* ->
// shutdown. frame() invokes update_cb(dt, geometry) to obtain the Frame to draw.
class IOverlayBackend {
public:
    virtual ~IOverlayBackend() = default;

    virtual bool init(const FovConfig& fov_cfg) = 0;
    virtual void shutdown() = 0;

    // Pump runtime lifecycle/events. Returns false when the app should exit.
    virtual bool poll_events() = 0;

    // Render one frame. dt is seconds since the previous rendered frame. When
    // the headset is visible, update_cb(dt, geometry) is called to obtain the
    // frame contents. Returns false if the loop should stop.
    virtual bool frame(const std::function<Frame(double dt, const ViewGeometry&)>& update_cb) = 0;
};
