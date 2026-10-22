// TikTok for Switch: the For You feed with real videos and comments.
#include "app/app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "net/async.h"
#include "tiktok/player.h"
#include "ui/icons.h"
#include "ui/images.h"
#include "ui/input.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "util/log.h"
#include "util/strings.h"

using namespace Platform;

namespace {

const Rect kCard{444, 12, 392, 696};
const Rect kRight{940, 12, 324, 696};
const Color kWhite(255, 255, 255);

// Per-comment wrapped lines, rebuilt when the list changes.
std::vector<std::vector<std::string>> g_commentLines;

std::string RegionPath() { return Platform::DataDir() + "region.txt"; }

std::string Count(int64_t n) {
    if (n < 1000) return std::to_string(n);
    const char* unit = "K";
    double v = n / 1000.0;
    if (n >= 1000000000) unit = "B", v = n / 1e9;
    else if (n >= 1000000) unit = "M", v = n / 1e6;
    std::string s = Str::Format(v < 100 ? "%.1f" : "%.0f", v);
    if (Str::EndsWith(s, ".0")) s.resize(s.size() - 2);
    return s + unit;
}

std::string RelTime(int64_t sec) {
    if (sec <= 0) return "";
    int64_t d = Platform::NowMs() / 1000 - sec;
    if (d < 60) return "just now";
    if (d < 3600) return Str::Format("%lldm ago", (long long)(d / 60));
    if (d < 86400) return Str::Format("%lldh ago", (long long)(d / 3600));
    if (d < 86400 * 7) return Str::Format("%lldd ago", (long long)(d / 86400));
    if (d < 86400 * 365) return Str::Format("%lldw ago", (long long)(d / 604800));
    return Str::Format("%lldy ago", (long long)(d / 31536000));
}

std::vector<std::string> Wrap(const std::string& text, Face f, int size, float maxW, size_t maxLines) {
    std::vector<std::string> lines;
    bool cut = false;
    for (const std::string& para : Str::Split(text, '\n')) {
        std::string line;
        for (const std::string& w : Str::Split(para, ' ')) {
            if (w.empty()) continue;
            std::string cand = line.empty() ? w : line + " " + w;
            if (line.empty() || Text::Width(cand, f, size) <= maxW) {
                line = cand;
                continue;
            }
            lines.push_back(line);
            line = w;
        }
        if (!line.empty()) lines.push_back(line);
        if (lines.size() >= maxLines) {
            cut = lines.size() > maxLines;
            break;
        }
    }
    if (lines.size() > maxLines) lines.resize(maxLines);
    if (cut && !lines.empty()) lines.back() += " \xE2\x80\xA6";
    return lines;
}

// Fits w x h inside r, centred.
Rect Contain(const Rect& r, float w, float h) {
    if (w <= 0 || h <= 0) return r;
    float s = std::min(r.w / w, r.h / h);
    return Rect{r.x + (r.w - w * s) / 2, r.y + (r.h - h * s) / 2, w * s, h * s};
}

float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

}  // namespace

void App::Init() {
    if (FILE* f = fopen(RegionPath().c_str(), "rb")) {
        char buf[8] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        std::string r = Str::Trim(std::string(buf, n));
        if (r.size() == 2) region = r;
    }
    LoadMore();
}

void App::Shutdown() { Player::Close(); }

void App::Toast(const std::string& text) {
    toast = text;
    toastUntil = Ui::Time() + 3;
}

void App::LoadMore() {
    if (feedBusy) return;
    feedBusy = true;
    feedError.clear();
    Api::Feed(region, [this](bool ok, std::vector<Api::Video> vids, const std::string& err) {
        feedBusy = false;
        if (!ok) {
            feedError = err;
            feedRetryAt = Ui::Time() + 6;
            return;
        }
        size_t before = feed.size();
        for (auto& v : vids)
            if (seen.insert(v.id).second) feed.push_back(std::move(v));
        if (feed.size() == before) feedRetryAt = Ui::Time() + 2;
        if (cur < 0 && !feed.empty()) Go(0);
    });
}

void App::Go(int index) {
    if (index < 0 || feed.empty() || index == cur) return;
    if (index >= (int)feed.size()) {
        LoadMore();
        return;
    }
    slideFrom = index > cur ? 1.f : -1.f;
    slideAt = Ui::Time();
    cur = index;
    photo = 0;
    photoAt = Ui::Time();
    Player::Open(feed[cur].play);
    if (commentsOpen) {
        commentsOpen = false;
        ToggleComments();
    }
    if (cur >= (int)feed.size() - 3) LoadMore();
}

void App::Like() {
    const Api::Video* v = Current();
    if (!v) return;
    if (liked.count(v->id)) {
        liked.erase(v->id);
    } else {
        liked.insert(v->id);
        heartAt = Ui::Time();
    }
}

void App::ToggleComments() {
    commentsOpen = !commentsOpen;
    const Api::Video* v = Current();
    if (!commentsOpen || !v || commentsFor == v->id) return;
    commentsFor = v->id;
    comments.clear();
    g_commentLines.clear();
    commentsCursor = 0;
    commentsMore = false;
    commentsError.clear();
    commentsScroll = 0;
    commentsBusy = false;
    LoadComments();
}

void App::LoadComments() {
    const Api::Video* v = Current();
    if (commentsBusy || !v) return;
    commentsBusy = true;
    std::string id = v->id;
    Api::Comments(*v, commentsCursor, [this, id](bool ok, std::vector<Api::Comment> list, int64_t next, bool more,
                                                 const std::string& err) {
        if (id != commentsFor) return;
        commentsBusy = false;
        if (!ok) {
            commentsError = err;
            return;
        }
        for (auto& c : list) comments.push_back(std::move(c));
        commentsCursor = next;
        commentsMore = more && !list.empty();
    });
}

void App::ChangeRegion() {
    std::string out;
    if (!Platform::Keyboard("Country code for the For You feed (US, GB, DE, FR, JP...)", region, out, 2)) return;
    for (char& c : out) c = (char)toupper((unsigned char)c);
    if (out.size() != 2 || !isalpha((unsigned char)out[0]) || !isalpha((unsigned char)out[1])) {
        Toast("Use a two-letter country code, like US or GB");
        return;
    }
    if (out == region) return;
    region = out;
    if (FILE* f = fopen(RegionPath().c_str(), "wb")) {
        fwrite(region.data(), 1, region.size(), f);
        fclose(f);
    }
    Player::Close();
    feed.clear();
    seen.clear();
    cur = -1;
    commentsOpen = false;
    commentsFor.clear();
    LoadMore();
    Toast("For You is now from " + region);
}

bool App::Frame(const Platform::Input& in, float dt) {
    Ui::BeginFrame(in, dt);
    int pumped = Async::Pump();
    bool newFrame = Player::Update();
    const double now = Ui::Time();
    const Api::Video* v = Current();

    if (Ui::Pressed(BTN_PLUS)) quit = true;
    if (Ui::Pressed(BTN_A)) Player::SetPaused(!Player::Paused());
    if (Ui::Pressed(BTN_X)) {
        heartX = kCard.x + kCard.w / 2;
        heartY = kCard.y + kCard.h / 2;
        Like();
    }
    if (Ui::Pressed(BTN_Y) || (commentsOpen && Ui::Pressed(BTN_B))) ToggleComments();
    if (Ui::Pressed(BTN_MINUS)) {
        ChangeRegion();
        v = Current();
    }
    int seek = (Ui::Pressed(BTN_R) || Ui::Repeat(BTN_RIGHT)) - (Ui::Pressed(BTN_L) || Ui::Repeat(BTN_LEFT));
    if (seek && v) {
        if (v->images.size() > 1) {
            photo = (photo + seek + (int)v->images.size()) % (int)v->images.size();
            photoAt = now;
        } else {
            Player::Seek(Player::Position() + seek * 5.0);
        }
    }
    if (commentsOpen) {
        commentsScroll += Ui::NavY() * 110.0f + Ui::StickScrollDelta();
        commentsScroll = std::clamp(commentsScroll, 0.f, commentsMax);
        if (commentsMore && !commentsBusy && commentsScroll > commentsMax - 500) LoadComments();
        if (int p = Ui::PageY()) Go(cur + p);
    } else {
        int step = Ui::NavY() + Ui::PageY();
        if (step) Go(cur + (step > 0 ? 1 : -1));
    }

    // Swipe the card like a phone: up for the next video, down for the last one.
    static bool wasTouching = false;
    static float startY = 0, lastX = 0, lastY = 0;
    if (in.touching) {
        lastX = in.tx;
        lastY = in.ty;
        if (!wasTouching && kCard.Contains(in.tx, in.ty)) {
            swiping = true;
            startY = in.ty;
        }
        if (swiping) swipeY = in.ty - startY;
    } else if (swiping) {
        swiping = false;
        if (swipeY < -90) Go(cur + 1);
        else if (swipeY > 90) Go(cur - 1);
        swipeY = 0;
    }
    wasTouching = in.touching;
    Ui::Hot(kCard, [this] {
        double t = Ui::Time();
        Player::SetPaused(!Player::Paused());
        if (t - lastTap < 0.32) {  // double tap: undo the first tap's pause and like
            heartX = lastX;
            heartY = lastY;
            if (const Api::Video* cv = Current(); cv && !liked.count(cv->id)) Like();
            else heartAt = t;
            lastTap = -10;
        } else {
            lastTap = t;
        }
    });

    if (v && v->images.size() > 1 && !Player::Paused() && now - photoAt > 3.5) {
        photo = (photo + 1) % (int)v->images.size();
        photoAt = now;
    }
    if (!feedBusy && now >= feedRetryAt && (feed.empty() || cur >= (int)feed.size() - 3)) LoadMore();

    const bool animating = now - slideAt < 0.4 || now - heartAt < 1.0 || swiping || toastUntil > now;
    bool want = newFrame || pumped > 0 || Ui::Active() || Ui::Animating() || animating || Images::Busy() ||
                Player::Loading() || now - lastDraw > (v && !Player::Paused() ? 0.1 : 0.5);
    if (want) {
        Draw();
        lastDraw = now;
    }
    Ui::EndFrame(want);
    Images::EndFrame(want);
    return want;
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void App::Draw() {
    Gfx::Clear(T().bg);
    DrawSidebar();
    Rect card = kCard;
    const double now = Ui::Time();
    float t = (float)((now - slideAt) / 0.3);
    if (t < 1) card.y += slideFrom * (1 - EaseOut(t)) * 300;
    if (swiping) card.y += swipeY * 0.35f;
    DrawCard(card);
    DrawRail(card);
    if (commentsOpen) DrawComments(kRight);
    else DrawUpNext(kRight);

    if (toastUntil > now) {
        float w = Text::Width(toast, Face::SemiBold, 16) + 40;
        Rect r{(Gfx::W - w) / 2, 640, w, 44};
        Gfx::FillRound(r, 22, Color::Hex(0x2F2F2F, 240));
        Text::DrawCentered(toast, r.x + r.w / 2, r.y + 11, Face::SemiBold, 16, kWhite);
    }
}

void App::DrawSidebar() {
    const Theme& th = T();
    Icon::Logo(36, 30, 42);

    Rect item{24, 110, 380, 52};
    Gfx::FillRound(item, 10, th.surface2);
    Icon::Note(item.x + 30, item.y + 26, 24, th.accent);
    Text::Draw("For You", item.x + 56, item.y + 13, Face::Bold, 20, th.accent);

    Rect reg{24, 170, 380, 52};
    Ui::Hot(reg, [this] { ChangeRegion(); });
    Text::Draw("Region", reg.x + 56, reg.y + 13, Face::Medium, 18, th.text);
    Text::DrawRight(region, reg.Right() - 54, reg.y + 13, Face::Bold, 18, th.textStrong);
    Icon::Button(reg.Right() - 40, reg.y + 26, "-", th.bg, th.textMuted);
    Gfx::StrokeCircle(reg.x + 30, reg.y + 26, 10, 2, th.text);
    Gfx::Line(reg.x + 20, reg.y + 26, reg.x + 40, reg.y + 26, 2, th.text);

    const std::vector<std::pair<std::string, std::string>> keys = {
        {"\xE2\x86\x95", "Next / previous"}, {"A", "Pause"},     {"X", "Like"},     {"Y", "Comments"},
        {"L", "Back 5s"},                     {"R", "Forward 5s"}, {"-", "Region"}, {"+", "Exit"},
    };
    float y = 720 - 30 - keys.size() * 34.0f;
    for (const auto& [key, label] : keys) {
        Icon::Button(36, y + 14, key, th.bg, th.textMuted);
        Text::Draw(label, 80, y + 3, Face::Medium, 16, th.textMuted);
        y += 34;
    }
    if (!feed.empty()) Text::Draw(Str::Format("%d / %d", cur + 1, (int)feed.size()), 36, 250, Face::Medium, 15, th.textFaint);
}

void App::DrawCard(const Rect& card) {
    const Theme& th = T();
    const double now = Ui::Time();
    Gfx::FillRound(card, 14, th.surface);
    Gfx::PushClip(card);
    const Api::Video* v = Current();
    if (!v) {
        if (!feedError.empty() && !feedBusy) {
            Text::DrawCentered("Couldn't load For You", card.x + card.w / 2, card.y + 300, Face::Bold, 20, kWhite);
            auto lines = Wrap(feedError, Face::Regular, 15, card.w - 60, 4);
            for (size_t i = 0; i < lines.size(); i++)
                Text::DrawCentered(lines[i], card.x + card.w / 2, card.y + 336 + i * 22, Face::Regular, 15,
                                   th.textMuted);
        } else {
            Icon::Spinner(card.x + card.w / 2, card.y + card.h / 2, 18, th.accent, now);
        }
        Gfx::PopClip();
        return;
    }

    int tw = 0, thh = 0;
    if (!v->images.empty()) {
        const Images::Img* img = Images::Get(v->images[photo % v->images.size()], 1280);
        if (img && img->tex) Gfx::Image(img->tex, Contain(card, (float)img->w, (float)img->h));
        else Icon::Spinner(card.x + card.w / 2, card.y + card.h / 2, 18, th.accent, now);
    } else if (SDL_Texture* tex = Player::Texture(&tw, &thh)) {
        Gfx::Image(tex, Contain(card, (float)tw, (float)thh));
    } else if (const Images::Img* cov = Images::Get(v->cover, 720); cov && cov->tex) {
        Gfx::Image(cov->tex, Contain(card, (float)cov->w, (float)cov->h));
    }
    if (Player::Loading() && v->images.empty())
        Icon::Spinner(card.x + card.w / 2, card.y + card.h / 2, 18, kWhite, now);
    std::string err = Player::Error();
    if (!err.empty()) {
        Gfx::FillRound(Rect{card.x + 30, card.y + 300, card.w - 60, 90}, 12, Color::Hex(0, 170));
        Text::DrawCentered("Couldn't play this one", card.x + card.w / 2, card.y + 314, Face::Bold, 17, kWhite);
        Text::DrawCentered(err, card.x + card.w / 2, card.y + 342, Face::Regular, 14, th.textMuted, card.w - 80);
    }
    if (Player::Paused()) Icon::Play(card.x + card.w / 2, card.y + card.h / 2, 70, kWhite.Alpha(0.6f));

    // Caption over a fade at the bottom, like the app.
    Gfx::VGradient(Rect{card.x, card.Bottom() - 240, card.w, 240}, Color::Hex(0, 0), Color::Hex(0, 190));
    float y = card.Bottom() - 44;
    Icon::Note(card.x + 24, y + 11, 16, kWhite);
    Text::Draw(v->music.empty() ? "original sound" : v->music, card.x + 40, y, Face::Medium, 15, kWhite, card.w - 60);
    auto lines = Wrap(v->caption, Face::Regular, 16, card.w - 36, 3);
    y -= 6 + lines.size() * 22.0f;
    for (size_t i = 0; i < lines.size(); i++)
        Text::Draw(lines[i], card.x + 18, y + i * 22, Face::Regular, 16, kWhite, card.w - 36);
    y -= 30;
    float nw = Text::Draw(v->author, card.x + 18, y, Face::Bold, 18, kWhite, card.w - 120);
    if (!v->nickname.empty() && v->nickname != v->author)
        Text::Draw(v->nickname, card.x + 26 + nw, y + 2, Face::Regular, 15, th.textMuted, card.w - nw - 50);

    // Photo dots or the playback bar.
    if (v->images.size() > 1) {
        float dx = card.x + card.w / 2 - (v->images.size() - 1) * 7.0f;
        for (size_t i = 0; i < v->images.size() && i < 20; i++)
            Gfx::FillCircle(dx + i * 14, card.y + 22, 3.5f, kWhite.Alpha(i == (size_t)photo ? 1.f : 0.4f));
    }
    double dur = Player::Duration();
    if (dur > 0 && v->images.empty()) {
        float f = (float)std::clamp(Player::Position() / dur, 0.0, 1.0);
        Gfx::FillRect(Rect{card.x, card.Bottom() - 3, card.w, 3}, kWhite.Alpha(0.25f));
        Gfx::FillRect(Rect{card.x, card.Bottom() - 3, card.w * f, 3}, kWhite);
    }

    float ht = (float)(now - heartAt);
    if (ht < 0.9f) {
        float grow = ht < 0.15f ? 0.6f + ht / 0.15f * 0.55f : 1.15f - std::min(ht - 0.15f, 0.1f);
        float a = ht < 0.55f ? 1.f : 1.f - (ht - 0.55f) / 0.35f;
        Icon::Heart(heartX, heartY - ht * 40, 110 * grow, th.accent.Alpha(a));
    }
    Gfx::PopClip();
}

void App::DrawRail(const Rect& card) {
    const Api::Video* v = Current();
    if (!v) return;
    const Theme& th = T();
    const float cx = card.Right() + 44;
    float y = card.Bottom() - 430;

    // Avatar with the red follow badge.
    Gfx::FillCircle(cx, y, 29, kWhite);
    const Images::Img* av = Images::Get(v->avatar, 128);
    if (av && av->tex) Gfx::ImageCircle(av->tex, cx, y, 27);
    else Gfx::FillCircle(cx, y, 27, th.surface3);
    Gfx::FillCircle(cx, y + 29, 11, th.accent);
    Icon::Plus(cx, y + 29, 12, kWhite);
    y += 92;

    struct Btn {
        int kind;
        std::string count;
    };
    const bool isLiked = liked.count(v->id) > 0, isSaved = saved.count(v->id) > 0;
    const Btn btns[] = {{0, Count(v->likes + isLiked)},
                        {1, Count(v->comments)},
                        {2, Count(v->saves + isSaved)},
                        {3, Count(v->shares)}};
    for (const Btn& b : btns) {
        Color bg = th.surface3;
        Gfx::FillCircle(cx, y, 25, bg);
        switch (b.kind) {
            case 0: Icon::Heart(cx, y + 1, 26, isLiked ? th.accent : kWhite); break;
            case 1: Icon::Bubble(cx, y + 1, 26, commentsOpen ? th.link : kWhite, bg); break;
            case 2: Icon::Bookmark(cx, y, 24, isSaved ? th.warn : kWhite, bg); break;
            default: Icon::Share(cx, y, 26, kWhite); break;
        }
        Text::DrawCentered(b.count, cx, y + 30, Face::Bold, 14, th.text);
        Rect hit{cx - 32, y - 30, 64, 82};
        switch (b.kind) {
            case 0:
                Ui::Hot(hit, [this] {
                    heartX = kCard.x + kCard.w / 2;
                    heartY = kCard.y + kCard.h / 2;
                    Like();
                });
                break;
            case 1: Ui::Hot(hit, [this] { ToggleComments(); }); break;
            case 2:
                Ui::Hot(hit, [this, id = v->id] {
                    if (!saved.erase(id)) saved.insert(id);
                });
                break;
            default:
                Ui::Hot(hit, [this, url = v->WebUrl()] { Toast(url.substr(8)); });
                break;
        }
        y += 84;
    }

    // The record that spins on the app.
    float spin = (float)Ui::Time();
    y = card.Bottom() - 34;
    Gfx::FillCircle(cx, y, 22, Color::Hex(0x222222));
    Gfx::StrokeCircle(cx, y, 16, 1, Color::Hex(0x3A3A3A));
    if (av && av->tex) Gfx::ImageCircle(av->tex, cx, y, 11);
    if (!Player::Paused()) Icon::Note(cx + 30 + std::sin(spin * 2) * 3, y - 26 - std::fmod(spin * 14, 18.f), 12, kWhite.Alpha(0.7f));
}

void App::DrawUpNext(const Rect& area) {
    const Theme& th = T();
    Text::Draw("Up next", area.x + 4, area.y + 4, Face::Bold, 20, kWhite);
    float y = area.y + 46;
    for (int i = cur + 1; i < (int)feed.size() && i <= cur + 3; i++) {
        const Api::Video& n = feed[i];
        Rect thumb{area.x, y, 104, 184};
        Gfx::FillRound(thumb, 10, th.surface2);
        if (const Images::Img* c = Images::Get(n.cover, 360); c && c->tex) Gfx::ImageCover(c->tex, c->w, c->h, thumb, 10);
        Text::Draw(Count(n.plays), thumb.x + 8, thumb.Bottom() - 26, Face::Bold, 13, kWhite);
        float tx = thumb.Right() + 14, tw = area.Right() - tx;
        Text::Draw(n.author, tx, y + 4, Face::Bold, 16, kWhite, tw);
        auto lines = Wrap(n.caption.empty() ? n.music : n.caption, Face::Regular, 14, tw, 4);
        for (size_t l = 0; l < lines.size(); l++)
            Text::Draw(lines[l], tx, y + 30 + l * 20, Face::Regular, 14, th.textMuted, tw);
        Icon::Heart(tx + 8, thumb.Bottom() - 14, 14, th.textMuted);
        Text::Draw(Count(n.likes), tx + 22, thumb.Bottom() - 24, Face::Medium, 14, th.textMuted);
        int idx = i;
        Ui::Hot(Rect{area.x, y, area.w, 184}, [this, idx] { Go(idx); });
        y += 204;
    }
    if (feedBusy && cur + 1 >= (int)feed.size()) Icon::Spinner(area.x + area.w / 2, y + 40, 14, th.accent, Ui::Time());
}

void App::DrawComments(const Rect& area) {
    const Theme& th = T();
    const Api::Video* v = Current();
    Gfx::FillRound(area, 14, th.surface);
    std::string title = v ? Count(v->comments) + " comments" : "Comments";
    Text::DrawCentered(title, area.x + area.w / 2, area.y + 16, Face::Bold, 17, kWhite);
    Rect close{area.Right() - 50, area.y + 8, 42, 42};
    Icon::Close(close.x + 21, close.y + 21, 16, th.text);
    Ui::Hot(close, [this] { ToggleComments(); });

    Rect list{area.x, area.y + 56, area.w, area.h - 56};
    const float textX = list.x + 64, textW = list.w - 64 - 46;
    while (g_commentLines.size() < comments.size())
        g_commentLines.push_back(Wrap(comments[g_commentLines.size()].text, Face::Regular, 15, textW, 12));

    Gfx::PushClip(list);
    float y = list.y + 6 - commentsScroll;
    for (size_t i = 0; i < comments.size(); i++) {
        const Api::Comment& c = comments[i];
        const auto& lines = g_commentLines[i];
        float h = 24 + lines.size() * 21.0f + 30;
        if (y + h > list.y && y < list.Bottom()) {
            const Images::Img* av = Images::Get(c.avatar, 96);
            if (av && av->tex) Gfx::ImageCircle(av->tex, list.x + 34, y + 20, 18);
            else Gfx::FillCircle(list.x + 34, y + 20, 18, th.surface3);
            Text::Draw(c.nickname, textX, y, Face::SemiBold, 14, th.textMuted, textW);
            for (size_t l = 0; l < lines.size(); l++)
                Text::Draw(lines[l], textX, y + 22 + l * 21, Face::Regular, 15, th.text, textW);
            std::string meta = RelTime(c.time);
            if (c.replies > 0) meta += Str::Format("   %s %s", Count(c.replies).c_str(), c.replies == 1 ? "reply" : "replies");
            Text::Draw(meta, textX, y + 24 + lines.size() * 21.0f, Face::Regular, 13, th.textFaint);
            Icon::Heart(list.Right() - 26, y + 22, 16, th.textMuted);
            Text::DrawCentered(Count(c.likes), list.Right() - 26, y + 34, Face::Regular, 12, th.textMuted);
        }
        y += h + 10;
    }
    float contentH = y + commentsScroll - list.y;
    commentsMax = std::max(0.f, contentH - list.h + 20);
    if (commentsBusy) {
        Icon::Spinner(list.x + list.w / 2, std::min(y + 30, list.Bottom() - 30), 14, th.accent, Ui::Time());
    } else if (!commentsError.empty()) {
        Text::DrawCentered("Couldn't load comments", list.x + list.w / 2, y + 10, Face::SemiBold, 15, th.text);
        Text::DrawCentered(commentsError, list.x + list.w / 2, y + 34, Face::Regular, 13, th.textMuted, list.w - 30);
    } else if (comments.empty()) {
        Text::DrawCentered("No comments yet", list.x + list.w / 2, y + 30, Face::SemiBold, 15, th.textMuted);
    }
    Gfx::PopClip();
    Ui::ScrollZone(list, &commentsScroll, commentsMax);
}
