#pragma once
#include "frame.h"
#include "scene.h"

#include <memory>
#include <string>
#include <vector>

// ── Instruction panel rasteriser ──────────────────────────────────────────────
//
// Portable stb_truetype implementation. Renders a compact, content-sized panel:
// rounded dark card with a soft shadow, a bold title (first line), a lighter
// subtitle (remaining lines), and optional progress pips or bar. Text is UTF-8;
// glyphs missing from the bundled font are looked up in system fallback fonts
// (loaded lazily) so translated strings do not render as garbage.

struct PanelContent {
    std::string text;  // '\n'-separated; first line is the title
    Tone tone = Tone::Normal;
    int pips_total = 0;  // > 0: pip row
    int pips_done = 0;
    float bar = -1.0f;  // >= 0 (and no pips): progress bar fraction

    bool operator==(const PanelContent& o) const {
        return text == o.text && tone == o.tone && pips_total == o.pips_total && pips_done == o.pips_done &&
               bar == o.bar;
    }
    bool operator!=(const PanelContent& o) const { return !(*this == o); }
};

class TextRenderer {
public:
    // Panel texels per metre on the overlay plane (≈34 px/deg at 3 m, roughly
    // the angular resolution of current high-end headsets).
    static constexpr float kPixelsPerMetre = 640.0f;
    // Transparent margin around the card that holds its shadow.
    static constexpr int kShadowPad = 24;
    // Hard limits so a swapchain/overlay sized once always fits.
    static constexpr int kMaxWidth = 1088;
    static constexpr int kMaxHeight = 576;

    TextRenderer();
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Load the primary font; `fallbacks` are only opened if a glyph is missing.
    bool load(const std::string& primary_ttf, const std::vector<std::string>& fallbacks = {});
    bool ready() const;

    // Rasterise the panel into `out` (resized to fit, version bumped). The card
    // body is limited to max_body_px wide; text wraps within it.
    void render_panel(const PanelContent& content, int max_body_px, Image& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// UTF-8 → code points; malformed sequences become U+FFFD.
std::u32string decode_utf8(const std::string& s);

// Normalise instruction text: drop control characters (keeping '\n'), trim
// each line, drop empty lines, capitalise each line's first letter.
std::string normalize_instruction(const std::string& s);
