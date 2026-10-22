#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Decoded image: one frame for stills, several for animated GIF/WebP.
struct DecodedImage {
    int w = 0, h = 0;
    std::vector<std::string> frames;  // RGBA, w*h*4 each
    std::vector<int> delaysMs;
};

namespace Anim {

// Frames are scaled to fit maxDim and, together, to stay under byteBudget.
// maxFrames = 1 decodes only the first frame (a still).
bool DecodeGif(const uint8_t* data, size_t size, int maxDim, size_t byteBudget, int maxFrames, DecodedImage& out);
bool DecodeWebP(const uint8_t* data, size_t size, int maxDim, size_t byteBudget, int maxFrames, DecodedImage& out);

// Area-average downscale of an RGBA buffer (premultiplied to avoid dark fringes).
std::string Scale(const uint8_t* rgba, int w, int h, int nw, int nh);
// Picks the frame size for an animation of `frames` frames.
void FitSize(int w, int h, int frames, int maxDim, size_t budget, int& nw, int& nh);

}  // namespace Anim
