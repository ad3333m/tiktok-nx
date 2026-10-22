#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// TikTok has no public API, so feeds and comments come from tikwm.com, which
// proxies TikTok's own and hands back direct, watermark-free MP4 links.
// Free use is limited to one request a second; calls here are spaced and retried.
namespace Api {

struct Video {
    std::string id, caption, cover, play;  // play: MP4, or the music for photo posts
    std::string author, nickname, avatar;
    std::string music;                     // "Song - Artist"
    std::vector<std::string> images;       // photo (slideshow) posts
    int64_t plays = 0, likes = 0, comments = 0, shares = 0, saves = 0;
    int duration = 0;
    std::string WebUrl() const { return "https://www.tiktok.com/@" + author + "/video/" + id; }
};

struct Comment {
    std::string text, nickname, avatar;
    int64_t likes = 0, time = 0, replies = 0;
};

using FeedCb = std::function<void(bool ok, std::vector<Video>, const std::string& err)>;
using CommentsCb = std::function<void(bool ok, std::vector<Comment>, int64_t cursor, bool more, const std::string& err)>;

// Callbacks run on the main thread.
void Feed(const std::string& region, FeedCb cb);
void Comments(const Video& v, int64_t cursor, CommentsCb cb);

// Parsers, exposed for the self-check.
bool ParseFeed(const std::string& json, std::vector<Video>& out, std::string& err);
bool ParseComments(const std::string& json, std::vector<Comment>& out, int64_t& cursor, bool& more, std::string& err);

}  // namespace Api
