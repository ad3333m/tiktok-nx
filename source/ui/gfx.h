#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <SDL.h>

// Anti-aliased drawing on top of SDL_RenderGeometry. Every shape gets a 1px feathered
// edge so circles and rounded cards stay smooth on the 720p panel.
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    constexpr Color() = default;
    constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
    static constexpr Color Hex(uint32_t rgb, uint8_t a = 255) {
        return Color((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, a);
    }
    Color Alpha(float f) const { return Color(r, g, b, (uint8_t)(a * (f < 0 ? 0 : f > 1 ? 1 : f))); }
    Color WithA(uint8_t na) const { return Color(r, g, b, na); }
    uint32_t Rgb() const { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

Color Mix(Color a, Color b, float t);
Color Lighten(Color c, float amount);  // towards white (negative: towards black)
float Luma(Color c);

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
};

namespace Gfx {

constexpr int W = 1280;
constexpr int H = 720;

void Init(SDL_Renderer* r);
SDL_Renderer* Renderer();

void Clear(Color c);
void PushClip(const Rect& r);
void PopClip();
Rect CurrentClip();
bool Visible(const Rect& r);  // intersects the clip

void FillRect(const Rect& r, Color c);
void FillRound(const Rect& r, float radius, Color c);
void FillRoundCorners(const Rect& r, float tl, float tr, float br, float bl, Color c);
void StrokeRound(const Rect& r, float radius, float width, Color c);
void FillCircle(float cx, float cy, float radius, Color c);
void StrokeCircle(float cx, float cy, float radius, float width, Color c);
void Arc(float cx, float cy, float radius, float a0, float a1, float width, Color c);
void Line(float x0, float y0, float x1, float y1, float width, Color c);
void Polyline(const std::vector<SDL_FPoint>& pts, float width, Color c, bool closed = false);
void FillPolygon(const std::vector<SDL_FPoint>& pts, Color c);  // convex
void Shadow(const Rect& r, float radius, float blur, Color c);
void VGradient(const Rect& r, Color top, Color bottom);

// Textures. Rounded/circle variants feather their edges too.
void Image(SDL_Texture* t, const Rect& dst, float radius = 0, Color tint = Color(255, 255, 255));
void ImageCover(SDL_Texture* t, int tw, int th, const Rect& dst, float radius = 0, Color tint = Color(255, 255, 255));
void ImageCircle(SDL_Texture* t, float cx, float cy, float radius, Color tint = Color(255, 255, 255));

}  // namespace Gfx
