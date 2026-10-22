#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ui/gfx.h"

// Vector icons, drawn centred on (cx, cy) inside a box of size s.
namespace Icon {

void Hash(float cx, float cy, float s, Color c);
void Speaker(float cx, float cy, float s, Color c);
void Megaphone(float cx, float cy, float s, Color c);
void Forum(float cx, float cy, float s, Color c);
void Thread(float cx, float cy, float s, Color c);
void Gear(float cx, float cy, float s, Color c, Color hole);
void People(float cx, float cy, float s, Color c);
void Plus(float cx, float cy, float s, Color c);
void Close(float cx, float cy, float s, Color c);
void Check(float cx, float cy, float s, Color c);
void Chevron(float cx, float cy, float s, Color c, int dir);  // 0 right, 1 down, 2 left, 3 up
void Reply(float cx, float cy, float s, Color c);
void Smile(float cx, float cy, float s, Color c);
void Pencil(float cx, float cy, float s, Color c);
void Trash(float cx, float cy, float s, Color c);
void Image(float cx, float cy, float s, Color c);
void File(float cx, float cy, float s, Color c);
void Send(float cx, float cy, float s, Color c);
void Lock(float cx, float cy, float s, Color c);
void Palette(float cx, float cy, float s, Color c, Color bg);
void Info(float cx, float cy, float s, Color c);
void Power(float cx, float cy, float s, Color c);
void Eye(float cx, float cy, float s, Color c);
void Spinner(float cx, float cy, float r, Color c, double t);
// The TikTok wordmark, h tall with its top-left at (x, y).
void Logo(float x, float y, float h, float alpha = 1.0f);
void Heart(float cx, float cy, float s, Color c);
void Bubble(float cx, float cy, float s, Color c, Color dots);
void Bookmark(float cx, float cy, float s, Color c, Color notch);
void Share(float cx, float cy, float s, Color c);
void Note(float cx, float cy, float s, Color c);
void Play(float cx, float cy, float s, Color c);
void Status(float cx, float cy, float r, const std::string& status, Color ring);

// Controller button glyph ("A", "B", "ZL", "+" ...). Returns the width used.
float Button(float x, float cy, const std::string& label, Color fg, Color bg);
float ButtonWidth(const std::string& label);
// A row of "[A] Open  [X] Message" hints, right-aligned at `right`.
void Hints(float right, float cy, const std::vector<std::pair<std::string, std::string>>& items, Color text,
           Color glyphBg, Color glyphFg);
float HintsWidth(const std::vector<std::pair<std::string, std::string>>& items);

}  // namespace Icon
