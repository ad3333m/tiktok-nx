#include "platform/platform.h"

#include <sys/stat.h>
#include <sys/time.h>

#include <cstring>

#include <switch.h>

namespace Platform {
namespace {

PadState g_pad;
bool g_netOk = false;
bool g_plOk = false;

}  // namespace

bool Init() {
    romfsInit();
    socketInitializeDefault();
    g_netOk = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    g_plOk = R_SUCCEEDED(plInitialize(PlServiceType_User));

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();

    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/TikTok", 0777);
    mkdir("sdmc:/switch/TikTok/cache", 0777);
    return true;
}

void Shutdown() {
    SetCpuBoost(false);
    if (g_plOk) plExit();
    if (g_netOk) nifmExit();
    socketExit();
    romfsExit();
}

bool Running() { return appletMainLoop(); }

Input PollInput() {
    Input in;
    padUpdate(&g_pad);
    const u64 held = padGetButtons(&g_pad);
    const u64 down = padGetButtonsDown(&g_pad);
    const u64 up = padGetButtonsUp(&g_pad);
    struct Map {
        u64 hid;
        uint32_t ours;
    };
    static const Map kMap[] = {
        {HidNpadButton_A, BTN_A},         {HidNpadButton_B, BTN_B},         {HidNpadButton_X, BTN_X},
        {HidNpadButton_Y, BTN_Y},         {HidNpadButton_L, BTN_L},         {HidNpadButton_R, BTN_R},
        {HidNpadButton_ZL, BTN_ZL},       {HidNpadButton_ZR, BTN_ZR},       {HidNpadButton_Plus, BTN_PLUS},
        {HidNpadButton_Minus, BTN_MINUS}, {HidNpadButton_Up, BTN_UP},       {HidNpadButton_Down, BTN_DOWN},
        {HidNpadButton_Left, BTN_LEFT},   {HidNpadButton_Right, BTN_RIGHT}, {HidNpadButton_StickL, BTN_LSTICK},
        {HidNpadButton_StickR, BTN_RSTICK},
    };
    for (const auto& m : kMap) {
        if (held & m.hid) in.held |= m.ours;
        if (down & m.hid) in.down |= m.ours;
        if (up & m.hid) in.up |= m.ours;
    }
    HidAnalogStickState l = padGetStickPos(&g_pad, 0);
    HidAnalogStickState r = padGetStickPos(&g_pad, 1);
    in.lx = l.x / 32767.0f;
    in.ly = l.y / 32767.0f;
    in.rx = r.x / 32767.0f;
    in.ry = r.y / 32767.0f;

    HidTouchScreenState ts = {};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        in.touching = true;
        in.tx = (float)ts.touches[0].x;
        in.ty = (float)ts.touches[0].y;
    }
    return in;
}

std::string DataDir() { return "sdmc:/switch/TikTok/"; }
std::string DownloadDir() { return "sdmc:/switch/"; }
std::string AssetPath(const std::string& rel) { return "romfs:/" + rel; }

std::vector<FontBlob> SystemFonts() {
    std::vector<FontBlob> out;
    if (!g_plOk) return out;
    struct T {
        PlSharedFontType type;
        const char* name;
    };
    static const T kTypes[] = {
        {PlSharedFontType_Standard, "Standard"},
        {PlSharedFontType_ChineseSimplified, "ChineseSimplified"},
        {PlSharedFontType_ExtChineseSimplified, "ExtChineseSimplified"},
        {PlSharedFontType_ChineseTraditional, "ChineseTraditional"},
        {PlSharedFontType_KO, "Korean"},
        {PlSharedFontType_NintendoExt, "NintendoExt"},
    };
    for (const auto& t : kTypes) {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, t.type)) && fd.address && fd.size)
            out.push_back({fd.address, fd.size, t.name});
    }
    return out;
}

bool Keyboard(const std::string& guide, const std::string& initial, std::string& out, int maxLen, bool secret,
              bool multiline) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    if (secret) swkbdConfigMakePresetPassword(&kbd);
    else swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, (u32)maxLen);
    if (multiline) {
        swkbdConfigSetTextDrawType(&kbd, SwkbdTextDrawType_Box);
        swkbdConfigSetReturnButtonFlag(&kbd, 1);
    }
    std::vector<char> buf((size_t)maxLen * 4 + 16, 0);
    Result rc = swkbdShow(&kbd, buf.data(), buf.size());
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    out.assign(buf.data(), strnlen(buf.data(), buf.size()));
    return true;
}

int64_t NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

LocalTime ToLocal(int64_t unixMs) {
    LocalTime lt;
    TimeCalendarTime ct;
    TimeCalendarAdditionalInfo info;
    if (R_SUCCEEDED(timeToCalendarTimeWithMyRule((u64)(unixMs / 1000), &ct, &info))) {
        lt.year = ct.year;
        lt.month = ct.month;
        lt.day = ct.day;
        lt.hour = ct.hour;
        lt.minute = ct.minute;
        lt.second = ct.second;
        lt.wday = (int)info.wday;
    }
    return lt;
}

bool NetworkUp() {
    if (!g_netOk) return true;
    NifmInternetConnectionType type;
    u32 strength = 0;
    NifmInternetConnectionStatus status;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) return false;
    return status == NifmInternetConnectionStatus_Connected;
}

void Random(void* buf, size_t len) { randomGet(buf, len); }

bool AppletMode() {
    AppletType t = appletGetAppletType();
    return t != AppletType_Application && t != AppletType_SystemApplication;
}

void SetCpuBoost(bool on) {
    static bool boosted = false;
    if (on == boosted) return;
    if (R_SUCCEEDED(appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal))) boosted = on;
}

std::string DeviceName() { return "Nintendo Switch"; }

}  // namespace Platform
