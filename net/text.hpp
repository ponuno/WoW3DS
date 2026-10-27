// text.hpp — chat text for an ASCII-only console (the 3DS bottom screen font has no Cyrillic):
// UTF-8 -> ASCII with Russian transliteration (other non-ASCII -> '?', WoW colour/link codes removed),
// and word wrapping to the console width.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace w3n {

// |cAARRGGBB ... |r colour codes and |Hitem:...|h[Name]|h links keep only the visible text.
inline std::string stripWowCodes(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '|' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'c' && i + 10 <= s.size()) { i += 9; continue; }
            if (c == 'r') { ++i; continue; }
            if (c == 'H') {   // |Hlink|h  -> skip to the closing |h
                size_t e = s.find("|h", i + 2);
                if (e == std::string::npos) break;
                i = e + 1;
                continue;
            }
            if (c == 'h') { ++i; continue; }
            if (c == '|') { o += '|'; ++i; continue; }
        }
        o += s[i];
    }
    return o;
}

inline std::string utf8ToConsole(const std::string& in) {
    static const char* kRu[64] = {   // U+0410..U+044F
        "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
        "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "\"", "Y", "'", "E", "Yu", "Ya",
        "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
        "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "\"", "y", "'", "e", "yu", "ya"};
    std::string s = stripWowCodes(in), o;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { o += '?'; ++i; continue; }
        if (i + n > s.size()) { o += '?'; break; }
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            uint8_t cc = (uint8_t)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { o += '?'; ++i; continue; }
        i += n;
        if (cp < 0x20) o += ' ';
        else if (cp < 0x7F) o += (char)cp;
        else if (cp >= 0x410 && cp <= 0x44F) o += kRu[cp - 0x410];
        else if (cp == 0x401) o += "Yo";
        else if (cp == 0x451) o += "yo";
        else if (cp == 0xAB || cp == 0xBB || cp == 0x201C || cp == 0x201D) o += '"';
        else if (cp == 0x2018 || cp == 0x2019) o += '\'';
        else if (cp == 0x2013 || cp == 0x2014) o += '-';
        else o += '?';
    }
    return o;
}

// Word wrap to 'width' columns; words longer than a line are cut. Continuation lines are
// indented by 'indent' spaces.
inline std::vector<std::string> wrapText(const std::string& s, size_t width, size_t indent = 1) {
    std::vector<std::string> out, words;
    if (width <= indent + 1) width = indent + 2;
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) words.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    std::string line;
    auto avail = [&] { return width - (out.empty() ? 0 : indent); };
    auto flush = [&] {
        out.push_back((out.empty() ? "" : std::string(indent, ' ')) + line);
        line.clear();
    };
    for (std::string w : words) {
        if (!line.empty() && line.size() + 1 + w.size() > avail()) flush();
        while (line.empty() && w.size() > avail()) {   // longer than a line: cut it
            line = w.substr(0, avail());
            w = w.substr(avail());
            flush();
        }
        if (w.empty()) continue;
        line += (line.empty() ? "" : " ") + w;
    }
    if (!line.empty() || out.empty()) flush();
    return out;
}

}  // namespace w3n
