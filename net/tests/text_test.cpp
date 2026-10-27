// text.hpp: transliteration, WoW codes, wrapping.
#include "../text.hpp"

#include <cstdio>

using namespace w3n;

int main() {
    int bad = 0;
    auto eq = [&](const std::string& got, const std::string& want, const char* what) {
        bool ok = got == want;
        if (!ok) std::printf("FAIL %s: '%s' != '%s'\n", what, got.c_str(), want.c_str());
        bad += !ok;
    };
    eq(utf8ToConsole("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD1\x89\xD1\x83\xD0\xBA\xD0\xB0!"), "Privet, shchuka!", "cyrillic");
    eq(utf8ToConsole("\xD0\x81\xD0\xB6 \xD1\x91"), "Yozh yo", "yo");
    eq(utf8ToConsole("caf\xC3\xA9 \xE2\x80\x94 ok"), "caf? - ok", "latin-1 + dash");
    eq(utf8ToConsole("bad \xC3"), "bad ?", "truncated sequence");
    eq(utf8ToConsole("\xFF\xFEx"), "??x", "invalid bytes");
    eq(utf8ToConsole("|cff1eff00|Hitem:7073:0:0:0|h[Broken Fang]|h|r x1"), "[Broken Fang] x1", "item link");
    eq(utf8ToConsole("a||b"), "a|b", "escaped bar");
    auto w = wrapText("the quick brown fox jumps over the lazy dog", 12, 1);
    eq(std::to_string(w.size()), "5", "wrap count");
    eq(w[0], "the quick", "wrap 0");
    eq(w[1], " brown fox", "wrap 1");
    eq(w[2], " jumps over", "wrap 2");
    eq(w[4], " dog", "wrap 4");
    auto w2 = wrapText("abcdefghijklmnopqrstuvwxyz", 10, 2);
    eq(w2[0], "abcdefghij", "cut 0");
    eq(w2[1], "  klmnopqr", "cut 1");
    eq(std::to_string(wrapText("", 40).size()), "1", "empty");
    for (size_t width = 3; width < 50; ++width)   // every line fits
        for (auto& l : wrapText("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor", width, 1))
            if (l.size() > width) { std::printf("FAIL width %zu: '%s'\n", width, l.c_str()); ++bad; break; }
    std::printf("TEXT: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
