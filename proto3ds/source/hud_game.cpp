#include "hud_game.hpp"

#include <ctype.h>
#include <math.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

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

void hudBuildGame(HudList& l, const HudInput& in) {
    l.clear();
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
            bool hurt = o.maxHealth() && o.health() < o.maxHealth() && !o.dead();
            if (isT || hurt || o.meleeVictim) {   // health bar for the target and units in a fight
                float f = o.maxHealth() ? (float)o.health() / (float)o.maxHealth() : 0;
                l.bar((int)pl.sx - 16, y + 11, 32, 4, f, hudRgba(40, 210, 40));
            }
            const bool aimed = in.pad && in.pad->aim && o.guid == in.pad->aim;
            if (aimed) nm = "> " + nm + " <";
            l.textCentered((int)pl.sx, y - 1, nm, isT || aimed ? kWhite : unitColor(g, o));
            // quest giver mark (stage 5.14): yellow ! = a quest for us, yellow ? = one to hand in, grey = not yet
            if (uint32_t st = g.questGiverStatus(o.guid)) {
                const char* mark = w3n::questMarkGlyph(st);
                if (mark[0] && !o.dead()) {   // drawn twice, one pixel apart: a bold mark
                    int my = y - (in.pad && in.pad->soft == o.guid ? 23 : 12);
                    uint32_t mc = w3n::questMarkGrey(st) ? kGrey : kGold;
                    l.textCentered((int)pl.sx, my, mark, mc);
                    l.textCentered((int)pl.sx + 1, my, mark, mc, false);
                }
            }
            if (in.pad && in.pad->soft && o.guid == in.pad->soft && !o.self)   // interact prompt
                l.textCentered((int)pl.sx, y - 12, std::string("[Y] ") + ui::trText(in.pad->softVerb), kGold);
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

    // ---- player frame (and our buffs / debuffs under it)
    // UI stage 2: dim at rest, bars only (no portrait), our auras as icons on the right
    const bool rest = in.dim && !g.inCombat() && !g.cast.active && !tgt;
    l.fade = rest ? 150 : 255;
    if (me) {
        uint32_t hp = me->health(), hpMax = me->maxHealth();
        l.rect(4, 4, 18, 27, hudRgba(0, 0, 0, 170));
        l.outline(4, 4, 18, 27, g.inCombat() ? hudRgba(230, 50, 40) : kBorder);
        l.textCentered(13, 11, std::to_string(me->level()), kGold);
        l.bar(25, 5, 128, 12, hpMax ? (float)hp / hpMax : 0, me->dead() ? hudRgba(120, 120, 120) : hudRgba(30, 200, 30));
        char b[32];
        snprintf(b, sizeof b, "%u / %u", (unsigned)hp, (unsigned)hpMax);
        l.textCentered(89, 6, b, kWhite);
        Power pw = powerOf(*me);
        if (pw.max) {
            l.bar(25, 21, 128, 9, (float)pw.cur / pw.max, powerColor(pw.type));
            snprintf(b, sizeof b, "%u / %u", (unsigned)pw.cur, (unsigned)pw.max);
            l.textCentered(89, 20, b, kWhite);
        }
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
    // ---- target frame, its auras, the target of the target (stage 5.16)
    if (tgt) {
        const int tx = 166, tw = 150;
        panel(l, tx, 3, tw, 30);
        std::string nm = HudList::fit(tgt->self ? std::string(in.selfName) : tgt->name.empty() ? std::string("...") : tgt->name, tw - 26);
        l.text(tx + 4, 4, nm, unitColor(g, *tgt));
        l.textRight(tx + tw - 4, 4, tgt->level() ? std::to_string(tgt->level()) : "??", kGold);
        float f = tgt->maxHealth() ? (float)tgt->health() / tgt->maxHealth() : 0;
        l.bar(tx + 4, 17, tw - 8, 10, f, tgt->dead() ? hudRgba(120, 120, 120) : hudRgba(30, 200, 30));
        char pc[16];
        snprintf(pc, sizeof pc, "%u%%", (unsigned)(f * 100 + 0.5f));
        l.textCentered(tx + tw / 2, 17, pc, kWhite);
        w3n::CastState tc = g.castOf(tgt->guid, now);
        int ay = 36;
        if (tc.active && tc.durMs && !tgt->self) {
            float cf = std::min(1.0f, (float)(now - tc.startMs) / tc.durMs);
            l.bar(tx + 4, ay, tw - 8, 9, cf, hudRgba(255, 120, 20));
            l.textCentered(tx + tw / 2, ay - 1, HudList::fit(g.spellNames.name(tc.spell), tw - 12), kWhite);
            ay += 13;
        }
        if (tgt->dead() && (tgt->field(w3n::UF::UNIT_DYNAMIC_FLAGS) & 1)) l.textCentered(tx + tw / 2, ay, "Lootable", kGold);
        else if (tgt->dead()) l.textCentered(tx + tw / 2, ay, "Dead", kGrey);
        else if (!tgt->self) {
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

    // ---- cast bar (bottom right: the place of the action bar and the cast bar of the original is the touch screen here)
    if (g.cast.active && g.cast.durMs) {
        uint32_t el = now - g.cast.startMs;
        float f = std::min(1.0f, (float)el / (float)g.cast.durMs);
        const int w = 180, x = (kHudW - w) / 2, y = 214;   // the crossbars sit in the corners: the centre is free
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
            panel(l, 91, 178, 218, 30);
            l.text(95, 181, t1, pv.mode == 1 ? hudRgba(255, 90, 90) : hudRgba(110, 255, 110));
            l.text(95, 193, t2, kWhite);
        } else {
            // UI stage 2: crossbars in the bottom corners (the centre stays on the world) - D-pad diamond bottom left,
            // face buttons bottom right, icons 22 px,
            // the button's glyph in the corner, cooldowns as a dark sweep from the top and seconds
            static const int kDx[4] = {0, 23, 0, -23}, kDy[4] = {-19, 0, 19, 0};   // up, right, down, left
            static const char* kGlyph[8] = {"↑", "→", "↓", "←", "X", "A", "B", "Y"};
            static std::unordered_map<uint32_t, uint32_t> cdFull;   // longest cooldown seen per action: the sweep's 100 %
            const int cs = 22;
            for (int b = 0; b < 8; ++b) {
                int cx = b < 4 ? 37 : kHudW - 37, cy = 206;
                int x = cx + kDx[b % 4] - cs / 2, y = cy + kDy[b % 4] - cs / 2;
                const HudBarCell& c = pv.cells[b];
                l.rect(x, y, cs, cs, c.fixed ? hudRgba(30, 30, 40, 160) : hudRgba(10, 10, 20, 190));
                if (c.icon.ok()) l.image(x, y, cs, cs, c.icon);
                else if (!c.name.empty()) {
                    std::string nm = c.fixed ? ascii(c.name) : auraInitials(c.name);
                    l.textCentered(x + cs / 2, y + 6, HudList::fit(nm, cs), c.fixed ? kGrey : kWhite, false);
                }
                if (c.cdMs && c.cdKey) {
                    uint32_t& full = cdFull[c.cdKey];
                    full = std::max(full, c.cdMs);
                    int h = (int)(cs * std::min(1.0f, (float)c.cdMs / full) + 0.5f);
                    l.rect(x, y, cs, h, hudRgba(0, 0, 0, 170));
                    char t[8];
                    snprintf(t, sizeof t, "%u", (unsigned)((c.cdMs + 999) / 1000));
                    if (c.cdMs > 1600) l.textCentered(x + cs / 2, y + 6, t, hudRgba(255, 230, 40));
                } else if (c.cdKey) {
                    cdFull.erase(c.cdKey);
                }
                if (c.count) l.textRight(x + cs, y + cs - 10, std::to_string(c.count), kWhite);
                l.outline(x - 1, y - 1, cs + 2, cs + 2, c.active ? hudRgba(255, 210, 40) : pv.binding ? hudRgba(90, 230, 255) : kBorder);
                l.text(x + 1, y - 2, kGlyph[b], kGold);
            }
            for (int k = 0; k < 2; ++k) {   // ZL / ZR lit when their set (or both) is on
                bool on = (pv.set & (1 << k)) != 0;
                int x = k ? kHudW - 28 : 4, y = 162;   // above its cluster: ZL over the D-pad, ZR over the face buttons
                l.rect(x, y, 24, 11, on ? hudRgba(120, 90, 20, 220) : hudRgba(0, 0, 0, 120));
                l.textCentered(x + 12, y, k ? "ZR" : "ZL", on ? kGold : kGrey, false);
            }
            char t[16];
            snprintf(t, sizeof t, "p%d", pv.page + 1);
            l.text(32, 162, t, kGrey);
        }
    }
}
