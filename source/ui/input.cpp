#include "ui/input.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Ui {
namespace {

struct HotSpot {
    Rect r;
    std::function<void()> tap, hold;
};

struct Zone {
    Rect r;
    float* value;
    float max;
    bool inverted;
};

Platform::Input g_in;
float g_dt = 0.016f;
double g_time = 0;
bool g_consumed = false;
std::unordered_map<uint32_t, float> g_held;  // button -> seconds held
std::unordered_map<uint32_t, float> g_next;  // button -> time until next repeat
std::vector<HotSpot> g_hot, g_hotPrev;
std::vector<Zone> g_zones, g_zonesPrev;

struct Touch {
    bool down = false;
    float x = 0, y = 0, sx = 0, sy = 0, lastY = 0;
    double start = 0;
    bool dragging = false, longFired = false;
    float* target = nullptr;
    bool inverted = false;
    float max = 0;
    float velocity = 0;
    float* fling = nullptr;
} g_touch;

float StickDir(float v) { return std::fabs(v) > 0.45f ? (v > 0 ? 1.f : -1.f) : 0.f; }

// Scales the repeat rate by how hard the stick is pushed: light tilt stays at
// `rate`, full deflection speeds up to roughly 3x. D-pad (|v|==0) uses `rate`.
float RepeatRate(float stickV, float rate) {
    float m = std::fabs(stickV);
    if (m < 0.45f) return rate;
    float t = (m - 0.45f) / 0.55f;  // 0..1 across the useful range
    if (t > 1.0f) t = 1.0f;
    return rate / (1.0f + 2.5f * t);
}

bool RepeatState(uint32_t key, bool held, float delay, float rate) {
    if (!held) {
        g_held.erase(key);
        g_next.erase(key);
        return false;
    }
    auto it = g_held.find(key);
    if (it == g_held.end()) {
        g_held[key] = 0;
        g_next[key] = delay;
        return true;
    }
    it->second += g_dt;
    float& next = g_next[key];
    next -= g_dt;
    if (next <= 0) {
        next += rate;
        return true;
    }
    return false;
}

const HotSpot* HitTest(float x, float y) {
    for (auto it = g_hotPrev.rbegin(); it != g_hotPrev.rend(); ++it)
        if (it->r.Contains(x, y)) return &*it;
    return nullptr;
}

void ClampValue(float* v, float max) { *v = std::clamp(*v, 0.0f, std::max(0.0f, max)); }

}  // namespace

void BeginFrame(const Platform::Input& in, float dt) {
    g_in = in;
    g_dt = std::min(dt, 0.1f);
    g_time += g_dt;
    g_consumed = false;

    // Touch state machine.
    Touch& t = g_touch;
    if (in.touching && !t.down) {
        t = Touch{};
        t.down = true;
        t.sx = t.x = in.tx;
        t.sy = t.y = t.lastY = in.ty;
        t.start = g_time;
        for (auto it = g_zonesPrev.rbegin(); it != g_zonesPrev.rend(); ++it) {
            if (it->r.Contains(in.tx, in.ty)) {
                t.target = it->value;
                t.inverted = it->inverted;
                t.max = it->max;
                break;
            }
        }
    } else if (in.touching && t.down) {
        t.x = in.tx;
        t.y = in.ty;
        if (!t.dragging && std::hypot(t.x - t.sx, t.y - t.sy) > 12) t.dragging = true;
        if (t.target && std::none_of(g_zonesPrev.begin(), g_zonesPrev.end(),
                                     [&](const Zone& z) { return z.value == t.target; }))
            t.target = nullptr;  // its view closed mid-drag
        if (t.dragging && t.target) {
            float dy = t.y - t.lastY;
            *t.target += t.inverted ? dy : -dy;
            ClampValue(t.target, t.max);
            t.velocity = t.velocity * 0.6f + (t.inverted ? dy : -dy) / std::max(g_dt, 0.001f) * 0.4f;
        }
        t.lastY = t.y;
        if (!t.dragging && !t.longFired && g_time - t.start > 0.5) {
            if (const HotSpot* h = HitTest(t.sx, t.sy)) {
                if (h->hold) {
                    t.longFired = true;
                    auto fn = h->hold;
                    fn();
                }
            }
        }
    } else if (!in.touching && t.down) {
        t.down = false;
        if (!t.dragging && !t.longFired && g_time - t.start < 0.8) {
            if (const HotSpot* h = HitTest(t.sx, t.sy)) {
                if (h->tap) {
                    auto fn = h->tap;
                    fn();
                }
            }
        }
        if (t.dragging && t.target && std::fabs(t.velocity) > 60) {
            t.fling = t.target;
        } else {
            t.velocity = 0;
        }
        t.dragging = false;
    }
    // Fling inertia while its zone is still on screen.
    if (!t.down && t.fling) {
        bool alive = false;
        for (const auto& z : g_zonesPrev)
            if (z.value == t.fling) {
                alive = true;
                t.max = z.max;
            }
        if (!alive || std::fabs(t.velocity) < 20) {
            t.fling = nullptr;
            t.velocity = 0;
        } else {
            *t.fling += t.velocity * g_dt;
            ClampValue(t.fling, t.max);
            t.velocity *= std::pow(0.05f, g_dt);
        }
    }
    g_hot.clear();
    g_zones.clear();
}

void EndFrame(bool drew) {
    if (drew) {
        g_hotPrev.swap(g_hot);
        g_zonesPrev.swap(g_zones);
    }
    g_hot.clear();
    g_zones.clear();
}

const Platform::Input& In() { return g_in; }
float Dt() { return g_dt; }
double Time() { return g_time; }

bool Active() {
    return g_in.held || g_in.down || g_in.up || g_in.touching || g_touch.down || std::fabs(g_in.lx) > 0.15f ||
           std::fabs(g_in.ly) > 0.15f || std::fabs(g_in.rx) > 0.15f || std::fabs(g_in.ry) > 0.15f;
}

bool Pressed(uint32_t b) { return !g_consumed && (g_in.down & b) != 0; }
bool Held(uint32_t b) { return !g_consumed && (g_in.held & b) != 0; }

bool Repeat(uint32_t b, float delay, float rate) {
    bool r = RepeatState(b, (g_in.held & b) != 0, delay, rate);
    return r && !g_consumed;
}

int NavY() {
    bool up = (g_in.held & Platform::BTN_UP) || StickDir(g_in.ly) > 0;
    bool down = (g_in.held & Platform::BTN_DOWN) || StickDir(g_in.ly) < 0;
    float rate = RepeatRate(g_in.ly, 0.07f);
    bool u = RepeatState(0x10000001u, up && !down, 0.28f, rate);
    bool d = RepeatState(0x10000002u, down && !up, 0.28f, rate);
    if (g_consumed) return 0;
    return u ? -1 : d ? 1 : 0;
}

int NavX() {
    bool left = (g_in.held & Platform::BTN_LEFT) || StickDir(g_in.lx) < 0;
    bool right = (g_in.held & Platform::BTN_RIGHT) || StickDir(g_in.lx) > 0;
    float rate = RepeatRate(g_in.lx, 0.11f);
    bool l = RepeatState(0x10000003u, left && !right, 0.35f, rate);
    bool r = RepeatState(0x10000004u, right && !left, 0.35f, rate);
    if (g_consumed) return 0;
    return l ? -1 : r ? 1 : 0;
}

int PageY() {
    bool u = RepeatState(0x10000005u, (g_in.held & Platform::BTN_ZL) != 0, 0.35f, 0.14f);
    bool d = RepeatState(0x10000006u, (g_in.held & Platform::BTN_ZR) != 0, 0.35f, 0.14f);
    if (g_consumed) return 0;
    return u ? -1 : d ? 1 : 0;
}

float StickScrollDelta(float pixelsPerSecond) {
    // Right stick: +y is up (console coord), so flip for scroll-down-as-positive.
    float v = g_in.ry;
    if (std::fabs(v) < 0.15f) return 0;
    // Soft curve so a light push nudges and a hard push flies.
    float m = (std::fabs(v) - 0.15f) / 0.85f;
    if (m > 1.0f) m = 1.0f;
    float curved = m * m;
    return -v / std::fabs(v) * curved * pixelsPerSecond * g_dt;
}

void Consume() { g_consumed = true; }

void Hot(const Rect& r, std::function<void()> onTap, std::function<void()> onLong) {
    g_hot.push_back(HotSpot{r, std::move(onTap), std::move(onLong)});
}

void Block(const Rect& r, std::function<void()> onTap) {
    g_hot.push_back(HotSpot{r, onTap ? std::move(onTap) : [] {}, nullptr});
    g_zones.push_back(Zone{r, nullptr, 0, false});
}

void ScrollZone(const Rect& r, float* value, float maxValue, bool inverted) {
    g_zones.push_back(Zone{r, value, maxValue, inverted});
    if (g_touch.target == value) g_touch.max = maxValue;
}

bool Touching() { return g_touch.down; }
bool Dragging() { return g_touch.dragging; }
bool Pressing(const Rect& r) { return g_touch.down && !g_touch.dragging && r.Contains(g_touch.sx, g_touch.sy); }
bool Animating() { return g_touch.fling != nullptr; }

}  // namespace Ui
