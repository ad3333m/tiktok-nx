# TikTok for Nintendo Switch

A homebrew `.nro` that plays TikTok's real For You feed on the Switch.

- Full-screen 9:16 videos with sound, looping like the app; photo posts play as slideshows with their music
- The action rail: avatar, likes, comments, saves, shares
- Real comments in a side panel
- "Up next" previews, region picker for the feed

## Install

Download `TikTok.nro` from [Releases](../../releases/tag/latest), copy it to `sdmc:/switch/`, and open it
from the Homebrew Menu.

## Controls

| Button | Action |
| --- | --- |
| Up / Down, ZL / ZR, swipe | Previous / next video |
| A, tap | Pause |
| X, double tap | Like |
| Y | Comments (B closes, Up / Down scroll) |
| L / R, Left / Right | Seek 5 s (or change photo) |
| - | Feed region |
| + | Exit |

## How it works

TikTok has no public API, so the feed and comments come from [tikwm.com](https://www.tikwm.com), which hands
back direct MP4 links. Videos stream over curl in byte ranges and decode with ffmpeg (NVDEC via nvtegra when it
works, the CPU otherwise). There's no login, so likes and saves stay on the console for the session.

Search and profiles aren't in yet: tikwm puts those endpoints behind a browser check.

Built with devkitPro (SDL2, curl, FreeType, ffmpeg) on GitHub Actions. Not affiliated with TikTok or ByteDance.
