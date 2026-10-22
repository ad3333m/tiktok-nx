#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ui/gfx.h"

enum class Face { Regular, Medium, SemiBold, Bold, Italic, Mono, Count };

// FreeType glyphs packed into GPU atlas pages. Inter for Latin text, the console's
// shared fonts as fallback for CJK/Korean, Twemoji images for emoji.
namespace Text {

struct Metrics {
    float ascent = 0, descent = 0, lineHeight = 0;
};

bool Init();
void Shutdown();

Metrics GetMetrics(Face f, int size);
// Advance of one code point (no emoji handling).
float Advance(uint32_t cp, Face f, int size);
float EmojiSize(int size);

// Width of a string including inline emoji.
float Width(std::string_view s, Face f, int size);
// Draws s with its line box's top-left at (x, y); lines are `lineHeight` tall.
// With maxW > 0 the text is cut with an ellipsis. Returns the width drawn.
float Draw(std::string_view s, float x, float y, Face f, int size, Color c, float maxW = 0);
float DrawCentered(std::string_view s, float cx, float y, Face f, int size, Color c, float maxW = 0);
float DrawRight(std::string_view s, float right, float y, Face f, int size, Color c);
std::string Ellipsize(std::string_view s, Face f, int size, float maxW);

// Low level, used by the rich-text renderer: draws one glyph at the pen position
// (baseline y). Glyph quads are batched; call Flush() before drawing anything else.
void Glyph(uint32_t cp, float penX, float baseline, Face f, int size, Color c);
void Flush();

}  // namespace Text
