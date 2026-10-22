#pragma once

#include <cstddef>
#include <string>

// Unicode emoji are drawn as Twemoji images (the Switch fonts have no colour emoji).
namespace EmojiText {

// If an emoji sequence starts at p, returns its length in bytes and its Twemoji code
// (e.g. "1f44d-1f3fb"); otherwise 0.
size_t Match(const char* p, const char* end, std::string* code);
std::string TwemojiUrl(const std::string& code);
// True when the text is only emoji (and spaces), which Discord shows large.
bool OnlyEmoji(const std::string& s, int* count);

}  // namespace EmojiText
