#pragma once

#include <string>

#include <SDL.h>

// Plays one video at a time, looping like TikTok. curl streams the MP4 in byte
// ranges (the Switch's ffmpeg has no HTTPS), ffmpeg decodes it (NVDEC through
// nvtegra on the console when that works, CPU threads otherwise) and the sound
// goes to an SDL audio queue, which is also the clock the frames follow.
namespace Player {

void Init();  // after SDL_Init(SDL_INIT_AUDIO)
void Shutdown();

void Open(const std::string& url);
void Close();

// Main thread, once per frame: puts the frame that's due on the texture.
// Returns true when the picture changed.
bool Update();
SDL_Texture* Texture(int* w, int* h);  // nullptr until the first frame

bool Loading();  // opened, nothing heard or seen yet
std::string Error();
double Position();  // seconds
double Duration();
void SetPaused(bool paused);
bool Paused();
void Seek(double seconds);

}  // namespace Player
