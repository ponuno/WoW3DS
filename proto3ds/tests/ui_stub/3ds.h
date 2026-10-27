// PC stand-in for the parts of libctru that game_ui.cpp uses (keys, console, software keyboard).
#pragma once
#include <cstdint>
#include <cstdio>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
enum {
    KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3, KEY_DRIGHT = 1 << 4, KEY_DLEFT = 1 << 5,
    KEY_DUP = 1 << 6, KEY_DDOWN = 1 << 7, KEY_R = 1 << 8, KEY_L = 1 << 9, KEY_X = 1 << 10, KEY_Y = 1 << 11,
    KEY_ZL = 1 << 14, KEY_ZR = 1 << 15, KEY_TOUCH = 1 << 20,
};
inline void consoleClear() {}   // the test draws every page from scratch
#include <cstring>
#include <string>
typedef struct { int type; } SwkbdState;
typedef enum { SWKBD_BUTTON_LEFT = 0, SWKBD_BUTTON_MIDDLE, SWKBD_BUTTON_RIGHT, SWKBD_BUTTON_NONE } SwkbdButton;
enum { SWKBD_TYPE_NORMAL = 0, SWKBD_TYPE_QWERTY, SWKBD_TYPE_NUMPAD, SWKBD_TYPE_WESTERN };
// The test types into the "keyboard": the next text it returns (empty = the user cancels).
inline std::string& stubKeyboardText() { static std::string s; return s; }
inline int& stubKeyboardCalls() { static int n = 0; return n; }
inline void swkbdInit(SwkbdState* s, int type, int, int) { s->type = type; }
inline void swkbdSetHintText(SwkbdState*, const char*) {}
inline void swkbdSetInitialText(SwkbdState*, const char*) {}
inline SwkbdButton swkbdInputText(SwkbdState*, char* buf, size_t n) {
    ++stubKeyboardCalls();
    std::string& t = stubKeyboardText();
    if (t.empty()) { buf[0] = 0; return SWKBD_BUTTON_LEFT; }
    snprintf(buf, n, "%s", t.c_str());
    t.clear();
    return SWKBD_BUTTON_RIGHT;
}
