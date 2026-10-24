#pragma once

#include <functional>
#include <string>
#include <vector>

// Account-side calls go through a tiny HTTP proxy the user runs on their PC
// (see docs/proxy.md). When the proxy is unset, these calls short-circuit.
namespace Account {

struct User {
    std::string username, nickname, avatar;
    int64_t followers = 0, following = 0, hearts = 0;
};

void SetProxy(const std::string& url, const std::string& key);
bool IsConfigured();
std::string ProxyUrl();
std::string ProxyKey();

using MeCb = std::function<void(bool ok, User u, const std::string& err)>;
using ActionCb = std::function<void(bool ok, const std::string& err)>;

void Me(MeCb cb);
void LikeComment(const std::string& id, bool like, ActionCb cb);
void Repost(const std::string& videoId, bool repost, ActionCb cb);

}  // namespace Account
