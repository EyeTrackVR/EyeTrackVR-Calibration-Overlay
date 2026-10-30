#pragma once
#include "fov.h"
#include "frame.h"
#include "scene.h"
#include "text.h"

#include <deque>

// ── Presenter ─────────────────────────────────────────────────────────────────
//
// Turns the Scene a mode produces into the Frame a backend composites, adding
// everything purely cosmetic: backdrop fade in/out, dot pop-in/out, the
// converging attention ring, the dot's motion trail, and the instruction panel.
//
// Panel layout is the important part. The panel is placed in one of a few
// slots (centre when no target is shown; below or above the target field
// otherwise), each verified to be fully visible to both eyes on this headset.
// Whenever the target — its current position, its destination, or where a
// moving target is about to be — would overlap the panel, the panel fades out
// and re-appears in a free slot. It never starts appearing or moving while the
// mode is capturing (Scene::capture_hold), so nothing pulls the eye away from
// the target while samples are being labelled.
//
// Backend-agnostic and deterministic (pure function of scenes + dt), which is
// what the unit tests exercise.

class Presenter {
public:
    struct Rect {
        float x0, y0, x1, y1;
    };  // overlay-plane metres

    explicit Presenter(TextRenderer* text);

    Frame update(const Scene& s, double dt, const ViewGeometry& g);

    void begin_outro();
    bool outro_active() const { return outro_; }
    bool outro_done() const;

    // Panel card (without its shadow) as currently shown, in overlay-plane
    // metres; false if the panel is not on screen. For tests/diagnostics.
    bool panel_body(Rect& r) const;
    float panel_alpha() const { return panel_alpha_out_; }

private:
    enum Slot { CENTER = 0, LOWER = 1, UPPER = 2, SLOT_COUNT };

    struct Circle {
        float x, y, r;
    };

    void update_target(const Scene& s, float dt);
    void update_panel(const Scene& s, float dt, const ViewGeometry& g);
    void layout_slots(const ViewGeometry& g);
    bool slot_rect(int slot, Rect& r) const;
    bool collides(const Rect& r, const std::vector<Circle>& cs) const;
    void raster(const PanelContent& c, const ViewGeometry& g);

    TextRenderer* text_;

    bool outro_ = false;
    float backdrop_ = 0.0f;  // 0..1 progress

    // Dot.
    float dot_vis_ = 0.0f;  // 0..1 visibility progress
    float pop_ = 1.0f;      // 0..1 pop-in progress
    float dot_x_ = 0.0f, dot_y_ = 0.0f, dot_size_ = 0.075f;
    float prev_x_ = 0.0f, prev_y_ = 0.0f;
    float vel_x_ = 0.0f, vel_y_ = 0.0f;
    bool have_prev_ = false;

    // Motion trail: recent dot positions (presenter clock), newest last.
    struct TrailPt {
        double t;
        float x, y;
    };
    std::deque<TrailPt> trail_;
    double clock_ = 0.0;
    void trail_quad(float dot_size, float dot_alpha, Quad& out) const;

    // Ring.
    float ring_in_ = 0.0f;
    float ring_x_ = 0.0f, ring_y_ = 0.0f, ring_size_ = 0.0f;
    bool ring_active_ = false;
    float ring_alpha_ = 0.0f;

    // Panel.
    Image img_;
    PanelContent shown_;
    bool have_img_ = false;
    float body_w_m_ = 0.0f, body_h_m_ = 0.0f;  // card size (overlay-plane metres)
    float panel_t_ = 0.0f;                     // 0..1 fade progress
    bool panel_on_ = false;                    // fading in / shown (vs out / hidden)
    int slot_ = CENTER;
    float slot_y_[SLOT_COUNT] = {0.0f, -0.8f, 0.7f};
    bool slot_ok_[SLOT_COUNT] = {true, true, true};
    float panel_alpha_out_ = 0.0f;
    float panel_scale_out_ = 1.0f;
    std::vector<Circle> hits_;  // what the panel must avoid this frame
};
