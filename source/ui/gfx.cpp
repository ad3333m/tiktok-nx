#include "ui/gfx.h"

#include <algorithm>
#include <cmath>

Color Mix(Color a, Color b, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return Color((uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t),
                 (uint8_t)(a.a + (b.a - a.a) * t));
}

Color Lighten(Color c, float amount) {
    if (amount >= 0) return Mix(c, Color(255, 255, 255, c.a), amount);
    return Mix(c, Color(0, 0, 0, c.a), -amount);
}

float Luma(Color c) { return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f; }

namespace Gfx {
namespace {

SDL_Renderer* R = nullptr;
std::vector<Rect> g_clips;
std::vector<SDL_Vertex> g_v;
std::vector<int> g_i;
constexpr float kPi = 3.14159265358979f;

SDL_Color C(Color c) { return SDL_Color{c.r, c.g, c.b, c.a}; }

struct UvMap {
    Rect r;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    SDL_FPoint At(float x, float y) const {
        return SDL_FPoint{u0 + (x - r.x) / r.w * (u1 - u0), v0 + (y - r.y) / r.h * (v1 - v0)};
    }
};

void Flush(SDL_Texture* tex) {
    if (!g_v.empty()) SDL_RenderGeometry(R, tex, g_v.data(), (int)g_v.size(), g_i.data(), (int)g_i.size());
    g_v.clear();
    g_i.clear();
}

SDL_Vertex V(float x, float y, Color c, const UvMap* uv) {
    SDL_Vertex v;
    v.position = SDL_FPoint{x, y};
    v.color = C(c);
    v.tex_coord = uv ? uv->At(x, y) : SDL_FPoint{0, 0};
    // SDL rejects textured geometry with coordinates outside 0..1 (the feathered edge pokes out).
    v.tex_coord.x = std::clamp(v.tex_coord.x, 0.0f, 1.0f);
    v.tex_coord.y = std::clamp(v.tex_coord.y, 0.0f, 1.0f);
    return v;
}

// Per-vertex outward miter vectors of a convex (or gently concave) closed outline.
void Normals(const std::vector<SDL_FPoint>& p, std::vector<SDL_FPoint>& n, bool closed) {
    const size_t count = p.size();
    n.assign(count, SDL_FPoint{0, 0});
    // Winding sign so normals point outwards for either orientation.
    float area = 0;
    for (size_t i = 0; i < count; i++) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % count];
        area += a.x * b.y - b.x * a.y;
    }
    const float s = area >= 0 ? 1.f : -1.f;
    auto edgeN = [&](size_t i, size_t j) {
        float dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) return SDL_FPoint{0, 0};
        return SDL_FPoint{dy / len * s, -dx / len * s};
    };
    for (size_t i = 0; i < count; i++) {
        SDL_FPoint a, b;
        if (closed) {
            a = edgeN((i + count - 1) % count, i);
            b = edgeN(i, (i + 1) % count);
        } else {
            a = i > 0 ? edgeN(i - 1, i) : edgeN(i, i + 1);
            b = i + 1 < count ? edgeN(i, i + 1) : edgeN(i - 1, i);
        }
        SDL_FPoint m{a.x + b.x, a.y + b.y};
        float len = std::sqrt(m.x * m.x + m.y * m.y);
        if (len < 1e-4f) {
            n[i] = b;
            continue;
        }
        m.x /= len;
        m.y /= len;
        float d = m.x * b.x + m.y * b.y;
        float scale = d > 0.25f ? 1.0f / d : 4.0f;
        n[i] = SDL_FPoint{m.x * scale, m.y * scale};
    }
}

// Fills a closed outline with a feathered edge `feather` px wide.
void FillOutline(const std::vector<SDL_FPoint>& p, Color c, float feather, SDL_Texture* tex = nullptr,
                 const UvMap* uv = nullptr, float inset = 0.5f) {
    if (p.size() < 3) return;
    std::vector<SDL_FPoint> n;
    Normals(p, n, true);
    float cx = 0, cy = 0;
    for (const auto& q : p) {
        cx += q.x;
        cy += q.y;
    }
    cx /= p.size();
    cy /= p.size();
    const int base = (int)g_v.size();
    const int count = (int)p.size();
    g_v.push_back(V(cx, cy, c, uv));
    Color clear = c.WithA(0);
    for (int i = 0; i < count; i++) {
        g_v.push_back(V(p[i].x - n[i].x * inset, p[i].y - n[i].y * inset, c, uv));
        g_v.push_back(V(p[i].x + n[i].x * (feather - inset), p[i].y + n[i].y * (feather - inset), clear, uv));
    }
    for (int i = 0; i < count; i++) {
        int j = (i + 1) % count;
        int in_i = base + 1 + i * 2, out_i = in_i + 1;
        int in_j = base + 1 + j * 2, out_j = in_j + 1;
        g_i.insert(g_i.end(), {base, in_i, in_j});
        g_i.insert(g_i.end(), {in_i, out_i, in_j});
        g_i.insert(g_i.end(), {in_j, out_i, out_j});
    }
    Flush(tex);
}

void StrokeOutline(const std::vector<SDL_FPoint>& p, bool closed, float width, Color c) {
    if (p.size() < 2) return;
    std::vector<SDL_FPoint> n;
    Normals(p, n, closed);
    const float h = std::max(width * 0.5f - 0.5f, 0.0f);
    const float f = h + 1.0f;
    Color clear = c.WithA(0);
    const int base = (int)g_v.size();
    const int count = (int)p.size();
    for (int i = 0; i < count; i++) {
        g_v.push_back(V(p[i].x + n[i].x * f, p[i].y + n[i].y * f, clear, nullptr));
        g_v.push_back(V(p[i].x + n[i].x * h, p[i].y + n[i].y * h, c, nullptr));
        g_v.push_back(V(p[i].x - n[i].x * h, p[i].y - n[i].y * h, c, nullptr));
        g_v.push_back(V(p[i].x - n[i].x * f, p[i].y - n[i].y * f, clear, nullptr));
    }
    const int segs = closed ? count : count - 1;
    for (int i = 0; i < segs; i++) {
        int a = base + i * 4, b = base + ((i + 1) % count) * 4;
        for (int k = 0; k < 3; k++) {
            g_i.insert(g_i.end(), {a + k, a + k + 1, b + k});
            g_i.insert(g_i.end(), {a + k + 1, b + k + 1, b + k});
        }
    }
    Flush(nullptr);
}

void RoundPath(const Rect& r, float tl, float tr, float br, float bl, std::vector<SDL_FPoint>& out) {
    const float maxR = std::min(r.w, r.h) * 0.5f;
    tl = std::min(tl, maxR);
    tr = std::min(tr, maxR);
    br = std::min(br, maxR);
    bl = std::min(bl, maxR);
    out.clear();
    auto corner = [&](float cx, float cy, float rad, float a0) {
        int segs = std::clamp((int)(rad * 0.8f), 5, 16);
        for (int i = 0; i <= segs; i++) {
            float a = a0 + (kPi / 2) * i / segs;
            out.push_back(SDL_FPoint{cx + std::cos(a) * rad, cy + std::sin(a) * rad});
        }
    };
    // Clockwise in screen space starting at the top-left corner.
    if (tl < 0.5f) out.push_back({r.x, r.y});
    else corner(r.x + tl, r.y + tl, tl, kPi);
    if (tr < 0.5f) out.push_back({r.x + r.w, r.y});
    else corner(r.x + r.w - tr, r.y + tr, tr, kPi * 1.5f);
    if (br < 0.5f) out.push_back({r.x + r.w, r.y + r.h});
    else corner(r.x + r.w - br, r.y + r.h - br, br, 0);
    if (bl < 0.5f) out.push_back({r.x, r.y + r.h});
    else corner(r.x + bl, r.y + r.h - bl, bl, kPi * 0.5f);
}

void CirclePath(float cx, float cy, float rad, std::vector<SDL_FPoint>& out) {
    int segs = std::clamp((int)(rad * 1.1f), 16, 96);
    out.clear();
    for (int i = 0; i < segs; i++) {
        float a = 2 * kPi * i / segs;
        out.push_back(SDL_FPoint{cx + std::cos(a) * rad, cy + std::sin(a) * rad});
    }
}

}  // namespace

void Init(SDL_Renderer* r) {
    R = r;
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
}

SDL_Renderer* Renderer() { return R; }

void Clear(Color c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_RenderClear(R);
}

static void ApplyClip() {
    if (g_clips.empty()) {
        SDL_RenderSetClipRect(R, nullptr);
        return;
    }
    const Rect& c = g_clips.back();
    SDL_Rect sr{(int)std::floor(c.x), (int)std::floor(c.y), (int)std::ceil(c.w), (int)std::ceil(c.h)};
    if (sr.w <= 0 || sr.h <= 0) sr = SDL_Rect{-10, -10, 1, 1};
    SDL_RenderSetClipRect(R, &sr);
}

void PushClip(const Rect& r) {
    Rect c = r;
    if (!g_clips.empty()) {
        const Rect& p = g_clips.back();
        float x0 = std::max(c.x, p.x), y0 = std::max(c.y, p.y);
        float x1 = std::min(c.Right(), p.Right()), y1 = std::min(c.Bottom(), p.Bottom());
        c = Rect{x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
    g_clips.push_back(c);
    ApplyClip();
}

void PopClip() {
    if (!g_clips.empty()) g_clips.pop_back();
    ApplyClip();
}

Rect CurrentClip() { return g_clips.empty() ? Rect{0, 0, (float)W, (float)H} : g_clips.back(); }

bool Visible(const Rect& r) {
    Rect c = CurrentClip();
    return r.x < c.Right() && r.Right() > c.x && r.y < c.Bottom() && r.Bottom() > c.y;
}

void FillRect(const Rect& r, Color c) {
    if (c.a == 0 || r.w <= 0 || r.h <= 0) return;
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_FRect fr{r.x, r.y, r.w, r.h};
    SDL_RenderFillRectF(R, &fr);
}

void FillRound(const Rect& r, float radius, Color c) { FillRoundCorners(r, radius, radius, radius, radius, c); }

void FillRoundCorners(const Rect& r, float tl, float tr, float br, float bl, Color c) {
    if (c.a == 0 || r.w <= 0 || r.h <= 0) return;
    if (tl < 0.5f && tr < 0.5f && br < 0.5f && bl < 0.5f) {
        FillRect(r, c);
        return;
    }
    std::vector<SDL_FPoint> p;
    RoundPath(r, tl, tr, br, bl, p);
    FillOutline(p, c, 1.0f);
}

void StrokeRound(const Rect& r, float radius, float width, Color c) {
    if (c.a == 0) return;
    std::vector<SDL_FPoint> p;
    Rect in = r.Inset(width * 0.5f);
    RoundPath(in, radius - width * 0.5f, radius - width * 0.5f, radius - width * 0.5f, radius - width * 0.5f, p);
    StrokeOutline(p, true, width, c);
}

void FillCircle(float cx, float cy, float radius, Color c) {
    if (c.a == 0 || radius <= 0) return;
    std::vector<SDL_FPoint> p;
    CirclePath(cx, cy, radius, p);
    FillOutline(p, c, 1.0f);
}

void StrokeCircle(float cx, float cy, float radius, float width, Color c) {
    if (c.a == 0) return;
    std::vector<SDL_FPoint> p;
    CirclePath(cx, cy, radius - width * 0.5f, p);
    StrokeOutline(p, true, width, c);
}

void Arc(float cx, float cy, float radius, float a0, float a1, float width, Color c) {
    int segs = std::clamp((int)(std::fabs(a1 - a0) * radius * 0.5f), 6, 96);
    std::vector<SDL_FPoint> p;
    for (int i = 0; i <= segs; i++) {
        float a = a0 + (a1 - a0) * i / segs;
        p.push_back(SDL_FPoint{cx + std::cos(a) * radius, cy + std::sin(a) * radius});
    }
    StrokeOutline(p, false, width, c);
}

void Line(float x0, float y0, float x1, float y1, float width, Color c) {
    std::vector<SDL_FPoint> p{{x0, y0}, {x1, y1}};
    StrokeOutline(p, false, width, c);
    if (width >= 2.5f) {  // round caps
        FillCircle(x0, y0, width * 0.5f, c);
        FillCircle(x1, y1, width * 0.5f, c);
    }
}

void Polyline(const std::vector<SDL_FPoint>& pts, float width, Color c, bool closed) {
    StrokeOutline(pts, closed, width, c);
    if (!closed && width >= 2.5f && pts.size() >= 2) {
        for (size_t i = 1; i + 1 < pts.size(); i++) FillCircle(pts[i].x, pts[i].y, width * 0.5f, c);
        FillCircle(pts.front().x, pts.front().y, width * 0.5f, c);
        FillCircle(pts.back().x, pts.back().y, width * 0.5f, c);
    }
}

void FillPolygon(const std::vector<SDL_FPoint>& pts, Color c) { FillOutline(pts, c, 1.0f); }

void Shadow(const Rect& r, float radius, float blur, Color c) {
    std::vector<SDL_FPoint> p;
    RoundPath(r, radius, radius, radius, radius, p);
    FillOutline(p, c, blur, nullptr, nullptr, blur * 0.5f);
}

void VGradient(const Rect& r, Color top, Color bottom) {
    const int base = (int)g_v.size();
    g_v.push_back(V(r.x, r.y, top, nullptr));
    g_v.push_back(V(r.Right(), r.y, top, nullptr));
    g_v.push_back(V(r.Right(), r.Bottom(), bottom, nullptr));
    g_v.push_back(V(r.x, r.Bottom(), bottom, nullptr));
    g_i.insert(g_i.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    Flush(nullptr);
}

void Image(SDL_Texture* t, const Rect& dst, float radius, Color tint) {
    if (!t) return;
    if (radius < 0.5f) {
        SDL_SetTextureColorMod(t, tint.r, tint.g, tint.b);
        SDL_SetTextureAlphaMod(t, tint.a);
        SDL_FRect fr{dst.x, dst.y, dst.w, dst.h};
        SDL_RenderCopyF(R, t, nullptr, &fr);
        SDL_SetTextureColorMod(t, 255, 255, 255);
        SDL_SetTextureAlphaMod(t, 255);
        return;
    }
    std::vector<SDL_FPoint> p;
    RoundPath(dst, radius, radius, radius, radius, p);
    UvMap uv{dst};
    FillOutline(p, tint, 1.0f, t, &uv);
}

void ImageCover(SDL_Texture* t, int tw, int th, const Rect& dst, float radius, Color tint) {
    if (!t || tw <= 0 || th <= 0) return;
    float srcAspect = (float)tw / th, dstAspect = dst.w / dst.h;
    UvMap uv{dst};
    if (srcAspect > dstAspect) {  // crop the sides
        float keep = dstAspect / srcAspect;
        uv.u0 = (1 - keep) * 0.5f;
        uv.u1 = 1 - uv.u0;
    } else {
        float keep = srcAspect / dstAspect;
        uv.v0 = (1 - keep) * 0.5f;
        uv.v1 = 1 - uv.v0;
    }
    std::vector<SDL_FPoint> p;
    RoundPath(dst, radius, radius, radius, radius, p);
    FillOutline(p, tint, radius < 0.5f ? 0.01f : 1.0f, t, &uv, radius < 0.5f ? 0.0f : 0.5f);
}

void ImageCircle(SDL_Texture* t, float cx, float cy, float radius, Color tint) {
    if (!t) return;
    std::vector<SDL_FPoint> p;
    CirclePath(cx, cy, radius, p);
    UvMap uv{Rect{cx - radius, cy - radius, radius * 2, radius * 2}};
    FillOutline(p, tint, 1.0f, t, &uv);
}

}  // namespace Gfx
