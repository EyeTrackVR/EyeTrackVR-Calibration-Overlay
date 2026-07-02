#pragma once
#include "scene.h"
#include "fov.h"

#include <functional>

// Common interface for the two rendering backends.
//
//  * XrApp  — a focused OpenXR/Vulkan scene session. Works on any PC OpenXR
//             runtime, but on SteamVR a scene session evicts the running game
//             (SteamVR allows only one scene app). Used when SteamVR is not
//             driving the session.
//  * OvrApp — an OpenVR overlay (IVROverlay) app. Composites the dot/text/grey
//             backdrop on TOP of whatever scene application is already running,
//             so calibrating never closes the current game. Used on SteamVR.
//
// Both are driven identically by main(): init -> {poll_events, frame}* ->
// shutdown. frame() invokes update_cb(dt, gaze) to obtain the Scene to draw.
class IOverlayBackend {
public:
    virtual ~IOverlayBackend() = default;

    virtual bool init(const FovConfig& fov_cfg, const char* dot_png,
                      const char* font_ttf) = 0;
    virtual void shutdown() = 0;

    // Pump runtime lifecycle/events. Returns false when the app should exit.
    virtual bool poll_events() = 0;

    // Render one frame. dt is seconds since the previous rendered frame. When
    // the headset is visible/focused, update_cb(dt, gaze) is called to obtain
    // the scene. Returns false if the loop should stop.
    virtual bool frame(
        const std::function<Scene(double dt, const GazeRange&)>& update_cb) = 0;
};
