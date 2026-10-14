#include "xr_app.h"
#include "config.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace {

constexpr XrPosef kIdentityPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

bool xr_ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    std::cout << "[ERROR] " << what << " failed (XrResult " << (int)r << ")\n";
    return false;
}

} // namespace

bool XrApp::init(const FovConfig& fov_cfg, const char* dot_png, const char* font_ttf) {
    fov_cfg_ = fov_cfg;
    if (!create_instance())  return false;
    if (!create_vulkan())    return false;
    if (!create_session())   return false;

    text_render_.load(font_ttf, 1024, 300);   // best-effort; text simply omitted if absent
    if (!create_swapchains(dot_png)) return false;
    return true;
}

bool XrApp::create_instance() {
    const char* exts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "EyeTrackVR Calibration",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    // Request 1.0 (not XR_CURRENT_API_VERSION, which is 1.1 in recent headers):
    // SteamVR's and several other PC runtimes only implement OpenXR 1.0 and
    // reject a 1.1 instance outright.
    ci.applicationInfo.apiVersion         = XR_API_VERSION_1_0;
    ci.enabledExtensionCount              = 1;
    ci.enabledExtensionNames              = exts;
    if (!xr_ok(xrCreateInstance(&ci, &instance_), "xrCreateInstance")) {
        std::cout << "[ERROR] No OpenXR runtime available? Ensure SteamVR/your runtime is active.\n";
        return false;
    }

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

    uint32_t n = 0;
    xrEnumerateEnvironmentBlendModes(instance_, system_, view_cfg_, 0, &n, nullptr);
    if (n > 0) {
        std::vector<XrEnvironmentBlendMode> modes(n);
        xrEnumerateEnvironmentBlendModes(instance_, system_, view_cfg_, n, &n, modes.data());
        blend_ = modes[0];
    }
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
    if (!xr_ok(pfnGetReqs_(instance_, system_, &reqs),
               "xrGetVulkanGraphicsRequirements2KHR"))
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

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next     = &gb;
    sci.systemId = system_;
    if (!xr_ok(xrCreateSession(instance_, &sci, &session_), "xrCreateSession"))
        return false;

    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.poseInReferenceSpace = kIdentityPose;
    rci.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_VIEW;    // head-locked
    if (!xr_ok(xrCreateReferenceSpace(session_, &rci, &view_space_),
               "xrCreateReferenceSpace(VIEW)"))
        return false;
    rci.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;   // world-locked
    return xr_ok(xrCreateReferenceSpace(session_, &rci, &local_space_),
                 "xrCreateReferenceSpace(LOCAL)");
}

bool XrApp::make_swapchain(uint32_t w, uint32_t h, QuadSwapchain& out) {
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(session_, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    xrEnumerateSwapchainFormats(session_, fc, &fc, formats.data());

    const int64_t prefs[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    int64_t chosen = formats.empty() ? (int64_t)VK_FORMAT_R8G8B8A8_SRGB : formats[0];
    for (int64_t p : prefs) {
        for (int64_t f : formats) if (f == p) { chosen = p; goto picked; }
    }
picked:
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
    xrEnumerateSwapchainImages(out.handle, ic, &ic,
                               (XrSwapchainImageBaseHeader*)imgs.data());
    out.images.clear();
    for (auto& im : imgs) out.images.push_back(im.image);
    out.w = w;
    out.h = h;
    return true;
}

bool XrApp::create_swapchains(const char* dot_png) {
    int n = 0;
    unsigned char* px = stbi_load(dot_png, &dot_w_, &dot_h_, &n, 4);
    if (!px) {
        std::cout << "[ERROR] Could not load dot image: " << dot_png << "\n";
        return false;
    }
    if (!dot_tex_.create(vk_, (uint32_t)dot_w_, (uint32_t)dot_h_)) { stbi_image_free(px); return false; }
    dot_tex_.upload(vk_, px);
    stbi_image_free(px);
    if (!make_swapchain((uint32_t)dot_w_, (uint32_t)dot_h_, dot_sc_)) return false;

    if (text_render_.ready()) {
        if (!text_tex_.create(vk_, (uint32_t)text_render_.width(),
                                   (uint32_t)text_render_.height()))
            return false;
        text_render_.render(" ");                 // prime the image layout
        text_tex_.upload(vk_, text_render_.pixels());
        last_text_ = " ";
        if (!make_swapchain((uint32_t)text_render_.width(),
                            (uint32_t)text_render_.height(), text_sc_))
            return false;
    }

    // Grey background: one projection swapchain per eye at the recommended
    // render resolution, filled each frame by upscaling a small grey texture.
    uint32_t vc = 0;
    xrEnumerateViewConfigurationViews(instance_, system_, view_cfg_, 0, &vc, nullptr);
    std::vector<XrViewConfigurationView> vcv(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance_, system_, view_cfg_, vc, &vc, vcv.data());
    if (vc < 2) { std::cout << "[ERROR] Expected 2 views\n"; return false; }
    proj_w_ = vcv[0].recommendedImageRectWidth;
    proj_h_ = vcv[0].recommendedImageRectHeight;

    constexpr uint8_t kGrey = 96;
    std::vector<uint8_t> grey((size_t)64 * 64 * 4);
    for (size_t i = 0; i < grey.size(); i += 4) {
        grey[i + 0] = kGrey; grey[i + 1] = kGrey; grey[i + 2] = kGrey; grey[i + 3] = 255;
    }
    if (!grey_tex_.create(vk_, 64, 64)) return false;
    grey_tex_.upload(vk_, grey.data());
    for (int e = 0; e < 2; ++e)
        if (!make_swapchain(proj_w_, proj_h_, proj_sc_[e])) return false;

    return true;
}

void XrApp::locate_and_update_gaze(XrTime t) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = view_cfg_;
    li.displayTime           = t;
    li.space                 = view_space_;

    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;

    // LOCAL-space views drive the world-locked grey background projection.
    li.space = local_space_;
    local_views_[0].type = local_views_[1].type = XR_TYPE_VIEW;
    xrLocateViews(session_, &li, &vs, 2, &count, local_views_);

    // VIEW-space views (eye offsets) drive the gaze geometry.
    li.space = view_space_;
    views_[0].type = views_[1].type = XR_TYPE_VIEW;
    if (!XR_SUCCEEDED(xrLocateViews(session_, &li, &vs, 2, &count, views_)) || count < 2)
        return;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return;

    if (compute_gaze_range(views_, cfg::OVERLAY_DIST, fov_cfg_, gaze_)) {
        if (!gaze_logged_) {
            gaze_logged_ = true;
            for (int e = 0; e < 2; ++e)
                std::cout << "[INFO] Eye " << (e ? "R" : "L") << ": fov up/down "
                          << views_[e].fov.angleUp * 57.2957795f << "/"
                          << views_[e].fov.angleDown * 57.2957795f << "°\n";
            log_gaze_range(gaze_, fov_cfg_, cfg::OVERLAY_DIST);
        }
    } else if (!gaze_warned_) {
        gaze_warned_ = true;
        std::cout << "[WARN] Could not resolve eye frusta; using fallback gaze range\n";
    }
}

void XrApp::blit_into_swapchain(QuadSwapchain& sc, const Texture& tex) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc.handle, &ai, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc.handle, &wi);

    blit_texture(vk_, tex, sc.images[idx], sc.w, sc.h);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
}

// Opaque grey background filling the whole view, submitted behind the quads.
void XrApp::render_background(std::vector<XrCompositionLayerBaseHeader*>& layers) {
    for (int e = 0; e < 2; ++e) {
        blit_into_swapchain(proj_sc_[e], grey_tex_);
        proj_views_[e] = XrCompositionLayerProjectionView{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        proj_views_[e].pose                 = local_views_[e].pose;
        proj_views_[e].fov                  = local_views_[e].fov;
        proj_views_[e].subImage.swapchain   = proj_sc_[e].handle;
        proj_views_[e].subImage.imageRect   = {{0, 0},
            {(int32_t)proj_sc_[e].w, (int32_t)proj_sc_[e].h}};
        proj_views_[e].subImage.imageArrayIndex = 0;
    }
    proj_layer_ = XrCompositionLayerProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    proj_layer_.layerFlags = 0;              // opaque
    proj_layer_.space      = local_space_;   // world-locked background
    proj_layer_.viewCount  = 2;
    proj_layer_.views      = proj_views_;
    layers.push_back((XrCompositionLayerBaseHeader*)&proj_layer_);
}

void XrApp::render_quad(QuadSwapchain& sc, const Texture& tex,
                        XrPosef pose, float w_m, float h_m,
                        XrCompositionLayerQuad& layer) {
    blit_into_swapchain(sc, tex);

    layer = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                     | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    layer.space                  = view_space_;
    layer.eyeVisibility          = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain     = sc.handle;
    layer.subImage.imageRect     = {{0, 0}, {(int32_t)sc.w, (int32_t)sc.h}};
    layer.subImage.imageArrayIndex = 0;
    layer.pose                   = pose;
    layer.size                   = {w_m, h_m};
}

bool XrApp::poll_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (true) {
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult r = xrPollEvent(instance_, &ev);
        if (r == XR_EVENT_UNAVAILABLE) break;
        if (!XR_SUCCEEDED(r)) break;

        if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            return false;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            state_ = e->state;
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = view_cfg_;
                xrBeginSession(session_, &bi);
                session_running_ = true;
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session_);
                session_running_ = false;
            } else if (state_ == XR_SESSION_STATE_EXITING ||
                       state_ == XR_SESSION_STATE_LOSS_PENDING) {
                return false;
            }
        }
    }
    return true;
}

bool XrApp::frame(const std::function<Scene(double, const GazeRange&)>& update_cb) {
    if (!session_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return true;
    }

    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    xrWaitFrame(session_, &fwi, &fs);

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(session_, &fbi);

    double dt = prev_time_ ? (double)(fs.predictedDisplayTime - prev_time_) * 1e-9 : 0.0;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;
    prev_time_ = fs.predictedDisplayTime;

    std::vector<XrCompositionLayerBaseHeader*> layers;
    if (fs.shouldRender) {
        locate_and_update_gaze(fs.predictedDisplayTime);
        Scene scene = update_cb(dt, gaze_);

        if (scene.backdrop_gray != backdrop_gray_) {
            backdrop_gray_ = scene.backdrop_gray;
            std::vector<uint8_t> pixels((size_t)64 * 64 * 4);
            for (size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = pixels[i + 1] = pixels[i + 2] = backdrop_gray_;
                pixels[i + 3] = 255;
            }
            grey_tex_.upload(vk_, pixels.data());
        }

        // Opaque grey background behind everything.
        render_background(layers);

        // Text first so the dot composites on top of it.
        if (scene.text_visible && text_render_.ready()) {
            if (scene.text != last_text_) {
                last_text_ = scene.text;
                text_render_.render(scene.text);
                text_tex_.upload(vk_, text_render_.pixels());
            }
            const float wm = 2.25f;
            const float hm = wm * ((float)text_sc_.h / (float)text_sc_.w);
            XrPosef p = kIdentityPose;
            const float text_scale = cfg::TEXT_DIST / cfg::OVERLAY_DIST;
            p.position = {gaze_.center_x * text_scale,
                          (gaze_.center_y - 0.825f) * text_scale,
                          -cfg::TEXT_DIST};
            render_quad(text_sc_, text_tex_, p, wm, hm, text_layer_);
            layers.push_back((XrCompositionLayerBaseHeader*)&text_layer_);
        }
        if (scene.dot_visible) {
            const float hm = scene.dot_size * ((float)dot_h_ / (float)dot_w_);
            XrPosef p = kIdentityPose;
            p.position = {scene.dot_x, scene.dot_y, -cfg::OVERLAY_DIST};
            render_quad(dot_sc_, dot_tex_, p, scene.dot_size, hm, dot_layer_);
            layers.push_back((XrCompositionLayerBaseHeader*)&dot_layer_);
        }
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime          = fs.predictedDisplayTime;
    fei.environmentBlendMode = blend_;
    fei.layerCount           = (uint32_t)layers.size();
    fei.layers               = layers.data();
    XrResult er = xrEndFrame(session_, &fei);
    if (!XR_SUCCEEDED(er)) {
        static bool logged = false;
        if (!logged) { logged = true;
            std::cout << "[ERROR] xrEndFrame failed (XrResult " << (int)er
                      << ") — layers may be rejected by this runtime\n";
        }
    }
    return true;
}

void XrApp::shutdown() {
    if (vk_.device) vkDeviceWaitIdle(vk_.device);

    if (dot_sc_.handle)  xrDestroySwapchain(dot_sc_.handle);
    if (text_sc_.handle) xrDestroySwapchain(text_sc_.handle);
    for (auto& sc : proj_sc_) if (sc.handle) xrDestroySwapchain(sc.handle);
    if (vk_.device) {
        dot_tex_.destroy(vk_);
        text_tex_.destroy(vk_);
        grey_tex_.destroy(vk_);
    }
    vk_.destroy();

    if (view_space_)  xrDestroySpace(view_space_);
    if (local_space_) xrDestroySpace(local_space_);
    if (session_)    xrDestroySession(session_);
    if (vk_.device)   vkDestroyDevice(vk_.device, nullptr);
    if (vk_.instance) vkDestroyInstance(vk_.instance, nullptr);
    if (instance_)   xrDestroyInstance(instance_);

    instance_ = XR_NULL_HANDLE;
    session_  = XR_NULL_HANDLE;
}
