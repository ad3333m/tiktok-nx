#include "ui/emoji.h"

#include <cstdio>
#include <vector>

#include "util/strings.h"

namespace EmojiText {
namespace {

bool DefaultEmoji(uint32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) {
        // Enclosed alphanumerics (🄰 etc.) are text unless followed by FE0F.
        if (cp >= 0x1F100 && cp <= 0x1F1E5) return cp == 0x1F18E || (cp >= 0x1F191 && cp <= 0x1F19A);
        return true;
    }
    switch (cp) {
        case 0x231A: case 0x231B: case 0x23E9: case 0x23EA: case 0x23EB: case 0x23EC: case 0x23F0: case 0x23F3:
        case 0x25FD: case 0x25FE: case 0x2614: case 0x2615: case 0x267F: case 0x2693: case 0x26A1: case 0x26AA:
        case 0x26AB: case 0x26BD: case 0x26BE: case 0x26C4: case 0x26C5: case 0x26CE: case 0x26D4: case 0x26EA:
        case 0x26F2: case 0x26F3: case 0x26F5: case 0x26FA: case 0x26FD: case 0x2705: case 0x270A: case 0x270B:
        case 0x2728: case 0x274C: case 0x274E: case 0x2753: case 0x2754: case 0x2755: case 0x2757: case 0x2795:
        case 0x2796: case 0x2797: case 0x27B0: case 0x27BF: case 0x2B1B: case 0x2B1C: case 0x2B50: case 0x2B55:
            return true;
        default:
            break;
    }
    return cp >= 0x2648 && cp <= 0x2653;
}

// Symbols that become emoji when followed by U+FE0F.
bool TextDefaultEmoji(uint32_t cp) {
    return cp == 0xA9 || cp == 0xAE || cp == 0x203C || cp == 0x2049 || cp == 0x2122 || cp == 0x2139 ||
           (cp >= 0x2194 && cp <= 0x21AA) || cp == 0x2328 || cp == 0x23CF || (cp >= 0x23ED && cp <= 0x23FA) ||
           cp == 0x24C2 || (cp >= 0x25AA && cp <= 0x25FE) || (cp >= 0x2600 && cp <= 0x27BF) || cp == 0x2934 ||
           cp == 0x2935 || (cp >= 0x2B05 && cp <= 0x2B07) || cp == 0x3030 || cp == 0x303D || cp == 0x3297 ||
           cp == 0x3299 || (cp >= 0x1F100 && cp <= 0x1F1E5);
}

bool IsModifier(uint32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }
bool IsRegional(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
bool IsTag(uint32_t cp) { return cp >= 0xE0020 && cp <= 0xE007F; }

uint32_t Peek(const char* p, const char* end, const char** next) {
    if (p >= end) {
        *next = p;
        return 0;
    }
    const char* q = p;
    uint32_t cp = Str::NextCp(q, end);
    *next = q;
    return cp;
}

}  // namespace

size_t Match(const char* p, const char* end, std::string* code) {
    const char* start = p;
    const char* q;
    uint32_t first = Peek(p, end, &q);
    if (!first) return 0;
    std::vector<uint32_t> cps;

    // Keycaps: [0-9#*] FE0F? 20E3
    if ((first >= '0' && first <= '9') || first == '#' || first == '*') {
        const char* r;
        uint32_t n = Peek(q, end, &r);
        if (n == 0xFE0F) n = Peek(r, end, &r);
        if (n != 0x20E3) return 0;
        if (code) *code = Str::Format("%x-20e3", (unsigned)first);
        return (size_t)(r - start);
    }
    // Flags: two regional indicators.
    if (IsRegional(first)) {
        const char* r;
        uint32_t n = Peek(q, end, &r);
        if (!IsRegional(n)) return 0;
        if (code) *code = Str::Format("%x-%x", (unsigned)first, (unsigned)n);
        return (size_t)(r - start);
    }

    const char* r;
    uint32_t n = Peek(q, end, &r);
    bool vs16 = n == 0xFE0F;
    if (!DefaultEmoji(first) && !(vs16 && TextDefaultEmoji(first))) return 0;
    cps.push_back(first);
    p = q;
    bool zwj = false;
    while (p < end) {
        n = Peek(p, end, &r);
        if (n == 0xFE0F || IsModifier(n) || IsTag(n)) {
            cps.push_back(n);
            p = r;
            continue;
        }
        if (n == 0x200D) {
            const char* r2;
            uint32_t nn = Peek(r, end, &r2);
            if (nn && (DefaultEmoji(nn) || TextDefaultEmoji(nn))) {
                zwj = true;
                cps.push_back(0x200D);
                cps.push_back(nn);
                p = r2;
                continue;
            }
        }
        break;
    }
    if (code) {
        code->clear();
        for (uint32_t cp : cps) {
            if (cp == 0xFE0F && !zwj) continue;  // Twemoji drops FE0F outside ZWJ sequences
            if (!code->empty()) *code += "-";
            *code += Str::Format("%x", (unsigned)cp);
        }
    }
    return (size_t)(p - start);
}

std::string TwemojiUrl(const std::string& code) {
    return "https://cdn.jsdelivr.net/gh/jdecked/twemoji@17.0.3/assets/72x72/" + code + ".png";
}

bool OnlyEmoji(const std::string& s, int* count) {
    const char* p = s.data();
    const char* end = p + s.size();
    int n = 0;
    while (p < end) {
        if (*p == ' ' || *p == '\n') {
            p++;
            continue;
        }
        // Custom emoji <:name:id> / <a:name:id>
        if (*p == '<' && end - p > 3 && (p[1] == ':' || (p[1] == 'a' && p[2] == ':'))) {
            const char* close = p;
            while (close < end && *close != '>') close++;
            if (close == end) return false;
            p = close + 1;
            n++;
            continue;
        }
        size_t len = Match(p, end, nullptr);
        if (!len) return false;
        p += len;
        n++;
    }
    if (count) *count = n;
    return n > 0;
}

}  // namespace EmojiText
