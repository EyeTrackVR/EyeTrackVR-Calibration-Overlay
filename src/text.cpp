#include "text.h"
#include "raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

using raster::Rgba;

namespace {

// ── Panel style (px at TextRenderer::kPixelsPerMetre) ─────────────────────────
constexpr float kTitleEm = 60.0f;  // ≈1.8° at 3 m
constexpr float kSubEm = 44.0f;
constexpr float kTitleLine = 1.24f;  // line height / em
constexpr float kSubLine = 1.30f;
constexpr float kPadX = 44.0f;
constexpr float kPadY = 28.0f;
constexpr float kTitleGap = 4.0f;  // title block → subtitle block
constexpr float kRadius = 24.0f;
constexpr float kMinBodyW = 320.0f;
constexpr int kMaxTitleLines = 2;
constexpr int kMaxSubLines = 3;
constexpr float kPipR = 7.5f;
constexpr float kPipStep = 25.0f;
constexpr float kRowGap = 18.0f;  // text → progress row
constexpr float kBarH = 7.0f;
constexpr float kBarMinW = 260.0f;
constexpr int kMaxPips = 24;

constexpr Rgba kCardTop = raster::hex(0x1C1F2B, 0.92f);
constexpr Rgba kCardBottom = raster::hex(0x10121A, 0.92f);
constexpr Rgba kBorder = raster::hex(0xFFFFFF, 0.10f);
constexpr Rgba kTitle = raster::hex(0xFFFFFF);
constexpr Rgba kTitleOk = raster::hex(0x8DF0B8);
constexpr Rgba kSub = raster::hex(0xC4C8D6);
constexpr Rgba kAccent = raster::hex(0xB58CFF);
constexpr Rgba kAccentOk = raster::hex(0x6BE3A0);
constexpr Rgba kTrack = raster::hex(0xFFFFFF, 0.16f);

struct Font {
    std::string path;
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool tried = false;
    bool ok = false;
    float ascent = 0.9f, descent = -0.25f;  // per em
};

bool load_font(Font& f) {
    f.tried = true;
    std::ifstream in(f.path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff n = in.tellg();
    if (n <= 0 || n > (std::streamoff)96 * 1024 * 1024) return false;
    f.data.resize((size_t)n);
    in.seekg(0);
    if (!in.read((char*)f.data.data(), n)) {
        f.data.clear();
        return false;
    }
    const int off = stbtt_GetFontOffsetForIndex(f.data.data(), 0);
    if (off < 0 || !stbtt_InitFont(&f.info, f.data.data(), off)) {
        f.data.clear();
        return false;
    }
    int a = 0, d = 0, g = 0;
    stbtt_GetFontVMetrics(&f.info, &a, &d, &g);
    const float s = stbtt_ScaleForMappingEmToPixels(&f.info, 1.0f);
    f.ascent = a * s;
    f.descent = d * s;
    f.ok = true;
    return true;
}

struct Placed {
    const Font* font;
    int glyph;
    float x;  // pen x within the line
    float scale;
};

struct Line {
    std::vector<Placed> glyphs;
    float width = 0.0f;
};

}  // namespace

struct TextRenderer::Impl {
    std::vector<std::unique_ptr<Font>> fonts;  // [0] = primary

    // First font (primary, then fallbacks in order) that has the glyph.
    const Font* font_for(char32_t cp, int& glyph) {
        for (size_t i = 0; i < fonts.size(); ++i) {
            Font& f = *fonts[i];
            if (!f.tried) {
                if (load_font(f)) std::cout << "[INFO] Loaded fallback font " << f.path << "\n";
            }
            if (!f.ok) continue;
            const int g = stbtt_FindGlyphIndex(&f.info, (int)cp);
            if (g != 0) {
                glyph = g;
                return &f;
            }
        }
        glyph = 0;  // .notdef box from the primary font
        return fonts[0].get();
    }

    float layout(const std::u32string& s, float em, Line* out) {
        float x = 0.0f;
        const Font* pf = nullptr;
        int pg = 0;
        for (char32_t cp : s) {
            int g = 0;
            const Font* f = font_for(cp, g);
            const float sc = stbtt_ScaleForMappingEmToPixels(&f->info, em);
            if (f == pf && pg && g) x += stbtt_GetGlyphKernAdvance(&f->info, pg, g) * sc;
            if (out) out->glyphs.push_back({f, g, x, sc});
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
            x += adv * sc;
            pf = f;
            pg = g;
        }
        if (out) out->width = x;
        return x;
    }

    // Greedy word wrap of one paragraph into at most max_lines lines.
    void wrap(const std::u32string& para, float em, float max_w, int max_lines, std::vector<Line>& lines) {
        std::vector<std::u32string> words;
        std::u32string cur;
        for (char32_t c : para) {
            if (c == U' ') {
                if (!cur.empty()) words.push_back(cur);
                cur.clear();
            } else
                cur.push_back(c);
        }
        if (!cur.empty()) words.push_back(cur);

        std::vector<std::u32string> out;
        std::u32string line;
        auto flush = [&] {
            if (!line.empty()) out.push_back(line);
            line.clear();
        };
        for (std::u32string w : words) {
            // Hard-break words wider than the line (long URLs, CJK runs).
            while (layout(w, em, nullptr) > max_w && w.size() > 1) {
                flush();
                size_t n = w.size() - 1;
                while (n > 1 && layout(w.substr(0, n), em, nullptr) > max_w) --n;
                out.push_back(w.substr(0, n));
                w = w.substr(n);
            }
            std::u32string trial = line;
            if (!trial.empty()) trial += U' ';
            trial += w;
            if (!line.empty() && layout(trial, em, nullptr) > max_w) {
                flush();
                line = w;
            } else
                line = trial;
        }
        flush();
        if ((int)out.size() > max_lines) {
            out.resize((size_t)max_lines);
            std::u32string& last = out.back();
            while (!last.empty() && layout(last + U"…", em, nullptr) > max_w) last.pop_back();
            while (!last.empty() && last.back() == U' ') last.pop_back();
            last += U'…';
        }
        for (const auto& s : out) {
            Line l;
            layout(s, em, &l);
            lines.push_back(std::move(l));
        }
    }
};

TextRenderer::TextRenderer() : impl_(std::make_unique<Impl>()) {}
TextRenderer::~TextRenderer() = default;

bool TextRenderer::load(const std::string& primary, const std::vector<std::string>& fallbacks) {
    impl_->fonts.clear();
    auto f = std::make_unique<Font>();
    f->path = primary;
    if (!load_font(*f)) {
        std::cout << "[WARN] Could not load font: " << primary << "\n";
        // Promote the first loadable fallback so text still works.
        for (const auto& p : fallbacks) {
            auto alt = std::make_unique<Font>();
            alt->path = p;
            if (load_font(*alt)) {
                std::cout << "[INFO] Using system font " << p << " instead\n";
                f = std::move(alt);
                break;
            }
        }
        if (!f->ok) return false;
    }
    impl_->fonts.push_back(std::move(f));
    for (const auto& p : fallbacks) {
        if (p == impl_->fonts[0]->path) continue;
        auto fb = std::make_unique<Font>();
        fb->path = p;  // opened lazily on first missing glyph
        impl_->fonts.push_back(std::move(fb));
    }
    return true;
}

bool TextRenderer::ready() const { return !impl_->fonts.empty() && impl_->fonts[0]->ok; }

namespace {

void draw_line(Image& img, const Line& line, float x, float baseline, Rgba col, bool bold) {
    std::vector<uint8_t> buf;
    for (const Placed& p : line.glyphs) {
        if (!p.glyph) continue;
        const int passes = bold ? 2 : 1;
        for (int pass = 0; pass < passes; ++pass) {
            const float fx = x + p.x + (pass ? 0.7f : 0.0f);
            const int ix = (int)std::floor(fx);
            const float sub = fx - ix;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&p.font->info, p.glyph, p.scale, p.scale, sub, 0.0f, &x0, &y0, &x1, &y1);
            const int w = x1 - x0, h = y1 - y0;
            if (w <= 0 || h <= 0) continue;
            buf.assign((size_t)w * h, 0);
            stbtt_MakeGlyphBitmapSubpixel(&p.font->info, buf.data(), w, h, w, p.scale, p.scale, sub, 0.0f, p.glyph);
            const int by = (int)std::lround(baseline);
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i) {
                    const uint8_t c = buf[(size_t)j * w + i];
                    if (!c) continue;
                    // Light-on-dark text reads thin when blended in sRGB; lift
                    // the coverage curve slightly to compensate.
                    const float cov = std::pow(c / 255.0f, 0.82f);
                    raster::blend_at(img, ix + x0 + i, by + y0 + j, col, cov);
                }
        }
    }
}

void draw_check(Image& img, float x, float y, float size, Rgba col) {
    const float th = size * 0.085f;
    const float ax = x + size * 0.14f, ay = y + size * 0.52f;
    const float bx = x + size * 0.40f, by = y + size * 0.78f;
    const float cx = x + size * 0.88f, cy = y + size * 0.24f;
    const int x0 = std::max(0, (int)(x - 2)), x1 = std::min(img.w - 1, (int)(x + size + 2));
    const int y0 = std::max(0, (int)(y - 2)), y1 = std::min(img.h - 1, (int)(y + size + 2));
    for (int py = y0; py <= y1; ++py)
        for (int px = x0; px <= x1; ++px) {
            const float fx = px + 0.5f, fy = py + 0.5f;
            const float d = std::min(raster::sd_capsule(fx, fy, ax, ay, bx, by, th),
                                     raster::sd_capsule(fx, fy, bx, by, cx, cy, th));
            const float cov = raster::coverage(d);
            if (cov > 0.0f) raster::blend_at(img, px, py, col, cov);
        }
}

}  // namespace

void TextRenderer::render_panel(const PanelContent& content, int max_body_px, Image& out) {
    if (!ready()) return;
    Impl& im = *impl_;
    const Font& prim = *im.fonts[0];
    const bool ok_tone = content.tone == Tone::Success;

    max_body_px = std::clamp(max_body_px, (int)kMinBodyW, kMaxWidth - 2 * kShadowPad);

    // Split into title + subtitle paragraphs.
    const std::u32string all = decode_utf8(normalize_instruction(content.text));
    std::vector<std::u32string> paras;
    {
        std::u32string cur;
        for (char32_t c : all) {
            if (c == U'\n') {
                paras.push_back(cur);
                cur.clear();
            } else
                cur.push_back(c);
        }
        paras.push_back(cur);
    }

    const float icon = ok_tone ? kTitleEm * 0.80f : 0.0f;
    const float icon_gap = ok_tone ? 12.0f : 0.0f;
    const float text_max = (float)max_body_px - 2.0f * kPadX;

    std::vector<Line> title, sub;
    im.wrap(paras[0], kTitleEm, text_max - icon - icon_gap, kMaxTitleLines, title);
    for (size_t i = 1; i < paras.size() && (int)sub.size() < kMaxSubLines; ++i) {
        std::vector<Line> tmp;
        im.wrap(paras[i], kSubEm, text_max, kMaxSubLines - (int)sub.size(), tmp);
        for (auto& l : tmp) sub.push_back(std::move(l));
    }

    // Progress row: pips for short sequences, a bar otherwise.
    int pips = content.pips_total;
    float bar = content.bar;
    if (pips > kMaxPips) {
        bar = (float)content.pips_done / (float)pips;
        pips = 0;
    }
    const bool has_row = pips > 0 || bar >= 0.0f;

    float text_w = 0.0f;
    for (size_t i = 0; i < title.size(); ++i)
        text_w = std::max(text_w, title[i].width + (i == 0 ? icon + icon_gap : 0.0f));
    for (const Line& l : sub) text_w = std::max(text_w, l.width);
    if (pips > 0) text_w = std::max(text_w, pips * kPipStep);
    if (pips == 0 && bar >= 0.0f) text_w = std::max(text_w, kBarMinW);

    const float body_w = std::clamp(std::ceil(text_w + 2.0f * kPadX), kMinBodyW, (float)max_body_px);
    const float lh_t = kTitleEm * kTitleLine, lh_s = kSubEm * kSubLine;
    float body_h = 2.0f * kPadY + title.size() * lh_t;
    if (!sub.empty()) body_h += kTitleGap + sub.size() * lh_s;
    if (has_row) body_h += kRowGap + (pips > 0 ? 2.0f * kPipR : kBarH);
    body_h = std::ceil(body_h);

    const int W = std::min(kMaxWidth, (int)body_w + 2 * kShadowPad);
    const int H = std::min(kMaxHeight, (int)body_h + 2 * kShadowPad);
    raster::alloc(out, W, H);

    const float bx = (float)kShadowPad, by = (float)kShadowPad;
    const float bw = (float)W - 2.0f * kShadowPad, bh = (float)H - 2.0f * kShadowPad;
    const float ccx = bx + bw * 0.5f, ccy = by + bh * 0.5f;

    // Card: soft drop shadow, vertical gradient body, hairline border.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            uint8_t* dst = &out.rgba[((size_t)y * W + x) * 4];
            const float ds = raster::sd_round_rect(px, py - 5.0f, ccx, ccy, bw * 0.5f, bh * 0.5f, kRadius);
            const float sa = 0.38f * (1.0f - raster::smoothstep(-6.0f, (float)kShadowPad - 2.0f, ds));
            if (sa > 0.0f) raster::blend(dst, raster::hex(0x000000), sa);
            const float d = raster::sd_round_rect(px, py, ccx, ccy, bw * 0.5f, bh * 0.5f, kRadius);
            const float cov = raster::coverage(d);
            if (cov > 0.0f) {
                const float t = raster::clamp01((py - by) / bh);
                raster::blend(dst, raster::mix(kCardTop, kCardBottom, t), cov);
                const float edge = raster::coverage(std::fabs(d + 1.0f) - 0.75f);
                if (edge > 0.0f) raster::blend(dst, kBorder, edge * cov);
            }
        }

    // Text block.
    float y = by + kPadY;
    auto baseline_for = [&](float em, float lh) {
        const float asc = prim.ascent * em, desc = prim.descent * em;
        return (lh - (asc - desc)) * 0.5f + asc;
    };
    for (size_t i = 0; i < title.size(); ++i) {
        const float extra = (i == 0) ? icon + icon_gap : 0.0f;
        float x = ccx - (title[i].width + extra) * 0.5f;
        if (i == 0 && ok_tone) {
            draw_check(out, x, y + (lh_t - icon) * 0.5f, icon, kAccentOk);
            x += icon + icon_gap;
        }
        draw_line(out, title[i], x, y + baseline_for(kTitleEm, lh_t), ok_tone ? kTitleOk : kTitle, true);
        y += lh_t;
    }
    if (!sub.empty()) {
        y += kTitleGap;
        for (const Line& l : sub) {
            draw_line(out, l, ccx - l.width * 0.5f, y + baseline_for(kSubEm, lh_s), kSub, false);
            y += lh_s;
        }
    }

    // Progress row.
    if (has_row) {
        y += kRowGap;
        const Rgba accent = ok_tone ? kAccentOk : kAccent;
        if (pips > 0) {
            const float cy = y + kPipR;
            const float x0 = ccx - (pips - 1) * kPipStep * 0.5f;
            for (int i = 0; i < pips; ++i) {
                const float cx = x0 + i * kPipStep;
                if (i < content.pips_done || ok_tone) {
                    raster::fill_disc(out, cx, cy, kPipR, accent);
                } else if (i == content.pips_done) {
                    raster::fill_disc(out, cx, cy, kPipR, kTrack);
                    raster::stroke_circle(out, cx, cy, kPipR - 1.0f, 1.1f, raster::hex(0xFFFFFF, 0.85f));
                } else {
                    raster::fill_disc(out, cx, cy, kPipR * 0.72f, kTrack);
                }
            }
        } else {
            const float w = std::min(bw - 2.0f * kPadX, std::max(kBarMinW, text_w));
            const float x0 = ccx - w * 0.5f, hh = kBarH * 0.5f, cy = y + hh;
            const float fill = raster::clamp01(bar);
            for (int py = (int)(cy - hh - 1); py <= (int)(cy + hh + 1); ++py)
                for (int px = (int)(x0 - 1); px <= (int)(x0 + w + 1); ++px) {
                    const float fx = px + 0.5f, fy = py + 0.5f;
                    const float dt = raster::sd_round_rect(fx, fy, ccx, cy, w * 0.5f, hh, hh);
                    raster::blend_at(out, px, py, kTrack, raster::coverage(dt));
                    if (fill > 0.0f) {
                        const float fw = std::max(w * fill, kBarH);
                        const float df = raster::sd_round_rect(fx, fy, x0 + fw * 0.5f, cy, fw * 0.5f, hh, hh);
                        raster::blend_at(out, px, py, accent, raster::coverage(df));
                    }
                }
        }
    }
}

// ── UTF-8 / normalisation ─────────────────────────────────────────────────────

std::u32string decode_utf8(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        int len = 0;
        char32_t cp = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + len > n) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        // Reject overlongs, surrogates and out-of-range values.
        static const char32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (!ok || cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += (size_t)len;
    }
    return out;
}

std::string normalize_instruction(const std::string& s) {
    std::string out;
    std::string line;
    auto flush = [&] {
        size_t a = line.find_first_not_of(' ');
        size_t b = line.find_last_not_of(' ');
        if (a != std::string::npos) {
            std::string t = line.substr(a, b - a + 1);
            if (t[0] >= 'a' && t[0] <= 'z') t[0] = (char)(t[0] - 'a' + 'A');
            if (!out.empty()) out.push_back('\n');
            out += t;
        }
        line.clear();
    };
    for (char ch : s) {
        const unsigned char c = (unsigned char)ch;
        if (c == '\n') {
            flush();
            continue;
        }
        if (c == '\t') {
            line.push_back(' ');
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;  // control characters (incl. '\r', NUL)
        line.push_back(ch);
    }
    flush();
    return out;
}
