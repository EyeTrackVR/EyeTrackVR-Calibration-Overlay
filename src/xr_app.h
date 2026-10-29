#pragma once
#include "backend.h"
#include "xr_vk.h"
#include "vk_base.h"
#include "fov.h"

#include <functional>
#include <string>
#include <vector>

// Owns the OpenXR instance/session and the Vulkan device (created via
// XR_KHR_vulkan_enable2). Drives the frame loop and composites the target,
// ring and instruction panel as head-locked quad layers over an opaque grey
// projection background.
//
// NOTE: a focused OpenXR session becomes the runtime's scene application. On
// SteamVR that evicts any running game, so the OpenVR overlay backend (OvrApp)
// is preferred there; this backend is used for runtimes where that is fine.
// With XR_EXTX_overlay (e.g. Monado) it runs as an overlay session instead.
class XrApp : public IOverlayBackend {
public:
    // Only reads the runtime's extension list and does not start a session.
    static bool runtime_supports_overlay();

    bool init(const FovConfig& fov_cfg) override;
    void shutdown() override;

    // Pump session lifecycle. Returns false when the app should exit.
    bool poll_events() override;

    // Render one frame. When the runtime wants this frame rendered,
    // update_cb(dt, geometry) is called between begin/end to obtain the frame;
    // dt is seconds since the previous rendered frame. While hidden, untracked
    // or unfocused the callback is skipped (the calibration pauses). Returns
    // false if the loop should stop.
    bool frame(const std::function<Frame(double dt, const ViewGeometry&)>& update_cb) override;

private:
    struct Swapchain {
        XrSwapchain          handle = XR_NULL_HANDLE;
        std::vector<VkImage> images;
        uint32_t             w = 0, h = 0;
        uint32_t             acquired = UINT32_MAX;   // index while acquired this frame
    };
    struct QuadLayer {
        Swapchain              sc;
        Texture                tex;
        uint64_t               version = ~0ull;   // staged Image::version
        float                  alpha = -1.0f;     // staged alpha
        int                    cw = 0, ch = 0;    // content size within the texture
        XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    };

    bool create_instance();
    bool create_vulkan();           // XR_KHR_vulkan_enable2 instance + device
    bool create_session();
    bool create_swapchains();
    bool make_swapchain(uint32_t w, uint32_t h, Swapchain& out);
    bool make_quad(uint32_t w, uint32_t h, QuadLayer& q);
    bool acquire(Swapchain& sc);
    void release(Swapchain& sc);
    void load_visibility_masks();
    void update_geometry(XrTime t);
    // Stage/blit one quad for this frame and fill its composition layer.
    bool prepare_quad(VkCommandBuffer cmd, QuadLayer& q, const Quad& quad, const Image* img);

    // OpenXR
    XrInstance       instance_   = XR_NULL_HANDLE;
    XrSystemId       system_     = XR_NULL_SYSTEM_ID;
    XrSession        session_    = XR_NULL_HANDLE;
    XrSpace          view_space_ = XR_NULL_HANDLE;   // head-locked: quads, background, gaze
    XrViewConfigurationType view_cfg_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrEnvironmentBlendMode  blend_     = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrSessionState   state_      = XR_SESSION_STATE_UNKNOWN;
    bool             session_running_ = false;
    bool             exit_requested_  = false;
    bool             exiting_         = false;
    XrTime           prev_time_  = 0;
    int              end_frame_errors_ = 0;
    int              gpu_failures_     = 0;   // consecutive frames whose GPU work failed
    int              frame_errors_     = 0;   // wait/begin frame failures (log throttle)

    // Extensions
    bool has_visibility_mask_ = false;
    bool has_overlay_         = false;
    PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetReqs_    = nullptr;
    PFN_xrCreateVulkanInstanceKHR           pfnCreateInst_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR       pfnGetDevice_  = nullptr;
    PFN_xrCreateVulkanDeviceKHR             pfnCreateDev_  = nullptr;
    PFN_xrGetVisibilityMaskKHR              pfnVisMask_    = nullptr;

    // Vulkan
    VkContext vk_;

    // Content
    QuadLayer panel_, ring_, trail_, dot_;

    // Grey full-view background (opaque projection layer behind the quads).
    Swapchain proj_sc_[2];
    Texture   grey_tex_;
    int       grey_level_ = -1;            // uploaded grey (after fade)
    XrCompositionLayerProjection     proj_layer_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView proj_views_[2]{};

    // Geometry
    FovConfig    fov_cfg_;
    ViewGeometry geom_;
    XrView       views_[2]{};
    bool         views_valid_ = false;     // views_ located this frame
    XrView       geom_views_[2]{};          // views the current geometry was built from
    bool         have_geom_views_ = false;
    std::vector<Vec2> masks_[2];            // lens-visible line loops (tangent space)
    bool         masks_dirty_ = true;
    bool         gaze_warned_ = false;
};
