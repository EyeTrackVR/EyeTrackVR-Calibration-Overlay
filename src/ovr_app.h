#pragma once
#include "backend.h"
#include "fov.h"
#include "text.h"

#include <string>

#ifdef HAVE_OPENVR
#include <openvr.h>
#endif

// OpenVR overlay backend. Renders the dot, instruction text and an opaque grey
// backdrop as IVROverlay overlays composited on TOP of the running scene
// application — so launching a calibration never closes the current game, and
// quitting simply returns the user to it (no relaunch).
//
// init() attaches to an ALREADY-RUNNING SteamVR (VRApplication_Background) and
// returns false if SteamVR is not running, letting main() fall back to the
// OpenXR backend. Unlike the OpenXR scene session, overlays do not require a
// Vulkan device: textures are uploaded straight from CPU memory.
class OvrApp : public IOverlayBackend {
public:
    bool init(const FovConfig& fov_cfg, const char* dot_png, const char* font_ttf) override;
    void shutdown() override;
    bool poll_events() override;
    bool frame(const std::function<Scene(double dt, const GazeRange&)>& update_cb) override;

private:
#ifdef HAVE_OPENVR
    // Build the binocular gaze range from the runtime's per-eye projection.
    bool compute_gaze();
    // Head-locked transform: place overlay h at (x, y, z) in HMD space.
    void set_transform(vr::VROverlayHandle_t h, float x, float y, float z);

    vr::IVRSystem*        sys_    = nullptr;
    vr::VROverlayHandle_t bg_h_   = vr::k_ulOverlayHandleInvalid;   // grey backdrop
    vr::VROverlayHandle_t dot_h_  = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t text_h_ = vr::k_ulOverlayHandleInvalid;
    bool                  dot_shown_  = false;
    bool                  text_shown_ = false;
    uint8_t               backdrop_gray_ = 96;
#endif

    FovConfig    fov_cfg_;
    GazeRange    gaze_;
    TextRenderer text_render_;
    std::string  last_text_;
    bool         have_text_   = false;
    bool         first_frame_ = true;
    double       prev_secs_   = 0.0;
};
