// Desktop stand-in for CI compile checks: a window driven by the keyboard/mouse.
#include "platform/platform.h"

#include <sys/stat.h>
#include <sys/time.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>

#include <SDL.h>

namespace Platform {
namespace {

std::vector<std::vector<uint8_t>> g_fontData;
bool g_quit = false;
bool g_mouseDown = false;
float g_mouseX = 0, g_mouseY = 0;

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return !out.empty();
}

}  // namespace

bool Init() {
    mkdir("data", 0777);
    mkdir("data/downloads", 0777);
    mkdir("data/cache", 0777);
    return true;
}

void Shutdown() {}

bool Running() { return !g_quit; }

Input PollInput() {
    Input in;
    static uint32_t prevHeld = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_quit = true;
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) g_mouseDown = true;
        if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) g_mouseDown = false;
        if (e.type == SDL_MOUSEMOTION) {
            g_mouseX = (float)e.motion.x;
            g_mouseY = (float)e.motion.y;
        }
    }
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    struct Map {
        SDL_Scancode sc;
        uint32_t b;
    };
    static const Map kMap[] = {
        {SDL_SCANCODE_RETURN, BTN_A}, {SDL_SCANCODE_BACKSPACE, BTN_B}, {SDL_SCANCODE_X, BTN_X},
        {SDL_SCANCODE_Y, BTN_Y},      {SDL_SCANCODE_Q, BTN_L},         {SDL_SCANCODE_E, BTN_R},
        {SDL_SCANCODE_1, BTN_ZL},     {SDL_SCANCODE_3, BTN_ZR},        {SDL_SCANCODE_P, BTN_PLUS},
        {SDL_SCANCODE_M, BTN_MINUS},  {SDL_SCANCODE_UP, BTN_UP},       {SDL_SCANCODE_DOWN, BTN_DOWN},
        {SDL_SCANCODE_LEFT, BTN_LEFT}, {SDL_SCANCODE_RIGHT, BTN_RIGHT},
    };
    for (const auto& m : kMap)
        if (k[m.sc]) in.held |= m.b;
    in.down = in.held & ~prevHeld;
    in.up = prevHeld & ~in.held;
    prevHeld = in.held;
    in.touching = g_mouseDown;
    in.tx = g_mouseX;
    in.ty = g_mouseY;
    return in;
}

std::string DataDir() { return "data/"; }
std::string DownloadDir() { return "data/downloads/"; }

std::string AssetPath(const std::string& rel) {
    const char* root = getenv("TIKTOK_ROMFS");
    return std::string(root ? root : "romfs") + "/" + rel;
}

std::vector<FontBlob> SystemFonts() {
    std::vector<FontBlob> out;
    static const char* kCandidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    if (g_fontData.empty()) {
        for (const char* p : kCandidates) {
            std::vector<uint8_t> d;
            if (ReadFile(p, d)) g_fontData.push_back(std::move(d));
        }
    }
    for (auto& d : g_fontData) out.push_back({d.data(), d.size(), "system"});
    return out;
}

bool Keyboard(const std::string& guide, const std::string& initial, std::string& out, int, bool, bool) {
    std::cout << guide << " [" << initial << "]: " << std::flush;
    if (!std::getline(std::cin, out)) return false;
    if (out.empty()) out = initial;
    return !out.empty();
}

int64_t NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

LocalTime ToLocal(int64_t unixMs) {
    time_t t = (time_t)(unixMs / 1000);
    struct tm tmv {};
    localtime_r(&t, &tmv);
    LocalTime lt;
    lt.year = tmv.tm_year + 1900;
    lt.month = tmv.tm_mon + 1;
    lt.day = tmv.tm_mday;
    lt.hour = tmv.tm_hour;
    lt.minute = tmv.tm_min;
    lt.second = tmv.tm_sec;
    lt.wday = tmv.tm_wday;
    return lt;
}

bool NetworkUp() { return true; }

void Random(void* buf, size_t len) {
    static std::random_device rd;
    uint8_t* b = static_cast<uint8_t*>(buf);
    for (size_t i = 0; i < len; i++) b[i] = (uint8_t)rd();
}

bool AppletMode() { return false; }
void SetCpuBoost(bool) {}
std::string DeviceName() { return "Desktop"; }

}  // namespace Platform
