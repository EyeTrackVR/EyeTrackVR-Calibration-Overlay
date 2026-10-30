#pragma once
#include "backend.h"
#include "fov.h"

#include <cstdint>
#include <string>

#ifdef HAVE_OPENVR
#include <openvr.h>
#endif

// OpenVR overlay backend. Renders the target, instruction panel and an opaque
// grey backdrop as IVROverlay overlays composited on TOP of the running scene
// application — so launching a calibration never closes the current game, and
// quitting simply returns the user to it (no relaunch).
//
// init() attaches to an ALREADY-RUNNING SteamVR (VRApplication_Background) and
// returns false if SteamVR is not running, letting main() fall back to the
// OpenXR backend. Unlike the OpenXR scene session, overlays do not require a
// Vulkan device: textures are uploaded straight from CPU memory.
class OvrApp : public IOverlayBackend {
public:
    bool init(const FovConfig& fov_cfg) override;
    void shutdown() override;
    bool poll_events() override;
    bool frame(const std::function<Frame(double dt, const ViewGeometry&)>& update_cb) override;

    // True once SteamVR was found running, even if overlay setup then failed.
    // main() must not fall back to an OpenXR scene session in that case: on
    // SteamVR it would evict the game the user is calibrating over.
    bool steamvr_running() const { return steamvr_running_; }

private:
    bool steamvr_running_ = false;
#ifdef HAVE_OPENVR
    struct Overlay {
        vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
        bool shown = false;
        float alpha = -1.0f;
        float width = -1.0f;
        float texel_aspect = 1.0f;
        float rot = 0.0f;
        float x = 1e9f, y = 1e9f, z = 1e9f;
        uint64_t version = 0;         // uploaded Image::version
        uint64_t failed_version = 0;  // last version whose upload failed (log once)
    };

    // Resolve the binocular gaze range from the runtime's per-eye projection,
    // eye poses and lens visibility. Returns false (geometry left as fallback)
    // if the headset is not present or reports unusable values.
    bool compute_geometry();
    bool create(const std::string& name, uint32_t sort, Overlay& o);
    void apply(Overlay& o, const Quad& q, const Image* img);
    void apply_backdrop(uint8_t gray, float alpha);
    void destroy_all();

    vr::IVRSystem* sys_ = nullptr;
    Overlay bg_[5];  // grey backdrop: front, left, right, top, bottom
    Overlay panel_, ring_, trail_, dot_;
    uint8_t bg_gray_ = 0;
    float bg_alpha_ = -1.0f;
    bool bg_shown_ = false;
    std::string key_prefix_;
    bool geom_dirty_ = false;
    double next_geom_try_ = 0.0;
    int sync_failures_ = 0;
    double next_sync_retry_ = 0.0;
    bool geom_warned_ = false;
#endif

    FovConfig fov_cfg_;
    ViewGeometry geom_;
    bool first_frame_ = true;
    double prev_secs_ = 0.0;
};
