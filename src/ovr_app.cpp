#include "ovr_app.h"
#include "config.h"
#include "platform.h"
#include "sprites.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <utility>
#include <string>
#include <thread>
#include <vector>

#ifndef HAVE_OPENVR

// Built without the OpenVR SDK: the backend is unavailable. init() fails so
// main() falls back to OpenXR, and the other entry points are inert.
bool OvrApp::init(const FovConfig&) { return false; }
void OvrApp::shutdown() {}
bool OvrApp::poll_events() { return false; }
bool OvrApp::frame(const std::function<Frame(double, const ViewGeometry&)>&) { return false; }

#else

namespace {

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Head-space transform (x right, y up, -z forward) from rotation + position.
vr::HmdMatrix34_t make_pose(const Quat& q, Vec3 p) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    vr::HmdMatrix34_t m{};
    m.m[0][0] = 1 - 2 * (y * y + z * z);
    m.m[0][1] = 2 * (x * y - z * w);
    m.m[0][2] = 2 * (x * z + y * w);
    m.m[1][0] = 2 * (x * y + z * w);
    m.m[1][1] = 1 - 2 * (x * x + z * z);
    m.m[1][2] = 2 * (y * z - x * w);
    m.m[2][0] = 2 * (x * z - y * w);
    m.m[2][1] = 2 * (y * z + x * w);
    m.m[2][2] = 1 - 2 * (x * x + y * y);
    m.m[0][3] = p.x;
    m.m[1][3] = p.y;
    m.m[2][3] = p.z;
    return m;
}

// Rotation part of an eye-to-head transform as a quaternion (row-major 3x4).
// Canted-display headsets (Pimax; some Beyond drivers) carry the cant here,
// and the raw projection is relative to that canted axis — dropping it shifts
// every target up/sideways by the cant angle.
Quat quat_from_ovr(const vr::HmdMatrix34_t& m) {
    Quat q;
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
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 0.0f) || !std::isfinite(n)) return Quat{};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
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
// the render frustum already matches the visible FOV) or the result is not
// plausible (some drivers return identity/garbage maps).
bool shrink_to_visible(vr::IVRSystem* sys, vr::EVREye eye, float& tanL, float& tanR, float& tanUp, float& tanDn) {
    vr::DistortionCoordinates_t top{}, bot{}, lef{}, rig{};
    if (!sys->ComputeDistortion(eye, 0.5f, 0.0f, &top) || !sys->ComputeDistortion(eye, 0.5f, 1.0f, &bot) ||
        !sys->ComputeDistortion(eye, 0.0f, 0.5f, &lef) || !sys->ComputeDistortion(eye, 1.0f, 0.5f, &rig))
        return false;
    // A panel edge can map outside the render target (it shows past the
    // rendered image); the target's own edge bounds visibility there.
    const float v0 = std::clamp(top.rfGreen[1], 0.0f, 1.0f);
    const float v1 = std::clamp(bot.rfGreen[1], 0.0f, 1.0f);
    const float u0 = std::clamp(lef.rfGreen[0], 0.0f, 1.0f);
    const float u1 = std::clamp(rig.rfGreen[0], 0.0f, 1.0f);
    if (!std::isfinite(u0 + u1 + v0 + v1) || u1 <= u0 || v1 <= v0) return false;
    // Render-target UVs are linear in tangent space across the raw frustum.
    const float nl = tanL + u0 * (tanR - tanL), nr = tanL + u1 * (tanR - tanL);
    const float nu = tanUp + v0 * (tanDn - tanUp), nd = tanUp + v1 * (tanDn - tanUp);
    // The visible region must contain the eye axis and keep most of each side;
    // anything else is a driver quirk, not optics.
    if (!(nl < 0.0f && nr > 0.0f && nu > 0.0f && nd < 0.0f)) return false;
    if (nl > 0.55f * tanL || nr < 0.55f * tanR || nu < 0.55f * tanUp || nd > 0.55f * tanDn) return false;
    tanL = nl;
    tanR = nr;
    tanUp = nu;
    tanDn = nd;
    return true;
}

// Lens-visible region from the hidden-area line loop, converted from render
// target UV (u right, v down — the texture convention every OpenVR UV uses)
// to Y-up tangents of the raw frustum. Validated later by sanitize_polygon().
std::vector<Vec2> visible_polygon(vr::IVRSystem* sys, vr::EVREye eye, float tanL, float tanR, float tanUp,
                                  float tanDn) {
    std::vector<Vec2> poly;
    const vr::HiddenAreaMesh_t mesh = sys->GetHiddenAreaMesh(eye, vr::k_eHiddenAreaMesh_LineLoop);
    if (!mesh.pVertexData || mesh.unTriangleCount < 3 || mesh.unTriangleCount > 100000) return poly;
    poly.reserve(mesh.unTriangleCount);
    for (uint32_t i = 0; i < mesh.unTriangleCount; ++i) {
        const float u = mesh.pVertexData[i].v[0], v = mesh.pVertexData[i].v[1];
        poly.push_back({tanL + u * (tanR - tanL), tanUp + v * (tanDn - tanUp)});
    }
    return poly;
}

constexpr float kDeg = kRad2Deg;

}  // namespace

bool OvrApp::init(const FovConfig& fov_cfg) {
    fov_cfg_ = fov_cfg;
    geom_ = fallback_view_geometry(fov_cfg_, cfg::OVERLAY_DIST);

    // Background: attach only to an already-running SteamVR. If SteamVR is not
    // up (e.g. a Virtual Desktop / native-OpenXR session), this fails cleanly
    // and main() falls back to the OpenXR backend — we never launch SteamVR.
    vr::EVRInitError ie = vr::VRInitError_None;
    sys_ = vr::VR_Init(&ie, vr::VRApplication_Background);
    if (ie != vr::VRInitError_None || !sys_) {
        std::cout << "[INFO] OpenVR/SteamVR not running (" << vr::VR_GetVRInitErrorAsSymbol(ie) << ")\n";
        sys_ = nullptr;
        return false;
    }
    steamvr_running_ = true;
    if (!vr::VROverlay()) {
        std::cout << "[ERROR] OpenVR overlay interface unavailable (SteamVR too old?)\n";
        vr::VR_Shutdown();
        sys_ = nullptr;
        return false;
    }
    key_prefix_ = "etvr.calib." + std::to_string(platform::process_id()) + ".";

    // SteamVR can be up with the headset still waking/connecting. Give it a few
    // seconds; geometry is retried during the run if it shows up later.
    const double wait_until = now_s() + 3.0;
    bool announced = false;
    while (!sys_->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd) && now_s() < wait_until) {
        if (!announced) {
            std::cout << "[INFO] Waiting for the headset to connect...\n";
            announced = true;
        }
        vr::VREvent_t ev;
        while (sys_->PollNextEvent(&ev, sizeof(ev))) {
            if (ev.eventType == vr::VREvent_Quit) {
                std::cout << "[INFO] SteamVR is shutting down\n";
                sys_->AcknowledgeQuit_Exiting();
                vr::VR_Shutdown();
                sys_ = nullptr;
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!compute_geometry()) std::cout << "[WARN] Headset geometry unavailable; using a generic FOV until it is\n";

    // Opaque grey backdrop: five faces of a cube around the head, so it fills
    // the view on any FOV (a single quad could not cover >~140° headsets).
    // Lowest sort order; replaces the XR build's projection background while
    // leaving the running game alive underneath.
    static const char* kFaces[5] = {"bg.front", "bg.left", "bg.right", "bg.top", "bg.bottom"};
    for (int i = 0; i < 5; ++i)
        if (!create(kFaces[i], 0, bg_[i])) {
            destroy_all();
            vr::VR_Shutdown();
            sys_ = nullptr;
            return false;
        }
    const float d = cfg::BACKDROP_DIST;
    const Quat face_q[5] = {
        Quat{},
        quat_axis_angle({0, 1, 0}, 90.0f * kDeg2Rad),
        quat_axis_angle({0, 1, 0}, -90.0f * kDeg2Rad),
        quat_axis_angle({1, 0, 0}, 90.0f * kDeg2Rad),
        quat_axis_angle({1, 0, 0}, -90.0f * kDeg2Rad),
    };
    const Vec3 face_p[5] = {{0, 0, -d}, {-d, 0, 0}, {d, 0, 0}, {0, d, 0}, {0, -d, 0}};
    for (int i = 0; i < 5; ++i) {
        vr::VROverlay()->SetOverlayWidthInMeters(bg_[i].h, 2.0f * d * 1.02f);
        const vr::HmdMatrix34_t m = make_pose(face_q[i], face_p[i]);
        vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(bg_[i].h, vr::k_unTrackedDeviceIndex_Hmd, &m);
    }

    if (!create("panel", 1, panel_) || !create("ring", 2, ring_) || !create("trail", 3, trail_) ||
        !create("dot", 4, dot_)) {
        destroy_all();
        vr::VR_Shutdown();
        sys_ = nullptr;
        return false;
    }

    std::cout << "[INFO] OpenVR overlay backend ready (calibrating over the running app)\n";
    return true;
}

bool OvrApp::create(const std::string& name, uint32_t sort, Overlay& o) {
    const std::string key = key_prefix_ + name;
    const std::string friendly = "EyeTrackVR calibration " + name;
    const vr::EVROverlayError e = vr::VROverlay()->CreateOverlay(key.c_str(), friendly.c_str(), &o.h);
    if (e != vr::VROverlayError_None) {
        std::cout << "[ERROR] Failed to create overlay " << name << ": "
                  << vr::VROverlay()->GetOverlayErrorNameFromEnum(e) << "\n";
        o.h = vr::k_ulOverlayHandleInvalid;
        return false;
    }
    vr::VROverlay()->SetOverlaySortOrder(o.h, sort);
    return true;
}

bool OvrApp::compute_geometry() {
    if (!sys_->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd)) return false;

    EyeGeom eyes[2];
    std::ostringstream eye_log;
    const vr::EVREye ids[2] = {vr::Eye_Left, vr::Eye_Right};
    for (int e = 0; e < 2; ++e) {
        float l = 0, r = 0, t = 0, b = 0;
        sys_->GetProjectionRaw(ids[e], &l, &r, &t, &b);
        if (!std::isfinite(l + r + t + b) || l == r || t == b) {
            if (!geom_warned_)
                std::cout << "[WARN] Eye " << e << " projection is unusable (" << l << ", " << r << ", " << t << ", "
                          << b << ")\n";
            geom_warned_ = true;
            return false;
        }
        // GetProjectionRaw's values are GL frustum tangents with the vertical
        // pair labelled in image order (pfTop = the numerically smaller edge):
        // GL-top = pfBottom, GL-bottom = pfTop. This follows from SteamVR's own
        // ComposeProjection (idy = 1/(fBottom - fTop) ≡ 2/(t - b) ⇒ t = fBottom)
        // and was verified against SteamVR's OpenXR XrFovf for the same HMD.
        // Decoding it as up = -pfTop instead mirrors the FOV vertically —
        // invisible on near-symmetric headsets, ~20° of target shift on a
        // Beyond 2. Decode by sign rather than by label: "up" is the positive
        // tangent under either labelling, so a driver that fills the pair the
        // other way round cannot mirror the FOV again.
        const float tanL = std::min(l, r), tanR = std::max(l, r);
        float tanUp = b, tanDn = t;  // documented order
        if ((t < 0.0f) != (b < 0.0f)) {
            tanUp = std::max(t, b);
            tanDn = std::min(t, b);
        }
        float vl = tanL, vr = tanR, vu = tanUp, vd = tanDn;

        // Shrink the (overgrown) render frustum to the lens-visible region.
        const bool vis = shrink_to_visible(sys_, ids[e], vl, vr, vu, vd);

        eyes[e].tan_left = vl;
        eyes[e].tan_right = vr;
        eyes[e].tan_up = vu;
        eyes[e].tan_down = vd;
        eyes[e].visible = visible_polygon(sys_, ids[e], tanL, tanR, tanUp, tanDn);

        // Full pose: canted displays put a real rotation in eye-to-head, and
        // the projection above is relative to that canted axis.
        const vr::HmdMatrix34_t m = sys_->GetEyeToHeadTransform(ids[e]);
        eyes[e].pose.p = {m.m[0][3], m.m[1][3], m.m[2][3]};
        eyes[e].pose.q = quat_from_ovr(m);

        eye_log << "[INFO] Eye " << (e ? "R" : "L") << ": raw fov L/R " << std::atan(tanL) * kDeg << "/"
                << std::atan(tanR) * kDeg << "° up/down " << std::atan(tanUp) * kDeg << "/" << std::atan(tanDn) * kDeg
                << "°" << (vis ? " -> visible (distortion map)" : " (no usable distortion map)") << "\n";
    }

    std::string notes;
    ViewGeometry g;
    if (!compute_view_geometry(eyes, cfg::OVERLAY_DIST, fov_cfg_, g, &notes)) {
        if (!geom_warned_) std::cout << eye_log.str() << "[WARN] Could not resolve a gaze range: " << notes << "\n";
        geom_warned_ = true;
        return false;
    }
    // Re-resolved after an event but nothing moved: keep quiet.
    const GazeRange& o = geom_.gaze;
    const bool same = !geom_.synthetic && std::fabs(o.rx - g.gaze.rx) < 1e-4f &&
                      std::fabs(o.ry_up - g.gaze.ry_up) < 1e-4f && std::fabs(o.ry_down - g.gaze.ry_down) < 1e-4f &&
                      std::fabs(o.center_y - g.gaze.center_y) < 1e-4f;
    if (same) return true;
    std::cout << eye_log.str();
    for (int e = 0; e < 2; ++e) log_eye(e ? "R" : "L", g.eyes[e]);
    if (!notes.empty()) std::cout << "[INFO] Geometry: " << notes << "\n";
    if (!geom_.synthetic) std::cout << "[INFO] Headset geometry changed; targets follow the new range\n";
    geom_ = std::move(g);
    log_view_geometry(geom_, fov_cfg_, cfg::OVERLAY_DIST);
    return true;
}

bool OvrApp::poll_events() {
    if (!sys_) return false;
    vr::VREvent_t ev;
    while (sys_->PollNextEvent(&ev, sizeof(ev))) {
        switch (ev.eventType) {
            case vr::VREvent_Quit:
                std::cout << "[INFO] SteamVR is shutting down\n";
                sys_->AcknowledgeQuit_Exiting();
                return false;
            case vr::VREvent_IpdChanged:
                geom_dirty_ = true;
                break;
            case vr::VREvent_TrackedDeviceActivated:
                if (ev.trackedDeviceIndex == vr::k_unTrackedDeviceIndex_Hmd) geom_dirty_ = true;
                break;
            default:
                break;
        }
    }
    return true;
}

void OvrApp::apply(Overlay& o, const Quad& q, const Image* img) {
    auto* ov = vr::VROverlay();
    if (!q.visible || q.alpha <= 0.002f || !img || img->w <= 0) {
        if (o.shown) {
            ov->HideOverlay(o.h);
            o.shown = false;
        }
        return;
    }
    if (img->version != o.version) {
        const vr::EVROverlayError e =
            ov->SetOverlayRaw(o.h, (void*)img->rgba.data(), (uint32_t)img->w, (uint32_t)img->h, 4);
        if (e == vr::VROverlayError_None) {
            o.version = img->version;
        } else {
            // Keep the version stale so the upload is retried next frame
            // (logged once per image, not on every retry).
            if (o.failed_version != img->version)
                std::cout << "[WARN] Overlay texture upload failed (" << img->w << "x" << img->h
                          << "): " << ov->GetOverlayErrorNameFromEnum(e) << "\n";
            o.failed_version = img->version;
            if (!o.shown) return;
        }
    }
    if (std::fabs(q.w - o.width) > 1e-5f) {
        ov->SetOverlayWidthInMeters(o.h, q.w);
        o.width = q.w;
    }
    // Overlay height follows the texture's aspect; stretched quads (the trail)
    // get there by widening the texels instead.
    const float aspect = q.h > 0.0f ? (q.w * img->h) / (q.h * img->w) : 1.0f;
    if (std::fabs(aspect - o.texel_aspect) > 1e-4f * o.texel_aspect) {
        ov->SetOverlayTexelAspect(o.h, aspect);
        o.texel_aspect = aspect;
    }
    if (std::fabs(q.alpha - o.alpha) > 1.0f / 512.0f) {
        ov->SetOverlayAlpha(o.h, q.alpha);
        o.alpha = q.alpha;
    }
    if (q.x != o.x || q.y != o.y || q.z != o.z || q.rot != o.rot) {
        const vr::HmdMatrix34_t m = make_pose(quat_axis_angle({0.0f, 0.0f, 1.0f}, q.rot), {q.x, q.y, q.z});
        ov->SetOverlayTransformTrackedDeviceRelative(o.h, vr::k_unTrackedDeviceIndex_Hmd, &m);
        o.x = q.x;
        o.y = q.y;
        o.z = q.z;
        o.rot = q.rot;
    }
    if (!o.shown) {
        ov->ShowOverlay(o.h);
        o.shown = true;
    }
}

void OvrApp::apply_backdrop(uint8_t gray, float alpha) {
    auto* ov = vr::VROverlay();
    if (gray != bg_gray_ || bg_alpha_ < 0.0f) {
        bg_gray_ = gray;
        uint8_t px[4 * 4 * 4];
        for (int i = 0; i < 16; ++i) {
            px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = gray;
            px[i * 4 + 3] = 255;
        }
        for (auto& f : bg_) ov->SetOverlayRaw(f.h, px, 4, 4, 4);
    }
    if (alpha <= 0.002f) {
        if (bg_shown_) {
            for (auto& f : bg_) ov->HideOverlay(f.h);
            bg_shown_ = false;
        }
        bg_alpha_ = 0.0f;
        return;
    }
    if (std::fabs(alpha - bg_alpha_) > 1.0f / 512.0f) {
        for (auto& f : bg_) ov->SetOverlayAlpha(f.h, alpha);
        bg_alpha_ = alpha;
    }
    if (!bg_shown_) {
        for (auto& f : bg_) ov->ShowOverlay(f.h);
        bg_shown_ = true;
    }
}

bool OvrApp::frame(const std::function<Frame(double, const ViewGeometry&)>& update_cb) {
    // Pace to the compositor so animation steps land on display frames (a free
    // running sleep beats against 90/120 Hz and judders). Falls back to a
    // short sleep if the runtime cannot sync (old SteamVR, headset asleep).
    // A timeout (headset asleep, compositor paused during an app switch) is not
    // a failure; real errors switch to the timer, re-probing every few seconds.
    bool synced = false;
    if (sync_failures_ < 10 || now_s() >= next_sync_retry_) {
        const vr::EVROverlayError e = vr::VROverlay()->WaitFrameSync(100);
        if (e == vr::VROverlayError_None) {
            if (sync_failures_ >= 10) std::cout << "[INFO] WaitFrameSync available again\n";
            sync_failures_ = 0;
            synced = true;
        } else if (e == vr::VROverlayError_TimedOut) {
            synced = true;  // already waited 100 ms
        } else {
            if (++sync_failures_ == 10)
                std::cout << "[INFO] WaitFrameSync unavailable (" << vr::VROverlay()->GetOverlayErrorNameFromEnum(e)
                          << "); using a timer\n";
            if (sync_failures_ >= 10) next_sync_retry_ = now_s() + 5.0;
        }
    }
    if (!synced) std::this_thread::sleep_for(std::chrono::milliseconds(8));

    const double now = now_s();
    double dt = first_frame_ ? 0.0 : (now - prev_secs_);
    first_frame_ = false;
    prev_secs_ = now;
    dt = std::clamp(dt, 0.0, 0.1);

    // Headset (re)connected, IPD changed, or still on the fallback: refresh.
    if (geom_dirty_ || (geom_.synthetic && now >= next_geom_try_)) {
        geom_dirty_ = false;
        next_geom_try_ = now + 1.0;
        const bool was_synthetic = geom_.synthetic;
        if (compute_geometry() && was_synthetic) std::cout << "[INFO] Headset geometry resolved\n";
    }

    const Frame f = update_cb(dt, geom_);

    apply_backdrop(f.backdrop_gray, f.backdrop_alpha);
    apply(panel_, f.panel, f.panel_image);
    apply(ring_, f.ring, &sprites::ring());
    apply(trail_, f.trail, &sprites::trail());
    apply(dot_, f.dot, &sprites::dot());
    return true;
}

void OvrApp::destroy_all() {
    auto* ov = vr::VROverlay();
    if (!ov) return;
    for (Overlay* o : {&bg_[0], &bg_[1], &bg_[2], &bg_[3], &bg_[4], &panel_, &ring_, &trail_, &dot_}) {
        if (o->h != vr::k_ulOverlayHandleInvalid) ov->DestroyOverlay(o->h);
        o->h = vr::k_ulOverlayHandleInvalid;
    }
}

void OvrApp::shutdown() {
    if (!sys_) return;
    destroy_all();
    vr::VR_Shutdown();
    sys_ = nullptr;
}

#endif  // HAVE_OPENVR
