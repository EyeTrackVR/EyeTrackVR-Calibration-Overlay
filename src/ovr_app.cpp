#include "ovr_app.h"
#include "config.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include <stb_image.h>   // implementation lives in xr_app.cpp

#ifndef HAVE_OPENVR

// Built without the OpenVR SDK: the backend is unavailable. init() fails so
// main() falls back to OpenXR, and the other entry points are inert.
bool OvrApp::init(const FovConfig&, const char*, const char*) { return false; }
void OvrApp::shutdown() {}
bool OvrApp::poll_events() { return false; }
bool OvrApp::frame(const std::function<Scene(double, const GazeRange&)>&) { return false; }

#else

namespace {

constexpr float kTextWidthM = 2.25f;   // instruction panel width (m), matches XR build
constexpr float kBackdropM  = 16.0f;   // grey backdrop quad size (m) — fills any FOV
constexpr uint8_t kGrey     = 96;

// Head-space transform (x right, y up, -z forward), identity rotation.
vr::HmdMatrix34_t make_pose(float x, float y, float z) {
    vr::HmdMatrix34_t m{};
    m.m[0][0] = 1.0f; m.m[1][1] = 1.0f; m.m[2][2] = 1.0f;
    m.m[0][3] = x;    m.m[1][3] = y;    m.m[2][3] = z;
    return m;
}

constexpr float kRad2Deg = 57.29577951308232f;

// Rotation part of an eye-to-head transform as a quaternion (row-major 3x4).
// Canted-display headsets (Bigscreen Beyond: ~5.3° pitch + ±6.2° yaw; Pimax)
// carry the cant here, and the raw projection is relative to that canted axis —
// dropping it shifts every target up/sideways by the cant angle.
XrQuaternionf quat_from_ovr(const vr::HmdMatrix34_t& m) {
    XrQuaternionf q{0.0f, 0.0f, 0.0f, 1.0f};
    const float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m.m[2][1] - m.m[1][2]) / s;
        q.y = (m.m[0][2] - m.m[2][0]) / s;
        q.z = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

// GetProjectionRaw returns the RENDER frustum, which drivers grow past the
// lens-visible region so barrel undistortion has source pixels to sample
// (lighthouse "grow_for_undistort" — the Beyond 2 reports a ~53° down
// half-angle for a ~50° visible one and ~43° up for ~39°). Placing targets by
// the render frustum therefore overshoots the visible FOV. Recover the visible
// extent by mapping the display's centre cross through the driver's distortion
// function: ComputeDistortion takes display-panel UVs and returns the render
// target UVs actually shown there, so the panel edge at the lens axis marks
// the last visible tangent in each cardinal direction. (Not the panel border
// bounding box: display corners map outside the render target, which would
// clamp the box back to the full frustum.) Takes/returns Y-up half-tangents;
// u=0 → left, v=0 → up. Returns false (leaving them untouched) if the driver
// has no distortion map (e.g. streamed HMDs that undistort on device — there
// the render frustum already matches the visible FOV).
bool shrink_to_visible(vr::IVRSystem* sys, vr::EVREye eye,
                       float& tanL, float& tanR, float& tanUp, float& tanDn) {
    vr::DistortionCoordinates_t top{}, bot{}, lef{}, rig{};
    if (!sys->ComputeDistortion(eye, 0.5f, 0.0f, &top) ||
        !sys->ComputeDistortion(eye, 0.5f, 1.0f, &bot) ||
        !sys->ComputeDistortion(eye, 0.0f, 0.5f, &lef) ||
        !sys->ComputeDistortion(eye, 1.0f, 0.5f, &rig))
        return false;
    // A panel edge can map outside the render target (it shows past the
    // rendered image); the target's own edge bounds visibility there.
    const float v0 = std::clamp(top.rfGreen[1], 0.0f, 1.0f);
    const float v1 = std::clamp(bot.rfGreen[1], 0.0f, 1.0f);
    const float u0 = std::clamp(lef.rfGreen[0], 0.0f, 1.0f);
    const float u1 = std::clamp(rig.rfGreen[0], 0.0f, 1.0f);
    if (u1 <= u0 || v1 <= v0) return false;
    // Render-target UVs are linear in tangent space across the raw frustum.
    const float nl = tanL + u0 * (tanR - tanL), nr = tanL + u1 * (tanR - tanL);
    const float nu = tanUp + v0 * (tanDn - tanUp), nd = tanUp + v1 * (tanDn - tanUp);
    tanL = nl; tanR = nr; tanUp = nu; tanDn = nd;
    return true;
}

} // namespace

bool OvrApp::init(const FovConfig& fov_cfg, const char* dot_png, const char* font_ttf) {
    fov_cfg_ = fov_cfg;

    // Background: attach only to an already-running SteamVR. If SteamVR is not
    // up (e.g. a Virtual Desktop / native-OpenXR session), this fails cleanly
    // and main() falls back to the OpenXR backend — we never launch SteamVR.
    vr::EVRInitError ie = vr::VRInitError_None;
    sys_ = vr::VR_Init(&ie, vr::VRApplication_Background);
    if (ie != vr::VRInitError_None || !sys_) {
        std::cout << "[INFO] OpenVR/SteamVR not running ("
                  << vr::VR_GetVRInitErrorAsSymbol(ie) << ")\n";
        sys_ = nullptr;
        return false;
    }
    if (!vr::VROverlay()) {
        std::cout << "[ERROR] OpenVR overlay interface unavailable\n";
        vr::VR_Shutdown();
        sys_ = nullptr;
        return false;
    }

    if (!compute_gaze())
        std::cout << "[WARN] Could not resolve eye projection; using fallback gaze range\n";

    // Opaque grey backdrop, drawn behind the dot/text (lowest sort order), big
    // enough to fill the field of view. Replaces the XR build's projection
    // background while leaving the running game alive underneath it.
    if (vr::VROverlay()->CreateOverlay("etvr.calib.bg", "Calibration Backdrop", &bg_h_)
            != vr::VROverlayError_None) {
        std::cout << "[ERROR] Failed to create backdrop overlay\n";
        vr::VR_Shutdown(); sys_ = nullptr; return false;
    }
    std::vector<uint8_t> grey(4 * 4 * 4);
    for (size_t i = 0; i < grey.size(); i += 4) {
        grey[i + 0] = kGrey; grey[i + 1] = kGrey; grey[i + 2] = kGrey; grey[i + 3] = 255;
    }
    vr::VROverlay()->SetOverlayRaw(bg_h_, grey.data(), 4, 4, 4);
    vr::VROverlay()->SetOverlayWidthInMeters(bg_h_, kBackdropM);
    vr::VROverlay()->SetOverlaySortOrder(bg_h_, 0);
    const float bg_scale = cfg::BACKDROP_DIST / cfg::OVERLAY_DIST;
    set_transform(bg_h_, gaze_.center_x * bg_scale, gaze_.center_y * bg_scale,
                  -cfg::BACKDROP_DIST);
    vr::VROverlay()->ShowOverlay(bg_h_);

    // Dot overlay (loaded from the PNG; alpha-composited over the backdrop).
    if (vr::VROverlay()->CreateOverlay("etvr.calib.dot", "Calibration Dot", &dot_h_)
            != vr::VROverlayError_None) {
        std::cout << "[ERROR] Failed to create dot overlay\n";
        vr::VR_Shutdown(); sys_ = nullptr; return false;
    }
    // Decode the PNG in-process and upload raw RGBA, like the backdrop/text.
    // SetOverlayFromFile is resolved inside SteamVR's own process, so a
    // CWD-relative path silently fails to load and the dot never appears.
    int dw = 0, dh = 0, dn = 0;
    unsigned char* px = stbi_load(dot_png, &dw, &dh, &dn, 4);
    if (!px) {
        std::cout << "[ERROR] Could not load dot image: " << dot_png << "\n";
        vr::VR_Shutdown(); sys_ = nullptr; return false;
    }
    vr::EVROverlayError oe =
        vr::VROverlay()->SetOverlayRaw(dot_h_, px, (uint32_t)dw, (uint32_t)dh, 4);
    stbi_image_free(px);
    if (oe != vr::VROverlayError_None) {
        std::cout << "[ERROR] Failed to upload dot texture: "
                  << vr::VROverlay()->GetOverlayErrorNameFromEnum(oe) << "\n";
        vr::VR_Shutdown(); sys_ = nullptr; return false;
    }
    vr::VROverlay()->SetOverlaySortOrder(dot_h_, 2);

    // Text overlay (rasterised on demand via stb_truetype, uploaded as raw RGBA).
    text_render_.load(font_ttf, 1024, 300);   // best-effort; omitted if font missing
    have_text_ = text_render_.ready();
    if (have_text_) {
        if (vr::VROverlay()->CreateOverlay("etvr.calib.text", "Calibration Text", &text_h_)
                == vr::VROverlayError_None) {
            vr::VROverlay()->SetOverlayWidthInMeters(text_h_, kTextWidthM);
            vr::VROverlay()->SetOverlaySortOrder(text_h_, 1);
        } else {
            std::cout << "[WARN] Could not create text overlay — continuing without it\n";
            have_text_ = false;
        }
    }

    std::cout << "[INFO] OpenVR overlay backend ready (calibrating over the running app)\n";
    return true;
}

bool OvrApp::compute_gaze() {
    // Build an XrView pair from SteamVR's per-eye projection + full eye-to-head
    // pose, then reuse the shared frustum-intersection math.
    //
    // GetProjectionRaw's values are the GL frustum tangents with the vertical
    // pair labelled in image order (pfTop = the numerically smaller edge):
    // GL-top = pfBottom, GL-bottom = pfTop. This follows from SteamVR's own
    // ComposeProjection (idy = 1/(fBottom - fTop) ≡ 2/(t - b) ⇒ t = fBottom)
    // and was verified against SteamVR's OpenXR XrFovf for the same HMD.
    // Decoding it as up = -pfTop instead mirrors the FOV vertically — invisible
    // on near-symmetric headsets, ~20° of target shift on a Beyond 2.
    XrView views[2]{};
    const vr::EVREye eyes[2] = {vr::Eye_Left, vr::Eye_Right};
    for (int e = 0; e < 2; ++e) {
        float l = 0, r = 0, t = 0, b = 0;
        sys_->GetProjectionRaw(eyes[e], &l, &r, &t, &b);
        float tanL = l, tanR = r, tanUp = b, tanDn = t;   // Y-up half-tangents
        const float raw_up = std::atan(tanUp) * kRad2Deg;
        const float raw_dn = std::atan(tanDn) * kRad2Deg;

        // Shrink the (overgrown) render frustum to the lens-visible region.
        const bool vis = shrink_to_visible(sys_, eyes[e], tanL, tanR, tanUp, tanDn);

        views[e].fov.angleLeft  = std::atan(tanL);
        views[e].fov.angleRight = std::atan(tanR);
        views[e].fov.angleUp    = std::atan(tanUp);
        views[e].fov.angleDown  = std::atan(tanDn);

        // Full pose: canted displays put a real rotation in eye-to-head, and
        // the projection above is relative to that canted axis.
        vr::HmdMatrix34_t m = sys_->GetEyeToHeadTransform(eyes[e]);
        views[e].pose.position    = {m.m[0][3], m.m[1][3], m.m[2][3]};
        views[e].pose.orientation = quat_from_ovr(m);

        // Eye optical axis in head space = R * (0,0,-1).
        const float fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];
        std::cout << "[INFO] Eye " << (e ? "R" : "L")
                  << ": raw fov up/down " << raw_up << "/" << raw_dn << "°"
                  << (vis ? ", visible " : ", visible (no distortion map) ")
                  << views[e].fov.angleUp * kRad2Deg << "/"
                  << views[e].fov.angleDown * kRad2Deg << "°"
                  << ", cant pitch/yaw "
                  << std::asin(std::clamp(fy, -1.0f, 1.0f)) * kRad2Deg << "/"
                  << std::atan2(-fx, -fz) * kRad2Deg << "°\n";
    }
    const bool ok = compute_gaze_range(views, cfg::OVERLAY_DIST, fov_cfg_, gaze_);
    if (ok) log_gaze_range(gaze_, fov_cfg_, cfg::OVERLAY_DIST);
    return ok;
}

void OvrApp::set_transform(vr::VROverlayHandle_t h, float x, float y, float z) {
    vr::HmdMatrix34_t m = make_pose(x, y, z);
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
        h, vr::k_unTrackedDeviceIndex_Hmd, &m);
}

bool OvrApp::poll_events() {
    if (!sys_) return false;
    vr::VREvent_t ev;
    while (sys_->PollNextEvent(&ev, sizeof(ev))) {
        if (ev.eventType == vr::VREvent_Quit) {
            sys_->AcknowledgeQuit_Exiting();
            return false;   // SteamVR shutting down
        }
    }
    return true;
}

bool OvrApp::frame(const std::function<Scene(double, const GazeRange&)>& update_cb) {
    using clock = std::chrono::steady_clock;
    const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    double dt = first_frame_ ? 0.0 : (now - prev_secs_);
    first_frame_ = false;
    prev_secs_   = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;

    Scene scene = update_cb(dt, gaze_);

    if (scene.backdrop_gray != backdrop_gray_) {
        backdrop_gray_ = scene.backdrop_gray;
        std::vector<uint8_t> pixels(4 * 4 * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = pixels[i + 1] = pixels[i + 2] = backdrop_gray_;
            pixels[i + 3] = 255;
        }
        vr::VROverlay()->SetOverlayRaw(bg_h_, pixels.data(), 4, 4, 4);
    }

    // Dot.
    if (scene.dot_visible) {
        vr::VROverlay()->SetOverlayWidthInMeters(dot_h_, scene.dot_size);
        set_transform(dot_h_, scene.dot_x, scene.dot_y, -cfg::OVERLAY_DIST);
        if (!dot_shown_) { vr::VROverlay()->ShowOverlay(dot_h_); dot_shown_ = true; }
    } else if (dot_shown_) {
        vr::VROverlay()->HideOverlay(dot_h_); dot_shown_ = false;
    }

    // Instruction text (re-rasterised only when the string changes).
    if (scene.text_visible && have_text_) {
        if (scene.text != last_text_) {
            last_text_ = scene.text;
            text_render_.render(scene.text);
            vr::VROverlay()->SetOverlayRaw(
                text_h_, (void*)text_render_.pixels(),
                (uint32_t)text_render_.width(), (uint32_t)text_render_.height(), 4);
        }
        const float text_scale = cfg::TEXT_DIST / cfg::OVERLAY_DIST;
        set_transform(text_h_, gaze_.center_x * text_scale,
                      (gaze_.center_y - 0.825f) * text_scale,
                      -cfg::TEXT_DIST);
        if (!text_shown_) { vr::VROverlay()->ShowOverlay(text_h_); text_shown_ = true; }
    } else if (text_shown_) {
        vr::VROverlay()->HideOverlay(text_h_); text_shown_ = false;
    }

    // Overlay transforms are device-relative, so SteamVR re-projects them every
    // compositor frame; we only need to push updates at a modest cadence.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    return true;
}

void OvrApp::shutdown() {
    if (!sys_) return;
    if (text_h_ != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(text_h_);
    if (dot_h_  != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(dot_h_);
    if (bg_h_   != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(bg_h_);
    vr::VR_Shutdown();
    sys_ = nullptr;
}

#endif // HAVE_OPENVR
