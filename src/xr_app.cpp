#include "xr_app.h"
#include "config.h"
#include "sprites.h"
#include "text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

constexpr XrPosef kIdentityPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

bool xr_ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    std::cout << "[ERROR] " << what << " failed (XrResult " << (int)r << ")\n";
    return false;
}

bool fatal(XrResult r) {
    return r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST ||
           r == XR_ERROR_RUNTIME_FAILURE || r == XR_ERROR_HANDLE_INVALID;
}

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

bool XrApp::runtime_supports_overlay() {
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (n) xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    for (const auto& p : props) {
        if (!std::strcmp(p.extensionName, XR_EXTX_OVERLAY_EXTENSION_NAME)) return true;
    }
    return false;
}

bool XrApp::init(const FovConfig& fov_cfg) {
    fov_cfg_ = fov_cfg;
    geom_ = fallback_view_geometry(fov_cfg_, cfg::OVERLAY_DIST);
    if (!create_instance())   return false;
    if (!create_vulkan())     return false;
    if (!create_session())    return false;
    if (!create_swapchains()) return false;
    load_visibility_masks();
    std::cout << "[INFO] OpenXR backend ready\n";
    return true;
}

bool XrApp::create_instance() {
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (n) xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    bool has_vk2 = false;
    for (const auto& p : props) {
        if (!std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) has_vk2 = true;
        if (!std::strcmp(p.extensionName, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME)) has_visibility_mask_ = true;
        if (!std::strcmp(p.extensionName, XR_EXTX_OVERLAY_EXTENSION_NAME)) has_overlay_ = true;
    }
    if (!has_vk2 && n > 0) {
        std::cout << "[ERROR] The active OpenXR runtime does not support XR_KHR_vulkan_enable2\n";
        return false;
    }
    std::vector<const char*> exts = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    if (has_visibility_mask_) exts.push_back(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
    if (has_overlay_) exts.push_back(XR_EXTX_OVERLAY_EXTENSION_NAME);

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "EyeTrackVR Calibration",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ci.applicationInfo.engineName, "etvr-overlay", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 2;
    // Request 1.0 (not XR_CURRENT_API_VERSION, which is 1.1 in recent headers):
    // SteamVR's and several other PC runtimes only implement OpenXR 1.0 and
    // reject a 1.1 instance outright.
    ci.applicationInfo.apiVersion         = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount              = (uint32_t)exts.size();
    ci.enabledExtensionNames              = exts.data();
    if (!xr_ok(xrCreateInstance(&ci, &instance_), "xrCreateInstance")) {
        std::cout << "[ERROR] No OpenXR runtime available? Ensure SteamVR/your runtime is active.\n";
        return false;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &ip)))
        std::cout << "[INFO] OpenXR runtime: " << ip.runtimeName << " "
                  << XR_VERSION_MAJOR(ip.runtimeVersion) << "." << XR_VERSION_MINOR(ip.runtimeVersion)
                  << "." << XR_VERSION_PATCH(ip.runtimeVersion)
                  << (has_visibility_mask_ ? " (visibility mask supported)" : "")
                  << (has_overlay_ ? " (overlay session)" : "") << "\n";

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    // The headset may not be presented through the active runtime the instant we
    // launch (it can connect a moment later). Retry on FORM_FACTOR_UNAVAILABLE
    // for a few seconds before giving up.
    XrResult sysr = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    for (int attempt = 0; attempt < 40; ++attempt) {
        sysr = xrGetSystem(instance_, &si, &system_);
        if (sysr != XR_ERROR_FORM_FACTOR_UNAVAILABLE) break;
        if (attempt == 0)
            std::cout << "[INFO] Waiting for an HMD on the active OpenXR runtime...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!xr_ok(sysr, "xrGetSystem")) {
        std::cout << "[ERROR] No HMD on the active OpenXR runtime.\n"
                     "        Make sure your headset is connected/streaming, and that the\n"
                     "        runtime you actually use for VR is set as the active OpenXR\n"
                     "        runtime (SteamVR: Settings > OpenXR > Set as default; or the\n"
                     "        Virtual Desktop / Oculus app, depending on how you connect).\n";
        return false;
    }

    uint32_t bn = 0;
    xrEnumerateEnvironmentBlendModes(instance_, system_, view_cfg_, 0, &bn, nullptr);
    if (bn > 0) {
        std::vector<XrEnvironmentBlendMode> modes(bn);
        xrEnumerateEnvironmentBlendModes(instance_, system_, view_cfg_, bn, &bn, modes.data());
        blend_ = modes[0];
        for (auto m : modes) if (m == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) blend_ = m;
    }

    if (has_visibility_mask_)
        xrGetInstanceProcAddr(instance_, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction*)&pfnVisMask_);
    return true;
}

bool XrApp::create_vulkan() {
    xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsRequirements2KHR",
                          (PFN_xrVoidFunction*)&pfnGetReqs_);
    xrGetInstanceProcAddr(instance_, "xrCreateVulkanInstanceKHR",
                          (PFN_xrVoidFunction*)&pfnCreateInst_);
    xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsDevice2KHR",
                          (PFN_xrVoidFunction*)&pfnGetDevice_);
    xrGetInstanceProcAddr(instance_, "xrCreateVulkanDeviceKHR",
                          (PFN_xrVoidFunction*)&pfnCreateDev_);
    if (!pfnGetReqs_ || !pfnCreateInst_ || !pfnGetDevice_ || !pfnCreateDev_) {
        std::cout << "[ERROR] XR_KHR_vulkan_enable2 entry points missing\n";
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!xr_ok(pfnGetReqs_(instance_, system_, &reqs), "xrGetVulkanGraphicsRequirements2KHR"))
        return false;

    // --- VkInstance (created by the runtime so it gets the extensions it needs) ---
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "EyeTrackVR Calibration";
    ai.pEngineName      = "none";
    ai.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vici.pApplicationInfo = &ai;

    XrVulkanInstanceCreateInfoKHR xvi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvi.systemId               = system_;
    xvi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xvi.vulkanCreateInfo       = &vici;

    VkResult vr = VK_SUCCESS;
    if (!xr_ok(pfnCreateInst_(instance_, &xvi, &vk_.instance, &vr), "xrCreateVulkanInstanceKHR")
        || vr != VK_SUCCESS) {
        std::cout << "[ERROR] VkInstance creation failed (VkResult " << (int)vr << ")\n";
        return false;
    }

    // --- VkPhysicalDevice the runtime requires ---
    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId       = system_;
    gi.vulkanInstance = vk_.instance;
    if (!xr_ok(pfnGetDevice_(instance_, &gi, &vk_.phys), "xrGetVulkanGraphicsDevice2KHR"))
        return false;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk_.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qps(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(vk_.phys, &qn, qps.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i) {
        if (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk_.queue_family = i; found = true; break; }
    }
    if (!found) { std::cout << "[ERROR] No graphics queue family\n"; return false; }

    // --- VkDevice ---
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk_.queue_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;

    XrVulkanDeviceCreateInfoKHR xdi{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdi.systemId               = system_;
    xdi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xdi.vulkanPhysicalDevice   = vk_.phys;
    xdi.vulkanCreateInfo       = &dci;
    if (!xr_ok(pfnCreateDev_(instance_, &xdi, &vk_.device, &vr), "xrCreateVulkanDeviceKHR")
        || vr != VK_SUCCESS) {
        std::cout << "[ERROR] VkDevice creation failed (VkResult " << (int)vr << ")\n";
        return false;
    }

    vkGetDeviceQueue(vk_.device, vk_.queue_family, 0, &vk_.queue);
    return vk_.init_pool();
}

bool XrApp::create_session() {
    XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance         = vk_.instance;
    gb.physicalDevice   = vk_.phys;
    gb.device           = vk_.device;
    gb.queueFamilyIndex = vk_.queue_family;
    gb.queueIndex       = 0;

    // Higher placements draw on top. Stay above other overlay apps during the
    // calibration (WayVR uses 5 and draws a skybox when no app is running).
    XrSessionCreateInfoOverlayEXTX overlay{XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};
    overlay.next                   = &gb;
    overlay.sessionLayersPlacement = 100;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next     = has_overlay_ ? static_cast<const void*>(&overlay) : &gb;
    sci.systemId = system_;
    if (!xr_ok(xrCreateSession(instance_, &sci, &session_), "xrCreateSession"))
        return false;

    // Everything is head-locked: quads, the uniform background and the gaze
    // geometry all live in VIEW space, so lost tracking never displaces them.
    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.poseInReferenceSpace = kIdentityPose;
    rci.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_VIEW;
    return xr_ok(xrCreateReferenceSpace(session_, &rci, &view_space_), "xrCreateReferenceSpace(VIEW)");
}

bool XrApp::make_swapchain(uint32_t w, uint32_t h, Swapchain& out) {
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(session_, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    if (fc) xrEnumerateSwapchainFormats(session_, fc, &fc, formats.data());

    // Prefer sRGB formats: the textures are sRGB-encoded and the blit then
    // round-trips them exactly.
    const int64_t prefs[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    int64_t chosen = formats.empty() ? (int64_t)VK_FORMAT_R8G8B8A8_SRGB : formats[0];
    bool picked = false;
    for (int64_t p : prefs) {
        for (int64_t f : formats) if (f == p) { chosen = p; picked = true; break; }
        if (picked) break;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format      = chosen;
    ci.sampleCount = 1;
    ci.width       = w;
    ci.height      = h;
    ci.faceCount   = 1;
    ci.arraySize   = 1;
    ci.mipCount    = 1;
    if (!xr_ok(xrCreateSwapchain(session_, &ci, &out.handle), "xrCreateSwapchain"))
        return false;

    uint32_t ic = 0;
    xrEnumerateSwapchainImages(out.handle, 0, &ic, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> imgs(ic, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (!ic || !xr_ok(xrEnumerateSwapchainImages(out.handle, ic, &ic,
                                                 (XrSwapchainImageBaseHeader*)imgs.data()),
                      "xrEnumerateSwapchainImages"))
        return false;
    out.images.clear();
    for (auto& im : imgs) out.images.push_back(im.image);
    out.w = w;
    out.h = h;
    return true;
}

bool XrApp::make_quad(uint32_t w, uint32_t h, QuadLayer& q) {
    return q.tex.create(vk_, w, h) && make_swapchain(w, h, q.sc);
}

bool XrApp::create_swapchains() {
    const Image& dot = sprites::dot();
    const Image& ring = sprites::ring();
    const Image& trail = sprites::trail();
    if (!make_quad((uint32_t)dot.w, (uint32_t)dot.h, dot_)) return false;
    if (!make_quad((uint32_t)ring.w, (uint32_t)ring.h, ring_)) return false;
    if (!make_quad((uint32_t)trail.w, (uint32_t)trail.h, trail_)) return false;
    if (!make_quad(TextRenderer::kMaxWidth, TextRenderer::kMaxHeight, panel_)) return false;

    // Grey background: one projection swapchain per eye at the recommended
    // render resolution, filled each frame by upscaling a tiny grey texture.
    uint32_t vc = 0;
    xrEnumerateViewConfigurationViews(instance_, system_, view_cfg_, 0, &vc, nullptr);
    std::vector<XrViewConfigurationView> vcv(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (vc) xrEnumerateViewConfigurationViews(instance_, system_, view_cfg_, vc, &vc, vcv.data());
    if (vc < 2) { std::cout << "[ERROR] Expected 2 views\n"; return false; }
    if (!grey_tex_.create(vk_, 4, 4)) return false;
    for (int e = 0; e < 2; ++e) {
        // The background is a flat colour: a reduced resolution is plenty and
        // keeps per-frame blits cheap on very high-resolution headsets.
        const uint32_t w = std::max(64u, vcv[e].recommendedImageRectWidth / 2);
        const uint32_t h = std::max(64u, vcv[e].recommendedImageRectHeight / 2);
        if (!make_swapchain(w, h, proj_sc_[e])) return false;
    }
    return true;
}

void XrApp::load_visibility_masks() {
    masks_dirty_ = false;
    if (!pfnVisMask_) return;
    for (uint32_t i = 0; i < 2; ++i) {
        masks_[i].clear();
        XrVisibilityMaskKHR m{XR_TYPE_VISIBILITY_MASK_KHR};
        if (XR_FAILED(pfnVisMask_(session_, view_cfg_, i, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, &m)) ||
            m.vertexCountOutput < 3)
            continue;
        std::vector<XrVector2f> v(m.vertexCountOutput);
        std::vector<uint32_t> idx(m.indexCountOutput);
        m.vertexCapacityInput = (uint32_t)v.size();
        m.vertices            = v.data();
        m.indexCapacityInput  = (uint32_t)idx.size();
        m.indices             = idx.empty() ? nullptr : idx.data();
        if (XR_FAILED(pfnVisMask_(session_, view_cfg_, i, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, &m)))
            continue;
        // Vertices lie on the view's z = -1 plane, i.e. they are Y-up tangents.
        if (!idx.empty()) {
            for (uint32_t k = 0; k < m.indexCountOutput; ++k)
                if (idx[k] < v.size()) masks_[i].push_back({v[idx[k]].x, v[idx[k]].y});
        } else {
            for (const auto& p : v) masks_[i].push_back({p.x, p.y});
        }
    }
}

void XrApp::update_geometry(XrTime t) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = view_cfg_;
    li.displayTime           = t;
    li.space                 = view_space_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    views_[0].type = views_[1].type = XR_TYPE_VIEW;
    views_valid_ = false;
    if (XR_FAILED(xrLocateViews(session_, &li, &vs, 2, &count, views_)) || count < 2) return;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;
    views_valid_ = true;

    auto differs = [](const XrView& a, const XrView& b) {
        auto d = [](float x, float y) { return std::fabs(x - y) > 1e-4f; };
        return d(a.fov.angleLeft, b.fov.angleLeft) || d(a.fov.angleRight, b.fov.angleRight) ||
               d(a.fov.angleUp, b.fov.angleUp) || d(a.fov.angleDown, b.fov.angleDown) ||
               d(a.pose.position.x, b.pose.position.x) || d(a.pose.position.y, b.pose.position.y) ||
               d(a.pose.position.z, b.pose.position.z) ||
               d(a.pose.orientation.x, b.pose.orientation.x) || d(a.pose.orientation.y, b.pose.orientation.y) ||
               d(a.pose.orientation.z, b.pose.orientation.z) || d(a.pose.orientation.w, b.pose.orientation.w);
    };
    if (masks_dirty_) { load_visibility_masks(); have_geom_views_ = false; }
    if (have_geom_views_ && !differs(views_[0], geom_views_[0]) && !differs(views_[1], geom_views_[1]))
        return;

    EyeGeom eyes[2];
    for (int e = 0; e < 2; ++e) {
        const XrView& v = views_[e];
        eyes[e].pose.p = {v.pose.position.x, v.pose.position.y, v.pose.position.z};
        eyes[e].pose.q = {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z,
                          v.pose.orientation.w};
        if (!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
            eyes[e].pose.p = {e ? 0.0315f : -0.0315f, 0.0f, 0.0f};
        eyes[e].tan_left  = std::tan(v.fov.angleLeft);
        eyes[e].tan_right = std::tan(v.fov.angleRight);
        eyes[e].tan_up    = std::tan(v.fov.angleUp);
        eyes[e].tan_down  = std::tan(v.fov.angleDown);
        eyes[e].visible   = masks_[e];
    }
    std::string notes;
    ViewGeometry g;
    geom_views_[0] = views_[0];
    geom_views_[1] = views_[1];
    have_geom_views_ = true;
    if (!compute_view_geometry(eyes, cfg::OVERLAY_DIST, fov_cfg_, g, &notes)) {
        if (!gaze_warned_) {
            gaze_warned_ = true;
            std::cout << "[WARN] Could not resolve eye frusta (" << notes << "); using fallback gaze range\n";
        }
        return;
    }
    const bool first = geom_.synthetic;
    geom_ = g;
    for (int e = 0; e < 2; ++e) log_eye(e ? "R" : "L", g.eyes[e]);
    if (!notes.empty()) std::cout << "[INFO] Geometry: " << notes << "\n";
    if (!first) std::cout << "[INFO] Headset geometry changed; targets follow the new range\n";
    log_view_geometry(geom_, fov_cfg_, cfg::OVERLAY_DIST);
}

bool XrApp::acquire(Swapchain& sc) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(sc.handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(sc.handle, &wi))) return false;   // runtime is failing; frame errors follow
    if (idx >= sc.images.size()) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc.handle, &ri);
        return false;
    }
    sc.acquired = idx;
    return true;
}

void XrApp::release(Swapchain& sc) {
    if (sc.acquired == UINT32_MAX) return;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
    sc.acquired = UINT32_MAX;
}

bool XrApp::prepare_quad(VkCommandBuffer cmd, QuadLayer& q, const Quad& quad, const Image* img) {
    if (!quad.visible || quad.alpha <= 0.002f || !img || img->w <= 0 ||
        (uint32_t)img->w > q.tex.w || (uint32_t)img->h > q.tex.h)
        return false;
    // Quantised layer opacity, applied to the texture's alpha on the CPU (no
    // reliance on per-layer colour-scale extensions, which runtimes vary on).
    const float a = std::round(std::clamp(quad.alpha, 0.0f, 1.0f) * 128.0f) / 128.0f;
    if (img->version != q.version || a != q.alpha) {
        q.tex.stage(img->rgba.data(), (uint32_t)img->w, (uint32_t)img->h, a);
        q.tex.record_upload(cmd, (uint32_t)img->w, (uint32_t)img->h);
        q.version = img->version;
        q.alpha = a;
        q.cw = img->w;
        q.ch = img->h;
    }
    if (!acquire(q.sc)) return false;
    record_blit(cmd, q.tex, (uint32_t)q.cw, (uint32_t)q.ch, q.sc.images[q.sc.acquired],
                (uint32_t)q.cw, (uint32_t)q.ch,
                (uint32_t)q.cw < q.sc.w || (uint32_t)q.ch < q.sc.h);

    q.layer = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                       | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    q.layer.space                    = view_space_;
    q.layer.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
    q.layer.subImage.swapchain       = q.sc.handle;
    q.layer.subImage.imageRect       = {{0, 0}, {q.cw, q.ch}};
    q.layer.subImage.imageArrayIndex = 0;
    q.layer.pose                     = kIdentityPose;
    q.layer.pose.position            = {quad.x, quad.y, quad.z};
    if (quad.rot != 0.0f) {
        const Quat r = quat_axis_angle({0.0f, 0.0f, 1.0f}, quad.rot);
        q.layer.pose.orientation = {r.x, r.y, r.z, r.w};
    }
    q.layer.size                     = {quad.w, quad.h};
    return true;
}

bool XrApp::poll_events() {
    while (true) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult r = xrPollEvent(instance_, &ev);
        if (r != XR_SUCCESS) break;   // XR_EVENT_UNAVAILABLE or error

        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            std::cout << "[WARN] OpenXR instance loss pending\n";
            exiting_ = true;
            return false;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            state_ = e->state;
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = view_cfg_;
                if (xr_ok(xrBeginSession(session_, &bi), "xrBeginSession")) {
                    session_running_ = true;
                } else {
                    exiting_ = true;
                    return false;
                }
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session_);
                session_running_ = false;
                if (exit_requested_) { exiting_ = true; return false; }
            } else if (state_ == XR_SESSION_STATE_EXITING ||
                       state_ == XR_SESSION_STATE_LOSS_PENDING) {
                exiting_ = true;
                return false;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
            masks_dirty_ = true;
            break;
        default:
            break;
        }
    }
    return !exiting_;
}

bool XrApp::frame(const std::function<Frame(double, const ViewGeometry&)>& update_cb) {
    if (exiting_) return false;
    if (!session_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return true;
    }

    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(session_, &fwi, &fs);
    auto frame_failed = [&](const char* what) {
        if (frame_errors_++ < 5) xr_ok(r, what);
        if (fatal(r) || frame_errors_ > 300) return false;   // ~seconds of failures: give up
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
    };
    if (XR_FAILED(r)) return frame_failed("xrWaitFrame");
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(session_, &fbi);
    if (XR_FAILED(r)) return frame_failed("xrBeginFrame");
    frame_errors_ = 0;

    double dt = prev_time_ ? (double)(fs.predictedDisplayTime - prev_time_) * 1e-9 : 0.0;
    dt = std::clamp(dt, 0.0, 0.1);
    prev_time_ = fs.predictedDisplayTime;

    std::vector<XrCompositionLayerBaseHeader*> layers;
    if (fs.shouldRender && update_cb) update_geometry(fs.predictedDisplayTime);
    // Pause the calibration while its target can't be seen: no tracked view, or
    // a normal session that another app has taken focus from. Capture signals
    // would otherwise fire for dots the user never saw.
    const bool can_see = views_valid_ && (has_overlay_ || state_ == XR_SESSION_STATE_FOCUSED);
    if (fs.shouldRender && update_cb && can_see) {
        const Frame f = update_cb(dt, geom_);

        VkCommandBuffer cmd = vk_.begin();
        if (cmd == VK_NULL_HANDLE) {
            // The GPU is gone: stop rather than keep emitting capture signals
            // for targets that can no longer be shown.
            XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
            fe.displayTime = fs.predictedDisplayTime;
            fe.environmentBlendMode = blend_;
            xrEndFrame(session_, &fe);
            return false;
        }

        // Opaque background. There is no game behind a scene session, so the
        // backdrop "fade" is a fade from/to black.
        const int level = (int)std::lround(f.backdrop_gray * std::clamp(f.backdrop_alpha, 0.0f, 1.0f));
        if (level != grey_level_) {
            uint8_t px[4 * 4 * 4];
            for (int i = 0; i < 16; ++i) {
                px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = (uint8_t)level;
                px[i * 4 + 3] = 255;
            }
            grey_tex_.stage(px, 4, 4);
            grey_tex_.record_upload(cmd, 4, 4);
            grey_level_ = level;
        }
        bool bg_ok = views_valid_;
        for (int e = 0; e < 2 && bg_ok; ++e) {
            bg_ok = acquire(proj_sc_[e]);
            if (bg_ok)
                record_blit(cmd, grey_tex_, 4, 4, proj_sc_[e].images[proj_sc_[e].acquired],
                            proj_sc_[e].w, proj_sc_[e].h);
        }
        const bool panel_ok = prepare_quad(cmd, panel_, f.panel, f.panel_image);
        const bool ring_ok  = prepare_quad(cmd, ring_, f.ring, &sprites::ring());
        const bool trail_ok = prepare_quad(cmd, trail_, f.trail, &sprites::trail());
        const bool dot_ok   = prepare_quad(cmd, dot_, f.dot, &sprites::dot());

        const bool gpu_ok = vk_.submit_wait();
        for (auto* sc : {&proj_sc_[0], &proj_sc_[1], &panel_.sc, &ring_.sc, &trail_.sc, &dot_.sc})
            release(*sc);
        if (!gpu_ok) {
            // Uploads recorded this frame never happened: force them again.
            grey_level_ = -1;
            for (auto* q : {&panel_, &ring_, &trail_, &dot_}) q->version = ~0ull;
            if (++gpu_failures_ >= 3) {
                std::cout << "[ERROR] Repeated GPU failures; stopping the calibration\n";
                XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
                fe.displayTime = fs.predictedDisplayTime;
                fe.environmentBlendMode = blend_;
                xrEndFrame(session_, &fe);
                return false;
            }
        } else {
            gpu_failures_ = 0;
        }

        if (gpu_ok) {
            if (bg_ok) {
                for (int e = 0; e < 2; ++e) {
                    proj_views_[e] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    proj_views_[e].pose = views_[e].pose;
                    proj_views_[e].fov  = views_[e].fov;
                    proj_views_[e].subImage.swapchain = proj_sc_[e].handle;
                    proj_views_[e].subImage.imageRect = {{0, 0}, {(int32_t)proj_sc_[e].w, (int32_t)proj_sc_[e].h}};
                    proj_views_[e].subImage.imageArrayIndex = 0;
                }
                proj_layer_ = XrCompositionLayerProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                proj_layer_.layerFlags = 0;   // opaque
                proj_layer_.space      = view_space_;
                proj_layer_.viewCount  = 2;
                proj_layer_.views      = proj_views_;
                layers.push_back((XrCompositionLayerBaseHeader*)&proj_layer_);
            }
            if (panel_ok) layers.push_back((XrCompositionLayerBaseHeader*)&panel_.layer);
            if (ring_ok)  layers.push_back((XrCompositionLayerBaseHeader*)&ring_.layer);
            if (trail_ok) layers.push_back((XrCompositionLayerBaseHeader*)&trail_.layer);
            if (dot_ok)   layers.push_back((XrCompositionLayerBaseHeader*)&dot_.layer);
        }
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime          = fs.predictedDisplayTime;
    fei.environmentBlendMode = blend_;
    fei.layerCount           = (uint32_t)layers.size();
    fei.layers               = layers.data();
    r = xrEndFrame(session_, &fei);
    if (XR_FAILED(r)) {
        if (end_frame_errors_++ == 0)
            std::cout << "[ERROR] xrEndFrame failed (XrResult " << (int)r
                      << ") — layers may be rejected by this runtime\n";
        if (fatal(r)) return false;
    }
    return true;
}

void XrApp::shutdown() {
    // End the session the way the spec asks (request exit, keep the frame loop
    // alive until STOPPING) so runtimes return cleanly to their home/idle state.
    if (session_ && session_running_ && !exiting_) {
        exit_requested_ = true;
        if (XR_SUCCEEDED(xrRequestExitSession(session_))) {
            const double deadline = now_s() + 1.5;
            while (session_running_ && now_s() < deadline) {
                if (!poll_events()) break;
                if (session_running_ && !frame({})) break;
            }
        }
    }

    if (vk_.device) vkDeviceWaitIdle(vk_.device);

    for (auto* sc : {&dot_.sc, &ring_.sc, &trail_.sc, &panel_.sc, &proj_sc_[0], &proj_sc_[1]})
        if (sc->handle) { xrDestroySwapchain(sc->handle); sc->handle = XR_NULL_HANDLE; }
    if (vk_.device) {
        dot_.tex.destroy(vk_);
        ring_.tex.destroy(vk_);
        trail_.tex.destroy(vk_);
        panel_.tex.destroy(vk_);
        grey_tex_.destroy(vk_);
        vk_.destroy();
    }

    if (view_space_)  { xrDestroySpace(view_space_); view_space_ = XR_NULL_HANDLE; }
    if (session_)     { xrDestroySession(session_); session_ = XR_NULL_HANDLE; }
    if (vk_.device)   { vkDestroyDevice(vk_.device, nullptr); vk_.device = VK_NULL_HANDLE; }
    if (vk_.instance) { vkDestroyInstance(vk_.instance, nullptr); vk_.instance = VK_NULL_HANDLE; }
    if (instance_)    { xrDestroyInstance(instance_); instance_ = XR_NULL_HANDLE; }
}
