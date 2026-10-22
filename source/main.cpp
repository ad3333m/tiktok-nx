#include <SDL.h>

#include <cstring>
#include <functional>

#include "app/app.h"
#include "net/async.h"
#include "net/http.h"
#include "platform/platform.h"
#include "tiktok/player.h"
#include "ui/gfx.h"
#include "ui/images.h"
#include "ui/input.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "util/log.h"

#ifndef __SWITCH__
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// CI preview: `tiktok --shots dir` plays the real feed headless and saves PNGs.
struct Shots {
    struct Step {
        const char* name;
        std::function<bool(App&, double)> ready;  // (app, seconds into this step)
        std::function<void(App&)> after;
    };
    std::string dir;
    std::vector<Step> steps;
    size_t at = 0;
    double start = -1;

    static bool Playing(App&, double) {
        return Player::Texture(nullptr, nullptr) && Player::Position() > 2.5;
    }

    // Returns true when every shot is taken.
    bool Take(App& app, SDL_Surface* s) {
        double now = Ui::Time();
        if (start < 0) start = now;
        Step& st = steps[at];
        bool timedOut = now - start > 60;
        if (!st.ready(app, now - start) && !timedOut) return false;
        if (timedOut) LOG("shot %s timed out", st.name);
        std::string path = dir + "/" + st.name + ".png";
        stbi_write_png(path.c_str(), s->w, s->h, 4, s->pixels, s->pitch);
        LOG("saved %s (video at %.1fs)", path.c_str(), Player::Position());
        if (st.after) st.after(app);
        start = -1;
        return ++at == steps.size();
    }
};
#endif

int main(int argc, char** argv) {
    std::string shotDir;
#ifndef __SWITCH__
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--shots")) shotDir = argv[i + 1];
#else
    (void)argc;
    (void)argv;
#endif
    Platform::Init();
    Log::Init(Platform::DataDir() + "log.txt");
    LOG("TikTok starting (%s)", Platform::AppletMode() ? "applet" : "application");
    Http::GlobalInit();
    Async::Start();
    InitTheme();

    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Surface* surface = nullptr;
    if (shotDir.empty()) {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
        win = SDL_CreateWindow("TikTok", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, Gfx::W, Gfx::H, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    } else {
        SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS);
        surface = SDL_CreateRGBSurfaceWithFormat(0, Gfx::W, Gfx::H, 32, SDL_PIXELFORMAT_RGBA32);
        ren = SDL_CreateSoftwareRenderer(surface);
    }
    if (!ren) {
        LOG("SDL setup failed: %s", SDL_GetError());
        return 1;
    }
    Gfx::Init(ren);
    if (!Text::Init()) LOG("fonts missing");
    Images::Init(Platform::DataDir() + "cache/");
    Player::Init();

    App app;
    app.Init();
#ifndef __SWITCH__
    Shots shots{shotDir,
                {
                    {"feed", Shots::Playing, [](App& a) { a.ToggleComments(); }},
                    {"comments", [](App& a, double t) { return t > 3 && !a.commentsBusy; },
                     [](App& a) {
                         a.ToggleComments();
                         a.Go(a.cur + 1);
                     }},
                    {"next", Shots::Playing, [](App&) { Player::SetPaused(true); }},
                    {"paused", [](App&, double t) { return t > 1; }, nullptr},
                }};
#endif
    uint64_t last = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    while (Platform::Running() && !app.quit) {
        Platform::Input in = Platform::PollInput();
        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)((now - last) / freq);
        last = now;
        bool drew = app.Frame(in, dt);
#ifndef __SWITCH__
        if (surface) {
            if (drew && shots.Take(app, surface)) break;
            SDL_Delay(16);
            continue;
        }
#endif
        if (drew) SDL_RenderPresent(ren);
        else SDL_Delay(8);
    }
    LOG("TikTok exiting");
    Http::AbortAll();
    app.Shutdown();
    Player::Shutdown();
    Async::Stop();
    Images::Shutdown();
    Text::Shutdown();
    SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (surface) SDL_FreeSurface(surface);
    SDL_Quit();
    Http::GlobalCleanup();
    Platform::Shutdown();
    return 0;
}
