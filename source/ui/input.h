#pragma once

#include <functional>
#include <vector>

#include "platform/platform.h"
#include "ui/gfx.h"

// Per-frame input helpers: button repeat, stick navigation, and touch (taps resolve
// against the hotspots drawn last frame; drags scroll the zone under the finger).
namespace Ui {

void BeginFrame(const Platform::Input& in, float dt);
// drew=false keeps last frame's touch targets (nothing was redrawn this frame).
void EndFrame(bool drew);
const Platform::Input& In();
float Dt();
double Time();
bool Active();  // any input this frame (for redraw decisions)

bool Pressed(uint32_t button);
bool Held(uint32_t button);
// True on press, then repeatedly while held.
bool Repeat(uint32_t button, float delay = 0.38f, float rate = 0.075f);
// D-pad or left stick, with repeat: -1, 0 or +1. Repeat speeds up when the
// stick is pushed hard (full tilt ≈ 18 steps/sec, light tilt ≈ 7 steps/sec).
int NavY();
int NavX();
// ZL / ZR for page jumps in long lists (-1 for ZL, +1 for ZR), with repeat.
int PageY();
// Right-stick scroll delta in pixels for this frame (positive = scroll down).
// Independent of NavY so you can read a list while the selection stays put.
float StickScrollDelta(float pixelsPerSecond = 1400.0f);
// Swallows the rest of this frame's button presses (after a screen change).
void Consume();

// Touch.
void Hot(const Rect& r, std::function<void()> onTap, std::function<void()> onLong = nullptr);
void Block(const Rect& r, std::function<void()> onTap = nullptr);  // eats taps behind a dialog
void ScrollZone(const Rect& r, float* value, float maxValue, bool inverted = false);
bool Touching();
bool Dragging();
bool Pressing(const Rect& r);  // finger is down on r (for pressed-state visuals)
bool Animating();              // a fling is still moving

}  // namespace Ui
