#pragma once
#include "backend.h"
#include "xr_vk.h"
#include "vk_base.h"
#include "fov.h"
#include "text.h"

#include <functional>
#include <string>
#include <vector>

// Owns the OpenXR instance/session and the Vulkan device (created via
// XR_KHR_vulkan_enable2). Drives the frame loop and composites the dot + text as
// head-locked quad layers over an opaque grey background projection.
//
// NOTE: a focused OpenXR session becomes the runtime's scene application. On
// SteamVR that evicts any running game, so the OpenVR overlay backend (OvrApp)
// is preferred there; this backend is used for runtimes where that is fine.
class XrApp : public IOverlayBackend {
public:
    bool init(const FovConfig& fov_cfg, const char* dot_png, const char* font_ttf) override;
    void shutdown() override;

    // Pump session lifecycle. Returns false when the app should exit.
    bool poll_events() override;
    bool session_running() const { return session_running_; }

    // Render one frame. When the headset is visible/focused, update_cb(dt, gaze)
    // is called between begin/end to obtain the scene; dt is seconds since the
    // previous rendered frame. When not focused the callback is skipped (the
    // calibration pauses). Returns false if the loop should stop.
    bool frame(const std::function<Scene(double dt, const GazeRange&)>& update_cb) override;

    const GazeRange& gaze() const { return gaze_; }

private:
    struct QuadSwapchain {
        XrSwapchain          handle = XR_NULL_HANDLE;
        std::vector<VkImage> images;
        uint32_t             w = 0, h = 0;
    };

    bool create_instance();
    bool create_vulkan();           // XR_KHR_vulkan_enable2 instance + device
    bool create_session();
    bool create_swapchains(const char* dot_png);
    bool make_swapchain(uint32_t w, uint32_t h, QuadSwapchain& out);
    void blit_into_swapchain(QuadSwapchain& sc, const Texture& tex);
    void render_quad(QuadSwapchain& sc, const Texture& tex,
                     XrPosef pose, float w_m, float h_m,
                     XrCompositionLayerQuad& layer);
    void render_background(std::vector<XrCompositionLayerBaseHeader*>& layers);
    void locate_and_update_gaze(XrTime t);

    // OpenXR
    XrInstance       instance_   = XR_NULL_HANDLE;
    XrSystemId       system_     = XR_NULL_SYSTEM_ID;
    XrSession        session_    = XR_NULL_HANDLE;
    XrSpace          view_space_  = XR_NULL_HANDLE;   // head-locked (quads, gaze)
    XrSpace          local_space_ = XR_NULL_HANDLE;   // world-locked (background)
    XrViewConfigurationType view_cfg_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrEnvironmentBlendMode  blend_     = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrSessionState   state_      = XR_SESSION_STATE_UNKNOWN;
    bool             session_running_ = false;
    XrTime           prev_time_  = 0;

    // XR_KHR_vulkan_enable2 entry points
    PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetReqs_   = nullptr;
    PFN_xrCreateVulkanInstanceKHR           pfnCreateInst_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR       pfnGetDevice_  = nullptr;
    PFN_xrCreateVulkanDeviceKHR             pfnCreateDev_  = nullptr;

    // Vulkan
    VkContext vk_;

    // Content
    QuadSwapchain dot_sc_, text_sc_;
    Texture       dot_tex_, text_tex_;
    TextRenderer  text_render_;
    std::string   last_text_;
    int           dot_w_ = 1, dot_h_ = 1;   // PNG pixel dims (for aspect)

    // Grey full-view background (opaque projection layer behind the quads).
    QuadSwapchain proj_sc_[2];               // one per eye
    Texture       grey_tex_;
    uint32_t      proj_w_ = 0, proj_h_ = 0;

    // Per-frame view/FOV
    FovConfig  fov_cfg_;
    GazeRange  gaze_;
    XrView     views_[2]{};         // located in VIEW space (gaze geometry)
    XrView     local_views_[2]{};   // located in LOCAL space (background projection)
    bool       gaze_logged_ = false;

    // Composition layers kept alive across xrEndFrame.
    XrCompositionLayerQuad            dot_layer_{};
    XrCompositionLayerQuad            text_layer_{};
    XrCompositionLayerProjection      proj_layer_{};
    XrCompositionLayerProjectionView  proj_views_[2]{};
};
