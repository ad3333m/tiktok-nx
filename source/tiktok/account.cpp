#include "tiktok/account.h"

#include <mutex>

#include "net/async.h"
#include "net/http.h"
#include "util/json.h"
#include "util/log.h"
#include "util/strings.h"

namespace Account {
namespace {

std::mutex g_mu;
std::string g_url, g_key;

std::string Build(const std::string& path) {
    if (g_url.empty()) return "";
    std::string u = g_url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u + path + (path.find('?') == std::string::npos ? "?" : "&") + "key=" + Str::UrlEncode(g_key);
}

Http::Response Call(const std::string& path, const std::string& method = "GET", const std::string& body = "") {
    std::string url;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        url = Build(path);
    }
    Http::Request req;
    req.url = url;
    req.method = method;
    req.body = body;
    req.timeoutMs = 15000;
    req.headers = {"Accept: application/json"};
    if (!body.empty()) req.headers.push_back("Content-Type: application/json");
    Http::Response r = Http::Perform(req);
    if (!r.Ok()) LOG("account %s %s: %ld %s", method.c_str(), path.c_str(), r.status, r.error.c_str());
    return r;
}

std::string ErrorOf(const Http::Response& r) {
    if (!r.error.empty()) return r.error;
    auto doc = J::Parse(r.body);
    std::string msg = J::S(J::Root(doc), "error");
    if (!msg.empty()) return msg;
    return Str::Format("HTTP %ld", r.status);
}

}  // namespace

void SetProxy(const std::string& url, const std::string& key) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_url = url;
    g_key = key;
}

bool IsConfigured() {
    std::lock_guard<std::mutex> lk(g_mu);
    return !g_url.empty();
}

std::string ProxyUrl() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_url;
}

std::string ProxyKey() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_key;
}

void Me(MeCb cb) {
    if (!IsConfigured()) {
        Async::Post([cb] { cb(false, {}, "no server set"); });
        return;
    }
    Async::Run(Async::Pool::Api, [cb] {
        Http::Response r = Call("/me");
        User u;
        std::string err;
        if (r.Ok()) {
            auto doc = J::Parse(r.body);
            yyjson_val* root = J::Root(doc);
            yyjson_val* user = J::Get(root, "user");
            u.username = J::S(user, "uniqueId");
            u.nickname = J::S(user, "nickname");
            u.avatar = J::S(user, "avatarMedium");
            if (u.avatar.empty()) u.avatar = J::S(user, "avatarThumb");
            yyjson_val* stats = J::Get(root, "stats");
            u.followers = J::I(stats, "followerCount");
            u.following = J::I(stats, "followingCount");
            u.hearts = J::I(stats, "heartCount");
            if (u.username.empty()) err = "server reply had no user";
        } else {
            err = ErrorOf(r);
        }
        bool ok = r.Ok() && err.empty();
        Async::Post([cb, ok, u = std::move(u), err]() mutable { cb(ok, std::move(u), err); });
    });
}

void LikeComment(const std::string& id, bool like, ActionCb cb) {
    (void)id; (void)like;
    Async::Post([cb] { cb(false, "not implemented yet"); });
}

void Repost(const std::string& videoId, bool repost, ActionCb cb) {
    (void)videoId; (void)repost;
    Async::Post([cb] { cb(false, "not implemented yet"); });
}

}  // namespace Account
