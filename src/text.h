#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── Text rasteriser ───────────────────────────────────────────────────────────
//
// Replaces the GDI update_text() from the OpenVR build with a portable
// stb_truetype implementation. Produces a fixed-size RGBA texture with the
// supplied string word-wrapped and centred — white glyphs on the same dark
// translucent panel as the original (RGB 15,15,25, alpha 230).

class TextRenderer {
public:
    // Load a .ttf and allocate the RGBA target. Returns false on failure.
    bool load(const char* ttf_path, int tex_w, int tex_h, float pixel_height = 52.0f);

    // Rasterise `text` (ASCII; honours embedded '\n') into the internal buffer.
    void render(const std::string& text);

    const uint8_t* pixels() const { return pixels_.data(); }
    int width()  const { return w_; }
    int height() const { return h_; }
    bool ready() const { return !font_data_.empty(); }

private:
    int measure_run(const char* s, int len, float scale) const;

    std::vector<uint8_t> font_data_;
    std::vector<uint8_t> pixels_;
    int   w_ = 0, h_ = 0;
    float px_height_ = 52.0f;
    // Opaque stbtt_fontinfo kept as bytes to avoid leaking stb into the header.
    std::vector<uint8_t> font_info_;
};
