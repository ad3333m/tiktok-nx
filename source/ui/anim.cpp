#include "ui/anim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <webp/demux.h>

namespace Anim {
namespace {

constexpr int kMaxKeptFrames = 150;

int FixDelay(int ms) { return ms <= 10 ? 100 : ms; }  // what browsers do with 0/1 cs delays

// ---- GIF ------------------------------------------------------------------------------------

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool Has(size_t n) const { return (size_t)(end - p) >= n; }
    uint8_t U8() { return Has(1) ? *p++ : 0; }
    uint16_t U16() {
        if (!Has(2)) {
            p = end;
            return 0;
        }
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
        return v;
    }
    void SkipBlocks() {
        while (Has(1)) {
            uint8_t n = *p++;
            if (n == 0) return;
            if (!Has(n)) {
                p = end;
                return;
            }
            p += n;
        }
    }
    void ReadBlocks(std::vector<uint8_t>& out) {
        while (Has(1)) {
            uint8_t n = *p++;
            if (n == 0) return;
            if (!Has(n)) {
                out.insert(out.end(), p, end);
                p = end;
                return;
            }
            out.insert(out.end(), p, p + n);
            p += n;
        }
    }
};

// LZW-decodes into `out` (w*h palette indices). Returns false on corrupt data.
bool Lzw(const std::vector<uint8_t>& in, int minCode, std::vector<uint8_t>& out) {
    if (minCode < 2 || minCode > 8) return false;
    const int clear = 1 << minCode, eoi = clear + 1;
    std::vector<uint16_t> prefix(4096);
    std::vector<uint8_t> suffix(4096), stack(4097);
    for (int i = 0; i < clear; i++) suffix[i] = (uint8_t)i;
    int codeSize = minCode + 1, next = eoi + 1, prev = -1;
    uint8_t first = 0;
    size_t outPos = 0, bitPos = 0;
    const size_t totalBits = in.size() * 8;
    while (outPos < out.size() && bitPos + codeSize <= totalBits) {
        int code = 0;
        for (int i = 0; i < codeSize; i++, bitPos++)
            code |= ((in[bitPos >> 3] >> (bitPos & 7)) & 1) << i;
        if (code == clear) {
            codeSize = minCode + 1;
            next = eoi + 1;
            prev = -1;
            continue;
        }
        if (code == eoi) break;
        if (prev < 0) {
            if (code >= clear) return false;
            out[outPos++] = (uint8_t)code;
            prev = code;
            first = (uint8_t)code;
            continue;
        }
        int in_code = code, sp = 0;
        if (code >= next) {
            if (code > next) return false;
            stack[sp++] = first;
            code = prev;
        }
        while (code > eoi) {
            if (sp >= 4096) return false;
            stack[sp++] = suffix[code];
            code = prefix[code];
        }
        first = (uint8_t)code;
        stack[sp++] = first;
        while (sp > 0 && outPos < out.size()) out[outPos++] = stack[--sp];
        if (next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = first;
            next++;
            if (next == (1 << codeSize) && codeSize < 12) codeSize++;
        }
        prev = in_code;
    }
    return true;
}

int CountGifFrames(const uint8_t* data, size_t size) {
    Reader r{data, data + size};
    if (!r.Has(13)) return 0;
    r.p += 10;
    uint8_t flags = r.U8();
    r.p += 2;
    if (flags & 0x80) r.p += 3 * (1 << ((flags & 7) + 1));
    int frames = 0;
    while (r.Has(1)) {
        uint8_t b = r.U8();
        if (b == 0x21) {
            r.U8();
            r.SkipBlocks();
        } else if (b == 0x2C) {
            if (!r.Has(9)) break;
            r.p += 8;
            uint8_t f = r.U8();
            if (f & 0x80) r.p += 3 * (1 << ((f & 7) + 1));
            r.U8();
            r.SkipBlocks();
            frames++;
        } else {
            break;
        }
    }
    return frames;
}

}  // namespace

void FitSize(int w, int h, int frames, int maxDim, size_t budget, int& nw, int& nh) {
    float s = std::min(1.0f, (float)maxDim / std::max(w, h));
    const size_t perFrame = (size_t)(w * s) * (size_t)(h * s) * 4;
    const size_t total = perFrame * (size_t)std::max(1, frames);
    if (total > budget && total > 0) s *= std::sqrt((float)budget / (float)total);
    nw = std::max(1, (int)(w * s));
    nh = std::max(1, (int)(h * s));
}

std::string Scale(const uint8_t* src, int w, int h, int nw, int nh) {
    if (nw == w && nh == h) return std::string(reinterpret_cast<const char*>(src), (size_t)w * h * 4);
    std::string out((size_t)nw * nh * 4, '\0');
    uint8_t* dst = reinterpret_cast<uint8_t*>(out.data());
    for (int y = 0; y < nh; y++) {
        const int sy0 = (int)((int64_t)y * h / nh), sy1 = std::max(sy0 + 1, (int)((int64_t)(y + 1) * h / nh));
        for (int x = 0; x < nw; x++) {
            const int sx0 = (int)((int64_t)x * w / nw), sx1 = std::max(sx0 + 1, (int)((int64_t)(x + 1) * w / nw));
            uint32_t acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const uint8_t* px = src + ((size_t)yy * w + sx0) * 4;
                for (int xx = sx0; xx < sx1; xx++, px += 4) {
                    acc[0] += px[0] * px[3];
                    acc[1] += px[1] * px[3];
                    acc[2] += px[2] * px[3];
                    acc[3] += px[3];
                    n++;
                }
            }
            uint8_t* o = dst + ((size_t)y * nw + x) * 4;
            if (acc[3]) {
                o[0] = (uint8_t)(acc[0] / acc[3]);
                o[1] = (uint8_t)(acc[1] / acc[3]);
                o[2] = (uint8_t)(acc[2] / acc[3]);
            }
            o[3] = (uint8_t)(acc[3] / n);
        }
    }
    return out;
}

bool DecodeGif(const uint8_t* data, size_t size, int maxDim, size_t budget, int maxFrames, DecodedImage& out) {
    Reader r{data, data + size};
    if (!r.Has(13) || memcmp(data, "GIF8", 4) != 0) return false;
    r.p += 6;
    const int W = r.U16(), H = r.U16();
    const uint8_t flags = r.U8();
    r.U8();  // background colour: browsers clear to transparent instead
    r.U8();
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) return false;
    uint8_t globalPal[256 * 3] = {0};
    int globalCount = 0;
    if (flags & 0x80) {
        globalCount = 1 << ((flags & 7) + 1);
        if (!r.Has(globalCount * 3)) return false;
        memcpy(globalPal, r.p, globalCount * 3);
        r.p += globalCount * 3;
    }

    int total = maxFrames == 1 ? 1 : CountGifFrames(data, size);
    if (total <= 0) return false;
    if (maxFrames > 0) total = std::min(total, maxFrames);
    const int step = (total + kMaxKeptFrames - 1) / kMaxKeptFrames;
    const int kept = (total + step - 1) / step;
    int nw, nh;
    FitSize(W, H, kept, maxDim, budget, nw, nh);
    out = DecodedImage{};
    out.w = nw;
    out.h = nh;

    std::vector<uint8_t> canvas((size_t)W * H * 4, 0), saved;
    std::vector<uint8_t> lzw, indices;
    int disposal = 0, transparent = -1, delay = 100;
    int prevDisposal = 0, px = 0, py = 0, pw = 0, ph = 0;
    int frameNo = 0, pendingDelay = 0;

    while (r.Has(1) && frameNo < total) {
        uint8_t b = r.U8();
        if (b == 0x3B) break;
        if (b == 0x21) {
            uint8_t label = r.U8();
            if (label == 0xF9 && r.Has(6)) {
                r.U8();  // block size (4)
                uint8_t f = r.U8();
                delay = FixDelay(r.U16() * 10);
                uint8_t ti = r.U8();
                disposal = (f >> 2) & 7;
                transparent = (f & 1) ? ti : -1;
                r.SkipBlocks();
            } else {
                r.SkipBlocks();
            }
            continue;
        }
        if (b != 0x2C) break;
        if (!r.Has(9)) break;
        const int fx = r.U16(), fy = r.U16(), fw = r.U16(), fh = r.U16();
        const uint8_t f = r.U8();
        const uint8_t* pal = globalPal;
        int palCount = globalCount;
        uint8_t localPal[256 * 3];
        if (f & 0x80) {
            palCount = 1 << ((f & 7) + 1);
            if (!r.Has(palCount * 3)) break;
            memcpy(localPal, r.p, palCount * 3);
            r.p += palCount * 3;
            pal = localPal;
        }
        const bool interlaced = (f & 0x40) != 0;
        const int minCode = r.U8();
        lzw.clear();
        r.ReadBlocks(lzw);
        if (fw <= 0 || fh <= 0) continue;

        // Undo the previous frame as its disposal asks.
        if (prevDisposal == 2) {
            for (int y = py; y < py + ph && y < H; y++)
                if (y >= 0) memset(&canvas[((size_t)y * W + std::max(px, 0)) * 4], 0, (size_t)std::max(0, std::min(pw, W - px)) * 4);
        } else if (prevDisposal == 3 && !saved.empty()) {
            canvas = saved;
        }
        if (disposal == 3) saved = canvas;

        indices.assign((size_t)fw * fh, 0);
        if (!Lzw(lzw, minCode, indices) && frameNo == 0) return false;
        for (int row = 0; row < fh; row++) {
            int y = row;
            if (interlaced) {
                // Rows arrive in four passes: every 8th from 0, every 8th from 4, every 4th from 2, every 2nd from 1.
                const int p1 = (fh + 7) / 8, p2 = (fh + 3) / 8, p3 = (fh + 1) / 4;
                if (row < p1) y = row * 8;
                else if (row < p1 + p2) y = (row - p1) * 8 + 4;
                else if (row < p1 + p2 + p3) y = (row - p1 - p2) * 4 + 2;
                else y = (row - p1 - p2 - p3) * 2 + 1;
            }
            const int cy = fy + y;
            if (cy < 0 || cy >= H) continue;
            const uint8_t* src = &indices[(size_t)row * fw];
            for (int x = 0; x < fw; x++) {
                const int cx = fx + x;
                if (cx < 0 || cx >= W) continue;
                const int idx = src[x];
                if (idx == transparent || idx >= palCount) continue;
                uint8_t* dst = &canvas[((size_t)cy * W + cx) * 4];
                dst[0] = pal[idx * 3];
                dst[1] = pal[idx * 3 + 1];
                dst[2] = pal[idx * 3 + 2];
                dst[3] = 255;
            }
        }
        pendingDelay += delay;
        if (frameNo % step == 0 || frameNo == total - 1) {
            out.frames.push_back(Scale(canvas.data(), W, H, nw, nh));
            out.delaysMs.push_back(pendingDelay);
            pendingDelay = 0;
        }
        frameNo++;
        prevDisposal = disposal;
        px = fx;
        py = fy;
        pw = fw;
        ph = fh;
        disposal = 0;
        transparent = -1;
        delay = 100;
    }
    return !out.frames.empty();
}

bool DecodeWebP(const uint8_t* data, size_t size, int maxDim, size_t budget, int maxFrames, DecodedImage& out) {
    WebPData wd{data, size};
    WebPAnimDecoderOptions opts;
    if (!WebPAnimDecoderOptionsInit(&opts)) return false;
    opts.color_mode = MODE_RGBA;
    opts.use_threads = 0;
    WebPAnimDecoder* dec = WebPAnimDecoderNew(&wd, &opts);
    if (!dec) return false;
    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec, &info) || info.canvas_width == 0) {
        WebPAnimDecoderDelete(dec);
        return false;
    }
    int total = std::max(1, (int)info.frame_count);
    if (maxFrames > 0) total = std::min(total, maxFrames);
    const int step = (total + kMaxKeptFrames - 1) / kMaxKeptFrames;
    const int kept = (total + step - 1) / step;
    int nw, nh;
    FitSize((int)info.canvas_width, (int)info.canvas_height, kept, maxDim, budget, nw, nh);
    out = DecodedImage{};
    out.w = nw;
    out.h = nh;
    int frameNo = 0, lastTs = 0, pending = 0;
    while (frameNo < total && WebPAnimDecoderHasMoreFrames(dec)) {
        uint8_t* buf = nullptr;
        int ts = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) break;
        pending += FixDelay(ts - lastTs);
        lastTs = ts;
        if (frameNo % step == 0 || frameNo == total - 1) {
            out.frames.push_back(Scale(buf, (int)info.canvas_width, (int)info.canvas_height, nw, nh));
            out.delaysMs.push_back(pending);
            pending = 0;
        }
        frameNo++;
    }
    WebPAnimDecoderDelete(dec);
    return !out.frames.empty();
}

}  // namespace Anim
