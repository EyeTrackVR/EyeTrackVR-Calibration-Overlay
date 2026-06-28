#include "text.h"

#include <cstring>
#include <fstream>
#include <iostream>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace {
constexpr uint8_t kBgR = 15, kBgG = 15, kBgB = 25, kBgA = 230;
constexpr float   kMarginFrac = 0.04f;   // side margin as a fraction of width
} // namespace

bool TextRenderer::load(const char* ttf_path, int tex_w, int tex_h, float pixel_height) {
    std::ifstream f(ttf_path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cout << "[WARN] Could not open font: " << ttf_path << "\n";
        return false;
    }
    const std::streamsize n = f.tellg();
    f.seekg(0);
    font_data_.resize((size_t)n);
    if (!f.read((char*)font_data_.data(), n)) {
        std::cout << "[WARN] Could not read font: " << ttf_path << "\n";
        font_data_.clear();
        return false;
    }

    font_info_.resize(sizeof(stbtt_fontinfo));
    auto* info = reinterpret_cast<stbtt_fontinfo*>(font_info_.data());
    if (!stbtt_InitFont(info, font_data_.data(),
                        stbtt_GetFontOffsetForIndex(font_data_.data(), 0))) {
        std::cout << "[WARN] stbtt_InitFont failed\n";
        font_data_.clear();
        return false;
    }

    w_ = tex_w;
    h_ = tex_h;
    px_height_ = pixel_height;
    pixels_.assign((size_t)w_ * h_ * 4, 0);
    return true;
}

int TextRenderer::measure_run(const char* s, int len, float scale) const {
    auto* info = reinterpret_cast<const stbtt_fontinfo*>(font_info_.data());
    float width = 0.0f;
    for (int i = 0; i < len; ++i) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(info, (unsigned char)s[i], &adv, &lsb);
        width += adv * scale;
    }
    return (int)(width + 0.5f);
}

void TextRenderer::render(const std::string& text) {
    if (!ready()) return;
    auto* info = reinterpret_cast<stbtt_fontinfo*>(font_info_.data());

    // Fill the translucent panel background.
    for (size_t i = 0; i < pixels_.size(); i += 4) {
        pixels_[i + 0] = kBgR;
        pixels_[i + 1] = kBgG;
        pixels_[i + 2] = kBgB;
        pixels_[i + 3] = kBgA;
    }

    const float scale = stbtt_ScaleForPixelHeight(info, px_height_);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
    const int line_h = (int)((ascent - descent + line_gap) * scale + 0.5f);
    const int max_w  = (int)(w_ * (1.0f - 2.0f * kMarginFrac));

    // Split into explicit lines (honouring '\n'), then word-wrap each to max_w.
    std::vector<std::string> lines;
    {
        auto wrap = [&](const std::string& para) {
            std::string line;
            size_t i = 0;
            while (i < para.size()) {
                size_t sp = para.find(' ', i);
                std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                std::string trial = line.empty() ? word : line + " " + word;
                if (measure_run(trial.c_str(), (int)trial.size(), scale) > max_w && !line.empty()) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = trial;
                }
                if (sp == std::string::npos) break;
                i = sp + 1;
            }
            lines.push_back(line);
        };
        size_t pos = 0;
        while (true) {
            size_t nl = text.find('\n', pos);
            std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            wrap(para);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    const int block_h = (int)lines.size() * line_h;
    int pen_y = (h_ - block_h) / 2;
    if (pen_y < 0) pen_y = 0;
    const int baseline_off = (int)(ascent * scale + 0.5f);

    for (const std::string& line : lines) {
        const int line_w = measure_run(line.c_str(), (int)line.size(), scale);
        float pen_x = (float)(w_ - line_w) / 2.0f;
        const int baseline = pen_y + baseline_off;

        for (size_t c = 0; c < line.size(); ++c) {
            const int ch = (unsigned char)line[c];
            int gw, gh, xoff, yoff;
            unsigned char* bmp = stbtt_GetCodepointBitmap(
                info, 0, scale, ch, &gw, &gh, &xoff, &yoff);

            int adv, lsb;
            stbtt_GetCodepointHMetrics(info, ch, &adv, &lsb);

            if (bmp) {
                const int gx = (int)(pen_x + 0.5f) + xoff;
                const int gy = baseline + yoff;
                for (int j = 0; j < gh; ++j) {
                    const int py = gy + j;
                    if (py < 0 || py >= h_) continue;
                    for (int k = 0; k < gw; ++k) {
                        const int px = gx + k;
                        if (px < 0 || px >= w_) continue;
                        const uint8_t cov = bmp[j * gw + k];
                        if (!cov) continue;
                        uint8_t* dst = &pixels_[((size_t)py * w_ + px) * 4];
                        // Blend white glyph over the panel by coverage.
                        const float a = cov / 255.0f;
                        dst[0] = (uint8_t)(dst[0] + (255 - dst[0]) * a);
                        dst[1] = (uint8_t)(dst[1] + (255 - dst[1]) * a);
                        dst[2] = (uint8_t)(dst[2] + (255 - dst[2]) * a);
                        if (cov > dst[3]) dst[3] = cov < kBgA ? kBgA : cov;
                    }
                }
                stbtt_FreeBitmap(bmp, nullptr);
            }
            pen_x += adv * scale;
        }
        pen_y += line_h;
    }
}
