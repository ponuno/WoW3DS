// game_ui_keyboard.cpp — the on-screen English keyboard of the touch screen (stage 5.46) and the chat destinations.
//
// The system keyboard applet (swkbd) freezes the game for seconds and follows the console's language; this one is drawn
// like any other window of the touch screen: a text field, four rows of keys (digits, qwerty, asdf, shift + zxcv +
// backspace) and a bottom row with the symbols layer, punctuation, space and OK. Shift types one capital, a double tap
// is Caps. Numbers (money fields) get a keypad. The character stands still while it is up (B / Cancel closes it).
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "game_ui.hpp"
#include "text.hpp"
#include "ui_style.hpp"

using namespace ui;

namespace {

// last UTF-8 character off
void popChar(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    s.erase(i);
}

// the tail of 's' that fits in w pixels
std::string tail(std::string s, int w, ui::Font f) {
    while (!s.empty() && HudList::width(s, f) > w) {
        size_t i = 1;
        while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
        s.erase(0, i);
    }
    return s;
}

}  // namespace

void GameUi::askText(const Input& in, const std::string& hint, const std::string& initial, bool numbers, TextDone done, size_t maxLen) {
    if (!kb_.on) kbReturn_ = open_;
    kb_ = Kb();
    kb_.on = true;
    kb_.numbers = numbers;
    kb_.hint = hint;
    kb_.text = initial.substr(0, maxLen);
    kb_.maxLen = numbers ? std::min<size_t>(maxLen, 9) : maxLen;
    kb_.done = std::move(done);
    open_ = Panel::Keyboard;
    press_ = Press();
    (void)in;
    dirty_ = true;
}

void GameUi::closeKeyboard() {
    kb_ = Kb();
    open_ = kbReturn_ == Panel::Keyboard ? Panel::Tabs : kbReturn_;
    dirty_ = true;
}

void GameUi::keyboardKey(w3n::GameSession& g, const Input& in, int code) {
    if (!kb_.on) return;
    dirty_ = true;
    switch (code) {
        case kKbBack: popChar(kb_.text); return;
        case kKbShift:
            if (kb_.symbols) return;
            if (kb_.caps) kb_.caps = kb_.shift = false;
            else if (kb_.shift) { kb_.caps = true; kb_.shift = false; }
            else kb_.shift = true;
            return;
        case kKbSymbols: kb_.symbols = !kb_.symbols; kb_.shift = false; return;
        case kKbCancel: closeKeyboard(); return;
        case kKbOk: {
            TextDone cb = std::move(kb_.done);
            std::string text = kb_.text;
            closeKeyboard();
            if (cb) cb(g, in.nowMs, text);
            return;
        }
        default: break;
    }
    if (code < 32 || code > 126 || kb_.text.size() >= kb_.maxLen) return;
    char c = (char)code;
    if ((kb_.shift || kb_.caps) && !kb_.symbols && c >= 'a' && c <= 'z') c = (char)(c - 32);
    kb_.text.push_back(c);
    kb_.shift = false;   // Shift is for one letter; Caps stays
}

void GameUi::drawKeyboard(w3n::GameSession& g, const Input& in) {
    (void)g;
    (void)in;
    // the text field with a cursor, the placeholder when empty, the count of characters
    const int fy = kTop + 2, fh = 22, fw = 252;
    list_.rect(2, fy, fw, fh, kBgLight);
    list_.outline(2, fy, fw, fh, kFrame);
    if (kb_.text.empty()) list_.text(7, fy + 5, kb_.hint, kGrey, true, Font::Small);
    else list_.text(7, fy + 4, tail(kb_.text, fw - 22, Font::Normal) + "_", kText, true, Font::Normal);
    btn(2 + fw + 2, fy, 62, fh, tr("Cancel", "Отмена"), Act::KeyboardKey, kKbCancel, Style::Normal, Font::Small);

    const bool up = kb_.shift || kb_.caps;
    auto key = [&](int x, int y, int w, int h, int code, Style st = Style::Normal) {
        std::string label;
        if (code >= 32 && code <= 126) {
            char c = (char)code;
            if (up && !kb_.symbols && c >= 'a' && c <= 'z') c = (char)(c - 32);
            label.assign(1, c);
            if (c == ' ') label = "";
        }
        btn(x, y, w, h, label, Act::KeyboardKey, code, st, Font::Normal);
    };
    if (kb_.numbers) {   // a keypad: 3 x 4
        static const int kPad[4][3] = {{'1', '2', '3'}, {'4', '5', '6'}, {'7', '8', '9'}, {kKbBack, '0', kKbOk}};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) {
                const int code = kPad[r][c], x = 86 + c * 52, y = kTop + 30 + r * 34;
                if (code == kKbBack) btn(x, y, 50, 30, "<-", Act::KeyboardKey, code, Style::Normal, Font::Normal);
                else if (code == kKbOk) btn(x, y, 50, 30, "OK", Act::KeyboardKey, code, Style::Primary, Font::Normal);
                else key(x, y, 50, 30, code);
            }
        return;
    }
    static const char* kMain[3] = {"1234567890", "qwertyuiop", "asdfghjkl'"};
    static const char* kSym[3] = {"1234567890", "!@#$%^&*()", "-_=+[]{}:;"};
    const char* const* rows = kb_.symbols ? kSym : kMain;
    const int top = kTop + 28, step = 29, kh = 27;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; i < 10; ++i) key(1 + i * 32, top + r * step, 30, kh, (unsigned char)rows[r][i]);
    // row 4: Shift, seven keys, Backspace
    const int y4 = top + 3 * step;
    btn(1, y4, 46, kh, "Shift", Act::KeyboardKey, kKbShift, kb_.caps ? Style::ToggleOn : kb_.shift ? Style::ToggleOn : Style::Toggle, Font::Small);
    {
        const char* r4 = kb_.symbols ? "~|\\\"<>`" : "zxcvbnm";
        for (int i = 0; i < 7; ++i) key(49 + i * 32, y4, 30, kh, (unsigned char)r4[i]);
    }
    btn(273, y4, 46, kh, "<-", Act::KeyboardKey, kKbBack, Style::Normal, Font::Normal);
    // row 5: symbols, comma, space, period, slash, question mark, OK
    const int y5 = top + 4 * step;
    btn(1, y5, 46, kh, kb_.symbols ? "abc" : "?123", Act::KeyboardKey, kKbSymbols, kb_.symbols ? Style::ToggleOn : Style::Toggle, Font::Small);
    key(49, y5, 30, kh, ',');
    key(81, y5, 94, kh, ' ');
    key(177, y5, 30, kh, '.');
    key(209, y5, 30, kh, '/');
    key(241, y5, 30, kh, '?');
    btn(273, y5, 46, kh, "OK", Act::KeyboardKey, kKbOk, Style::Primary, Font::Normal);
}

// ------------------------------------------------------------------ chat destinations
std::string GameUi::whisperTarget(const w3n::GameSession& g) const {
    for (size_t i = g.chat.size(); i-- > 0;) {
        const w3n::ChatLine& l = g.chat[i];
        std::string who;
        if (l.type == w3n::Chat::Whisper) who = !l.name.empty() ? l.name : g.objects.nameOf(l.sender);
        else if (l.type == w3n::Chat::WhisperInform) who = !l.channel.empty() ? l.channel : l.name.empty() ? g.objects.nameOf(l.sender) : l.name;
        if (!who.empty()) return who;
    }
    return g.lastWhisperTo;
}

std::vector<std::string> GameUi::chatDests(const w3n::GameSession& g) const {
    std::vector<std::string> v = {"s", "y"};
    if (g.party.inGroup) v.push_back(g.party.raid ? "r" : "p");
    if (g.party.inGroup && g.party.raid) v.push_back("p");
    if (g.guildId()) v.push_back("g");
    if (!whisperTarget(g).empty()) v.push_back("w");
    const std::vector<std::string>& ch = g.channelSlots();
    for (size_t i = 0; i < ch.size() && i < 9; ++i)
        if (!ch[i].empty()) v.push_back("c" + std::to_string(i + 1));
    return v;
}

std::string GameUi::chatDestLabel(const w3n::GameSession& g, const std::string& d) const {
    if (d == "s") return tr("Say", "Сказать");
    if (d == "y") return tr("Yell", "Крик");
    if (d == "p") return tr("Party", "Группа");
    if (d == "r") return tr("Raid", "Рейд");
    if (d == "g") return tr("Guild", "Гильдия");
    if (d == "w") { std::string n = whisperTarget(g); return (n.empty() ? std::string(tr("Whisper", "Шёпот")) : std::string("-> ") + n); }
    if (d.size() >= 2 && d[0] == 'c') {
        size_t n = (size_t)atoi(d.c_str() + 1);
        const std::vector<std::string>& ch = g.channelSlots();
        if (n >= 1 && n <= ch.size() && !ch[n - 1].empty()) {
            std::string name = ch[n - 1];
            size_t dash = name.find(" - ");
            if (dash != std::string::npos) name.resize(dash);   // "General - Elwynn Forest" -> "General"
            return std::to_string(n) + ". " + name;
        }
    }
    return tr("Say", "Сказать");
}

std::string GameUi::chatDestPrefix(const w3n::GameSession& g, const std::string& d) const {
    if (d == "y") return "/y ";
    if (d == "p") return "/p ";
    if (d == "r") return "/raid ";
    if (d == "g") return "/g ";
    if (d == "w") { std::string n = whisperTarget(g); return n.empty() ? std::string("/s ") : "/w " + n + " "; }
    if (d.size() >= 2 && d[0] == 'c') {
        size_t n = (size_t)atoi(d.c_str() + 1);
        if (n >= 1 && n <= g.channelSlots().size() && !g.channelSlots()[n - 1].empty()) return "/" + std::to_string(n) + " ";
    }
    return "/s ";
}
