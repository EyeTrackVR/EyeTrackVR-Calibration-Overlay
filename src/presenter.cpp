#include "presenter.h"
#include "config.h"
#include "raster.h"
#include "sprites.h"

#include <algorithm>
#include <cmath>

namespace {

// Timings (seconds).
constexpr float kBackdropIn = 0.35f;
constexpr float kBackdropOut = 0.45f;
constexpr float kDotIn = 0.16f;
constexpr float kDotOut = 0.15f;
constexpr float kPop = 0.24f;
constexpr float kRingIn = 0.12f;
constexpr float kPanelIn = 0.28f;
constexpr float kPanelOut = 0.12f;
constexpr float kPredictS = 0.35f;    // look-ahead for continuously moving targets
constexpr float kTrailS = 0.28f;      // how far back in time the trail reaches
constexpr float kTrailAlpha = 0.55f;  // trail opacity at its head (under the dot)

// Layout.
constexpr float kPanelMargin = 0.07f;  // clearance card ↔ target edge (≈1.3° at 3 m)
constexpr float kLowerDeg = -15.0f;    // preferred elevation of the lower slot
constexpr float kUpperDeg = 13.0f;
constexpr float kMaxPanelW = 1.55f;  // card width limit (≈29° at 3 m)
constexpr float kSlotStep = 0.04f;

float ease_out_cubic(float t) {
    t = raster::clamp01(t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
float ease_in_out(float t) {
    t = raster::clamp01(t);
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}
float ease_out_back(float t) {
    t = raster::clamp01(t);
    constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}
float ramp(float v, bool up, float dt, float in_s, float out_s) {
    return up ? std::min(1.0f, v + dt / in_s) : std::max(0.0f, v - dt / out_s);
}

}  // namespace

Presenter::Presenter(TextRenderer* text) : text_(text) {}

void Presenter::begin_outro() { outro_ = true; }

bool Presenter::outro_done() const { return outro_ && backdrop_ <= 0.0f && dot_vis_ <= 0.0f && panel_t_ <= 0.0f; }

Frame Presenter::update(const Scene& s, double dt_in, const ViewGeometry& g) {
    const float dt = (float)std::clamp(dt_in, 0.0, 0.25);
    Frame f;

    backdrop_ = ramp(backdrop_, !outro_, dt, kBackdropIn, kBackdropOut);
    f.backdrop_gray = s.backdrop_gray;
    f.backdrop_alpha = ease_in_out(backdrop_);

    update_target(s, dt);
    update_panel(s, dt, g);

    if (dot_vis_ > 0.001f) {
        const float a = ease_out_cubic(dot_vis_);
        const float scale = pop_ < 1.0f ? 0.45f + 0.55f * ease_out_back(pop_) : 0.70f + 0.30f * a;
        const float sz = dot_size_ * scale;
        f.dot = {true, dot_x_, dot_y_, -cfg::OVERLAY_DIST, sz, sz, a};
        trail_quad(sz, a, f.trail);
    }
    if (ring_alpha_ > 0.002f) {
        const float k = cfg::RING_DIST / cfg::OVERLAY_DIST;
        f.ring = {true, ring_x_ * k, ring_y_ * k, -cfg::RING_DIST, ring_size_ * k, ring_size_ * k, ring_alpha_};
    }
    if (panel_alpha_out_ > 0.002f && have_img_) {
        const float k = cfg::TEXT_DIST / cfg::OVERLAY_DIST;
        const float w = img_.w / TextRenderer::kPixelsPerMetre * k * panel_scale_out_;
        const float h = img_.h / TextRenderer::kPixelsPerMetre * k * panel_scale_out_;
        f.panel = {true, g.gaze.center_x * k, slot_y_[slot_] * k, -cfg::TEXT_DIST, w, h, panel_alpha_out_};
        f.panel_image = &img_;
    }
    return f;
}

void Presenter::update_target(const Scene& s, float dt) {
    const bool want = s.dot_visible && !outro_;
    clock_ += dt;
    if (want) {
        if (dot_vis_ <= 0.0f) {
            pop_ = 0.0f;
            have_prev_ = false;
        }  // fresh appearance
        // A fresh or jumped dot starts a new trail; otherwise record where it
        // is, keeping one sample older than the window to interpolate from.
        if (dot_vis_ <= 0.0f || s.dot_cut) trail_.clear();
        trail_.push_back({clock_, s.dot_x, s.dot_y});
        while (trail_.size() > 2 && trail_[1].t <= clock_ - kTrailS) trail_.pop_front();
        if (have_prev_ && dt > 0.0f) {
            vel_x_ = (s.dot_x - prev_x_) / dt;
            vel_y_ = (s.dot_y - prev_y_) / dt;
        } else {
            vel_x_ = vel_y_ = 0.0f;
        }
        prev_x_ = s.dot_x;
        prev_y_ = s.dot_y;
        have_prev_ = true;
        dot_x_ = s.dot_x;
        dot_y_ = s.dot_y;
        dot_size_ = s.dot_size;
    } else {
        vel_x_ = vel_y_ = 0.0f;
        have_prev_ = false;
        trail_.clear();
    }
    dot_vis_ = ramp(dot_vis_, want, dt, kDotIn, kDotOut);
    if (pop_ < 1.0f) pop_ = std::min(1.0f, pop_ + dt / kPop);

    const bool ring_want = want && s.ring_size > s.dot_size * 1.02f;
    if (ring_want) {
        const bool jumped = !ring_active_ || std::hypot(s.ring_x - ring_x_, s.ring_y - ring_y_) > 0.02f;
        if (jumped) ring_in_ = 0.0f;
        ring_x_ = s.ring_x;
        ring_y_ = s.ring_y;
        ring_size_ = s.ring_size;
    }
    ring_active_ = ring_want;
    ring_in_ = ramp(ring_in_, ring_want, dt, kRingIn, kRingIn);
    // Fade the ring as it closes on the dot, so only the dot remains at capture.
    const float conv = dot_size_ > 0.0f ? raster::smoothstep(0.05f, 0.8f, (ring_size_ - dot_size_) / dot_size_) : 0.0f;
    ring_alpha_ = ring_in_ * conv * ease_out_cubic(dot_vis_);
}

// The trail is a comet streak from where the dot was kTrailS ago to where it is
// now: a straight chord, which on the smooth pursuit paths (ellipse orbit,
// spiral) deviates from the arc by millimetres, and on glides is exact. It
// grows behind a moving dot and shrinks back into it once the dot stops, so
// it is gone well before a stationary target's capture signal.
void Presenter::trail_quad(float dot_size, float dot_alpha, Quad& out) const {
    if (trail_.size() < 2) return;
    const double t0 = clock_ - kTrailS;
    float tx = trail_.front().x, ty = trail_.front().y;
    for (size_t i = 1; i < trail_.size(); ++i) {
        const TrailPt& a = trail_[i - 1];
        const TrailPt& b = trail_[i];
        if (b.t < t0) continue;
        const double span = b.t - a.t;
        const float f = span > 0.0 ? (float)std::clamp((t0 - a.t) / span, 0.0, 1.0) : 1.0f;
        tx = a.x + (b.x - a.x) * f;
        ty = a.y + (b.y - a.y) * f;
        break;
    }
    const float hx = trail_.back().x, hy = trail_.back().y;
    const float len = std::hypot(hx - tx, hy - ty);
    // Only worth drawing once it reaches out from under the dot.
    const float vis_r = 0.5f * dot_size * sprites::kSpriteEdge;
    const float a = kTrailAlpha * dot_alpha * raster::smoothstep(0.6f * vis_r, 1.6f * vis_r, len);
    if (a <= 0.002f) return;
    const float k = cfg::TRAIL_DIST / cfg::OVERLAY_DIST;
    out.visible = true;
    out.x = (hx + tx) * 0.5f * k;
    out.y = (hy + ty) * 0.5f * k;
    out.z = -cfg::TRAIL_DIST;
    out.w = len * k;
    out.h = 2.0f * vis_r * sprites::kTrailWidth * k;
    out.alpha = a;
    out.rot = std::atan2(hy - ty, hx - tx);
}

void Presenter::layout_slots(const ViewGeometry& g) {
    const float k = cfg::TEXT_DIST / cfg::OVERLAY_DIST;
    const float cx = g.gaze.center_x;
    const float hw = (have_img_ ? body_w_m_ : 0.9f) * 0.5f;
    const float hh = (have_img_ ? body_h_m_ : 0.25f) * 0.5f;
    const float D = cfg::OVERLAY_DIST;
    auto fits = [&](float y) {
        return g.rect_visible((cx - hw) * k, (y - hh) * k, (cx + hw) * k, (y + hh) * k, -cfg::TEXT_DIST);
    };

    // Centre: straight ahead, nudged up/down only if a lens edge clips it.
    slot_ok_[CENTER] = false;
    for (int i = 0; i <= 12 && !slot_ok_[CENTER]; ++i) {
        for (int sgn : {-1, 1}) {
            const float y = sgn * i * kSlotStep;
            if (fits(y)) {
                slot_y_[CENTER] = y;
                slot_ok_[CENTER] = true;
                break;
            }
            if (i == 0) break;
        }
    }
    // Lower/upper: preferred elevation, pulled towards the centre until the whole
    // card is visible to both eyes (narrow or vertically asymmetric headsets).
    const float want_y[2] = {std::tan(kLowerDeg * kDeg2Rad) * D, std::tan(kUpperDeg * kDeg2Rad) * D};
    for (int side = 0; side < 2; ++side) {
        const int slot = side == 0 ? LOWER : UPPER;
        const float dir = side == 0 ? 1.0f : -1.0f;  // step direction towards centre
        float y = want_y[side];
        slot_ok_[slot] = false;
        while ((side == 0 ? y + hh <= -0.02f : y - hh >= 0.02f)) {
            if (fits(y)) {
                slot_ok_[slot] = true;
                break;
            }
            y += dir * kSlotStep;
        }
        slot_y_[slot] = y;
    }
}

bool Presenter::slot_rect(int slot, Rect& r) const {
    if (!have_img_) return false;
    const float hw = body_w_m_ * 0.5f, hh = body_h_m_ * 0.5f;
    r = {-hw, slot_y_[slot] - hh, hw, slot_y_[slot] + hh};
    return true;
}

bool Presenter::collides(const Rect& r, const std::vector<Circle>& cs) const {
    for (const Circle& c : cs) {
        const float x0 = r.x0 - kPanelMargin, x1 = r.x1 + kPanelMargin;
        const float y0 = r.y0 - kPanelMargin, y1 = r.y1 + kPanelMargin;
        const float qx = std::clamp(c.x, x0, x1), qy = std::clamp(c.y, y0, y1);
        if (std::hypot(c.x - qx, c.y - qy) < c.r) return true;
    }
    return false;
}

void Presenter::raster(const PanelContent& c, const ViewGeometry& g) {
    const float width_m = std::min(kMaxPanelW, 0.70f * (g.gaze.vis_right - g.gaze.vis_left));
    const int max_px = (int)(width_m * TextRenderer::kPixelsPerMetre);
    text_->render_panel(c, max_px, img_);
    body_w_m_ = (img_.w - 2 * TextRenderer::kShadowPad) / TextRenderer::kPixelsPerMetre;
    body_h_m_ = (img_.h - 2 * TextRenderer::kShadowPad) / TextRenderer::kPixelsPerMetre;
    shown_ = c;
    have_img_ = img_.w > 0;
}

void Presenter::update_panel(const Scene& s, float dt, const ViewGeometry& g) {
    // What the card must stay clear of: the dot where it is, where it is
    // heading (the ring marks the destination while the dot glides there), and
    // for continuously moving targets where it is about to be.
    hits_.clear();
    if (dot_vis_ > 0.02f && !outro_) {
        const float r = dot_size_ * 0.5f;
        hits_.push_back({dot_x_, dot_y_, r});
        if (ring_active_) {
            hits_.push_back({ring_x_, ring_y_, r});
        } else if (std::hypot(vel_x_, vel_y_) > 0.05f) {
            hits_.push_back({dot_x_ + vel_x_ * kPredictS, dot_y_ + vel_y_ * kPredictS, r});
            hits_.push_back({dot_x_ + vel_x_ * kPredictS * 0.5f, dot_y_ + vel_y_ * kPredictS * 0.5f, r});
        }
    }
    const bool target_present = !hits_.empty();

    PanelContent want;
    bool want_vis = s.text_visible && !outro_ && text_ && text_->ready();
    if (want_vis) {
        want.text = normalize_instruction(s.text);
        want_vis = !want.text.empty();
    }
    want.tone = s.tone;
    want.pips_total = std::max(0, s.progress_total);
    want.pips_done = std::clamp(s.progress_done, 0, std::max(0, s.progress_total));
    // Quantise the bar so a continuously advancing fraction re-rasterises ~100x, not per frame.
    want.bar = (s.progress_total <= 0 && s.progress_frac >= 0.0f)
                   ? std::round(std::min(s.progress_frac, 1.0f) * 100.0f) / 100.0f
                   : -1.0f;

    const bool text_changed = !have_img_ || want.text != shown_.text || want.tone != shown_.tone ||
                              want.pips_total != shown_.pips_total || (want.bar >= 0.0f) != (shown_.bar >= 0.0f);
    const bool progress_changed = have_img_ && !text_changed && want != shown_;

    if (panel_on_) {
        Rect cur;
        const bool need_out = !want_vis || text_changed || !slot_rect(slot_, cur) || !slot_ok_[slot_] ||
                              collides(cur, hits_) || (slot_ == CENTER && target_present);
        if (need_out)
            panel_on_ = false;
        else if (progress_changed)
            raster(want, g);  // same size; swap in place
    }

    if (!panel_on_ && panel_t_ <= 0.0f && want_vis) {
        if (text_changed || progress_changed) raster(want, g);
        layout_slots(g);
        if (!s.capture_hold) {
            // Below the target field is the comfortable reading position; the
            // upper slot is only a fallback while something occupies it.
            const int order_free[3] = {CENTER, LOWER, UPPER};
            const int order_target[2] = {LOWER, UPPER};
            const int* order = target_present ? order_target : order_free;
            const int n = target_present ? 2 : 3;
            for (int i = 0; i < n; ++i) {
                Rect r;
                if (slot_ok_[order[i]] && slot_rect(order[i], r) && !collides(r, hits_)) {
                    slot_ = order[i];
                    panel_on_ = true;
                    break;
                }
            }
        }
    }

    panel_t_ = ramp(panel_t_, panel_on_, dt, kPanelIn, kPanelOut);
    panel_alpha_out_ = raster::smoothstep(0.0f, 1.0f, panel_t_);
    panel_scale_out_ = 0.96f + 0.04f * ease_out_cubic(panel_t_);
}

bool Presenter::panel_body(Rect& r) const {
    if (panel_alpha_out_ <= 0.002f) return false;
    return slot_rect(slot_, r);
}
