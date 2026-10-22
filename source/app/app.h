#pragma once

#include <set>
#include <string>
#include <vector>

#include "platform/platform.h"
#include "tiktok/api.h"
#include "ui/gfx.h"

// The whole app is one For You screen: the video in a 9:16 card, TikTok's action
// rail beside it, and either "Up next" or the comments on the right.
struct App {
    bool quit = false;
    std::string region = "US";

    std::vector<Api::Video> feed;
    std::set<std::string> seen, liked, saved;
    int cur = -1;
    bool feedBusy = false;
    std::string feedError;
    double feedRetryAt = 0;

    // Card slide when switching videos (+1 = coming up from below).
    float slideFrom = 0;
    double slideAt = -10;

    // Photo posts.
    int photo = 0;
    double photoAt = 0;

    bool commentsOpen = false;
    std::string commentsFor;
    std::vector<Api::Comment> comments;
    int64_t commentsCursor = 0;
    bool commentsMore = false, commentsBusy = false;
    std::string commentsError;
    float commentsScroll = 0, commentsMax = 0;

    double heartAt = -10;
    float heartX = 0, heartY = 0;
    double lastTap = -10;
    bool swiping = false;
    float swipeY = 0;
    double toastUntil = 0;
    std::string toast;
    double lastDraw = 0;

    void Init();
    void Shutdown();
    bool Frame(const Platform::Input& in, float dt);  // true when it drew

    void LoadMore();
    void Go(int index);
    void Like();
    void ToggleComments();
    void LoadComments();
    void ChangeRegion();
    void Toast(const std::string& text);

    void Draw();
    void DrawSidebar();
    void DrawCard(const Rect& card);
    void DrawRail(const Rect& card);
    void DrawUpNext(const Rect& area);
    void DrawComments(const Rect& area);
    const Api::Video* Current() const { return cur >= 0 && cur < (int)feed.size() ? &feed[cur] : nullptr; }
};
