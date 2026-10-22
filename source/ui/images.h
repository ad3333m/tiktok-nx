#pragma once

#include <string>

#include <SDL.h>

#include "ui/anim.h"

// Remote images (avatars, icons, emoji, attachments, GIFs): downloaded and decoded on
// the image pool, uploaded on the main thread, evicted least-recently-used.
// Animated GIF/WebP play automatically: `tex` is the frame for the current time.
namespace Images {

struct Img {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    bool failed = false;
    bool animated = false;
};

void Init(const std::string& cacheDir);
void Shutdown();
// Returns the image once it's ready (nullptr before, or img->failed on error).
// Calling it every frame the image is visible keeps it cached and prioritised.
// Add "#still" to a URL to get only its first frame.
const Img* Get(const std::string& url, int maxDim = 1024);
void Put(const std::string& url, SDL_Texture* tex, int w, int h);  // for built-in/mock art
// drew: whether this frame was drawn (animation timing follows drawn frames).
void EndFrame(bool drew);
bool Busy();          // downloads or uploads in flight
bool AnimationDue();  // a visible animation needs its next frame
void SetAnimate(bool on);
// Decodes PNG/JPEG/GIF/WebP bytes (all frames of an animation unless maxFrames = 1).
bool Decode(const std::string& bytes, int maxDim, int maxFrames, DecodedImage& out);
SDL_Texture* MakeTexture(const std::string& rgba, int w, int h);

}  // namespace Images
