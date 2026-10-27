// ui_style.hpp — the interface's palette (dark gold, after the original but with more contrast) and its two
// languages (UI stage 7): tr("Bags", "Сумки") picks by PadConfig::lang; texts the network layer writes (English) are
// translated where they are common (trText).
#pragma once
#include <stdint.h>

#include <string>

#include "hud.hpp"

namespace ui {

extern int g_lang;   // 0 English, 1 Russian
inline const char* tr(const char* en, const char* ru) { return g_lang ? ru : en; }
std::string trText(const std::string& en);   // known server / session lines in the interface language

// palette
constexpr uint32_t kBg = hudRgba(14, 11, 8), kBgLight = hudRgba(30, 24, 17), kPanel = hudRgba(22, 18, 13, 240);
constexpr uint32_t kRow = hudRgba(36, 29, 20, 220), kRowAlt = hudRgba(28, 23, 16, 220), kRowSel = hudRgba(78, 60, 22, 240);
constexpr uint32_t kFrame = hudRgba(176, 140, 64), kFrameDim = hudRgba(96, 78, 40);
constexpr uint32_t kGold = hudRgba(255, 209, 0), kText = hudRgba(238, 232, 214), kGrey = hudRgba(150, 146, 136);
constexpr uint32_t kRed = hudRgba(255, 84, 70), kGreen = hudRgba(96, 230, 96), kBlue = hudRgba(110, 170, 255);
constexpr uint32_t kCyan = hudRgba(110, 220, 255), kOrange = hudRgba(255, 140, 40), kYellow = hudRgba(255, 230, 60);
constexpr uint32_t kWhite = hudRgba(255, 255, 255), kBlack = hudRgba(0, 0, 0);

uint32_t qualityColor(uint32_t q);          // grey, white, green, blue, purple, orange
uint32_t classColor(uint32_t cls);
uint32_t powerColor(uint8_t type);
const char* className(uint32_t cls);

}  // namespace ui
