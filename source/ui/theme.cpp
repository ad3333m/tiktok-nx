#include "ui/theme.h"

namespace {

Theme g_theme;

Color H(uint32_t rgb, uint8_t a = 255) { return Color::Hex(rgb, a); }

}  // namespace

const Theme& T() { return g_theme; }

void InitTheme() {
    Theme& t = g_theme;
    // TikTok's dark mode: black page, #FE2C55 for likes and buttons, #25F4EE cyan.
    t.bg = H(0x000000);
    t.surface = H(0x121212);
    t.surface2 = H(0x1F1F1F);
    t.surface3 = H(0x2F2F2F);
    t.border = H(0x2A2A2A);
    t.header = H(0x000000);
    t.input = H(0x1F1F1F);
    t.text = H(0xE8E8E8);
    t.textStrong = H(0xFFFFFF);
    t.textMuted = H(0xA0A0A0);
    t.textFaint = H(0x6B6B6B);
    t.link = H(0x25F4EE);
    t.accent = H(0xFE2C55);
    t.accentText = H(0xFFFFFF);
    t.warn = H(0xFACE15);
    t.danger = H(0xFE2C55);
    t.scrim = H(0x000000, 190);
    t.codeBg = H(0x000000);
}
