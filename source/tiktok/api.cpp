#include "tiktok/api.h"

#include <chrono>
#include <mutex>
#include <thread>

#include "net/async.h"
#include "net/http.h"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

namespace Api {
namespace {

const char* kBase = "https://www.tikwm.com/api/";

// GET with the free tier's 1 request/second spacing, retried while tikwm says
// "try again" (code -1) or the connection drops.
Http::Response Call(const std::string& path) {
    static std::mutex mu;
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    Http::Response res;
    for (int attempt = 0; attempt < 4; attempt++) {
        {
            std::lock_guard<std::mutex> lk(mu);
            auto next = last + std::chrono::milliseconds(1100 + attempt * 900);
            std::this_thread::sleep_until(next);
            Http::Request req;
            req.url = kBase + path;
            req.headers = {"Accept: application/json"};
            res = Http::Perform(req);
            last = std::chrono::steady_clock::now();
        }
        if (res.Ok() && res.body.find("\"code\":0") != std::string::npos) return res;
        LOG("tikwm %s: status %ld %s %.120s", path.c_str(), res.status, res.error.c_str(), res.body.c_str());
    }
    return res;
}

std::string ErrorOf(const Http::Response& r) {
    if (!r.error.empty()) return r.error;
    if (r.body.find("Just a moment") != std::string::npos) return "tikwm.com is asking for a browser check";
    auto doc = J::Parse(r.body);
    std::string msg = J::S(J::Root(doc), "msg");
    return msg.empty() ? Str::Format("HTTP %ld", r.status) : msg;
}

}  // namespace

bool ParseFeed(const std::string& json, std::vector<Video>& out, std::string& err) {
    auto doc = J::Parse(json);
    yyjson_val* root = J::Root(doc);
    if (J::I(root, "code", -1) != 0) {
        err = J::S(root, "msg");
        return false;
    }
    yyjson_val* data = J::Get(root, "data");
    if (yyjson_is_obj(data)) data = J::Get(data, "videos");
    for (yyjson_val* v : J::Arr(data)) {
        if (J::Bool(v, "is_ad")) continue;
        Video o;
        o.id = J::S(v, "video_id");
        o.caption = J::S(v, "title");
        o.cover = J::S(v, "cover");
        o.play = J::S(v, "play");
        o.duration = (int)J::I(v, "duration");
        o.plays = J::I(v, "play_count");
        o.likes = J::I(v, "digg_count");
        o.comments = J::I(v, "comment_count");
        o.shares = J::I(v, "share_count");
        o.saves = J::I(v, "collect_count");
        yyjson_val* a = J::Get(v, "author");
        o.author = J::S(a, "unique_id");
        o.nickname = J::S(a, "nickname");
        o.avatar = J::S(a, "avatar");
        yyjson_val* m = J::Get(v, "music_info");
        o.music = J::S(m, "title");
        std::string artist = J::S(m, "author");
        if (!artist.empty() && o.music.find(artist) == std::string::npos) o.music += " - " + artist;
        for (yyjson_val* img : J::Arr(v, "images")) o.images.push_back(J::Str(img));
        if (!o.images.empty()) o.play = J::S(m, "play");  // a slideshow's "play" is its soundtrack
        if (!o.id.empty() && (!o.play.empty() || !o.images.empty())) out.push_back(std::move(o));
    }
    return true;
}

bool ParseComments(const std::string& json, std::vector<Comment>& out, int64_t& cursor, bool& more,
                   std::string& err) {
    auto doc = J::Parse(json);
    yyjson_val* root = J::Root(doc);
    if (J::I(root, "code", -1) != 0) {
        err = J::S(root, "msg");
        return false;
    }
    yyjson_val* data = J::Get(root, "data");
    for (yyjson_val* c : J::Arr(data, "comments")) {
        Comment o;
        o.text = J::S(c, "text");
        o.likes = J::I(c, "digg_count");
        o.time = J::I(c, "create_time");
        o.replies = J::I(c, "reply_total");
        yyjson_val* u = J::Get(c, "user");
        o.nickname = J::S(u, "nickname");
        if (o.nickname.empty()) o.nickname = J::S(u, "unique_id");
        o.avatar = J::S(u, "avatar");
        out.push_back(std::move(o));
    }
    cursor = J::I(data, "cursor");
    more = J::Bool(data, "hasMore");
    return true;
}

void Feed(const std::string& region, FeedCb cb) {
    Async::Run(Async::Pool::Api, [region, cb] {
        Http::Response r = Call("feed/list?count=10&region=" + Str::UrlEncode(region));
        std::vector<Video> vids;
        std::string err;
        bool ok = r.Ok() && ParseFeed(r.body, vids, err);
        if (!ok && err.empty()) err = ErrorOf(r);
        Async::Post([cb, ok, vids = std::move(vids), err]() mutable { cb(ok, std::move(vids), err); });
    });
}

void Comments(const Video& v, int64_t cursor, CommentsCb cb) {
    std::string url = v.WebUrl();
    Async::Run(Async::Pool::Api, [url, cursor, cb] {
        Http::Response r = Call(Str::Format("comment/list?count=20&cursor=%lld&url=", (long long)cursor) +
                                Str::UrlEncode(url));
        std::vector<Comment> list;
        int64_t next = 0;
        bool more = false;
        std::string err;
        bool ok = r.Ok() && ParseComments(r.body, list, next, more, err);
        if (!ok && err.empty()) err = ErrorOf(r);
        Async::Post([=, list = std::move(list)]() mutable { cb(ok, std::move(list), next, more, err); });
    });
}

}  // namespace Api
