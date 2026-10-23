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

const Rect kCard{300, 0, 480, 720};
const Rect kRight{880, 10, 390, 700};
const Rect kPanel{340, 120, 600, 420};
const Color kWhite(255, 255, 255);

struct Region {
    const char* code;
    const char* name;
};
const Region kRegions[] = {
    {"GB", "United Kingdom"}, {"US", "United States"}, {"IE", "Ireland"},       {"CA", "Canada"},
    {"AU", "Australia"},      {"NZ", "New Zealand"},   {"DE", "Germany"},       {"FR", "France"},
    {"ES", "Spain"},          {"IT", "Italy"},         {"NL", "Netherlands"},   {"BE", "Belgium"},
    {"SE", "Sweden"},         {"NO", "Norway"},        {"DK", "Denmark"},       {"FI", "Finland"},
    {"PL", "Poland"},         {"PT", "Portugal"},      {"AT", "Austria"},       {"CH", "Switzerland"},
    {"BR", "Brazil"},         {"MX", "Mexico"},        {"AR", "Argentina"},     {"JP", "Japan"},
    {"KR", "South Korea"},    {"PH", "Philippines"},   {"ID", "Indonesia"},     {"MY", "Malaysia"},
    {"SG", "Singapore"},      {"TH", "Thailand"},      {"VN", "Vietnam"},       {"ZA", "South Africa"},
    {"NG", "Nigeria"},        {"EG", "Egypt"},         {"AE", "United Arab Emirates"}, {"SA", "Saudi Arabia"},
    {"TR", "Turkey"},
};
const int kRegionCount = (int)(sizeof(kRegions) / sizeof(kRegions[0]));
const char* const kFonts[] = {"TikTok Sans", "Minecraft", "Inter", "Nintendo"};
const char* const kSizes[] = {"Large", "Fit"};

struct Setting {
    const char* label;
    const char* help;
};
const Setting kSettings[] = {
    {"Region", "Whose For You feed you see"},
    {"Font", "Text across the app"},
    {"Video size", "Large fills the card, Fit shows the whole frame"},
};
constexpr int kSettingCount = 3;
constexpr float kRowH = 76, kOptH = 48;
constexpr int kOptVisible = 7;

// Per-comment wrapped lines, rebuilt when the list changes.
std::vector<std::vector<std::string>> g_commentLines;

std::string SettingsPath() { return Platform::DataDir() + "settings.txt"; }

std::string RegionName(const std::string& code) {
    for (const Region& r : kRegions)
        if (code == r.code) return r.name;
    return code;
}

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

// Portrait pictures fill the box (cropping the edges) when `fill`; anything else fits.
void DrawPicture(SDL_Texture* t, int tw, int th, const Rect& box, bool fill) {
    if (!fill || th <= tw) {
        Gfx::Image(t, Contain(box, (float)tw, (float)th));
        return;
    }
    float s = std::max(box.w / tw, box.h / th);
    float vw = box.w / s, vh = box.h / s;
    SDL_Rect src{(int)((tw - vw) / 2), (int)((th - vh) / 2), (int)vw, (int)vh};
    SDL_FRect dst{box.x, box.y, box.w, box.h};
    SDL_RenderCopyF(Gfx::Renderer(), t, &src, &dst);
}

float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

int OptionCount(int row) { return row == 0 ? kRegionCount : row == 1 ? 4 : 2; }

std::string OptionLabel(int row, int i) {
    if (row == 0) return kRegions[i].name;
    if (row == 1) return i == 3 ? "Nintendo (console)" : kFonts[i];
    return kSizes[i];
}

}  // namespace

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

void App::Init() {
    if (FILE* f = fopen(SettingsPath().c_str(), "rb")) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            std::string s = Str::Trim(line);
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            if (k == "region" && v.size() == 2) region = v;
            else if (k == "font") font = v;
            else if (k == "video") fillVideo = v != "fit";
        }
        fclose(f);
    }
    if (font != Text::Family() && !Text::SetFamily(font)) {
        font = "TikTok Sans";
        Text::SetFamily(font);
    }
    LoadMore();
}

void App::SaveSettings() {
    FILE* f = fopen(SettingsPath().c_str(), "wb");
    if (!f) return;
    fprintf(f, "region=%s\nfont=%s\nvideo=%s\n", region.c_str(), font.c_str(), fillVideo ? "large" : "fit");
    fclose(f);
}

void App::SetRegion(const std::string& code) {
    if (code == region) return;
    region = code;
    SaveSettings();
    Player::Close();
    feed.clear();
    seen.clear();
    cur = -1;
    commentsOpen = false;
    commentsFor.clear();
    LoadMore();
    Toast("For You is now from " + RegionName(region));
}

void App::UpdateSettings() {
    auto current = [&](int row) {
        if (row == 0) {
            for (int i = 0; i < kRegionCount; i++)
                if (region == kRegions[i].code) return i;
            return 0;
        }
        if (row == 1) {
            for (int i = 0; i < 4; i++)
                if (font == kFonts[i]) return i;
            return 0;
        }
        return fillVideo ? 0 : 1;
    };
    auto apply = [&](int row, int i) {
        if (row == 0) {
            SetRegion(kRegions[i].code);
        } else if (row == 1) {
            if (font != kFonts[i] && Text::SetFamily(kFonts[i])) {
                font = kFonts[i];
                SaveSettings();
            }
        } else {
            fillVideo = i == 0;
            SaveSettings();
        }
        dropdown = -1;
    };
    // Touch picks set these; they run here so the lists stay consistent.
    static int tapRow = -1, tapOpt = -1;
    if (tapOpt >= 0 && dropdown >= 0) {
        apply(dropdown, tapOpt);
    } else if (tapRow >= 0) {
        settingsSel = tapRow;
        dropdown = tapRow;
        dropSel = current(tapRow);
    }
    tapRow = tapOpt = -1;

    if (dropdown >= 0) {
        int n = OptionCount(dropdown);
        dropSel = std::clamp(dropSel + Ui::NavY() + Ui::PageY() * kOptVisible, 0, n - 1);
        if (Ui::Pressed(BTN_A)) apply(dropdown, dropSel);
        else if (Ui::Pressed(BTN_B)) dropdown = -1;
    } else {
        settingsSel = std::clamp(settingsSel + Ui::NavY(), 0, kSettingCount - 1);
        if (Ui::Pressed(BTN_A)) {
            dropdown = settingsSel;
            dropSel = current(settingsSel);
        } else if (Ui::Pressed(BTN_B) || Ui::Pressed(BTN_MINUS)) {
            settingsOpen = false;
        }
    }
    if (dropdown >= 0) {
        float top = dropSel * kOptH, view = kOptVisible * kOptH;
        if (top < dropScroll) dropScroll = top;
        if (top + kOptH > dropScroll + view) dropScroll = top + kOptH - view;
    }

    // Register the touch targets drawn by DrawSettings.
    Ui::Block(Rect{0, 0, (float)Gfx::W, (float)Gfx::H}, [this] {
        if (dropdown >= 0) dropdown = -1;
        else settingsOpen = false;
    });
    Ui::Block(kPanel);
    Ui::Hot(Rect{kPanel.Right() - 56, kPanel.y + 10, 46, 46}, [this] { settingsOpen = false; });
    for (int i = 0; i < kSettingCount; i++)
        Ui::Hot(Rect{kPanel.x + 16, kPanel.y + 76 + i * kRowH, kPanel.w - 32, kRowH - 8}, [i] { tapRow = i; });
    if (dropdown >= 0) {
        Rect row{kPanel.x + 16, kPanel.y + 76 + dropdown * kRowH, kPanel.w - 32, kRowH - 8};
        int n = OptionCount(dropdown);
        float h = std::min(n, kOptVisible) * kOptH + 8;
        Rect list{row.Right() - 300, row.Bottom() + 4, 300, h};
        if (list.Bottom() > Gfx::H - 10) list.y = row.y - 4 - h;
        Ui::Block(list);
        for (int i = 0; i < n; i++) {
            float y = list.y + 4 + i * kOptH - dropScroll;
            if (y + kOptH <= list.y || y >= list.Bottom()) continue;
            Ui::Hot(Rect{list.x, std::max(y, list.y), list.w, kOptH}, [i] { tapOpt = i; });
        }
        float maxScroll = std::max(0.f, n * kOptH - kOptVisible * kOptH);
        Ui::ScrollZone(list, &dropScroll, maxScroll);
    }
}

// ---------------------------------------------------------------------------
// feed
// ---------------------------------------------------------------------------

void App::Shutdown() { Player::Close(); }

void App::Toast(const std::string& text) {
    toast = text;
    toastUntil = Ui::Time() + 3;
}

void App::LoadMore() {
    if (feedBusy) return;
    feedBusy = true;
    feedError.clear();
    std::string want = region;
    Api::Feed(region, [this, want](bool ok, std::vector<Api::Video> vids, const std::string& err) {
        feedBusy = false;
        if (want != region) return;  // the region changed while this was loading
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

bool App::Frame(const Platform::Input& in, float dt) {
    Ui::BeginFrame(in, dt);
    int pumped = Async::Pump();
    bool newFrame = Player::Update();
    const double now = Ui::Time();
    const std::string fontBefore = font;

    if (Ui::Pressed(BTN_PLUS)) quit = true;
    if (settingsOpen) {
        UpdateSettings();
    } else {
        const Api::Video* v = Current();
        if (Ui::Pressed(BTN_MINUS)) {
            settingsOpen = true;
            dropdown = -1;
        }
        if (Ui::Pressed(BTN_A)) Player::SetPaused(!Player::Paused());
        if (Ui::Pressed(BTN_X)) {
            heartX = kCard.x + kCard.w / 2;
            heartY = kCard.y + kCard.h / 2;
            Like();
        }
        if (Ui::Pressed(BTN_Y) || (commentsOpen && Ui::Pressed(BTN_B))) ToggleComments();
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
    }

    // Swipe the card like a phone: up for the next video, down for the last one.
    static bool wasTouching = false;
    static float startY = 0, lastX = 0, lastY = 0;
    if (in.touching) {
        lastX = in.tx;
        lastY = in.ty;
        if (!wasTouching && !settingsOpen && kCard.Contains(in.tx, in.ty)) {
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
    if (!settingsOpen) {
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
    }

    const Api::Video* v = Current();
    if (v && v->images.size() > 1 && !Player::Paused() && now - photoAt > 3.5) {
        photo = (photo + 1) % (int)v->images.size();
        photoAt = now;
    }
    if (!feedBusy && now >= feedRetryAt && (feed.empty() || cur >= (int)feed.size() - 3)) LoadMore();
    if (font != fontBefore) g_commentLines.clear();  // widths changed with the font

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
    if (settingsOpen) DrawSettings();

    if (toastUntil > now) {
        float w = Text::Width(toast, Face::SemiBold, 16) + 40;
        Rect r{(Gfx::W - w) / 2, 650, w, 44};
        Gfx::FillRound(r, 22, Color::Hex(0x2F2F2F, 240));
        Text::DrawCentered(toast, r.x + r.w / 2, r.y + 11, Face::SemiBold, 16, kWhite);
    }
}

void App::DrawSidebar() {
    const Theme& th = T();
    Icon::Logo(28, 28, 40);

    Rect home{16, 100, 268, 50};
    Gfx::FillRound(home, 10, th.surface2);
    Icon::Svg("house", home.x + 28, home.y + 25, 22, th.accent);
    Text::Draw("For You", home.x + 54, home.y + 13, Face::Bold, 19, th.accent);

    Rect set{16, 156, 268, 50};
    Ui::Hot(set, [this] {
        settingsOpen = true;
        dropdown = -1;
    });
    Icon::Svg("gear", set.x + 28, set.y + 25, 21, th.text);
    Text::Draw("Settings", set.x + 54, set.y + 13, Face::SemiBold, 19, th.text);

    Text::Draw(RegionName(region), 32, 222, Face::Medium, 15, th.textFaint, 250);
    if (!feed.empty())
        Text::Draw(Str::Format("Video %d of %d", cur + 1, (int)feed.size()), 32, 244, Face::Medium, 15, th.textFaint);

    const std::vector<std::pair<std::string, std::string>> keys = {
        {"\xE2\x86\x95", "Next / previous"}, {"A", "Pause"},       {"X", "Like"},       {"Y", "Comments"},
        {"L", "Back 5s"},                     {"R", "Forward 5s"}, {"-", "Settings"}, {"+", "Exit"},
    };
    float y = 720 - 26 - keys.size() * 34.0f;
    for (const auto& [key, label] : keys) {
        Icon::Button(30, y + 14, key, th.bg, th.textMuted);
        Text::Draw(label, 74, y + 3, Face::Medium, 16, th.textMuted);
        y += 34;
    }
}

void App::DrawCard(const Rect& card) {
    const Theme& th = T();
    const double now = Ui::Time();
    Gfx::FillRect(card, th.surface);
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
        if (img && img->tex) DrawPicture(img->tex, img->w, img->h, card, fillVideo);
        else Icon::Spinner(card.x + card.w / 2, card.y + card.h / 2, 18, th.accent, now);
    } else if (SDL_Texture* tex = Player::Texture(&tw, &thh)) {
        DrawPicture(tex, tw, thh, card, fillVideo);
    } else if (const Images::Img* cov = Images::Get(v->cover, 720); cov && cov->tex) {
        DrawPicture(cov->tex, cov->w, cov->h, card, fillVideo);
    }
    if (Player::Loading() && v->images.empty())
        Icon::Spinner(card.x + card.w / 2, card.y + card.h / 2, 18, kWhite, now);
    std::string err = Player::Error();
    if (!err.empty()) {
        Gfx::FillRound(Rect{card.x + 30, card.y + 300, card.w - 60, 90}, 12, Color::Hex(0, 170));
        Text::DrawCentered("Couldn't play this one", card.x + card.w / 2, card.y + 314, Face::Bold, 17, kWhite);
        Text::DrawCentered(err, card.x + card.w / 2, card.y + 342, Face::Regular, 14, th.textMuted, card.w - 80);
    }
    if (Player::Paused()) Icon::Svg("play", card.x + card.w / 2 + 6, card.y + card.h / 2, 76, kWhite.Alpha(0.55f));

    // Caption over a fade at the bottom, like the app.
    Gfx::VGradient(Rect{card.x, card.Bottom() - 250, card.w, 250}, Color::Hex(0, 0), Color::Hex(0, 190));
    float y = card.Bottom() - 46;
    Icon::Svg("music", card.x + 26, y + 11, 15, kWhite);
    Text::Draw(v->music.empty() ? "original sound" : v->music, card.x + 44, y, Face::Medium, 15, kWhite, card.w - 64);
    auto lines = Wrap(v->caption, Face::Regular, 16, card.w - 40, 3);
    y -= 6 + lines.size() * 22.0f;
    for (size_t i = 0; i < lines.size(); i++)
        Text::Draw(lines[i], card.x + 20, y + i * 22, Face::Regular, 16, kWhite, card.w - 40);
    y -= 30;
    float nw = Text::Draw(v->author, card.x + 20, y, Face::Bold, 18, kWhite, card.w - 140);
    if (!v->nickname.empty() && v->nickname != v->author)
        Text::Draw(v->nickname, card.x + 28 + nw, y + 2, Face::Regular, 15, th.textMuted, card.w - nw - 56);

    // Photo dots or the playback bar.
    if (v->images.size() > 1) {
        float dx = card.x + card.w / 2 - (std::min<size_t>(v->images.size(), 20) - 1) * 7.0f;
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
        Icon::Svg("heart", heartX, heartY - ht * 40, 110 * grow, th.accent.Alpha(a));
    }
    Gfx::PopClip();
}

void App::DrawRail(const Rect& card) {
    const Api::Video* v = Current();
    if (!v) return;
    const Theme& th = T();
    const float cx = card.Right() + 46;
    float y = card.Bottom() - 440;

    // Avatar with the red follow badge.
    Gfx::FillCircle(cx, y, 29, kWhite);
    const Images::Img* av = Images::Get(v->avatar, 128);
    if (av && av->tex) Gfx::ImageCircle(av->tex, cx, y, 27);
    else Gfx::FillCircle(cx, y, 27, th.surface3);
    Gfx::FillCircle(cx, y + 29, 11, th.accent);
    Icon::Plus(cx, y + 29, 12, kWhite);
    y += 94;

    const bool isLiked = liked.count(v->id) > 0, isSaved = saved.count(v->id) > 0;
    struct Btn {
        const char* icon;
        std::string count;
        Color color;
        float size;
    };
    const Btn btns[] = {
        {"heart", Count(v->likes + isLiked), isLiked ? th.accent : kWhite, 30},
        {"comment-dots", Count(v->comments), commentsOpen ? th.link : kWhite, 30},
        {"bookmark", Count(v->saves + isSaved), isSaved ? th.warn : kWhite, 27},
        {"share", Count(v->shares), kWhite, 30},
    };
    for (int i = 0; i < 4; i++) {
        const Btn& b = btns[i];
        Gfx::FillCircle(cx, y, 26, th.surface3);
        Icon::Svg(b.icon, cx, y, b.size, b.color);
        Text::DrawCentered(b.count, cx, y + 31, Face::Bold, 14, th.text);
        Rect hit{cx - 34, y - 30, 68, 84};
        if (i == 0) {
            Ui::Hot(hit, [this] {
                heartX = kCard.x + kCard.w / 2;
                heartY = kCard.y + kCard.h / 2;
                Like();
            });
        } else if (i == 1) {
            Ui::Hot(hit, [this] { ToggleComments(); });
        } else if (i == 2) {
            Ui::Hot(hit, [this, id = v->id] {
                if (!saved.erase(id)) saved.insert(id);
            });
        } else {
            Ui::Hot(hit, [this, url = v->WebUrl()] { Toast(url.substr(8)); });
        }
        y += 86;
    }

    // The record that spins on the app.
    float spin = (float)Ui::Time();
    y = card.Bottom() - 36;
    Gfx::FillCircle(cx, y, 22, Color::Hex(0x222222));
    Gfx::StrokeCircle(cx, y, 16, 1, Color::Hex(0x3A3A3A));
    if (av && av->tex) Gfx::ImageCircle(av->tex, cx, y, 11);
    if (!Player::Paused())
        Icon::Svg("music", cx + 30 + std::sin(spin * 2) * 3, y - 26 - std::fmod(spin * 14, 18.f), 12,
                  kWhite.Alpha(0.7f));
}

void App::DrawUpNext(const Rect& area) {
    const Theme& th = T();
    Text::Draw("Up next", area.x + 4, area.y + 4, Face::Bold, 20, kWhite);
    float y = area.y + 46;
    for (int i = cur + 1; i < (int)feed.size() && i <= cur + 3; i++) {
        const Api::Video& n = feed[i];
        Rect thumb{area.x, y, 106, 188};
        Gfx::FillRound(thumb, 10, th.surface2);
        if (const Images::Img* c = Images::Get(n.cover, 360); c && c->tex)
            Gfx::ImageCover(c->tex, c->w, c->h, thumb, 10);
        Text::Draw(Count(n.plays), thumb.x + 8, thumb.Bottom() - 26, Face::Bold, 13, kWhite);
        float tx = thumb.Right() + 14, tw = area.Right() - tx;
        Text::Draw(n.author, tx, y + 4, Face::Bold, 16, kWhite, tw);
        auto lines = Wrap(n.caption.empty() ? n.music : n.caption, Face::Regular, 14, tw, 5);
        for (size_t l = 0; l < lines.size(); l++)
            Text::Draw(lines[l], tx, y + 30 + l * 20, Face::Regular, 14, th.textMuted, tw);
        Icon::Svg("heart", tx + 8, thumb.Bottom() - 14, 14, th.textMuted);
        Text::Draw(Count(n.likes), tx + 22, thumb.Bottom() - 24, Face::Medium, 14, th.textMuted);
        int idx = i;
        Ui::Hot(Rect{area.x, y, area.w, 188}, [this, idx] { Go(idx); });
        y += 208;
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
    const float textX = list.x + 64, textW = list.w - 64 - 50;
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
            if (c.replies > 0)
                meta += Str::Format("   %s %s", Count(c.replies).c_str(), c.replies == 1 ? "reply" : "replies");
            Text::Draw(meta, textX, y + 24 + lines.size() * 21.0f, Face::Regular, 13, th.textFaint);
            Icon::Svg("heart", list.Right() - 28, y + 22, 16, th.textMuted);
            Text::DrawCentered(Count(c.likes), list.Right() - 28, y + 34, Face::Regular, 12, th.textMuted);
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

void App::DrawSettings() {
    const Theme& th = T();
    Gfx::FillRect(Rect{0, 0, (float)Gfx::W, (float)Gfx::H}, th.scrim);
    Gfx::Shadow(kPanel, 18, 24, Color::Hex(0, 160));
    Gfx::FillRound(kPanel, 18, th.surface);
    Text::Draw("Settings", kPanel.x + 28, kPanel.y + 22, Face::Bold, 24, kWhite);
    Icon::Close(kPanel.Right() - 33, kPanel.y + 33, 16, th.text);

    const std::string values[kSettingCount] = {RegionName(region), font == "Nintendo" ? "Nintendo (console)" : font,
                                               fillVideo ? "Large" : "Fit"};
    for (int i = 0; i < kSettingCount; i++) {
        Rect row{kPanel.x + 16, kPanel.y + 76 + i * kRowH, kPanel.w - 32, kRowH - 8};
        if (i == settingsSel) Gfx::FillRound(row, 12, dropdown == i ? th.surface3 : th.surface2);
        Text::Draw(kSettings[i].label, row.x + 16, row.y + 10, Face::SemiBold, 18, kWhite);
        Text::Draw(kSettings[i].help, row.x + 16, row.y + 36, Face::Regular, 14, th.textMuted, 280);
        Icon::Svg("chevron-down", row.Right() - 26, row.y + row.h / 2, 14, th.textMuted);
        Text::DrawRight(values[i], row.Right() - 46, row.y + 22, Face::Medium, 17, th.text);
    }
    Icon::Hints(kPanel.Right() - 24, kPanel.Bottom() - 30,
                {{"A", dropdown >= 0 ? "Choose" : "Open"}, {"B", dropdown >= 0 ? "Back" : "Close"}}, th.textMuted,
                th.textMuted, th.bg);

    if (dropdown < 0) return;
    Rect row{kPanel.x + 16, kPanel.y + 76 + dropdown * kRowH, kPanel.w - 32, kRowH - 8};
    const int n = OptionCount(dropdown);
    float h = std::min(n, kOptVisible) * kOptH + 8;
    Rect list{row.Right() - 300, row.Bottom() + 4, 300, h};
    if (list.Bottom() > Gfx::H - 10) list.y = row.y - 4 - h;
    Gfx::Shadow(list, 12, 18, Color::Hex(0, 170));
    Gfx::FillRound(list, 12, th.surface2);
    Gfx::PushClip(list.Inset(4));
    const std::string& chosenValue = values[dropdown];
    for (int i = 0; i < n; i++) {
        float y = list.y + 4 + i * kOptH - dropScroll;
        if (y + kOptH <= list.y || y >= list.Bottom()) continue;
        Rect opt{list.x + 4, y, list.w - 8, kOptH};
        if (i == dropSel) Gfx::FillRound(opt, 8, th.surface3);
        std::string label = OptionLabel(dropdown, i);
        bool chosen = label == chosenValue;
        Text::Draw(label, opt.x + 14, opt.y + 13, chosen ? Face::Bold : Face::Medium, 16, chosen ? th.accent : kWhite,
                   opt.w - 60);
        if (dropdown == 0) Text::DrawRight(kRegions[i].code, opt.Right() - 40, opt.y + 14, Face::Medium, 14, th.textFaint);
        if (chosen) Icon::Svg("check", opt.Right() - 18, opt.y + kOptH / 2, 14, th.accent);
    }
    Gfx::PopClip();
}
