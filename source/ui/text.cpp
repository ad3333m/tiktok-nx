#include "ui/text.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include "platform/platform.h"
#include "ui/emoji.h"
#include "ui/images.h"
#include "util/log.h"
#include "util/strings.h"

namespace Text {
namespace {

constexpr int kPage = 1024;

struct GlyphRec {
    int page = -1;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float left = 0, top = 0, w = 0, h = 0, advance = 0;
};

struct Page {
    SDL_Texture* tex = nullptr;
    int x = 1, y = 1, rowH = 0;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
};

FT_Library g_ft = nullptr;
std::array<FT_Face, (int)Face::Count> g_primary{};
std::vector<FT_Face> g_fallback;
std::vector<std::vector<uint8_t>> g_fileData;  // FT_New_Memory_Face needs the bytes alive
std::vector<Page> g_pages;
std::unordered_map<uint64_t, GlyphRec> g_glyphs;
std::unordered_map<uint32_t, std::unique_ptr<std::array<const GlyphRec*, 128>>> g_ascii;
std::unordered_map<uint32_t, Metrics> g_metrics;

bool LoadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

FT_Face OpenFace(const std::string& rel) {
    std::vector<uint8_t> data;
    if (!LoadFile(Platform::AssetPath(rel), data)) {
        LOG("text: missing font %s", rel.c_str());
        return nullptr;
    }
    g_fileData.push_back(std::move(data));
    auto& d = g_fileData.back();
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(g_ft, d.data(), (FT_Long)d.size(), 0, &face) != 0) return nullptr;
    return face;
}

uint32_t SizeKey(Face f, int size) { return ((uint32_t)f << 16) | (uint32_t)size; }

bool NewPage() {
    Page p;
    p.tex = SDL_CreateTexture(Gfx::Renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kPage, kPage);
    if (!p.tex) return false;
    SDL_SetTextureBlendMode(p.tex, SDL_BLENDMODE_BLEND);
    std::vector<uint32_t> clear((size_t)kPage * kPage, 0x00FFFFFFu);
    SDL_UpdateTexture(p.tex, nullptr, clear.data(), kPage * 4);
    g_pages.push_back(std::move(p));
    return true;
}

const GlyphRec* Rasterize(uint32_t cp, Face f, int size) {
    const uint64_t key = (uint64_t)cp | ((uint64_t)size << 24) | ((uint64_t)f << 40);
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return &it->second;

    GlyphRec rec;
    FT_Face face = g_primary[(int)f] ? g_primary[(int)f] : g_primary[0];
    FT_UInt index = face ? FT_Get_Char_Index(face, cp) : 0;
    bool fallback = false;
    if (!index) {
        for (FT_Face fb : g_fallback) {
            FT_UInt fi = FT_Get_Char_Index(fb, cp);
            if (fi) {
                face = fb;
                index = fi;
                fallback = true;
                break;
            }
        }
    }
    if (!face) return &(g_glyphs[key] = rec);
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
    if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) != 0) return &(g_glyphs[key] = rec);
    if (fallback && (f == Face::Bold || f == Face::SemiBold)) FT_GlyphSlot_Embolden(face->glyph);
    if (fallback && f == Face::Italic) FT_GlyphSlot_Oblique(face->glyph);
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return &(g_glyphs[key] = rec);

    FT_GlyphSlot g = face->glyph;
    rec.advance = g->advance.x / 64.0f;
    const FT_Bitmap& bm = g->bitmap;
    if (bm.width > 0 && bm.rows > 0) {
        int w = (int)bm.width, h = (int)bm.rows;
        if (g_pages.empty() && !NewPage()) return &(g_glyphs[key] = rec);
        Page* page = &g_pages.back();
        if (page->x + w + 1 > kPage) {
            page->x = 1;
            page->y += page->rowH + 1;
            page->rowH = 0;
        }
        if (page->y + h + 1 > kPage) {
            if (!NewPage()) return &(g_glyphs[key] = rec);
            page = &g_pages.back();
        }
        std::vector<uint32_t> px((size_t)w * h);
        for (int y = 0; y < h; y++) {
            const uint8_t* row = bm.buffer + y * bm.pitch;
            for (int x = 0; x < w; x++) px[(size_t)y * w + x] = ((uint32_t)row[x] << 24) | 0x00FFFFFFu;
        }
        SDL_Rect dst{page->x, page->y, w, h};
        SDL_UpdateTexture(page->tex, &dst, px.data(), w * 4);
        rec.page = (int)g_pages.size() - 1;
        rec.u0 = (float)page->x / kPage;
        rec.v0 = (float)page->y / kPage;
        rec.u1 = (float)(page->x + w) / kPage;
        rec.v1 = (float)(page->y + h) / kPage;
        rec.left = (float)g->bitmap_left;
        rec.top = (float)g->bitmap_top;
        rec.w = (float)w;
        rec.h = (float)h;
        page->x += w + 1;
        if (h > page->rowH) page->rowH = h;
    }
    return &(g_glyphs[key] = rec);
}

const GlyphRec* Get(uint32_t cp, Face f, int size) {
    if (cp < 128) {
        auto& arr = g_ascii[SizeKey(f, size)];
        if (!arr) {
            arr = std::make_unique<std::array<const GlyphRec*, 128>>();
            arr->fill(nullptr);
        }
        const GlyphRec*& slot = (*arr)[cp];
        if (!slot) slot = Rasterize(cp, f, size);
        return slot;
    }
    return Rasterize(cp, f, size);
}

bool Invisible(uint32_t cp) { return cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || cp == 0x200B || cp < 0x20; }

}  // namespace

bool Init() {
    if (FT_Init_FreeType(&g_ft) != 0) return false;
    g_primary[(int)Face::Regular] = OpenFace("fonts/Inter-Regular.ttf");
    g_primary[(int)Face::Medium] = OpenFace("fonts/Inter-Medium.ttf");
    g_primary[(int)Face::SemiBold] = OpenFace("fonts/Inter-SemiBold.ttf");
    g_primary[(int)Face::Bold] = OpenFace("fonts/Inter-Bold.ttf");
    g_primary[(int)Face::Italic] = OpenFace("fonts/Inter-Italic.ttf");
    g_primary[(int)Face::Mono] = OpenFace("fonts/JetBrainsMono-Regular.ttf");
    for (const auto& blob : Platform::SystemFonts()) {
        FT_Face face = nullptr;
        if (FT_New_Memory_Face(g_ft, static_cast<const FT_Byte*>(blob.data), (FT_Long)blob.size, 0, &face) == 0)
            g_fallback.push_back(face);
    }
    // Inter covers Latin/Greek/Cyrillic; symbols missing from Mono fall back to Inter.
    if (g_primary[(int)Face::Regular]) g_fallback.insert(g_fallback.begin(), g_primary[(int)Face::Regular]);
    return g_primary[(int)Face::Regular] != nullptr;
}

void Shutdown() {
    for (auto& p : g_pages)
        if (p.tex) SDL_DestroyTexture(p.tex);
    g_pages.clear();
    g_glyphs.clear();
    g_ascii.clear();
    if (g_ft) FT_Done_FreeType(g_ft);
    g_ft = nullptr;
}

Metrics GetMetrics(Face f, int size) {
    auto it = g_metrics.find(SizeKey(f, size));
    if (it != g_metrics.end()) return it->second;
    Metrics m;
    FT_Face face = g_primary[(int)f] ? g_primary[(int)f] : g_primary[0];
    if (face) {
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
        m.ascent = face->size->metrics.ascender / 64.0f;
        m.descent = -face->size->metrics.descender / 64.0f;
    } else {
        m.ascent = size * 0.95f;
        m.descent = size * 0.25f;
    }
    m.lineHeight = std::round(size * 1.32f);
    g_metrics[SizeKey(f, size)] = m;
    return m;
}

float Advance(uint32_t cp, Face f, int size) {
    if (Invisible(cp)) return 0;
    const GlyphRec* g = Get(cp, f, size);
    return g ? g->advance : 0;
}

float EmojiSize(int size) { return std::round(size * 1.25f); }

void Glyph(uint32_t cp, float penX, float baseline, Face f, int size, Color c) {
    if (Invisible(cp)) return;
    const GlyphRec* g = Get(cp, f, size);
    if (!g || g->page < 0) return;
    Page& p = g_pages[g->page];
    float x0 = std::round(penX + g->left), y0 = std::round(baseline - g->top);
    float x1 = x0 + g->w, y1 = y0 + g->h;
    SDL_Color col{c.r, c.g, c.b, c.a};
    int base = (int)p.v.size();
    p.v.push_back(SDL_Vertex{{x0, y0}, col, {g->u0, g->v0}});
    p.v.push_back(SDL_Vertex{{x1, y0}, col, {g->u1, g->v0}});
    p.v.push_back(SDL_Vertex{{x1, y1}, col, {g->u1, g->v1}});
    p.v.push_back(SDL_Vertex{{x0, y1}, col, {g->u0, g->v1}});
    p.idx.insert(p.idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void Flush() {
    for (auto& p : g_pages) {
        if (p.v.empty()) continue;
        SDL_RenderGeometry(Gfx::Renderer(), p.tex, p.v.data(), (int)p.v.size(), p.idx.data(), (int)p.idx.size());
        p.v.clear();
        p.idx.clear();
    }
}

float Width(std::string_view s, Face f, int size) {
    float w = 0;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end) {
        size_t n = EmojiText::Match(p, end, nullptr);
        if (n) {
            w += EmojiSize(size) + 2;
            p += n;
            continue;
        }
        w += Advance(Str::NextCp(p, end), f, size);
    }
    return w;
}

std::string Ellipsize(std::string_view s, Face f, int size, float maxW) {
    if (Width(s, f, size) <= maxW) return std::string(s);
    const float ell = Advance(0x2026, f, size);
    std::string out;
    float w = 0;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end) {
        const char* start = p;
        float adv;
        size_t n = EmojiText::Match(p, end, nullptr);
        if (n) {
            adv = EmojiSize(size) + 2;
            p += n;
        } else {
            adv = Advance(Str::NextCp(p, end), f, size);
        }
        if (w + adv + ell > maxW) break;
        out.append(start, p - start);
        w += adv;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + "\xE2\x80\xA6";
}

float Draw(std::string_view text, float x, float y, Face f, int size, Color c, float maxW) {
    std::string cut;
    std::string_view s = text;
    if (maxW > 0 && Width(text, f, size) > maxW) {
        cut = Ellipsize(text, f, size, maxW);
        s = cut;
    }
    const Metrics m = GetMetrics(f, size);
    const float baseline = std::round(y + (m.lineHeight - (m.ascent + m.descent)) * 0.5f + m.ascent);
    const float es = EmojiSize(size);
    struct Pending {
        std::string url;
        float x;
    };
    std::vector<Pending> emojis;
    float pen = x;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end) {
        std::string code;
        size_t n = EmojiText::Match(p, end, &code);
        if (n) {
            emojis.push_back({EmojiText::TwemojiUrl(code), pen + 1});
            pen += es + 2;
            p += n;
            continue;
        }
        uint32_t cp = Str::NextCp(p, end);
        Glyph(cp, pen, baseline, f, size, c);
        pen += Advance(cp, f, size);
    }
    Flush();
    for (const auto& e : emojis) {
        const Images::Img* img = Images::Get(e.url, 72);
        if (img && img->tex) Gfx::Image(img->tex, Rect{e.x, std::round(y + (m.lineHeight - es) * 0.5f), es, es}, 0, Color(255, 255, 255, c.a));
    }
    return pen - x;
}

float DrawCentered(std::string_view s, float cx, float y, Face f, int size, Color c, float maxW) {
    float w = Width(s, f, size);
    if (maxW > 0 && w > maxW) w = maxW;
    return Draw(s, std::round(cx - w * 0.5f), y, f, size, c, maxW);
}

float DrawRight(std::string_view s, float right, float y, Face f, int size, Color c) {
    float w = Width(s, f, size);
    return Draw(s, std::round(right - w), y, f, size, c);
}

}  // namespace Text
