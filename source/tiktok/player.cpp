#include "tiktok/player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "net/http.h"
#include "ui/gfx.h"
#include "util/log.h"
#include "util/strings.h"

namespace Player {
namespace {

constexpr int kRate = 48000;
constexpr int kBytesPerSec = kRate * 4;  // S16 stereo
constexpr size_t kWantFrames = 6;        // decode ahead this far...
constexpr size_t kMaxFrames = 12;        // ...and never further
constexpr double kWantAudio = 0.7;       // seconds queued

SDL_AudioDeviceID g_dev = 0;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_hwBroken{false};
std::atomic<double> g_seekTo{-1};
std::atomic<double> g_duration{0};

// Shared with the decode thread, under g_mu.
std::mutex g_mu;
std::deque<std::pair<double, AVFrame*>> g_frames;
std::string g_error;
bool g_open = false, g_hasAudio = false, g_hasVideo = false;
bool g_needFirst = true;  // show the next frame without waiting for the clock
double g_audioEnd = -1;   // pts just past the last queued sample, -1 = nothing queued
double g_shownPts = 0;
double g_wallStart = -1;  // clock origin for silent videos
double g_pausedAt = -1;

// Main thread only.
SDL_Texture* g_tex = nullptr;
int g_tw = 0, g_th = 0;
Uint32 g_tfmt = 0;
bool g_haveFrame = false;

double Now() { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

void ClearFrames() {
    for (auto& f : g_frames) av_frame_free(&f.second);
    g_frames.clear();
}

double QueuedAudio() { return g_dev ? SDL_GetQueuedAudioSize(g_dev) / (double)kBytesPerSec : 0; }

// Under g_mu. -1 until there's something to time against.
double Clock() {
    if (g_hasAudio) return g_audioEnd < 0 ? -1 : g_audioEnd - QueuedAudio();
    if (g_wallStart < 0) return -1;
    return (g_pausedAt >= 0 ? g_pausedAt : Now()) - g_wallStart;
}

// ---- curl-backed input -------------------------------------------------------

struct Stream {
    std::string url, err;
    int64_t size = -1, pos = 0, bufStart = 0;
    std::string buf;
};

constexpr int64_t kChunk = 512 * 1024;

bool Fill(Stream& s) {
    Http::Request req;
    req.url = s.url;
    req.cancel = &g_stop;
    req.timeoutMs = 30000;
    req.maxBytes = kChunk * 2;
    req.headers = {Str::Format("Range: bytes=%lld-%lld", (long long)s.pos, (long long)(s.pos + kChunk - 1)),
                   "User-Agent: Mozilla/5.0 (Nintendo Switch) TikTokNX/1.0"};
    for (int attempt = 0; attempt < 3 && !g_stop; attempt++) {
        Http::Response r = Http::Perform(req);
        if (r.status == 206 || (r.status == 200 && s.pos == 0)) {
            s.bufStart = r.status == 206 ? s.pos : 0;
            s.size = r.status == 206 ? (r.total ? r.total : -1) : (int64_t)r.body.size();
            s.buf = std::move(r.body);
            return true;
        }
        if (r.status == 416) {
            s.size = s.pos;
            return false;
        }
        s.err = r.error.empty() ? Str::Format("video server said HTTP %ld", r.status) : r.error;
    }
    return false;
}

int ReadPacket(void* opaque, uint8_t* out, int n) {
    Stream& s = *static_cast<Stream*>(opaque);
    if (s.size >= 0 && s.pos >= s.size) return AVERROR_EOF;
    if (s.pos < s.bufStart || s.pos >= s.bufStart + (int64_t)s.buf.size()) {
        if (!Fill(s)) return g_stop ? AVERROR_EXIT : (s.size >= 0 && s.pos >= s.size ? AVERROR_EOF : AVERROR(EIO));
        if (s.pos >= s.bufStart + (int64_t)s.buf.size()) return AVERROR_EOF;
    }
    int64_t off = s.pos - s.bufStart;
    int k = (int)std::min<int64_t>(n, (int64_t)s.buf.size() - off);
    memcpy(out, s.buf.data() + off, k);
    s.pos += k;
    return k;
}

int64_t SeekPacket(void* opaque, int64_t off, int whence) {
    Stream& s = *static_cast<Stream*>(opaque);
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return s.size;
    if (whence == SEEK_CUR) off += s.pos;
    else if (whence == SEEK_END) {
        if (s.size < 0) return -1;
        off += s.size;
    }
    if (off < 0) return -1;
    s.pos = off;
    return off;
}

int Interrupt(void*) { return g_stop.load() ? 1 : 0; }

// ---- decoding -----------------------------------------------------------------

AVPixelFormat PickHw(AVCodecContext* c, const AVPixelFormat* fmts) {
    AVPixelFormat hw = (AVPixelFormat)(intptr_t)c->opaque;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == hw) return hw;
    return fmts[0];
}

struct Session {
    Stream stream;
    AVIOContext* io = nullptr;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vc = nullptr;
    AVCodecContext* ac = nullptr;
    AVBufferRef* hwdev = nullptr;
    AVPixelFormat hwFmt = AV_PIX_FMT_NONE;
    SwrContext* swr = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int vi = -1, ai = -1;
    double skipUntil = -1;
    std::string audioBuf;

    ~Session() {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        sws_freeContext(sws);
        swr_free(&swr);
        avcodec_free_context(&vc);
        avcodec_free_context(&ac);
        av_buffer_unref(&hwdev);
        avformat_close_input(&fmt);
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
    }

    double Pts(const AVFrame* f, int idx) const {
        int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (ts == AV_NOPTS_VALUE) return 0;
        return ts * av_q2d(fmt->streams[idx]->time_base);
    }

    std::string OpenVideo(bool useHw) {
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
        if (!codec) return "no decoder for this video";
        vc = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(vc, fmt->streams[vi]->codecpar);
        AVHWDeviceType type = av_hwdevice_find_type_by_name("nvtegra");
        if (useHw && type != AV_HWDEVICE_TYPE_NONE) {
            for (int i = 0;; i++) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
                if (!cfg) break;
                if (cfg->device_type == type && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                    hwFmt = cfg->pix_fmt;
                    break;
                }
            }
            if (hwFmt != AV_PIX_FMT_NONE && av_hwdevice_ctx_create(&hwdev, type, nullptr, nullptr, 0) >= 0) {
                vc->hw_device_ctx = av_buffer_ref(hwdev);
                vc->opaque = (void*)(intptr_t)hwFmt;
                vc->get_format = PickHw;
            } else {
                hwFmt = AV_PIX_FMT_NONE;
            }
        }
        if (hwFmt == AV_PIX_FMT_NONE) vc->thread_count = 3;  // core 3 belongs to the system
        vc->flags2 |= AV_CODEC_FLAG2_FAST;
        if (avcodec_open2(vc, codec, nullptr) < 0) return "couldn't start the video decoder";
        LOG("video %dx%d %s, %s decode", vc->width, vc->height, codec->name, hwFmt != AV_PIX_FMT_NONE ? "nvtegra" : "cpu");
        return "";
    }

    std::string OpenAudio() {
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id);
        if (!codec) return "no decoder for this sound";
        ac = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ac, fmt->streams[ai]->codecpar);
        if (avcodec_open2(ac, codec, nullptr) < 0) return "couldn't start the audio decoder";
        AVChannelLayout stereo;
        av_channel_layout_default(&stereo, 2);
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kRate, &ac->ch_layout, ac->sample_fmt,
                                ac->sample_rate, 0, nullptr) < 0 ||
            swr_init(swr) < 0)
            return "couldn't convert this sound";
        return "";
    }

    // Returns false when the hardware path broke and we should start over on the CPU.
    bool TakeVideo(AVFrame* f) {
        double pts = Pts(f, vi);
        if (pts < skipUntil) return true;
        AVFrame* out = av_frame_alloc();
        if (f->format == hwFmt && hwFmt != AV_PIX_FMT_NONE) {
            if (av_hwframe_transfer_data(out, f, 0) < 0) {
                av_frame_free(&out);
                return false;
            }
            av_frame_copy_props(out, f);
        } else {
            av_frame_ref(out, f);
        }
        AVPixelFormat pf = (AVPixelFormat)out->format;
        if (pf != AV_PIX_FMT_YUV420P && pf != AV_PIX_FMT_YUVJ420P && pf != AV_PIX_FMT_NV12) {
            AVFrame* conv = av_frame_alloc();
            conv->format = AV_PIX_FMT_YUV420P;
            conv->width = out->width;
            conv->height = out->height;
            av_frame_get_buffer(conv, 0);
            sws = sws_getCachedContext(sws, out->width, out->height, pf, out->width, out->height, AV_PIX_FMT_YUV420P,
                                       SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            sws_scale(sws, out->data, out->linesize, 0, out->height, conv->data, conv->linesize);
            av_frame_free(&out);
            out = conv;
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_frames.emplace_back(pts, out);
        return true;
    }

    void TakeAudio(AVFrame* f) {
        double pts = Pts(f, ai);
        if (pts < skipUntil) return;
        int cap = swr_get_out_samples(swr, f->nb_samples);
        if (cap <= 0) return;
        audioBuf.resize((size_t)cap * 4);
        uint8_t* dst = (uint8_t*)audioBuf.data();
        int n = swr_convert(swr, &dst, cap, (const uint8_t**)f->extended_data, f->nb_samples);
        if (n <= 0) return;
        std::lock_guard<std::mutex> lk(g_mu);
        SDL_QueueAudio(g_dev, dst, (Uint32)n * 4);
        g_audioEnd = pts + n / (double)kRate;
    }

    // Sends one packet (or nullptr to drain) and takes every frame it gives back.
    bool Decode(AVCodecContext* c, const AVPacket* p) {
        if (avcodec_send_packet(c, p) < 0 && p) return c != vc || hwFmt == AV_PIX_FMT_NONE;
        while (avcodec_receive_frame(c, frame) >= 0) {
            if (c == vc) {
                if (!TakeVideo(frame)) return false;
            } else {
                TakeAudio(frame);
            }
            av_frame_unref(frame);
        }
        return true;
    }

    void SeekTo(double t) {
        int64_t ts = (int64_t)(t * AV_TIME_BASE);
        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        if (vc) avcodec_flush_buffers(vc);
        if (ac) avcodec_flush_buffers(ac);
        skipUntil = t > 0.05 ? t : -1;
        std::lock_guard<std::mutex> lk(g_mu);
        ClearFrames();
        if (g_dev) SDL_ClearQueuedAudio(g_dev);
        g_audioEnd = -1;
        g_needFirst = true;
        g_wallStart = -1;
    }

    // "" when stopped, "cpu" to retry without NVDEC, anything else is an error.
    std::string Run(const std::string& url, bool useHw) {
        stream.url = url;
        if (!Fill(stream)) return g_stop ? "" : (stream.err.empty() ? "couldn't load the video" : stream.err);
        uint8_t* iobuf = (uint8_t*)av_malloc(64 * 1024);
        io = avio_alloc_context(iobuf, 64 * 1024, 0, &stream, ReadPacket, nullptr, SeekPacket);
        fmt = avformat_alloc_context();
        fmt->pb = io;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        fmt->interrupt_callback.callback = Interrupt;
        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
            fmt = nullptr;  // freed by ffmpeg
            return g_stop ? "" : "this video's file is unreadable";
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0 && g_stop) return "";
        if (fmt->duration > 0) g_duration = fmt->duration / (double)AV_TIME_BASE;
        vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        // An attached picture (album art on the music of a photo post) isn't video.
        if (vi >= 0 && (fmt->streams[vi]->disposition & AV_DISPOSITION_ATTACHED_PIC)) vi = -1;
        if (vi < 0 && ai < 0) return "nothing playable in this post";
        std::string err;
        if (vi >= 0 && !(err = OpenVideo(useHw)).empty()) return err;
        if (ai >= 0 && !(err = OpenAudio()).empty()) {
            LOG("audio off: %s", err.c_str());
            ai = -1;
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_hasVideo = vi >= 0;
            g_hasAudio = ai >= 0;
        }

        bool eof = false;
        while (!g_stop) {
            double seek = g_seekTo.exchange(-1);
            if (seek >= 0) {
                SeekTo(seek);
                eof = false;
            }
            size_t frames;
            double queued;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                frames = g_frames.size();
                queued = QueuedAudio();
            }
            bool videoFull = vi < 0 || frames >= kWantFrames;
            bool audioFull = ai < 0 || queued >= kWantAudio;
            if (eof) {
                // Loop once everything has played out.
                if (frames == 0 && queued < 0.02 && !g_paused) {
                    SeekTo(0);
                    eof = false;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                continue;
            }
            if (frames >= kMaxFrames || (videoFull && audioFull)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            int rc = av_read_frame(fmt, pkt);
            if (rc < 0) {
                if (g_stop) break;
                if (rc != AVERROR_EOF && !stream.err.empty()) return stream.err;
                if (vc && !Decode(vc, nullptr)) return "cpu";
                if (ac) Decode(ac, nullptr);
                if (vc) avcodec_flush_buffers(vc);
                if (ac) avcodec_flush_buffers(ac);
                eof = true;
                continue;
            }
            bool ok = true;
            if (pkt->stream_index == vi) ok = Decode(vc, pkt);
            else if (pkt->stream_index == ai) Decode(ac, pkt);
            av_packet_unref(pkt);
            if (!ok) return "cpu";
        }
        return "";
    }
};

void Thread(std::string url) {
    std::string err;
    {
        Session s;
        err = s.Run(url, !g_hwBroken);
    }
    if (err == "cpu" && !g_stop) {
        LOG("nvtegra failed, decoding on the CPU from now on");
        g_hwBroken = true;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            ClearFrames();
            g_needFirst = true;
        }
        Session s;
        err = s.Run(url, false);
    }
    if (!err.empty() && err != "cpu" && !g_stop) {
        LOG("player: %s", err.c_str());
        std::lock_guard<std::mutex> lk(g_mu);
        g_error = err;
    }
    Http::ThreadCleanup();
}

}  // namespace

void Init() {
    SDL_AudioSpec want{}, have{};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) LOG("no audio: %s", SDL_GetError());
}

void Shutdown() {
    Close();
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = nullptr;
    if (g_dev) SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

void Open(const std::string& url) {
    Close();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_open = true;
        g_needFirst = true;
        g_hasAudio = g_hasVideo = false;
        g_error.clear();
    }
    g_haveFrame = false;
    g_duration = 0;
    g_paused = false;
    if (g_dev) SDL_PauseAudioDevice(g_dev, 0);
    g_thread = std::thread(Thread, url);
}

void Close() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    g_stop = false;
    g_seekTo = -1;
    std::lock_guard<std::mutex> lk(g_mu);
    ClearFrames();
    if (g_dev) SDL_ClearQueuedAudio(g_dev);
    g_open = false;
    g_audioEnd = -1;
    g_wallStart = -1;
    g_pausedAt = -1;
    g_shownPts = 0;
    g_haveFrame = false;
}

bool Update() {
    AVFrame* show = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const double clk = Clock();
        const bool starved = g_hasAudio && QueuedAudio() < 0.05 && !g_paused;
        while (!g_frames.empty()) {
            auto [pts, f] = g_frames.front();
            bool due = g_needFirst || (clk >= 0 && pts <= clk + 0.005);
            // A full queue with the sound starved would stall both; let the picture move on.
            if (!due && !(starved && g_frames.size() >= kMaxFrames)) break;
            if (show) av_frame_free(&show);
            show = f;
            g_shownPts = pts;
            g_frames.pop_front();
            if (g_needFirst) {
                g_needFirst = false;
                if (!g_hasAudio) {
                    g_wallStart = Now() - pts;
                    g_pausedAt = g_paused ? Now() : -1;
                }
                break;
            }
            if (!due) break;
        }
    }
    if (!show) return false;
    const bool nv12 = show->format == AV_PIX_FMT_NV12;
    const Uint32 fmt = nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
    if (!g_tex || g_tw != show->width || g_th != show->height || g_tfmt != fmt) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(Gfx::Renderer(), fmt, SDL_TEXTUREACCESS_STREAMING, show->width, show->height);
        g_tw = show->width;
        g_th = show->height;
        g_tfmt = fmt;
    }
    if (g_tex) {
        if (nv12)
            SDL_UpdateNVTexture(g_tex, nullptr, show->data[0], show->linesize[0], show->data[1], show->linesize[1]);
        else
            SDL_UpdateYUVTexture(g_tex, nullptr, show->data[0], show->linesize[0], show->data[1], show->linesize[1],
                                 show->data[2], show->linesize[2]);
        g_haveFrame = true;
    }
    av_frame_free(&show);
    return true;
}

SDL_Texture* Texture(int* w, int* h) {
    if (!g_haveFrame) return nullptr;
    if (w) *w = g_tw;
    if (h) *h = g_th;
    return g_tex;
}

bool Loading() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_open || !g_error.empty()) return false;
    return g_hasVideo ? !g_haveFrame : g_audioEnd < 0;
}

std::string Error() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_error;
}

double Position() {
    std::lock_guard<std::mutex> lk(g_mu);
    double c = Clock();
    return c >= 0 ? c : g_shownPts;
}

double Duration() { return g_duration; }

void SetPaused(bool paused) {
    if (paused == g_paused) return;
    g_paused = paused;
    if (g_dev) SDL_PauseAudioDevice(g_dev, paused ? 1 : 0);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_hasAudio || g_wallStart < 0) {
        g_pausedAt = -1;
        return;
    }
    if (paused) {
        g_pausedAt = Now();
    } else if (g_pausedAt >= 0) {
        g_wallStart += Now() - g_pausedAt;
        g_pausedAt = -1;
    }
}

bool Paused() { return g_paused; }

void Seek(double seconds) {
    double d = g_duration;
    if (d > 0) seconds = std::min(seconds, d - 0.5);
    g_seekTo = std::max(0.0, seconds);
}

}  // namespace Player
