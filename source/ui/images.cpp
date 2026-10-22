#include "ui/images.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "net/async.h"
#include "net/http.h"
#include "platform/platform.h"
#include "stb_image.h"
#include "ui/gfx.h"
#include "util/log.h"
#include "util/strings.h"

namespace Images {
namespace {

enum class State { Queued, Ready, Failed };

struct Entry {
    State state = State::Queued;
    Img img;
    uint64_t lastUsed = 0;
    size_t bytes = 0;
    bool pinned = false;
    // Animation: every frame is its own texture; frames still waiting to be uploaded
    // are kept here so a long GIF doesn't stall a single frame.
    std::vector<SDL_Texture*> frames;
    std::vector<int> delays;
    int totalMs = 0;
    std::deque<std::string> pending;
    std::vector<int> pendingDelays;
};

struct Request {
    std::string url;
    int maxDim;
};

std::unordered_map<std::string, Entry> g_map;
uint64_t g_frame = 0;
size_t g_bytes = 0;
constexpr size_t kBudget = 112u << 20;
constexpr size_t kAnimBudget = 20u << 20;  // per animation, all frames together
constexpr size_t kUploadPerFrame = 6u << 20;

std::mutex g_qmu;
std::vector<Request> g_stack;  // newest request first out: what's on screen now wins
std::unordered_map<std::string, uint64_t> g_wanted;  // url -> last frame it was asked for
std::atomic<uint64_t> g_frameShared{0};
std::atomic<int> g_inflight{0};
std::atomic<bool> g_animate{true};
std::string g_cacheDir;
int64_t g_nextChange = INT64_MAX, g_nextChangeBuild = INT64_MAX;
bool g_uploading = false;

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string CachePath(const std::string& url) {
    return g_cacheDir + Str::Format("%016llx.img", (unsigned long long)Fnv(url));
}

bool ReadFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (8 << 20)) {
        fclose(f);
        return false;
    }
    out.resize((size_t)n);
    bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

void WriteFile(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

// Only content-addressed files are cached on the SD card (their URL changes when they do).
bool Cacheable(const std::string& url) {
    static const char* kPrefixes[] = {
        "https://cdn.discordapp.com/avatars/",  "https://cdn.discordapp.com/icons/",
        "https://cdn.discordapp.com/emojis/",   "https://cdn.discordapp.com/embed/avatars/",
        "https://media.discordapp.net/stickers/", "https://cdn.jsdelivr.net/gh/jdecked/twemoji",
        "https://media.tenor.com/",             "https://static.klipy.com/",
        "https://static2.klipy.com/",
    };
    for (const char* p : kPrefixes)
        if (Str::StartsWith(url, p)) return true;
    return false;
}

int64_t NowMs() { return (int64_t)SDL_GetTicks64(); }

void FreeEntry(Entry& e) {
    for (SDL_Texture* t : e.frames)
        if (t) SDL_DestroyTexture(t);
    if (e.frames.empty() && e.img.tex) SDL_DestroyTexture(e.img.tex);
    e.frames.clear();
    e.img.tex = nullptr;
    g_bytes -= std::min(g_bytes, e.bytes);
    e.bytes = 0;
}

void Worker() {
    Request req;
    {
        std::lock_guard<std::mutex> lk(g_qmu);
        if (g_stack.empty()) return;
        // Skip anything that scrolled out of view a while ago; it gets re-requested if needed.
        uint64_t now = g_frameShared.load();
        while (!g_stack.empty()) {
            req = g_stack.back();
            g_stack.pop_back();
            auto it = g_wanted.find(req.url);
            if (it != g_wanted.end() && now - it->second < 90) break;
            g_wanted.erase(req.url);
            std::string url = req.url;
            Async::Post([url] {
                auto e = g_map.find(url);
                if (e != g_map.end() && e->second.state == State::Queued) g_map.erase(e);
            });
            req.url.clear();
        }
        if (req.url.empty()) return;
        g_wanted.erase(req.url);
    }
    g_inflight++;
    // "#still" asks for the first frame only; it isn't part of the address.
    const size_t hash = req.url.find('#');
    const std::string fetchUrl = req.url.substr(0, hash);
    const bool still = hash != std::string::npos || !g_animate.load();
    std::string data;
    bool cacheable = Cacheable(fetchUrl) && !g_cacheDir.empty();
    std::string cachePath = cacheable ? CachePath(fetchUrl) : "";
    bool fromDisk = cacheable && ReadFile(cachePath, data);
    if (!fromDisk) {
        Http::Request hr;
        hr.url = fetchUrl;
        hr.timeoutMs = 40000;
        hr.maxBytes = 24u << 20;
        hr.headers.push_back("User-Agent: Mozilla/5.0 (X11; Linux x86_64) SwitchCord");
        Http::Response r = Http::Perform(hr);
        if (r.Ok()) data = std::move(r.body);
    }
    DecodedImage img;
    bool ok = !data.empty() && Decode(data, req.maxDim, still ? 1 : 0, img);
    if (ok && cacheable && !fromDisk && data.size() < (3u << 20)) WriteFile(cachePath, data);
    std::string url = req.url;
    Async::Post([url, ok, img = std::move(img)]() mutable {
        g_inflight--;
        auto it = g_map.find(url);
        if (it == g_map.end()) return;
        Entry& e = it->second;
        if (!ok) {
            e.state = State::Failed;
            e.img.failed = true;
            return;
        }
        // The first frame now; the rest trickle in from EndFrame.
        e.img.w = img.w;
        e.img.h = img.h;
        SDL_Texture* first = MakeTexture(img.frames[0], img.w, img.h);
        if (!first) {
            e.state = State::Failed;
            e.img.failed = true;
            return;
        }
        e.img.tex = first;
        e.frames.push_back(first);
        e.delays.push_back(img.delaysMs.empty() ? 100 : img.delaysMs[0]);
        e.bytes = (size_t)img.w * img.h * 4;
        g_bytes += e.bytes;
        for (size_t i = 1; i < img.frames.size(); i++) {
            e.pending.push_back(std::move(img.frames[i]));
            e.pendingDelays.push_back(i < img.delaysMs.size() ? img.delaysMs[i] : 100);
        }
        e.state = State::Ready;
        g_uploading = g_uploading || !e.pending.empty();
    });
}

void CleanCacheDir() {
    DIR* d = opendir(g_cacheDir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    while (struct dirent* ent = readdir(d)) {
        std::string n = ent->d_name;
        if (Str::EndsWith(n, ".img")) files.push_back(n);
    }
    closedir(d);
    if (files.size() < 6000) return;
    LOG("images: clearing %zu cached files", files.size());
    for (const auto& f : files) remove((g_cacheDir + f).c_str());
}

}  // namespace

void Init(const std::string& cacheDir) {
    g_cacheDir = cacheDir;
    if (!g_cacheDir.empty()) {
        mkdir(g_cacheDir.c_str(), 0777);
        CleanCacheDir();
    }
}

void Shutdown() {
    for (auto& kv : g_map) FreeEntry(kv.second);
    g_map.clear();
    g_bytes = 0;
}

void SetAnimate(bool on) { g_animate = on; }

bool Decode(const std::string& bytes, int maxDim, int maxFrames, DecodedImage& out) {
    const uint8_t* d = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t n = bytes.size();
    if (n > 6 && memcmp(d, "GIF8", 4) == 0) return Anim::DecodeGif(d, n, maxDim, kAnimBudget, maxFrames, out);
    if (n > 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0)
        return Anim::DecodeWebP(d, n, maxDim, kAnimBudget, maxFrames, out);
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(d, (int)n, &w, &h, &comp, 4);
    if (!px) return false;
    int nw, nh;
    Anim::FitSize(w, h, 1, maxDim, kAnimBudget, nw, nh);
    out = DecodedImage{};
    out.w = nw;
    out.h = nh;
    out.frames.push_back(Anim::Scale(px, w, h, nw, nh));
    out.delaysMs.push_back(0);
    stbi_image_free(px);
    return true;
}

SDL_Texture* MakeTexture(const std::string& rgba, int w, int h) {
    SDL_Texture* t = SDL_CreateTexture(Gfx::Renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    return t;
}

const Img* Get(const std::string& rawUrl, int maxDim) {
    if (rawUrl.empty()) return nullptr;
    // Built-in art ("mock://name") ignores the sizing query the views append.
    const std::string url = Str::StartsWith(rawUrl, "mock://") ? rawUrl.substr(0, rawUrl.find('?')) : rawUrl;
    auto it = g_map.find(url);
    if (it != g_map.end()) {
        Entry& e = it->second;
        e.lastUsed = g_frame;
        if (e.state == State::Failed) return &e.img;
        if (e.state == State::Ready) {
            // Play once every frame is uploaded; until then show the first.
            e.img.animated = e.frames.size() > 1 && e.pending.empty();
            if (e.img.animated && g_animate.load()) {
                const int64_t now = NowMs();
                int64_t t = now % e.totalMs, acc = 0;
                size_t i = 0;
                for (; i < e.delays.size(); i++) {
                    if (t < acc + e.delays[i]) break;
                    acc += e.delays[i];
                }
                if (i >= e.frames.size()) i = e.frames.size() - 1;
                e.img.tex = e.frames[i];
                g_nextChangeBuild = std::min(g_nextChangeBuild, now + (acc + e.delays[i] - t));
            } else if (!e.frames.empty()) {
                e.img.tex = e.frames[0];
            }
            return &e.img;
        }
        std::lock_guard<std::mutex> lk(g_qmu);
        g_wanted[url] = g_frame;
        return nullptr;
    }
    Entry& e = g_map[url];
    e.lastUsed = g_frame;
    {
        std::lock_guard<std::mutex> lk(g_qmu);
        g_stack.push_back({url, maxDim});
        g_wanted[url] = g_frame;
    }
    Async::Run(Async::Pool::Images, Worker);
    return nullptr;
}

void Put(const std::string& url, SDL_Texture* tex, int w, int h) {
    Entry& e = g_map[url];
    e.state = State::Ready;
    e.img = Img{tex, w, h, false, false};
    e.frames = {tex};
    e.delays = {100};
    e.pinned = true;
    e.lastUsed = g_frame;
}

void EndFrame(bool drew) {
    g_frame++;
    g_frameShared = g_frame;
    if (drew) {
        g_nextChange = g_nextChangeBuild;
        g_nextChangeBuild = INT64_MAX;
    }

    // Upload queued animation frames, a few megabytes per frame.
    if (g_uploading) {
        size_t budget = kUploadPerFrame;
        bool more = false;
        for (auto& kv : g_map) {
            Entry& e = kv.second;
            while (!e.pending.empty() && budget > 0) {
                SDL_Texture* t = MakeTexture(e.pending.front(), e.img.w, e.img.h);
                size_t sz = e.pending.front().size();
                e.pending.pop_front();
                int delay = e.pendingDelays.empty() ? 100 : e.pendingDelays.front();
                if (!e.pendingDelays.empty()) e.pendingDelays.erase(e.pendingDelays.begin());
                if (!t) {
                    e.pending.clear();
                    e.pendingDelays.clear();
                    break;
                }
                e.frames.push_back(t);
                e.delays.push_back(delay);
                e.bytes += sz;
                g_bytes += sz;
                budget = sz >= budget ? 0 : budget - sz;
            }
            e.totalMs = 0;
            for (int d : e.delays) e.totalMs += d;
            if (e.totalMs <= 0) e.totalMs = 100;
            if (!e.pending.empty()) more = true;
        }
        g_uploading = more;
    }

    if (g_bytes <= kBudget) return;
    std::vector<std::pair<uint64_t, std::string>> lru;
    for (auto& kv : g_map)
        if (kv.second.state == State::Ready && !kv.second.pinned && g_frame - kv.second.lastUsed > 30)
            lru.push_back({kv.second.lastUsed, kv.first});
    std::sort(lru.begin(), lru.end());
    for (auto& item : lru) {
        if (g_bytes <= kBudget * 3 / 4) break;
        auto it = g_map.find(item.second);
        FreeEntry(it->second);
        g_map.erase(it);
    }
}

bool Busy() {
    if (g_inflight.load() > 0 || g_uploading) return true;
    std::lock_guard<std::mutex> lk(g_qmu);
    return !g_stack.empty();
}

bool AnimationDue() { return g_animate.load() && NowMs() >= g_nextChange; }

}  // namespace Images
