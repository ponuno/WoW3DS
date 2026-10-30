#include <string.h>
#include "hud_game.hpp"

#include <ctype.h>
#include <math.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "charsheet.hpp"
#include "text.hpp"
#include "ui_style.hpp"

namespace {

using w3n::ObjType;
using w3n::WorldObject;

constexpr uint32_t kWhite = hudRgba(255, 255, 255), kGold = hudRgba(255, 209, 0), kGrey = hudRgba(160, 160, 160);
constexpr uint32_t kPanel = hudRgba(8, 8, 16, 150), kBorder = hudRgba(120, 100, 40, 255);

std::string ascii(const std::string& s) { return s; }   // UI stage 1: UTF-8 fonts


// Name colour like the original's reaction colours (stage 5.12: by faction; without factions.txt NPCs with functions
// are green and other creatures "neutral" yellow): hostile red, unfriendly orange, neutral yellow, friendly green;
// players of our side blue, enemy players red.
uint32_t unitColor(const w3n::GameSession& g, const WorldObject& o) {
    if (o.dead()) return hudRgba(150, 150, 150);
    if (g.duel.fighting && o.guid == g.duel.opponent) return hudRgba(255, 70, 60);   // UI stage 9: our duel opponent
    const w3n::Rank r = g.shownReaction(o);
    if (o.type == ObjType::Player) return (uint8_t)r <= (uint8_t)w3n::Rank::Hostile ? hudRgba(255, 70, 60) : hudRgba(110, 170, 255);
    if ((uint8_t)r <= (uint8_t)w3n::Rank::Hostile) return hudRgba(255, 60, 50);
    if (r == w3n::Rank::Unfriendly) return hudRgba(255, 130, 40);
    if (r == w3n::Rank::Neutral) return hudRgba(255, 210, 40);
    return hudRgba(80, 230, 80);
}

uint32_t powerColor(uint8_t type) {
    switch (type) {
        case 1: return hudRgba(215, 40, 40);     // rage
        case 2: return hudRgba(215, 130, 40);    // focus
        case 3: return hudRgba(235, 225, 40);    // energy
        default: return hudRgba(40, 80, 235);    // mana
    }
}

std::string num(uint32_t v) { return std::to_string(v); }


void panel(HudList& l, int x, int y, int w, int h) {
    l.rect(x, y, w, h, kPanel);
    l.outline(x, y, w, h, kBorder);
}

struct Power { uint8_t type; uint32_t cur, max; };
Power powerOf(const WorldObject& o) {
    Power p{0, 0, 0};
    p.type = (uint8_t)((o.field(w3n::UF::UNIT_BYTES_0) >> 24) & 0xFF);
    if (p.type > 3) p.type = 0;
    p.cur = o.field(w3n::UF::UNIT_POWER1 + p.type);
    p.max = o.field(w3n::UF::UNIT_MAXPOWER1 + p.type);
    if (p.type == 1) { p.cur /= 10; p.max /= 10; }
    return p;
}

// Buffs / debuffs (stage 5.16) as small tiles: initials of the spell name, gold border for buffs, red for debuffs,
// stacks in the corner, seconds left (ours only) under the tile.
std::string auraInitials(const std::string& name) {
    std::string out;
    size_t i = 0;
    while (i < name.size() && out.size() < 2) {
        while (i < name.size() && !isalnum((unsigned char)name[i])) ++i;
        size_t j = i;
        while (j < name.size() && isalnum((unsigned char)name[j])) ++j;
        std::string w = name.substr(i, j - i);
        if (!w.empty() && w != "of" && w != "the" && w != "and") out += (char)toupper((unsigned char)w[0]);
        i = j;
    }
    if (out.size() == 1) {   // one word: two letters of it
        for (char c : name) if (isalpha((unsigned char)c) && out.size() < 2 && (char)toupper((unsigned char)c) != out[0]) { out += (char)tolower((unsigned char)c); break; }
    }
    return out.empty() ? "?" : out;
}


// ---------------------------------------------------------------- unit frames (UI stage 8)
// The original's frame art (Interface\TargetingFrame\UI-TargetingFrame, skin sprite "unitframe": the 232 x 100 pixels from
// u = 24 of the 256 x 128 texture) is laid out the target's way round - portrait on the right, name and bars on the left;
// the player's frame is its mirror. Positions below are pixels of that texture (PlayerFrame.xml / TargetFrame.xml), drawn
// at kFrameScale. Without the sprite (the stub skin) the same layout is drawn with flat shapes.
constexpr float kFrameScale = 0.55f;   // (0.72 was too big on the 3DS screen)
constexpr int kPlayerFrameX = -9, kTargetFrameX = 124, kFrameY = -3;
constexpr float kPortraitCx = 182, kPortraitCy = 44, kPortraitR = 31;   // the round hole of the portrait
constexpr float kLevelCx = 203, kLevelCy = 66.5f;                        // the level's small circle
constexpr float kBarX0 = 31, kBarX1 = 150;                               // name background and bars
constexpr float kNameY0 = 22, kHealthY0 = 41, kHealthY1 = 52, kManaY1 = 64;

struct FrameGeo {
    int x0, y0;
    bool mirror;
    float x(float tx) const { return mirror ? x0 + (256.0f - tx) * kFrameScale : x0 + (tx - 24.0f) * kFrameScale; }
    float y(float ty) const { return y0 + ty * kFrameScale; }
    // a rectangle of texture pixels -> screen (x, y, w, h)
    void rect(float tx0, float ty0, float tx1, float ty1, int* rx, int* ry, int* rw, int* rh) const {
        const float a = x(tx0), b = x(tx1);
        *rx = (int)lroundf(std::min(a, b));
        *ry = (int)lroundf(y(ty0));
        *rw = (int)lroundf(std::max(a, b)) - *rx;
        *rh = (int)lroundf(y(ty1)) - *ry;
    }
};

// A filled circle of 1-pixel rows; with a picture, the picture cut round (the portrait), mirrored when flipU.
void disc(HudList& l, float cx, float cy, float r, uint32_t color, const ui::Image* im = nullptr, bool flipU = false) {
    for (int y = (int)floorf(cy - r); y < (int)ceilf(cy + r); ++y) {
        const float yc = (float)y + 0.5f - cy;
        if (fabsf(yc) >= r) continue;
        const float hw = sqrtf(r * r - yc * yc);
        const int x0 = (int)lroundf(cx - hw), x1 = (int)lroundf(cx + hw);
        if (x1 <= x0) continue;
        if (!im) { l.rect(x0, y, x1 - x0, 1, color); continue; }
        float u0 = ((float)x0 - (cx - r)) / (2 * r), u1 = ((float)x1 - (cx - r)) / (2 * r);
        const float v0 = ((float)y - (cy - r)) / (2 * r), v1 = ((float)y + 1 - (cy - r)) / (2 * r);
        if (flipU) { u0 = 1 - u0; u1 = 1 - u1; }
        l.imagePart(x0, y, x1 - x0, 1, *im, u0, v0, u1, v1, color);
    }
}

// The level's colour against ours, like the original's GetDifficultyColor (1.12): red 5+ above, orange 3+, yellow within 2,
// green above the grey level, grey (no experience) below it.
uint32_t difficultyColor(uint32_t level, uint32_t mine) {
    const int d = (int)level - (int)mine;
    if (d >= 5) return hudRgba(255, 26, 26);
    if (d >= 3) return hudRgba(255, 128, 64);
    if (d >= -2) return hudRgba(255, 209, 0);
    int grey = mine <= 5 ? 0 : mine <= 39 ? (int)mine - 5 - (int)mine / 10 : mine <= 59 ? (int)mine - 1 - (int)mine / 5 : (int)mine - 9;
    if ((int)level > grey) return hudRgba(64, 191, 64);
    return hudRgba(128, 128, 128);
}

// The name background's colour (the original's UnitSelectionColor): hostile red, unfriendly orange, neutral yellow,
// friendly green; players of our side blue; units tapped by someone else grey.
uint32_t selectionColor(const w3n::GameSession& g, const WorldObject& o) {
    const uint32_t dyn = o.field(w3n::UF::UNIT_DYNAMIC_FLAGS);
    if (o.type == ObjType::Unit && (dyn & 0x4) && !(dyn & 0x8)) return hudRgba(128, 128, 128);   // TAPPED, not by us
    if (g.duel.fighting && o.guid == g.duel.opponent) return hudRgba(255, 0, 0);
    const w3n::Rank r = g.shownReaction(o);
    if (o.type == ObjType::Player) return (uint8_t)r <= (uint8_t)w3n::Rank::Hostile ? hudRgba(255, 0, 0) : hudRgba(0, 0, 255);
    if ((uint8_t)r <= (uint8_t)w3n::Rank::Hostile) return hudRgba(255, 0, 0);
    if (r == w3n::Rank::Unfriendly) return hudRgba(255, 128, 0);
    if (r == w3n::Rank::Neutral) return hudRgba(255, 255, 0);
    return hudRgba(0, 255, 0);
}

void unitFrame(HudList& l, const HudInput& in, const WorldObject& u, bool player, int x0, int y0) {
    const w3n::GameSession& g = *in.game;
    const uint32_t now = in.nowMs;
    const FrameGeo fg{x0, y0, player};
    const WorldObject* me = g.objects.find(g.objects.self());
    const uint32_t dyn = u.field(w3n::UF::UNIT_DYNAMIC_FLAGS);
    const bool tappedByOther = !player && u.type == ObjType::Unit && (dyn & 0x4) && !(dyn & 0x8);
    const bool greyed = u.dead() || tappedByOther;
    const bool combat = player && g.inCombat();
    const bool resting = player && (u.field(w3n::UF::PLAYER_FLAGS) & 0x20) != 0;
    // which frame art: elite (and bosses) with the golden dragon, rare with the silver one
    const char* artName = "unitframe";
    if (!player && u.type == ObjType::Unit) {
        if (u.rank == 1 || u.rank == 3) artName = "unitframe_elite";
        else if (u.rank == 2) artName = "unitframe_rareelite";
        else if (u.rank == 4) artName = "unitframe_rare";
    }
    ui::Image art = ui::assets().sprite(artName);
    if (!art.ok() && u.rank == 2) art = ui::assets().sprite("unitframe_elite");
    if (!art.ok() && strcmp(artName, "unitframe") != 0) art = ui::assets().sprite("unitframe");
    const bool flat = !art.ok();
    const float pcx = fg.x(kPortraitCx), pcy = fg.y(kPortraitCy), pr = kPortraitR * kFrameScale;
    int bx, by, bw, bh;

    // the status glow behind the frame: red pulsing in combat, golden while resting (the original's UI-Player-Status)
    if (combat || resting) {
        const int a = combat ? 70 + (int)(50 * (0.5f + 0.5f * sinf((float)now * 0.006f))) : 45;
        const uint32_t c = combat ? hudRgba(255, 30, 20, a) : hudRgba(255, 210, 60, a);
        disc(l, pcx, pcy, pr + 4, c);
        fg.rect(kBarX0 - 3, kNameY0 - 2, kBarX1 + 2, kManaY1 + 3, &bx, &by, &bw, &bh);
        l.rect(bx, by, bw, bh, c);
    }
    // dark backing of the name and bars area (the original's PlayerFrameBackground, black at half)
    fg.rect(kBarX0, kNameY0, kBarX1, kManaY1, &bx, &by, &bw, &bh);
    l.rect(bx, by, bw, bh, hudRgba(0, 0, 0, 128));
    if (!player) {   // the target's name on its reaction colour
        fg.rect(kBarX0, kNameY0, kBarX1, kHealthY0, &bx, &by, &bw, &bh);
        l.rect(bx, by, bw, bh, hudAlpha(selectionColor(g, u), 150));
    }
    // ---- portrait: the rendered head (portrait.cpp), else the race's icon (players) or the name's initial
    if (flat) disc(l, pcx, pcy, pr + 3, hudRgba(150, 120, 50));
    disc(l, pcx, pcy, pr, hudRgba(10, 10, 14));
    const uint32_t tint = greyed ? hudRgba(110, 110, 110) : hudRgba(255, 255, 255);
    const ui::Image& pic = in.portrait[player ? 0 : 1];
    if (pic.ok()) {
        disc(l, pcx, pcy, pr, tint, &pic, player);   // ours looks towards our bars, like the target's towards its
    } else {
        ui::Image icon;
        if (u.type == ObjType::Player) {
            const uint32_t b0 = u.field(w3n::UF::UNIT_BYTES_0);
            const uint32_t race = b0 & 0xFF, gender = (b0 >> 16) & 0xFF;
            icon = ui::assets().namedIcon(("race_" + std::to_string(race * 2 + (gender & 1))).c_str());
        }
        if (icon.ok()) {
            disc(l, pcx, pcy, pr * 0.86f, tint, &icon);
        } else {
            std::string nm = u.self ? std::string(in.selfName) : u.name;
            std::string ini;
            size_t i = 0;
            if (!nm.empty()) { ui::nextCodepoint(nm, i); ini = nm.substr(0, i); }
            if (ini.empty()) ini = "?";
            const int lh = HudList::lineHeight(ui::Font::Large);
            l.textCentered((int)pcx, (int)(pcy - lh * 0.5f), ini, greyed ? kGrey : kGold, true, ui::Font::Large);
        }
    }
    // ---- bars: health (green, grey when dead / tapped by someone else) and the power (mana, rage, energy)
    const uint32_t hp = u.health(), hpMax = u.maxHealth();
    fg.rect(kBarX0, kHealthY0, kBarX1, kHealthY1, &bx, &by, &bw, &bh);
    l.bar(bx, by, bw, bh, hpMax ? (float)hp / (float)hpMax : 0, greyed ? hudRgba(128, 128, 128) : hudRgba(0, 220, 0));
    char b[40];
    const w3n::Life life = player ? g.life() : w3n::Life::Alive;
    if (life == w3n::Life::Ghost) snprintf(b, sizeof b, "%s", ui::tr("Ghost", "Призрак"));
    else if (u.dead()) snprintf(b, sizeof b, "%s", ui::tr("Dead", "Мёртв"));
    else if (player || u.type == ObjType::Player || g.inParty(u.guid)) snprintf(b, sizeof b, "%u / %u", (unsigned)hp, (unsigned)hpMax);
    else snprintf(b, sizeof b, "%u%%", (unsigned)(hpMax ? ((uint64_t)hp * 100 + hpMax / 2) / hpMax : 0));
    const int th = HudList::lineHeight();
    l.textCentered(bx + bw / 2, by + bh / 2 - th / 2, b, kWhite);
    Power pw = powerOf(u);
    fg.rect(kBarX0, kHealthY1 + 1, kBarX1, kManaY1, &bx, &by, &bw, &bh);
    if (pw.max) {
        l.bar(bx, by, bw, bh, (float)pw.cur / (float)pw.max, powerColor(pw.type));
        if (player || u.type == ObjType::Player) {
            snprintf(b, sizeof b, "%u / %u", (unsigned)pw.cur, (unsigned)pw.max);
            l.textCentered(bx + bw / 2, by + bh / 2 - th / 2, b, kWhite);
        }
    } else {
        l.rect(bx, by, bw, bh, hudRgba(0, 0, 0, 120));
    }
    // ---- the art over it all (portrait ring, the level's circle, the borders of the bars), or flat borders
    if (!flat) {
        const float w = 232.0f * kFrameScale, h = 100.0f * kFrameScale;
        const int ax = player ? x0 : x0, ay = y0;
        if (player) l.imagePart(ax, ay, (int)lroundf(w), (int)lroundf(h), art, 1, 0, 0, 1);   // mirrored
        else l.image(ax, ay, (int)lroundf(w), (int)lroundf(h), art);
    } else {
        fg.rect(kBarX0 - 1, kNameY0 - 1, kBarX1 + 1, kManaY1 + 1, &bx, &by, &bw, &bh);
        l.outline(bx, by, bw, bh, kBorder);
        if (u.rank && !player) {   // elite / rare without the art: a second ring, gold or silver
            const uint32_t rc = u.rank == 4 ? hudRgba(200, 200, 210) : hudRgba(255, 200, 40);
            for (int k = 0; k < 2; ++k) {
                const float rr = pr + 5 + k;
                for (int a = 0; a < 48; ++a) {
                    const float t = (float)a / 48.0f * 6.2831853f;
                    l.rect((int)(pcx + cosf(t) * rr), (int)(pcy + sinf(t) * rr), 1, 1, rc);
                }
            }
        }
        disc(l, fg.x(kLevelCx), fg.y(kLevelCy), 8.0f, hudRgba(150, 120, 50));
        disc(l, fg.x(kLevelCx), fg.y(kLevelCy), 6.5f, hudRgba(10, 10, 14));
    }
    // ---- name, level (or the combat / resting icon), party leader
    {
        const std::string nm = u.self ? std::string(in.selfName) : u.name.empty() ? std::string("...") : u.name;
        fg.rect(kBarX0, kNameY0, kBarX1, kHealthY0, &bx, &by, &bw, &bh);
        l.textCentered(bx + bw / 2, by + bh / 2 - th / 2, HudList::fit(nm, bw - 4), kGold);
    }
    const int lx = (int)lroundf(fg.x(kLevelCx)), ly = (int)lroundf(fg.y(kLevelCy));
    ui::Image state = combat ? ui::assets().sprite("state_combat") : resting ? ui::assets().sprite("state_rest") : ui::Image();
    if (state.ok()) {
        const int sz = (int)lroundf(31 * kFrameScale);
        l.image(lx - sz / 2, ly - sz / 2, sz, sz, state);
    } else {
        std::string lv = u.level() ? std::to_string(u.level()) : "??";
        uint32_t lc = kGold;
        bool skull = false;
        if (!player && me && u.level() && !u.self && g.canAttack(u)) {
            lc = difficultyColor(u.level(), me->level());
            skull = u.rank == 3 || u.level() >= me->level() + 10;
        }
        if (!player && !u.level()) skull = true;
        ui::Image sk = skull ? ui::assets().sprite("skull") : ui::Image();
        if (sk.ok()) l.image(lx - 6, ly - 6, 12, 12, sk);
        else {
            if (skull) { lv = "??"; lc = hudRgba(255, 26, 26); }
            if (combat) lc = hudRgba(255, 60, 50);
            else if (resting) lc = hudRgba(120, 200, 255);
            l.textCentered(lx, ly - th / 2, lv, lc);
        }
    }
    if (u.type == ObjType::Player && g.party.inGroup && g.party.leader == u.guid) {
        const int ix = (int)lroundf(fg.x(player ? 210.0f : 194.0f)), iy = (int)lroundf(fg.y(10));
        ui::Image crown = ui::assets().sprite("leader");
        if (crown.ok()) l.image(ix, iy, 12, 12, crown);
        else { disc(l, ix + 6.0f, iy + 6.0f, 5.5f, hudRgba(150, 120, 50)); l.textCentered(ix + 6, iy, "L", kWhite, false); }
    }
}

// ---------------------------------------------------------------- the pet's frame (UI stage 13)
// A small frame under the player's: name, level, health, power, the pet's mood (hunter pets: a green / yellow / red dot) and
// a mark for what it was told (attack / stay / passive). The original draws a portrait; the pet has no rendered one here.
void petFrame(HudList& l, const HudInput& in, const WorldObject& u, int x, int y) {
    const w3n::GameSession& g = *in.game;
    const int w = 118, h = 24;
    const bool dead = u.dead();
    l.rect(x, y, w, h, hudRgba(0, 0, 0, 140));
    if (g.target() == u.guid) l.outline(x - 1, y - 1, w + 2, h + 2, kGold);
    else l.outline(x - 1, y - 1, w + 2, h + 2, kBorder);
    std::string nm = g.petName();
    if (nm.empty()) nm = "...";
    const std::string lv = u.level() ? "L" + std::to_string(u.level()) : "";
    l.text(x + 3, y + 1, HudList::fit(nm, w - 6 - HudList::width(lv) - 12), dead ? kGrey : kGold);
    if (!lv.empty()) l.textRight(x + w - 14, y + 1, lv, kGrey);
    const uint32_t hp = u.health(), hpMax = u.maxHealth();
    l.bar(x + 3, y + 12, w - 6, 6, hpMax ? (float)hp / (float)hpMax : 0, dead ? hudRgba(128, 128, 128) : hudRgba(0, 220, 0));
    Power pw = powerOf(u);
    if (pw.max) l.bar(x + 3, y + 19, w - 6, 3, (float)pw.cur / (float)pw.max, powerColor(pw.type));
    // mood: 1 unhappy, 2 content, 3 happy (hunter pets only)
    const int mood = g.petHappiness();
    if (mood) {
        const uint32_t c = mood == 3 ? hudRgba(60, 230, 60) : mood == 2 ? hudRgba(255, 220, 40) : hudRgba(240, 50, 40);
        disc(l, (float)(x + w - 7), (float)(y + 6), 4.0f, hudRgba(0, 0, 0));
        disc(l, (float)(x + w - 7), (float)(y + 6), 3.0f, c);
    }
    // what it was told: an arrow-less text mark under the frame
    if (g.pet.active) {
        const char* mark = nullptr;
        if (!g.pet.enabled) mark = ui::tr("away", "нет");
        else if (g.pet.react == w3n::kPetPassive) mark = ui::tr("passive", "пассив.");
        else if (g.pet.command == w3n::kPetStay) mark = ui::tr("stay", "стоять");
        else if (g.pet.command == w3n::kPetAttack) mark = ui::tr("attack", "атака");
        if (mark) l.textRight(x + w, y + h + 1, mark, kGrey);
    }
}

// ---------------------------------------------------------------- cooldowns (UI stage 12)
// The original's clock: the time still to wait is a dark sector that shrinks clockwise from 12 o'clock; drawn as runs of
// 1-pixel rows (only quads here), over a circle (ConsolePort's round cells) or the square cell.
void cdSweep(HudList& l, int x, int y, int size, float frac, uint32_t color, bool round) {
    if (frac <= 0.0f) return;
    frac = std::min(frac, 1.0f);
    const float cx = x + size * 0.5f, cy = y + size * 0.5f, r = size * 0.5f, start = (1.0f - frac) * 6.2831853f;
    for (int py = y; py < y + size; ++py) {
        int runStart = -1;
        for (int px = x; px <= x + size; ++px) {
            bool dark = false;
            if (px < x + size) {
                const float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
                if (!round || dx * dx + dy * dy <= r * r) {
                    float a = atan2f(dx, -dy);   // clockwise from straight up
                    if (a < 0) a += 6.2831853f;
                    dark = a >= start;
                }
            }
            if (dark && runStart < 0) runStart = px;
            if (!dark && runStart >= 0) { l.rect(runStart, py, px - runStart, 1, color); runStart = -1; }
        }
    }
}

// Time left as the cells show it: "1h", "3m", "12", "4" (seconds rounded up)
std::string cdText(uint32_t ms) {
    const unsigned s = (ms + 999) / 1000;
    if (s >= 3600) return std::to_string((s + 1799) / 3600) + "h";
    if (s >= 60) return std::to_string((s + 29) / 60) + "m";
    return std::to_string(s);
}

// One action cell's cooldown: the GCD as a light sweep, its own cooldown as a dark one with the time in big digits
// (red in the last 5 seconds, OmniCC-like), and a short bright flash when a cooldown of 2 s or more is over.
void cellCooldown(HudList& l, int x, int y, int cs, const HudBarCell& c, bool round, uint32_t now) {
    static std::unordered_map<uint32_t, uint32_t> lastCd, flashAt, fullSeen;
    if (!c.cdKey) return;
    uint32_t& full = fullSeen[c.cdKey];
    if (c.cdMs) full = std::max(full, c.cdMs);
    const uint32_t total = c.cdTotalMs ? std::max(c.cdTotalMs, c.cdMs) : full;
    if (c.cdMs) {
        cdSweep(l, x, y, cs, total ? (float)c.cdMs / (float)total : 1.0f, hudRgba(0, 0, 0, 175), round);
        if (c.cdMs >= 1000) {
            const std::string t = cdText(c.cdMs);
            const ui::Font f = t.size() <= 2 ? ui::Font::Normal : ui::Font::Small;
            const uint32_t col = c.cdMs <= 5000 ? hudRgba(255, 80, 60) : c.cdMs < 60000 ? hudRgba(255, 230, 90) : kWhite;
            l.textCentered(x + cs / 2, y + cs / 2 - HudList::lineHeight(f) / 2, t, col, true, f);
        }
    } else if (c.gcdMs) {
        cdSweep(l, x, y, cs, (float)c.gcdMs / (float)w3n::kGlobalCooldownMs, hudRgba(0, 0, 0, 110), round);
    }
    uint32_t& last = lastCd[c.cdKey];
    if (last >= 2000 && c.cdMs == 0) flashAt[c.cdKey] = now ? now : 1;   // just became ready
    if (!c.cdMs) full = 0;
    last = c.cdMs;
    auto fl = flashAt.find(c.cdKey);
    if (fl != flashAt.end()) {
        const uint32_t age = now - fl->second;
        if (age > 450) flashAt.erase(fl);
        else {
            const int a = 230 - (int)age * 230 / 450, grow = (int)age * 5 / 450;
            if (round) disc(l, x + cs * 0.5f, y + cs * 0.5f, cs * 0.5f, hudRgba(255, 255, 220, a / 2));
            else l.rect(x, y, cs, cs, hudRgba(255, 255, 220, a / 2));
            l.outline(x - 1 - grow, y - 1 - grow, cs + 2 + 2 * grow, cs + 2 + 2 * grow, hudRgba(255, 240, 160, a));
        }
    }
}

}  // namespace

bool hudProject(const HudInput& in, const float w[3], float* sx, float* sy) {
    float cp = cosf(in.camPitch), sp = sinf(in.camPitch), cy = cosf(in.camYaw), sy_ = sinf(in.camYaw);
    float fwd[3] = {cp * cy, cp * sy_, sp};
    float right[3] = {sy_, -cy, 0.0f};
    float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                   right[0] * fwd[1] - right[1] * fwd[0]};
    float d[3] = {w[0] - in.camPos[0], w[1] - in.camPos[1], w[2] - in.camPos[2]};
    float zc = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
    if (zc < 0.6f) return false;
    float xc = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
    float yc = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    const float t = 0.57735027f;   // tan(30 deg): 60 degree vertical field of view
    *sx = kHudW * 0.5f + (xc / zc) / (t * (kHudW / (float)kHudH)) * (kHudW * 0.5f);
    *sy = kHudH * 0.5f - (yc / zc) / t * (kHudH * 0.5f);
    return true;
}

// UI stage 14: the character window, the top screen while the Equipment tab is open (like the original's character frame):
// the figure drawn from the model, the average item level of the worn gear, the stats in two columns. The slots are on the
// touch screen. (Replaces the HUD while it is shown.)
static void drawCharSheet(HudList& l, const HudInput& in) {
    using ui::tr;
    const w3n::GameSession& g = *in.game;
    const w3n::CharSheet c = w3n::buildCharSheet(g);
    l.fade = 255;
    l.rect(0, 0, kHudW, kHudH, hudRgba(10, 8, 6, 238));
    l.outline(1, 1, kHudW - 2, kHudH - 2, kBorder);
    const int lh = HudList::lineHeight() + 1;
    // title: name, level and class
    const uint32_t cls = g.classId();
    char b[96];
    l.textCentered(kHudW / 2, 5, ascii(in.selfName), kGold, true, ui::Font::Normal);
    snprintf(b, sizeof b, "%s %d %s", tr("Level", "Уровень"), c.level, ui::className(cls));
    l.textCentered(kHudW / 2, 5 + HudList::lineHeight(ui::Font::Normal), b, ui::classColor(cls));
    l.rect(8, 33, kHudW - 16, 1, hudAlpha(kBorder, 160));
    // the figure
    const int dx = 10, dy = 38, ds = 136;
    l.rect(dx - 1, dy - 1, ds + 2, ds + 2, hudRgba(0, 0, 0, 200));
    if (in.doll.ok()) l.image(dx, dy, ds, ds, in.doll);
    else l.textCentered(dx + ds / 2, dy + ds / 2 - 5, tr("loading...", "загрузка..."), kGrey);
    l.outline(dx - 1, dy - 1, ds + 2, ds + 2, hudAlpha(kBorder, 200));
    // item level under it
    const int iy = dy + ds + 6;
    l.textCentered(dx + ds / 2, iy, tr("Average item level", "Средний уровень предметов"), kGrey);
    if (c.itemLevel) snprintf(b, sizeof b, "%d", c.itemLevel);
    else snprintf(b, sizeof b, "-");
    l.textCentered(dx + ds / 2, iy + lh, b, kGold, true, ui::Font::Large);
    if (c.itemsWorn && c.itemsKnown < c.itemsWorn) {
        snprintf(b, sizeof b, "%d/%d %s", c.itemsKnown, c.itemsWorn, tr("items known", "предметов изучено"));
        l.textCentered(dx + ds / 2, iy + lh + HudList::lineHeight(ui::Font::Large), b, kGrey);
    }
    // the stats: two columns
    auto column = [&](int x, int w) {
        struct Col { int x, w, y; } col{x, w, 40};
        return col;
    };
    auto head = [&](auto& col, const char* t) {
        if (col.y > 40) col.y += 3;
        l.text(col.x, col.y, t, kGold);
        l.rect(col.x, col.y + lh - 1, col.w, 1, hudAlpha(kBorder, 120));
        col.y += lh + 1;
    };
    auto row = [&](auto& col, const char* label, const std::string& value, uint32_t vcol = kWhite) {
        l.text(col.x, col.y, label, kGrey);
        l.textRight(col.x + col.w, col.y, value, vcol);
        col.y += lh;
    };
    auto num = [](int v) { return std::to_string(v); };
    auto pct = [](float v) { char t[24]; snprintf(t, sizeof t, "%.2f%%", v); return std::string(t); };
    auto range = [](int a, int b2) { return std::to_string(a) + " - " + std::to_string(b2); };
    const uint32_t kGreen = hudRgba(30, 255, 30), kRed = hudRgba(255, 60, 60);
    const int x0 = dx + ds + 14, cw = (kHudW - 8 - x0 - 12) / 2;
    auto A = column(x0, cw), B = column(x0 + cw + 12, cw);
    head(A, tr("Attributes", "Характеристики"));
    static const char* const kStatEn[5] = {"Strength", "Agility", "Stamina", "Intellect", "Spirit"};
    static const char* const kStatRu[5] = {"Сила", "Ловкость", "Выносливость", "Интеллект", "Дух"};
    for (int i = 0; i < 5; ++i)
        row(A, tr(kStatEn[i], kStatRu[i]), num(c.stat[i]), c.statBonus[i] > 0 ? kGreen : c.statBonus[i] < 0 ? kRed : kWhite);
    row(A, tr("Armor", "Броня"), num(c.armor));
    head(A, tr("Resistance", "Сопротивление"));
    static const char* const kResEn[5] = {"Fire", "Nature", "Frost", "Shadow", "Arcane"};
    static const char* const kResRu[5] = {"Огонь", "Природа", "Лёд", "Тьма", "Тайная магия"};
    static const int kResIdx[5] = {1, 2, 3, 4, 5};
    for (int i = 0; i < 5; ++i) row(A, tr(kResEn[i], kResRu[i]), num(c.resist[kResIdx[i]]));
    head(B, tr("Melee", "Ближний бой"));
    row(B, tr("Damage", "Урон"), range(c.dmgMin, c.dmgMax));
    row(B, tr("Attack power", "Сила атаки"), num(c.attackPower));
    row(B, tr("Crit", "Крит"), pct(c.crit));
    head(B, tr("Defense", "Защита"));
    row(B, tr("Dodge", "Уклонение"), pct(c.dodge));
    row(B, tr("Parry", "Парирование"), pct(c.parry));
    row(B, tr("Block", "Блок"), pct(c.block));
    if (c.rangedAttackPower > 0 || c.rangedMax > 0) {
        head(B, tr("Ranged", "Дальний бой"));
        row(B, tr("Damage", "Урон"), range(c.rangedMin, c.rangedMax));
        row(B, tr("Attack power", "Сила атаки"), num(c.rangedAttackPower));
        row(B, tr("Crit", "Крит"), pct(c.rangedCrit));
    }
    if (c.spellPower > 0) {
        head(B, tr("Spell", "Заклинания"));
        row(B, tr("Bonus damage", "Бонус к урону"), num(c.spellPower));
    }
    snprintf(b, sizeof b, "%s %u%%", tr("Durability", "Прочность"), (unsigned)c.durabilityPct);
    l.textRight(kHudW - 12, kHudH - lh - 4, b, c.durabilityPct < 25 ? kRed : kGrey);
}

void hudBuildGame(HudList& l, const HudInput& in) {
    l.clear();
    if (in.charSheet) { drawCharSheet(l, in); return; }   // UI stage 14: the character window instead of the HUD
    const w3n::GameSession& g = *in.game;
    const uint32_t now = in.nowMs;
    const WorldObject* me = g.objects.find(g.objects.self());
    const WorldObject* tgt = g.target() ? g.objects.find(g.target()) : nullptr;
    const w3n::MovementInfo& mv = g.mover.info();

    // ---- name plates and floating numbers (behind the frames)
    {
        struct Plate { float dist; float sx, sy; const WorldObject* o; };
        std::vector<Plate> plates;
        for (auto& [guid, o] : g.objects.all()) {
            if (!o.hasPos || !o.isUnitLike()) continue;
            if (o.self && !in.showSelfPlate) continue;
            float p[3];
            o.positionAt(now, p);
            float dx = p[0] - mv.x, dy = p[1] - mv.y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > 40.0f && guid != g.target()) continue;
            float scale = o.fieldF(w3n::UF::OBJECT_SCALE_X);
            if (!(scale > 0.05f && scale < 20.0f)) scale = 1.0f;
            float head[3] = {p[0], p[1], p[2] + 2.0f * scale + 0.35f};
            float sx, sy;
            if (!hudProject(in, head, &sx, &sy)) continue;
            if (sx < -40 || sx > kHudW + 40 || sy < -10 || sy > kHudH + 10) continue;
            plates.push_back({dist, sx, sy, &o});
        }
        std::sort(plates.begin(), plates.end(), [](const Plate& a, const Plate& b) { return a.dist > b.dist; });
        if (plates.size() > 18) plates.erase(plates.begin(), plates.end() - 18);   // the nearest 18, far ones first
        for (const Plate& pl : plates) {
            const WorldObject& o = *pl.o;
            bool isT = &o == tgt;
            std::string nm = o.self ? ascii(in.selfName) : ascii(o.name);
            if (nm.empty()) continue;
            int y = (int)pl.sy - 12;
            // second line (stage 5.42): <Guild> under a player, <Subname> (profession/title) under an NPC; only for the
            // target and for units close enough to read it
            std::string sub;
            if (isT || pl.dist <= 25.0f) sub = ascii(g.unitSubtitle(o));
            const int subH = sub.empty() ? 0 : 10;
            bool hurt = o.maxHealth() && o.health() < o.maxHealth() && !o.dead();
            if (isT || hurt || o.meleeVictim) {   // health bar for the target and units in a fight
                float f = o.maxHealth() ? (float)o.health() / (float)o.maxHealth() : 0;
                l.bar((int)pl.sx - 16, y + 11 + subH, 32, 4, f, hudRgba(40, 210, 40));
            }
            // a cast in progress: a small bar over the name (the spell's name over it for the target), as the original
            // shows casts over the units' heads
            int markUp = 0;
            if (!o.self) {
                const w3n::CastState cs = g.castOf(o.guid, now);
                if (cs.active && cs.durMs) {
                    const float cf = std::min(1.0f, (float)(now - cs.startMs) / cs.durMs);
                    l.bar((int)pl.sx - 20, y - 5, 40, 3, cf, hudRgba(255, 178, 0));
                    markUp = 6;
                    if (isT) {
                        l.textCentered((int)pl.sx, y - 18, HudList::fit(g.spellNames.name(cs.spell), 110), hudRgba(255, 210, 90));
                        markUp = 17;
                    }
                }
            }
            const bool aimed = in.pad && in.pad->aim && o.guid == in.pad->aim;
            if (aimed) {   // UI stage 11: ConsolePort's targeting icon over the aimed unit, else arrows round its name
                const w3n::Rank rk = g.shownReaction(o);
                ui::Image ti = ui::assets().sprite(g.enemyLike(o) || (uint8_t)rk <= (uint8_t)w3n::Rank::Hostile ? "cp_target_enemy"
                                                   : rk == w3n::Rank::Neutral ? "cp_target_neutral" : "cp_target_friend");
                if (ti.ok()) { l.image((int)pl.sx - 9, y - 21 - markUp, 18, 18, ti); markUp += 18; }
                else nm = "> " + nm + " <";
            }
            l.textCentered((int)pl.sx, y - 1, nm, isT || aimed ? kWhite : unitColor(g, o));
            if (!sub.empty())
                l.textCentered((int)pl.sx, y + 9, HudList::fit(sub, 150), o.type == ObjType::Player ? hudRgba(120, 230, 120) : hudRgba(235, 215, 140));
            // quest giver mark (stage 5.14): yellow ! = a quest for us, yellow ? = one to hand in, grey = not yet
            if (uint32_t st = g.questGiverStatus(o.guid)) {
                const char* mark = w3n::questMarkGlyph(st);
                if (mark[0] && !o.dead()) {   // drawn twice, one pixel apart: a bold mark
                    int my = y - (in.pad && in.pad->soft == o.guid ? 23 : 12) - markUp;
                    uint32_t mc = w3n::questMarkGrey(st) ? kGrey : kGold;
                    l.textCentered((int)pl.sx, my, mark, mc);
                    l.textCentered((int)pl.sx + 1, my, mark, mc, false);
                }
            }
            if (in.pad && in.pad->soft && o.guid == in.pad->soft && !o.self)   // interact prompt
                l.textCentered((int)pl.sx, y - 12 - markUp, std::string("[Y] ") + ui::trText(in.pad->softVerb), kGold);
        }
        // a game object the interact button would use (stage 5.18): its name and the prompt
        if (in.pad && in.pad->soft) {
            const WorldObject* so = g.objects.find(in.pad->soft);
            if (so && so->type == ObjType::GameObject && so->hasPos) {
                float p[3] = {so->mv.x, so->mv.y, so->mv.z + 1.4f}, sx, sy;
                if (hudProject(in, p, &sx, &sy)) {
                    l.textCentered((int)sx, (int)sy - 12, ascii(so->name.empty() ? std::string("Object") : so->name), kGold);
                    l.textCentered((int)sx, (int)sy - 23, std::string("[Y] ") + ui::trText(in.pad->softVerb), kGold);
                }
            }
        }
        // the aim ring under the unit the C-Stick points at (L/R held)
        if (in.pad && in.pad->aim) {
            if (const WorldObject* a = g.objects.find(in.pad->aim)) {
                float p[3], sx, sy;
                a->positionAt(now, p);
                if (hudProject(in, p, &sx, &sy)) {
                    uint32_t c = in.pad->mode == 2 ? hudRgba(80, 255, 80) : hudRgba(255, 60, 60);
                    l.outline((int)sx - 12, (int)sy - 3, 24, 7, c);
                }
            }
        }
        // floating combat numbers: damage by us (yellow) over the victim, damage to us (red) over our head
        for (const w3n::MeleeHit& h : g.hits) {
            uint32_t age = now - h.ms;
            if (age > 1600) continue;
            bool mine = h.attacker == g.objects.self(), onMe = h.victim == g.objects.self();
            if (!mine && !onMe) continue;
            const WorldObject* v = g.objects.find(h.victim);
            float p[3] = {mv.x, mv.y, mv.z};
            if (v && v->hasPos && !onMe) v->positionAt(now, p);
            float head[3] = {p[0], p[1], p[2] + 2.6f};
            float sx, sy;
            if (!hudProject(in, head, &sx, &sy)) continue;
            std::string t = h.miss() ? "Miss" : h.victimState >= 2 && h.victimState <= 8 ? "Parry" : num((uint32_t)std::max<int32_t>(0, h.damage));
            if (h.crit()) t += "!";
            uint32_t col = onMe ? hudRgba(255, 60, 60) : h.crit() ? hudRgba(255, 120, 0) : hudRgba(255, 230, 40);
            int rise = (int)(age * 30 / 1000);
            if (age > 1100) col = (col & 0x00FFFFFF) | ((uint32_t)(255 - (age - 1100) * 255 / 500) << 24);
            l.textCentered((int)sx + (onMe ? -24 : 18), (int)sy - 22 - rise, t, col);
        }
    }

    // ---- player frame and target frame (UI stage 8: the original's unit frames - portrait, level, name, bars), our auras
    // as icons on the right, the target's under its frame
    const bool rest = in.dim && !g.inCombat() && !g.cast.active && !tgt;
    l.fade = rest ? 150 : 255;
    if (me) {
        unitFrame(l, in, *me, true, kPlayerFrameX, kFrameY);
        std::vector<w3n::AuraView> av = g.auras(*me, now);
        std::stable_sort(av.begin(), av.end(), [](const w3n::AuraView& a, const w3n::AuraView& b) { return a.debuff > b.debuff; });
        for (size_t i = 0; i < av.size() && i < 8; ++i) {
            const w3n::AuraView& a = av[i];
            int x = kHudW - 56 + (int)(i % 2) * 27, y = 4 + (int)(i / 2) * 31;
            ui::Image ic = ui::assets().spellIcon(a.spell);
            if (ic.ok()) l.image(x, y, 24, 24, ic);
            else {
                l.rect(x, y, 24, 24, hudRgba(20, 20, 30, 220));
                l.textCentered(x + 12, y + 7, auraInitials(g.spellNames.name(a.spell)), kWhite, false);
            }
            l.outline(x - 1, y - 1, 26, 26, a.debuff ? hudRgba(230, 40, 40) : hudRgba(200, 170, 60));
            if (a.stacks > 1) l.textRight(x + 24, y + 13, std::to_string(a.stacks), hudRgba(255, 230, 40));
            if (a.leftMs) {
                unsigned sec = (unsigned)((a.leftMs + 999) / 1000);
                std::string t = sec >= 3600 ? std::to_string(sec / 3600) + "h" : sec >= 60 ? std::to_string(sec / 60) + "m" : std::to_string(sec);
                l.textCentered(x + 12, y + 24, t, sec <= 10 ? hudRgba(255, 90, 90) : kWhite);
            }
        }
    }
    // UI stage 12: cooldowns of actions that are not on the crossbars shown now, under the player frame: icon, clock, time
    for (size_t i = 0; i < in.cooldowns.size() && i < 6; ++i) {
        const HudCooldown& hc = in.cooldowns[i];
        const int sz = 16, x = 4 + (int)i * 20, y = kFrameY + (int)(100 * kFrameScale) + 1;
        if (hc.icon.ok()) l.image(x, y, sz, sz, hc.icon);
        else { l.rect(x, y, sz, sz, hudRgba(20, 20, 30, 220)); l.textCentered(x + sz / 2, y + 3, auraInitials(hc.name), kWhite, false); }
        cdSweep(l, x, y, sz, hc.totalMs ? (float)hc.ms / (float)std::max(hc.totalMs, hc.ms) : 1.0f, hudRgba(0, 0, 0, 160), false);
        l.outline(x - 1, y - 1, sz + 2, sz + 2, kBorder);
        l.textCentered(x + sz / 2, y + sz, cdText(hc.ms), hc.ms <= 5000 ? hudRgba(255, 80, 60) : kWhite);
    }
    // UI stage 13: the pet (hunter, warlock) under the player frame
    if (const WorldObject* pu = g.petObject())
        if (pu->isUnitLike() && !pu->self) petFrame(l, in, *pu, 6, kFrameY + (int)(100 * kFrameScale) + 22);
    if (tgt) {
        unitFrame(l, in, *tgt, false, kTargetFrameX, kFrameY);
        const int tx = kTargetFrameX + 2, tw = (int)(150 * kFrameScale);
        int ay = kFrameY + (int)(84 * kFrameScale);
        // the target's cast under its frame, like the original's target spell bar: the spell's icon, a yellow bar
        // filling up, its name and the seconds left
        w3n::CastState tc = g.castOf(tgt->guid, now);
        if (tc.active && tc.durMs && !tgt->self) {
            const float cf = std::min(1.0f, (float)(now - tc.startMs) / tc.durMs);
            const int cw = (int)(208 * kFrameScale), cx = kTargetFrameX + (int)(18 * kFrameScale);
            ui::Image ic = ui::assets().spellIcon(tc.spell);
            if (ic.ok()) { l.image(cx, ay, 11, 11, ic); l.outline(cx - 1, ay - 1, 13, 13, kBorder); }
            const int bx0 = cx + 14, bw0 = cw - 14;
            l.bar(bx0, ay + 1, bw0, 9, cf, hudRgba(255, 178, 0), hudRgba(0, 0, 0, 170));
            l.outline(bx0 - 1, ay, bw0 + 2, 11, kBorder);
            char sec[16];
            snprintf(sec, sizeof sec, "%.1f", std::max(0.0f, ((float)tc.durMs - (float)(now - tc.startMs)) / 1000.0f));
            const int sw = HudList::width(sec);
            l.text(bx0 + 2, ay - 1, HudList::fit(g.spellNames.name(tc.spell), bw0 - sw - 6), kWhite);
            l.textRight(bx0 + bw0 - 2, ay - 1, sec, kWhite);
            ay += 14;
        }
        if (tgt->dead() && (tgt->field(w3n::UF::UNIT_DYNAMIC_FLAGS) & 1)) l.textCentered(tx + tw / 2, ay, ui::tr("Lootable", "Можно обыскать"), kGold);
        else if (!tgt->dead() && !tgt->self) {
            std::vector<w3n::AuraView> av = g.auras(*tgt, now);
            for (size_t i = 0; i < av.size() && i < 8; ++i) {
                int x = tx + 2 + (int)i * 18;
                ui::Image ic = ui::assets().spellIcon(av[i].spell);
                if (ic.ok()) l.image(x, ay, 16, 16, ic);
                else { l.rect(x, ay, 16, 16, hudRgba(20, 20, 30, 220)); l.text(x + 1, ay + 3, auraInitials(g.spellNames.name(av[i].spell)), kWhite, false); }
                l.outline(x - 1, ay - 1, 18, 18, av[i].debuff ? hudRgba(230, 40, 40) : hudRgba(200, 170, 60));
            }
        }
        // (the target of the target is on the touch screen, Combat tab)
    }

    // ---- quest tracker (stage 5.14): under the minimap like the original's quest watch - unfinished quests first
    if (in.questTracker) {
        std::vector<w3n::QuestLogEntry> log = g.questLog();
        std::stable_sort(log.begin(), log.end(), [](const w3n::QuestLogEntry& a, const w3n::QuestLogEntry& b) {
            return (a.complete || a.failed) < (b.complete || b.failed);
        });
        const int x = kHudW - 142, yMax = 170;
        int y = 132, shown = 0;
        for (const w3n::QuestLogEntry& e : log) {
            std::vector<std::string> lines = g.questProgressKnown(e);
            if (y + 10 * (1 + (int)std::min<size_t>(lines.size(), 3)) > yMax || shown >= 3) break;
            std::string t = ascii(g.questTitleKnown(e.id));
            while (t.size() > 1 && HudList::width(t) > 138) t.pop_back();
            l.rect(x - 2, y, HudList::width(t) + 4, 10, hudRgba(0, 0, 0, 80));   // a faint strip: readable over the sky
            l.text(x, y, t, e.failed ? hudRgba(255, 70, 70) : e.complete ? hudRgba(80, 230, 80) : kGold);
            y += 10;
            int k = 0;
            for (const std::string& line : lines) {
                if (k++ >= 3 || line == "(complete)" || line == "(failed)") continue;
                std::string tl = " - " + ascii(line);
                while (tl.size() > 1 && HudList::width(tl) > 140) tl.pop_back();
                size_t slash = line.rfind('/');
                bool done = slash != std::string::npos && line.substr(line.rfind(' ') + 1, slash - line.rfind(' ') - 1) == line.substr(slash + 1);
                l.rect(x - 2, y, HudList::width(tl) + 4, 10, hudRgba(0, 0, 0, 80));
                l.text(x, y, tl, done ? hudRgba(150, 150, 150) : kWhite);
                y += 10;
            }
            y += 3;
            ++shown;
        }
    }

    // ---- death (stage 5.10): what to do, in the middle of the screen like the original's dialog
    {
        const w3n::Life life = g.life();
        const float here[3] = {mv.x, mv.y, mv.z};
        if (life == w3n::Life::Dead) {
            panel(l, 110, 88, 180, 34);
            l.textCentered(200, 92, "You have died.", hudRgba(255, 70, 70));
            l.textCentered(200, 106, g.resurrectOffer.active ? "Resurrection offered (touch screen)" : "A: Release Spirit", kWhite);
        } else if (life == w3n::Life::Ghost) {
            char b[64];
            float d = g.corpseDistance(here);
            if (g.canReclaim(here, now)) snprintf(b, sizeof b, "Ghost - A: Resurrect");
            else if (d >= 0 && d <= 39.0f) snprintf(b, sizeof b, "Ghost - resurrect in %u s", (unsigned)((g.reclaimDelayLeftMs(now) + 999) / 1000));
            else if (d >= 0) snprintf(b, sizeof b, "Ghost - your corpse: %.0f yd", d);
            else snprintf(b, sizeof b, "Ghost");
            l.textCentered(200, 48, b, hudRgba(90, 230, 255));
        }
        if (g.spiritHealerConfirm) l.textCentered(200, 62, "Spirit healer: answer on the touch screen", kGold);
    }

    // ---- breath / fatigue / feign death (stage 5.26): the original's bars at the top of the screen, under the frames
    {
        int y = 82;
        for (int k = 0; k < w3n::kMirrorTimers; ++k) {
            const w3n::MirrorTimer& m = g.mirror[k];
            if (!m.active || !m.maxMs) continue;
            const uint32_t left = m.valueAt(now);
            if (m.scale > 0 && left >= m.maxMs) continue;   // filled back up: the server stops it soon
            static const uint32_t kCol[3] = {hudRgba(255, 230, 0), hudRgba(0, 128, 255), hudRgba(255, 178, 0)};
            const int w = 150, x = (kHudW - w) / 2;
            l.bar(x, y, w, 11, (float)left / (float)m.maxMs, kCol[k], hudRgba(0, 0, 0, 170));
            l.outline(x - 1, y - 1, w + 2, 13, kBorder);
            l.textCentered(x + w / 2, y - 1, w3n::mirrorTimerName((uint32_t)k), kWhite);
            y += 16;
        }
        if (g.fishBiting(now) && (now / 250) % 4 != 0)   // a bite: catch it with the interact button (the prompt at the bobber)
            l.textCentered(kHudW / 2, y + 4, "A fish bites!", kGold);
    }

    // ---- cast bar (bottom right: the place of the action bar and the cast bar of the original is the touch screen here)
    if (g.cast.active && g.cast.durMs) {
        uint32_t el = now - g.cast.startMs;
        float f = std::min(1.0f, (float)el / (float)g.cast.durMs);
        const int w = 180, x = (kHudW - w) / 2, y = 208;   // the crossbars sit in the corners: the centre is free
        l.bar(x, y, w, 12, f, hudRgba(255, 190, 20), hudRgba(0, 0, 0, 160));
        l.outline(x - 1, y - 1, w + 2, 14, kBorder);
        l.textCentered(x + w / 2, y, ascii(g.spellNames.name(g.cast.spell)), kWhite);
        char b[16];
        snprintf(b, sizeof b, "%.1f", std::max(0.0f, (g.cast.durMs - (float)el) / 1000.0f));
        l.textRight(x + w - 3, y, b, kWhite);
    }

    // (stage 5.11: no chat frame here any more - chat, combat and loot lines live in the Chat tab of the touch screen)

    // ---- controller bars (Forever): the set the modifiers select, D-pad diamond and face-button diamond
    if (in.pad && in.pad->show) {
        const HudPadView& pv = *in.pad;
        if (pv.mode) {   // targeting mode: what the face buttons do now instead of the bars
            const char* t1 = pv.mode == 1 ? "ENEMY: C-Stick aims, release R" : "FRIEND: C-Stick aims, release L";
            const char* t2 = pv.mode == 1 ? "B last  A assist  Y attack  X next" : "B yourself  A assist  X next";
            panel(l, 91, 172, 218, 30);
            ui::Image ti = ui::assets().sprite(pv.mode == 1 ? "cp_target_enemy" : "cp_target_friend");   // UI stage 11
            const int tx0 = ti.ok() ? 121 : 95;
            if (ti.ok()) l.image(94, 175, 24, 24, ti);
            l.text(tx0, 175, t1, pv.mode == 1 ? hudRgba(255, 90, 90) : hudRgba(110, 255, 110));
            l.text(tx0, 187, t2, kWhite);
        } else {
            // UI stage 2: crossbars in the bottom corners (the centre stays on the world) - D-pad diamond bottom left,
            // face buttons bottom right, icons 22 px,
            // the button's glyph in the corner, cooldowns as a dark sweep from the top and seconds
            static const int kDx[4] = {0, 23, 0, -23}, kDy[4] = {-19, 0, 19, 0};   // up, right, down, left
            static const char* kGlyph[8] = {"↑", "→", "↓", "←", "X", "A", "B", "Y"};
            const int cs = 22;
            // UI stage 11: with ConsolePort's art (ui.w3dp built with --consoleport) the cells are round, like its bars:
            // the icon cut round, its ring over it, the button's glyph picture in the corner
            const ui::Image ring = ui::assets().sprite("cp_ring"), ringHl = ui::assets().sprite("cp_ring_hl");
            static const char* kGlyphPic[8] = {"cp_dpad_up", "cp_dpad_right", "cp_dpad_down", "cp_dpad_left", "cp_btn_x", "cp_btn_a",
                                               "cp_btn_b", "cp_btn_y"};
            for (int b = 0; b < 8; ++b) {
                int cx = b < 4 ? 37 : kHudW - 37, cy = 199;   // (UI stage 8: up 7 px - the experience bar is under them)
                int x = cx + kDx[b % 4] - cs / 2, y = cy + kDy[b % 4] - cs / 2;
                const HudBarCell& c = pv.cells[b];
                if (ring.ok()) {
                    const float ccx = x + cs * 0.5f, ccy = y + cs * 0.5f, r = cs * 0.5f;
                    disc(l, ccx, ccy, r, c.fixed ? hudRgba(30, 30, 40, 170) : hudRgba(10, 10, 20, 200));
                    if (c.icon.ok()) disc(l, ccx, ccy, r - 0.5f, 0xFFFFFFFFu, &c.icon);
                    else if (!c.name.empty()) {
                        std::string nm = c.fixed ? ascii(c.name) : auraInitials(c.name);
                        l.textCentered(x + cs / 2, y + 6, HudList::fit(nm, cs), c.fixed ? kGrey : kWhite, false);
                    }
                    cellCooldown(l, x, y, cs, c, true, now);   // UI stage 12: the clock sweep, the time, the flash
                    if (c.count) l.textRight(x + cs, y + cs - 10, std::to_string(c.count), kWhite);
                    const int rs = cs + 8;
                    const bool lit = c.active || pv.binding;
                    l.image(x - 4, y - 4, rs, rs, lit && ringHl.ok() ? ringHl : ring,
                            c.active ? hudRgba(255, 210, 60) : pv.binding ? hudRgba(90, 230, 255) : hudRgba(255, 255, 255));
                    ui::Image glyph = ui::assets().sprite(kGlyphPic[b]);
                    if (glyph.ok()) l.image(x - 5, y - 5, 11, 11, glyph);
                    else l.text(x + 1, y - 2, kGlyph[b], kGold);
                    continue;
                }
                l.rect(x, y, cs, cs, c.fixed ? hudRgba(30, 30, 40, 160) : hudRgba(10, 10, 20, 190));
                if (c.icon.ok()) l.image(x, y, cs, cs, c.icon);
                else if (!c.name.empty()) {
                    std::string nm = c.fixed ? ascii(c.name) : auraInitials(c.name);
                    l.textCentered(x + cs / 2, y + 6, HudList::fit(nm, cs), c.fixed ? kGrey : kWhite, false);
                }
                cellCooldown(l, x, y, cs, c, false, now);   // UI stage 12: the clock sweep, the time, the flash
                if (c.count) l.textRight(x + cs, y + cs - 10, std::to_string(c.count), kWhite);
                l.outline(x - 1, y - 1, cs + 2, cs + 2, c.active ? hudRgba(255, 210, 40) : pv.binding ? hudRgba(90, 230, 255) : kBorder);
                l.text(x + 1, y - 2, kGlyph[b], kGold);
            }
            for (int k = 0; k < 2; ++k) {   // ZL / ZR lit when their set (or both) is on
                bool on = (pv.set & (1 << k)) != 0;
                int x = k ? kHudW - 28 : 4, y = 155;   // above its cluster: ZL over the D-pad, ZR over the face buttons
                l.rect(x, y, 24, 11, on ? hudRgba(120, 90, 20, 220) : hudRgba(0, 0, 0, 120));
                l.textCentered(x + 12, y, k ? "ZR" : "ZL", on ? kGold : kGrey, false);
            }
            char t[16];
            snprintf(t, sizeof t, "p%d", pv.page + 1);
            l.text(32, 155, t, kGrey);
        }
    }
    // ---- UI stage 8: the experience bar along the bottom edge, like the original's above its action bar: purple, blue
    // while rested, the rested experience ahead of it lighter up to the tick, 20 segments (no numbers: the bar says enough)
    static uint32_t levelSeen = 0, levelUpMs = 0, levelUpTo = 0;
    static uint64_t xpOf = 0;
    if (g.objects.self() != xpOf) { xpOf = g.objects.self(); levelSeen = levelUpMs = levelUpTo = 0; }
    if (me) {
        const uint32_t xp = me->field(w3n::UF::PLAYER_XP), next = me->field(w3n::UF::PLAYER_NEXT_LEVEL_XP);
        const uint32_t rested = me->field(w3n::UF::PLAYER_REST_STATE_EXPERIENCE);
        const bool isRested = ((me->field(w3n::UF::PLAYER_BYTES_2) >> 24) & 0xFF) == 1 && rested > 0;
        const uint32_t lv = me->level();
        if (levelSeen && lv > levelSeen) { levelUpMs = now ? now : 1; levelUpTo = lv; }
        if (lv) levelSeen = lv;
        if (in.xpBar && next > 0) {
            l.fade = 255;
            const int h = 5, y = kHudH - h, w = kHudW;
            const uint32_t col = isRested ? hudRgba(0, 100, 224) : hudRgba(148, 0, 140);
            l.rect(0, y - 2, w, 1, hudRgba(0, 0, 0, 200));
            l.rect(0, y - 1, w, 1, hudAlpha(kBorder, 200));
            l.rect(0, y, w, h, hudRgba(0, 0, 0, 150));
            const float f = std::min(1.0f, (float)xp / (float)next);
            const float fr = std::min(1.0f, (float)(xp + (isRested ? rested : 0)) / (float)next);
            const int fx = (int)lroundf(f * w), rx = (int)lroundf(fr * w);
            if (rx > fx) l.rect(fx, y, rx - fx, h, hudAlpha(col, 80));
            // ConsolePort's XPBar.blp is the bar's FILL texture (the addon sets it with SetStatusBarTexture and tints it),
            // not a frame: drawn over the whole bar it hid the fill, so the bar always looked empty. It is clipped to f.
            const ui::Image cp = ui::assets().sprite("cp_xpbar");
            ui::Image tex = cp.ok() ? cp : ui::assets().sprite("bar");
            if (fx > 0) {
                if (tex.ok()) l.imagePart(0, y, fx, h, tex, 0, 0, f, 1, col);
                else { l.rect(0, y, fx, h, col); l.rect(0, y, fx, 1, hudAlpha(0xFFFFFFFFu, 60)); }
            }
            if (!cp.ok()) for (int i = 1; i < 20; ++i) l.rect(w * i / 20, y, 1, h, hudRgba(0, 0, 0, 150));   // the segments
            if (isRested && fr < 1.0f) {   // the rested tick
                ui::Image tick = ui::assets().sprite("exhaustion_tick");
                if (tick.ok()) l.image(rx - 6, y - 5, 12, 12, tick);
                else { l.rect(rx - 1, y - 3, 3, h + 3, hudRgba(120, 180, 255)); }
            }
        }
    }
    // ---- UI stage 8: the red error lines (the original's UIErrorsFrame): what could not be done, newest at the bottom
    {
        l.fade = 255;
        int y = 58;
        for (const w3n::UiError& e : g.uiErrors) {
            const uint32_t age = now - e.ms;
            if (age > 4000) continue;
            const int a = age > 3000 ? (int)(255 - (age - 3000) * 255 / 1000) : 255;
            l.textCentered(kHudW / 2, y, ui::trText(e.text), hudRgba(255, 26, 26, a), true, ui::Font::Normal);
            y += HudList::lineHeight(ui::Font::Normal);
        }
    }
    // ---- UI stage 9: duels (the countdown, leaving the area, who won) and following
    {
        l.fade = 255;
        const w3n::GameSession::DuelState& d = g.duel;
        if (d.fighting && d.startMs && now < d.startMs) {
            const unsigned sec = (unsigned)((d.startMs - now + 999) / 1000);
            l.textCentered(kHudW / 2, 96, std::to_string(sec), hudRgba(255, 209, 0), true, ui::Font::Large);
        }
        if (d.outOfBounds) {
            const uint32_t gone = now - d.outMs;
            const unsigned left = gone >= 10000 ? 0 : (unsigned)((10000 - gone + 999) / 1000);
            char t[96];
            snprintf(t, sizeof t, ui::tr("Leaving the duel area: you lose in %u s", "Вы покидаете место дуэли: поражение через %u с"), left);
            l.textCentered(kHudW / 2, 128, t, hudRgba(255, 60, 40), true, ui::Font::Normal);
        }
        if (!d.result.empty() && d.resultMs && now - d.resultMs < 5000)
            l.textCentered(kHudW / 2, 128, ui::trText(d.result), hudRgba(255, 209, 0), true, ui::Font::Normal);
        if (g.following) {
            const std::string t = std::string(ui::tr("Following ", "Следование: ")) + g.followingName;
            const int w = HudList::width(t);
            l.rect(kHudW / 2 - w / 2 - 3, 194, w + 6, HudList::lineHeight() + 1, hudRgba(0, 0, 0, 110));
            l.textCentered(kHudW / 2, 194, t, hudRgba(255, 230, 120));
        }
    }
    // ---- UI stage 10: aiming a ground spell - its name and the buttons (the ring itself is drawn in the world)
    if (in.placeHint) {   // two short lines between the crossbar clusters
        l.fade = 255;
        const std::string t1 = HudList::fit(in.placeHint, 230);
        const std::string t2 = ui::tr("C-Stick aim | again: cast | cancel", "C-Stick: прицел | ещё раз: каст | отмена");
        const int w = std::max(HudList::width(t1), HudList::width(t2)), lh = HudList::lineHeight(), y = 168;
        l.rect(kHudW / 2 - w / 2 - 4, y - 1, w + 8, 2 * lh + 2, hudRgba(0, 0, 0, 150));
        l.textCentered(kHudW / 2, y, t1, hudRgba(255, 210, 90));
        l.textCentered(kHudW / 2, y + lh, t2, kWhite);
    }
    // ---- UI stage 8: a new level, in big golden letters for a few seconds
    if (levelUpMs && now - levelUpMs < 6000) {
        const uint32_t age = now - levelUpMs;
        const int a = age > 5000 ? (int)(255 - (age - 5000) * 255 / 1000) : 255;
        char t[96];
        snprintf(t, sizeof t, ui::tr("Congratulations, you have reached level %u!", "Поздравляем, вы достигли %u-го уровня!"), (unsigned)levelUpTo);
        l.textCentered(kHudW / 2, 112, t, hudRgba(255, 209, 0, a), true, ui::Font::Large);
    }

    // ---- stage 5.45: the zone's name over the top screen on entering it (the original's ZoneTextFrame): gold, large, with the
    // subzone smaller under it; fades in and out
    if (in.zoneBig && in.zoneBig[0] && in.zoneAlpha > 0) {
        l.fade = 255;
        const int a = std::clamp(in.zoneAlpha, 0, 255);
        const int lh = HudList::lineHeight(ui::Font::Large);
        l.textCentered(kHudW / 2, 37, HudList::fit(in.zoneBig, kHudW - 24, ui::Font::Large), hudRgba(255, 209, 0, a), true, ui::Font::Large);
        if (in.zoneSmall && in.zoneSmall[0])
            l.textCentered(kHudW / 2, 37 + lh, HudList::fit(in.zoneSmall, kHudW - 24, ui::Font::Normal), hudRgba(255, 235, 160, a), true, ui::Font::Normal);
    }

    // ---- 5.35e: FPS and ping, small, at the bottom centre (never dimmed)
    if (in.showStats) {
        l.fade = 255;
        char t[40];
        if (in.pingMs) snprintf(t, sizeof t, "%.0f FPS  %lu ms", in.fps, (unsigned long)in.pingMs);
        else snprintf(t, sizeof t, "%.0f FPS  -- ms", in.fps);
        const int w = HudList::width(t), h = HudList::lineHeight();
        const int y = kHudH - h - (in.xpBar ? 9 : 2);
        l.rect(kHudW / 2 - w / 2 - 3, y - 1, w + 6, h + 2, hudRgba(0, 0, 0, 110));
        const uint32_t pc = !in.pingMs ? kGrey : in.pingMs < 120 ? hudRgba(120, 230, 120) : in.pingMs < 250 ? hudRgba(240, 210, 80)
                                                                                                   : hudRgba(255, 90, 80);
        // FPS white, ping coloured by how good it is
        char f[16];
        snprintf(f, sizeof f, "%.0f FPS  ", in.fps);
        // stage 6.12: how many objects are fading in / out right now (0 standing still; > 0 while things appear)
        char fd[16] = "";
        if (in.fading >= 0) snprintf(fd, sizeof fd, "  ~%d", in.fading);
        const int x0 = kHudW / 2 - w / 2 - HudList::width(fd) / 2;
        const int fw = l.text(x0, y, f, kWhite);
        const int pw = l.text(x0 + fw, y, t + strlen(f), pc);
        if (fd[0]) l.text(x0 + fw + pw, y, fd, kGrey);
    }

}
