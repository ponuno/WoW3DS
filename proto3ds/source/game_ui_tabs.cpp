// game_ui_tabs.cpp — the eight tabs of the touch screen (UI stages 3/4/6) and the START menu / target menu.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "game_ui.hpp"
#include "text.hpp"
#include "ui_style.hpp"

using namespace ui;

const char* gameUiPhrase(int i);

namespace {

float dist3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

uint32_t reactionColor(const w3n::GameSession& g, const w3n::WorldObject& o) {
    if (o.dead()) return kGrey;
    const w3n::Rank r = g.shownReaction(o);
    if (o.type == w3n::ObjType::Player) return (uint8_t)r <= (uint8_t)w3n::Rank::Hostile ? kRed : kBlue;
    if ((uint8_t)r <= (uint8_t)w3n::Rank::Hostile) return kRed;
    if (r == w3n::Rank::Unfriendly) return kOrange;
    if (r == w3n::Rank::Neutral) return kYellow;
    return kGreen;
}

uint32_t chatColor(uint8_t type) {
    using namespace w3n;
    switch (type) {
        case Chat::Yell: case Chat::MonsterYell: case Chat::RaidWarning: return hudRgba(255, 64, 64);
        case Chat::Whisper: case Chat::WhisperInform: case Chat::MonsterWhisper: return hudRgba(255, 128, 255);
        case Chat::Party: return hudRgba(170, 170, 255);
        case Chat::Raid: case Chat::RaidLeader: return hudRgba(255, 127, 0);
        case Chat::Guild: return hudRgba(64, 255, 64);
        case Chat::Officer: return hudRgba(64, 192, 64);
        case Chat::System: return kYellow;
        case Chat::Channel: return hudRgba(255, 192, 192);
        case Chat::Emote: case Chat::TextEmote: case Chat::MonsterEmote: return hudRgba(255, 128, 64);
        case Chat::Local: return kCyan;
        case Chat::Loot: return hudRgba(0, 170, 0);
        default: return kText;
    }
}

// Chat tabs: 0 all, 1 say (and other nearby speech), 2 group, 3 guild, 4 whispers, 5 system / combat / loot
bool chatInChannel(uint8_t type, int ch) {
    using namespace w3n;
    switch (ch) {
        case 1: return type == Chat::Say || type == Chat::Yell || type == Chat::Emote || type == Chat::TextEmote ||
                       type == Chat::MonsterSay || type == Chat::MonsterYell || type == Chat::MonsterEmote;
        case 2: return type == Chat::Party || type == Chat::Raid || type == Chat::RaidLeader || type == Chat::RaidWarning;
        case 3: return type == Chat::Guild || type == Chat::Officer;
        case 4: return type == Chat::Whisper || type == Chat::WhisperInform || type == Chat::MonsterWhisper;
        case 5: return type == Chat::System || type == Chat::Local || type == Chat::Loot || type == Chat::Skill || type == Chat::Channel;
        default: return true;
    }
}

const char* slotShortName(int s) {
    static const char* en[19] = {"Head", "Neck", "Shoul", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrist", "Hands",
                                 "Ring", "Ring", "Trink", "Trink", "Back", "Main", "Off", "Range", "Tabard"};
    static const char* ru[19] = {"Голова", "Шея", "Плечи", "Рубаш", "Грудь", "Пояс", "Ноги", "Ступни", "Запяст", "Кисти",
                                 "Кольцо", "Кольцо", "Аксес", "Аксес", "Спина", "Пр.рука", "Лев.рука", "Дальн", "Гербовая"};
    if (s < 0 || s >= 19) return s >= 19 && s < 23 ? tr("Bag", "Сумка") : "";
    return g_lang ? ru[s] : en[s];
}

std::string secs(uint32_t ms) {
    unsigned s = (unsigned)((ms + 999) / 1000);
    if (s >= 3600) return std::to_string(s / 3600) + tr("h", "ч");
    if (s >= 60) return std::to_string(s / 60) + tr("m", "м");
    return std::to_string(s);
}

}  // namespace

// ------------------------------------------------------------------ Combat: party, target of target, auras, cooldowns, nearby
void GameUi::drawCombat(w3n::GameSession& g, const Input& in) {
    const int L = 4, LW = 154, R = 164, RW = 152;
    rows_.clear();
    list_.text(L, kTop + 2, tr("Group and nearby", "Группа и рядом"), kGold);
    int y = kTop + 14;
    // party members first
    for (const w3n::PartyMember& pm : g.party.members) {
        if (y > kTabY - 26) break;
        w3n::PartyMember v = g.memberView(pm);
        const bool tgt = g.target() == pm.guid;
        list_.rect(L, y, LW, 24, tgt ? kRowSel : kRow);
        if (tgt) list_.outline(L, y, LW, 24, kGold);
        std::string nm = (g.party.leader == pm.guid ? "◆" : "") + pm.name;
        list_.textFit(L + 3, y + 1, nm, 110, !pm.online() ? kGrey : kText, false);
        if (v.level) list_.textRight(L + LW - 3, y + 1, std::to_string(v.level), kGold, false);
        if (!pm.online()) list_.text(L + 3, y + 12, tr("Offline", "Не в сети"), kGrey, false);
        else if (pm.status & 0x8) list_.text(L + 3, y + 12, tr("Ghost", "Призрак"), kCyan, false);
        else if ((pm.status & 0x4) || (v.statsKnown && v.maxHp && !v.hp)) list_.text(L + 3, y + 12, tr("Dead", "Мёртв"), kGrey, false);
        else {
            list_.bar(L + 3, y + 13, LW - 6, 5, v.maxHp ? (float)v.hp / v.maxHp : 0, hudRgba(30, 200, 30));
            if (v.maxPower) list_.rect(L + 3, y + 20, (int)((LW - 6) * (float)v.power / v.maxPower), 2, powerColor(v.powerType));
        }
        rows_.push_back(pm.guid);
        region(L, y, LW, 24, Act::TargetUnit, (int)rows_.size() - 1);
        y += 26;
    }
    // nearby units by distance
    struct Near { float d; const w3n::WorldObject* o; };
    std::vector<Near> near;
    for (auto& [guid, o] : g.objects.all()) {
        if (o.self || !o.hasPos || !o.isUnitLike() || g.inParty(guid)) continue;
        float p[3];
        o.positionAt(in.nowMs, p);
        near.push_back({dist3(p, in.pos), &o});
    }
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
    for (const Near& n : near) {
        if (y > kTabY - 24) break;
        const w3n::WorldObject& o = *n.o;
        const bool tgt = g.target() == o.guid, aimed = o.guid == aim_;
        list_.rect(L, y, LW, 22, tgt ? kRowSel : kRowAlt);
        if (tgt || aimed) list_.outline(L, y, LW, 22, tgt ? kGold : kCyan);
        std::string nm = o.name.empty() ? std::string("...") : o.name;
        list_.textFit(L + 3, y + 1, nm, 100, reactionColor(g, o), false);
        char b[24];
        snprintf(b, sizeof b, "%.0f%s", n.d, tr("yd", "м"));
        list_.textRight(L + LW - 3, y + 1, b, kGrey, false);
        list_.text(L + 106, y + 1, "L" + std::to_string(o.level()), kGold, false);
        float f = o.maxHealth() ? (float)o.health() / o.maxHealth() : 0;
        list_.rect(L + 3, y + 14, (int)((LW - 6) * f), 4, o.dead() ? kGrey : hudRgba(30, 190, 30));
        rows_.push_back(o.guid);
        region(L, y, LW, 22, Act::TargetUnit, (int)rows_.size() - 1);
        y += 24;
    }
    if (rows_.empty()) list_.text(L + 4, kTop + 20, tr("Nobody around.", "Рядом никого."), kGrey);

    // right column: target of target
    int ry = kTop + 2;
    list_.text(R, ry, tr("Target of target", "Цель цели"), kGold);
    ry += 12;
    window(R, ry, RW, 26);
    const w3n::WorldObject* t = g.objects.find(g.target());
    const w3n::WorldObject* tt = t && !t->dead() ? g.objects.find(g.targetOf(t->guid)) : nullptr;
    if (tt) {
        std::string nm = tt->self ? std::string(in.selfName) : tt->name;
        list_.textFit(R + 4, ry + 2, nm, RW - 8, tt->self ? kRed : reactionColor(g, *tt), false);
        list_.bar(R + 4, ry + 16, RW - 8, 5, tt->maxHealth() ? (float)tt->health() / tt->maxHealth() : 0, hudRgba(30, 200, 30));
    } else {
        list_.text(R + 4, ry + 7, t ? tr("(none)", "(нет)") : tr("No target", "Нет цели"), kGrey, false);
    }
    ry += 30;
    // our auras: tap = details (cancel there), long press too
    list_.text(R, ry, tr("Your effects", "Ваши эффекты"), kGold);
    ry += 12;
    const w3n::WorldObject* me = g.selfObject();
    std::vector<w3n::AuraView> av = me ? g.auras(*me, in.nowMs) : std::vector<w3n::AuraView>();
    if (av.empty()) list_.text(R + 2, ry + 4, tr("none", "нет"), kGrey, false);
    for (size_t i = 0; i < av.size() && i < 12; ++i) {
        const w3n::AuraView& a = av[i];
        int x = R + (int)(i % 6) * 25, yy = ry + (int)(i / 6) * 30;
        iconBox(x + 1, yy + 1, 22, spellIcon(a.spell), g.spellNames.name(a.spell), a.debuff ? kRed : kFrame);
        if (a.stacks > 1) list_.textRight(x + 24, yy + 12, std::to_string(a.stacks), kYellow, true);
        if (a.leftMs) list_.textCentered(x + 12, yy + 22, secs(a.leftMs), a.leftMs < 10000 ? kRed : kText, true);
        Button& b = region(x, yy, 25, 30, Act::BuffPick, (int)a.spell);
        b.infoKind = 3;
        b.infoId = a.spell;
    }
    ry += av.size() > 6 ? 62 : 32;
    // cooldowns of the bars' spells
    list_.text(R, ry, tr("Cooldowns", "Перезарядка"), kGold);
    ry += 12;
    int shown = 0;
    if (barsView_) {
        std::vector<uint32_t> seen;
        for (int s = 0; s < kPadSlots && ry + 20 <= kTabY; ++s) {
            const ActionSlot& a = barsView_->at(s);
            if (a.type != ActionSlot::Spell || std::find(seen.begin(), seen.end(), a.id) != seen.end()) continue;
            seen.push_back(a.id);
            uint32_t cd = g.cooldownLeftMs(a.id, in.nowMs);
            if (cd <= 1500) continue;   // the global cooldown is not news
            iconBox(R + 1, ry + 1, 16, spellIcon(a.id), g.spellNames.name(a.id), kFrameDim, true);
            list_.textFit(R + 22, ry + 2, g.spellNames.name(a.id), RW - 52, kText, false);
            list_.textRight(R + RW - 2, ry + 2, secs(cd), kYellow, false);
            ry += 20;
            ++shown;
        }
    }
    if (!shown) list_.text(R + 2, ry + 2, tr("all ready", "всё готово"), kGrey, false);
    if (g.cast.active) btn(R, kTabY - 24, RW, 22, tr("Cancel the cast", "Прервать заклинание"), Act::CancelCast, 0, Style::Danger, Font::Small);
}

// ------------------------------------------------------------------ Map: the zone (or continent) with us, units, quest givers
void GameUi::drawMap(w3n::GameSession& g, const Input& in) {
    const int mx = 2, my = kTop + 2, mw = 258, mh = 172;
    ui::MapView mv = mapMode_ == 0 ? assets().zoneAt(in.mapId, in.pos[0], in.pos[1]) : assets().continent(in.mapId);
    list_.rect(mx, my, mw, mh, hudRgba(20, 26, 30));
    float pu = 0.5f, pv = 0.5f;
    auto worldToUv = [&](float x, float y, float* u, float* v) {
        if (!mv.ok()) return false;
        *u = (mv.m->left - y) / (mv.m->left - mv.m->right);
        *v = (mv.m->top - x) / (mv.m->top - mv.m->bottom);
        return true;
    };
    // the visible window of the picture (zoom around us)
    float u0 = 0, v0 = 0, span = 1.0f / (float)mapZoom_;
    const bool onMap = mv.ok() && worldToUv(in.pos[0], in.pos[1], &pu, &pv) && pu >= 0 && pu <= 1 && pv >= 0 && pv <= 1;
    if (mv.ok()) {
        if (mapZoom_ > 1 && onMap) {
            u0 = std::clamp(pu - span / 2, 0.0f, 1.0f - span);
            v0 = std::clamp(pv - span / 2, 0.0f, 1.0f - span);
        }
        list_.imagePart(mx, my, mw, mh, mv.image, u0, v0, u0 + span, v0 + span);
    } else {   // no map in the pack: a schematic around us, 60 yd to the edge, north up
        for (int k = 1; k < 4; ++k) {
            list_.rect(mx, my + mh * k / 4, mw, 1, hudRgba(40, 50, 56));
            list_.rect(mx + mw * k / 4, my, 1, mh, hudRgba(40, 50, 56));
        }
        list_.textCentered(mx + mw / 2, my + mh - 12, tr("no map (convert ui.w3dp)", "нет карты (сконвертируйте ui.w3dp)"), kGrey, false);
    }
    list_.outline(mx - 1, my - 1, mw + 2, mh + 2, kFrame);
    // world -> screen
    const float yardsPerPx = 60.0f * 2 / mh;
    auto toScreen = [&](float x, float y, int* sx, int* sy) {
        if (mv.ok()) {
            float u, v;
            worldToUv(x, y, &u, &v);
            *sx = mx + (int)((u - u0) / span * mw);
            *sy = my + (int)((v - v0) / span * mh);
        } else {   // north (+x) up, west (+y) left
            *sx = mx + mw / 2 - (int)((y - in.pos[1]) / yardsPerPx);
            *sy = my + mh / 2 - (int)((x - in.pos[0]) / yardsPerPx);
        }
        return *sx >= mx && *sx < mx + mw && *sy >= my && *sy < my + mh;
    };
    list_.clip(mx, my, mw, mh);
    // units: quest givers as marks, others as dots by reaction
    for (auto& [guid, o] : g.objects.all()) {
        if (!o.hasPos || o.self || !o.isUnitLike()) continue;
        float p[3];
        o.positionAt(in.nowMs, p);
        int sx, sy;
        if (!toScreen(p[0], p[1], &sx, &sy)) continue;
        uint32_t st = g.questGiverStatus(guid);
        const char* mark = st && !o.dead() ? w3n::questMarkGlyph(st) : "";
        if (mark[0]) {
            ui::Image qi = assets().sprite(mark[0] == '!' ? "quest_new" : "quest_done");
            if (qi.ok() && !w3n::questMarkGrey(st)) list_.image(sx - 6, sy - 6, 12, 12, qi);
            else list_.textCentered(sx, sy - 6, mark, w3n::questMarkGrey(st) ? kGrey : kGold, true, Font::Normal);
        } else if (mapZoom_ > 1 || !mv.ok()) {
            list_.rect(sx - 1, sy - 1, 3, 3, reactionColor(g, o));
            if (guid == g.target()) list_.outline(sx - 3, sy - 3, 7, 7, kGold);
        }
    }
    // party members (also out of range: their last reported position)
    for (const w3n::PartyMember& pm : g.party.members) {
        float px = 0, py = 0;
        const w3n::WorldObject* o = g.objects.find(pm.guid);
        if (o && o->hasPos) { float p[3]; o->positionAt(in.nowMs, p); px = p[0]; py = p[1]; }
        else if (pm.statsKnown && (pm.x || pm.y)) { px = pm.x; py = pm.y; }
        else continue;
        int sx, sy;
        if (toScreen(px, py, &sx, &sy)) { list_.rect(sx - 2, sy - 2, 5, 5, kBlue); list_.outline(sx - 3, sy - 3, 7, 7, kBlack); }
    }
    // our corpse while a ghost
    if (g.life() == w3n::Life::Ghost && g.corpse.known && g.corpse.map == in.mapId) {
        int sx, sy;
        if (toScreen(g.corpse.x, g.corpse.y, &sx, &sy)) {
            list_.rect(sx - 4, sy, 9, 1, kCyan);
            list_.rect(sx, sy - 4, 1, 9, kCyan);
        }
    }
    // us: the arrow sprite turned to our facing (north up), else a dot with a nose
    {
        int sx, sy;
        if (toScreen(in.pos[0], in.pos[1], &sx, &sy)) {
            int k = (int)lroundf(in.yaw / 0.39269908f);
            k = ((k % 16) + 16) % 16;
            ui::Image ar = assets().sprite(("arrow" + std::to_string(k)).c_str());
            if (ar.ok()) list_.image(sx - 8, sy - 8, 16, 16, ar);
            else {
                list_.rect(sx - 2, sy - 2, 5, 5, kYellow);
                list_.outline(sx - 3, sy - 3, 7, 7, kBlack);
                int nx = sx - (int)lroundf(sinf(in.yaw) * 6), ny = sy - (int)lroundf(cosf(in.yaw) * 6);
                list_.rect(nx - 1, ny - 1, 3, 3, kYellow);
            }
        }
    }
    list_.noClip();
    list_.clip(0, kTop, kTouchW, kContentH);
    // zone name and the tracked quest over the map
    if (mv.ok()) {
        int w = HudList::width(mv.name, Font::Normal);
        list_.rect(mx, my, w + 8, 16, hudRgba(0, 0, 0, 140));
        list_.text(mx + 4, my + 1, mv.name, kGold, true, Font::Normal);
    }
    if (tracked_) {
        std::vector<w3n::QuestLogEntry> log = g.questLog();
        for (const w3n::QuestLogEntry& e : log) {
            if (e.id != tracked_) continue;
            std::vector<std::string> lines = g.questProgressKnown(e);
            int n = 1 + (int)std::min<size_t>(lines.size(), 2);
            int ty = my + mh - n * 12 - 2;
            list_.rect(mx, ty - 1, mw, n * 12 + 3, hudRgba(0, 0, 0, 150));
            list_.textFit(mx + 4, ty, g.questTitleKnown(e.id), mw - 8, e.complete ? kGreen : kGold, true);
            for (int k = 0; k < n - 1; ++k) list_.textFit(mx + 10, ty + 12 * (k + 1), lines[k], mw - 14, kText, true);
        }
    }
    // controls
    const int bx = 266, bw = 50;
    btn(bx, kTop + 2, bw, 22, tr("Zone", "Зона"), Act::MapMode, 0, mapMode_ == 0 ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(bx, kTop + 26, bw, 22, tr("World", "Мир"), Act::MapMode, 1, mapMode_ == 1 ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(bx, kTop + 56, bw, 26, "+", Act::MapZoom, mapZoom_ * 2, mapZoom_ < 4 ? Style::Normal : Style::Disabled, Font::Large);
    btn(bx, kTop + 84, bw, 26, "-", Act::MapZoom, mapZoom_ / 2, mapZoom_ > 1 ? Style::Normal : Style::Disabled, Font::Large);
    char b[32];
    if (onMap) snprintf(b, sizeof b, "%.1f %.1f", pu * 100, pv * 100);
    else snprintf(b, sizeof b, "%.0f %.0f", in.pos[0], in.pos[1]);
    list_.textCentered(bx + bw / 2, kTop + 118, b, kText);
    snprintf(b, sizeof b, "x%d", mapZoom_);
    list_.textCentered(bx + bw / 2, kTop + 132, b, kGrey);
    if (g.newMail) list_.textCentered(bx + bw / 2, kTop + 150, tr("Mail!", "Почта!"), kYellow);
}

// ------------------------------------------------------------------ Chat: channel tabs, lines, keyboard, quick phrases
void GameUi::drawChat(w3n::GameSession& g, const Input& in) {
    (void)in;
    const char* names[6] = {tr("All", "Все"), tr("Say", "Рядом"), tr("Group", "Группа"), tr("Guild", "Гильдия"), tr("Whisper", "Шёпот"),
                            tr("System", "Систем.")};
    for (int i = 0; i < 6; ++i)
        btn(2 + i * 53, kTop + 2, 51, 18, names[i], Act::ChatChannel, i, chatChannel_ == i ? Style::ToggleOn : Style::Toggle, Font::Small);
    const Font f = listFont();
    const int lh = HudList::lineHeight(f), top = kTop + 24, bottom = kTabY - 28, rows = (bottom - top) / lh;
    // newest lines first, wrapped
    struct Line { uint32_t color; std::string text; };
    std::vector<Line> lines;
    const int want = rows + chatScroll_;
    for (size_t i = g.chat.size(); i-- > 0 && (int)lines.size() < want;) {
        const w3n::ChatLine& l = g.chat[i];
        if (!chatInChannel(l.type, chatChannel_)) continue;
        std::vector<std::string> w = HudList::wrap(trText(g.lineText(l)), kTouchW - 10, f);
        for (size_t k = w.size(); k-- > 0 && (int)lines.size() < want;) lines.push_back({chatColor(l.type), (k ? "  " : "") + w[k]});
    }
    const int maxScroll = std::max(0, (int)lines.size() - rows);
    chatScroll_ = std::min(chatScroll_, maxScroll);
    list_.rect(2, top - 2, kTouchW - 4, bottom - top + 4, hudRgba(0, 0, 0, 90));
    for (int r = 0; r < rows; ++r) {
        int k = chatScroll_ + (rows - 1 - r);
        if (k < (int)lines.size()) list_.text(5, top + r * lh, lines[k].text, lines[k].color, true, f);
    }
    if (lines.empty()) list_.text(8, top + 4, tr("No messages.", "Сообщений нет."), kGrey);
    const int by = kTabY - 24;
    btn(2, by, 104, 22, tr("Write", "Написать"), Act::ChatWrite, chatChannel_, Style::Primary, Font::Small);
    btn(110, by, 96, 22, tr("Phrases", "Фразы"), Act::ChatPhrases, 0, phrases_ ? Style::ToggleOn : Style::Normal, Font::Small);
    btn(212, by, 50, 22, "▲", Act::ChatScroll, 3, chatScroll_ < maxScroll ? Style::Normal : Style::Disabled);
    btn(266, by, 50, 22, "▼", Act::ChatScroll, -3, chatScroll_ > 0 ? Style::Normal : Style::Disabled);
    if (phrases_) {   // quick phrases over the lines
        window(20, kTop + 22, kTouchW - 40, 128, true);
        // two pages: phrases, and (the animation stage) emotes (CMSG_TEXT_EMOTE) with sitting down / standing up
        btn(28, kTop + 27, 80, 18, tr("Phrases", "Фразы"), Act::EmotePick, 2000, phrasePage_ == 0 ? Style::ToggleOn : Style::Toggle, Font::Small);
        btn(112, kTop + 27, 80, 18, tr("Emotes", "Эмоции"), Act::EmotePick, 2001, phrasePage_ == 1 ? Style::ToggleOn : Style::Toggle, Font::Small);
        if (phrasePage_ == 0) {
            for (int i = 0; i < 8; ++i)
                btn(28 + (i % 2) * 134, kTop + 50 + (i / 2) * 24, 128, 22, gameUiPhrase(i), Act::PhrasePick, i, Style::Normal, Font::Small);
        } else {
            struct Em { const char* en; const char* ru; int id; };
            static const Em kEm[12] = {{"Wave", "Помахать", 101}, {"Bow", "Поклон", 17}, {"Cheer", "Ура", 21}, {"Dance", "Танец", 34},
                                       {"Laugh", "Смех", 60}, {"Point", "Указать", 72}, {"Salute", "Честь", 78}, {"Roar", "Рык", 75},
                                       {"Cry", "Плакать", 31}, {"Kneel", "На колено", 59}, {"Sleep", "Лечь", 1003}, {"Sit", "Сесть", 1000}};
            const w3n::WorldObject* me = g.selfObject();
            const bool down = me && me->standState() != 0;
            for (int i = 0; i < 12; ++i) {
                const bool toggle = kEm[i].id >= 1000;
                btn(28 + (i % 4) * 67, kTop + 50 + (i / 4) * 30, 63, 26, toggle && down ? tr("Stand", "Встать") : tr(kEm[i].en, kEm[i].ru),
                    Act::EmotePick, toggle && down ? 999 : kEm[i].id, Style::Normal, Font::Small);
            }
        }
    }
}

// ------------------------------------------------------------------ Bags: a grid of the bags' contents
void GameUi::drawItems(w3n::GameSession& g, const Input& in) {
    (void)in;
    btn(2, kTop + 2, 72, 18, tr("Bags", "Сумки"), Act::SubView, 0, Style::ToggleOn, Font::Small);
    btn(76, kTop + 2, 84, 18, tr("Equipment", "Экипировка"), Act::SubView, 1, Style::Toggle, Font::Small);
    std::vector<w3n::InvItem> items = listed(g);
    const int cols = 7, rowsN = 3, per = cols * rowsN, cell = 42;
    const int pages = std::max(1, ((int)items.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    if (selected_ >= (int)items.size()) selected_ = std::max(0, (int)items.size() - 1);
    const w3n::InvItem* sel = selected_ < (int)items.size() ? &items[selected_] : nullptr;
    if (sel) {
        const w3n::ItemInfo* info = g.itemInfoIfKnown(sel->entry);
        list_.textFit(164, kTop + 5, g.itemName(sel->entry), kTouchW - 168, qualityColor(info ? info->quality : 1));
    }
    if (items.empty()) list_.text(10, kTop + 30, tr("Your bags are empty.", "Ваши сумки пусты."), kGrey, true, Font::Normal);
    for (int k = 0; k < per; ++k) {
        int idx = page_ * per + k;
        if (idx >= (int)items.size()) break;
        const w3n::InvItem& it = items[idx];
        const int x = 6 + (k % cols) * cell, y = kTop + 24 + (k / cols) * cell;
        const w3n::ItemInfo* info = g.itemInfo(it.entry);
        iconBox(x + 1, y + 1, cell - 4, itemIcon(g, it.entry), g.itemName(it.entry), qualityColor(info ? info->quality : 1));
        if (it.count > 1) list_.textRight(x + cell - 4, y + cell - 14, std::to_string(it.count), kWhite, true);
        if (it.maxDurability && it.durability * 4 < it.maxDurability) list_.text(x + 3, y + 2, "!", kRed, true, Font::Normal);
        if (idx == selected_) { list_.outline(x - 1, y - 1, cell, cell, kGold); list_.outline(x - 2, y - 2, cell + 2, cell + 2, kGold); }
        Button& b = region(x - 1, y - 1, cell, cell, Act::SelectItem, idx);
        b.infoKind = 4;
        b.infoId = it.entry;
        b.guid = it.guid;
        if (info && info->usable()) { b.dragKind = 2; b.dragId = it.entry; }
    }
    // what the selection can do
    const int by = kTabY - 24;
    const w3n::ItemInfo* info = sel ? g.itemInfoIfKnown(sel->entry) : nullptr;
    int x = 2;
    if (sel && info && info->usable()) {
        btn(x, by, 50, 22, tr("Use", "Исп."), Act::Use, 0, Style::Primary, Font::Small); x += 52;
        btn(x, by, 50, 22, tr("Bind", "Кнопка"), Act::BindItem, 0, Style::Normal, Font::Small); x += 52;
    } else if (sel && info && info->openable() && !info->lockId) {
        btn(x, by, 50, 22, tr("Open", "Откр."), Act::OpenItem, 0, Style::Primary, Font::Small); x += 52;
    }
    if (sel && info && info->equippable()) { btn(x, by, 54, 22, tr("Equip", "Надеть"), Act::Equip, 0, Style::Normal, Font::Small); x += 56; }
    if (sel) btn(x, by, 46, 22, tr("Del", "Удал."), Act::Destroy, 0, Style::Danger, Font::Small);
    if (pages > 1) pager(by, page_, pages);
    else {
        char b[32];
        snprintf(b, sizeof b, "%s %u", tr("free", "свободно"), (unsigned)g.freeBagSlots());
        list_.textRight(kTouchW - 6, by + 6, b, kGrey);
    }
}

// ------------------------------------------------------------------ Equipment: the paper doll
void GameUi::drawPaperDoll(w3n::GameSession& g, const Input& in) {
    (void)in;
    btn(2, kTop + 2, 72, 18, tr("Bags", "Сумки"), Act::SubView, 0, Style::Toggle, Font::Small);
    btn(76, kTop + 2, 84, 18, tr("Equipment", "Экипировка"), Act::SubView, 1, Style::ToggleOn, Font::Small);
    std::vector<w3n::InvItem> items = listed(g);   // equipped + the bag slots
    if (selected_ >= (int)items.size()) selected_ = std::max(0, (int)items.size() - 1);
    auto cellAt = [&](int slot, int x, int y, int size) {
        int idx = -1;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].slot == slot) idx = (int)i;
        if (idx >= 0) {
            const w3n::InvItem& it = items[idx];
            const w3n::ItemInfo* info = g.itemInfo(it.entry);
            iconBox(x, y, size, itemIcon(g, it.entry), g.itemName(it.entry), qualityColor(info ? info->quality : 1));
            if (it.maxDurability && it.durability * 4 < it.maxDurability) list_.text(x + 2, y + 1, "!", kRed, true, Font::Normal);
            if (idx == selected_ && selTapped_) { list_.outline(x - 2, y - 2, size + 4, size + 4, kGold); }
            Button& b = region(x - 1, y - 1, size + 2, size + 2, Act::SelectItem, idx);
            b.infoKind = 4;
            b.infoId = it.entry;
            b.guid = it.guid;
        } else {
            ui::Image bg = assets().namedIcon(("slot_" + std::to_string(std::min(slot, 19))).c_str());
            if (bg.ok()) list_.image(x, y, size, size, bg, hudRgba(150, 150, 150));
            else {
                list_.rect(x, y, size, size, kRowAlt);
                list_.textCentered(x + size / 2, y + size / 2 - 5, HudList::fit(slotShortName(slot), size - 2), hudRgba(110, 104, 92), false);
            }
            list_.outline(x - 1, y - 1, size + 2, size + 2, kFrameDim);
        }
    };
    static const int left[8] = {0, 1, 2, 14, 4, 3, 18, 8}, right[8] = {9, 5, 6, 7, 10, 11, 12, 13};
    const int cs = 34, step = 38, y0 = kTop + 26;
    for (int i = 0; i < 8; ++i) {
        cellAt(left[i], 4 + (i % 2) * step, y0 + (i / 2) * step, cs);
        cellAt(right[i], kTouchW - 4 - cs - (1 - i % 2) * step, y0 + (i / 2) * step, cs);
    }
    // the middle: who we are, the bags, the weapons
    const int cx0 = 4 + 2 * step + 4, cx1 = kTouchW - 4 - 2 * step - 4, cw = cx1 - cx0;
    const w3n::WorldObject* me = g.selfObject();
    list_.textCentered(cx0 + cw / 2, y0, in.selfName, kGold, true, Font::Normal);
    if (me) {
        char b[64];
        snprintf(b, sizeof b, "%s %u %s", tr("Level", "Уровень"), (unsigned)me->level(), className(g.classId()));
        list_.textCentered(cx0 + cw / 2, y0 + 16, b, classColor(g.classId()));
        uint32_t dur = 0, durMax = 0;
        for (const w3n::InvItem& it : items) { dur += it.durability; durMax += it.maxDurability; }
        if (durMax) {
            snprintf(b, sizeof b, "%s %u%%", tr("Durability", "Прочность"), (unsigned)(dur * 100ull / durMax));
            list_.textCentered(cx0 + cw / 2, y0 + 28, b, dur * 4 < durMax ? kRed : kGrey);
        }
    }
    for (int k = 0; k < 4; ++k) cellAt(19 + k, cx0 + 4 + k * 32, y0 + 44, 28);
    for (int k = 0; k < 3; ++k) cellAt(15 + k, cx0 + (cw - 3 * step) / 2 + k * step + 2, y0 + 3 * step, cs);
    // the selected item
    const int by = y0 + 80;
    const w3n::InvItem* sel = selected_ < (int)items.size() ? &items[selected_] : nullptr;
    if (sel) {
        const w3n::ItemInfo* info = g.itemInfoIfKnown(sel->entry);
        list_.textFit(cx0, by - 2, g.itemName(sel->entry), cw, qualityColor(info ? info->quality : 1));
        btn(cx0, by + 12, cw / 2 - 2, 20, tr("Remove", "Снять"), Act::Unequip, 0, Style::Normal, Font::Small);
        btn(cx0 + cw / 2 + 2, by + 12, cw / 2 - 2, 20, tr("Info", "Инфо"), Act::ItemInfo, 0, Style::Normal, Font::Small);
    }
}

// ------------------------------------------------------------------ Spells: the book (tap = bind, drag = bind, hold = info)
void GameUi::drawSpells(w3n::GameSession& g, const Input& in) {
    btn(2, kTop + 2, 72, 18, tr("Book", "Книга"), Act::SubView, 0, Style::ToggleOn, Font::Small);
    btn(76, kTop + 2, 72, 18, tr("Talents", "Таланты"), Act::SubView, 1, Style::Toggle, Font::Small);
    if (barsView_) {
        char b[48];
        snprintf(b, sizeof b, "%s %d/%d", tr("bar page", "стр. панели"), barsView_->page + 1, ActionLayout::kPages);
        list_.textRight(kTouchW - 6, kTop + 5, b, kGrey);
    }
    std::vector<uint32_t> sp = bookSpells(g);
    const int per = 10, rowH = 25, top = kTop + 22;
    const int pages = std::max(1, ((int)sp.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    const Font f = listFont();
    for (int k = 0; k < per; ++k) {
        int idx = page_ * per + k;
        if (idx >= (int)sp.size()) break;
        uint32_t s = sp[idx];
        const int x = 2 + (k % 2) * 158, y = top + (k / 2) * rowH;
        const bool armed = bindArmed_ && bindWhat_.type == ActionSlot::Spell && bindWhat_.id == s;
        list_.rect(x, y, 156, rowH - 2, armed ? kRowSel : (k / 2) % 2 ? kRowAlt : kRow);
        uint32_t cd = g.cooldownLeftMs(s, in.nowMs);
        iconBox(x + 2, y + 1, 21, spellIcon(s), g.spellNames.name(s), kFrameDim, cd > 1500);
        list_.textFit(x + 27, y + (f == Font::Small ? 1 : 2), g.spellNames.name(s), 126, kText, false, f);
        std::string rk = g.spellNames.rank(s);
        if (f == Font::Small) list_.text(x + 27, y + 11, cd > 1500 ? secs(cd) : rk, cd > 1500 ? kYellow : kGrey, false);
        Button& b = region(x, y, 156, rowH - 2, Act::BindSpell, (int)s);
        b.infoKind = 2;
        b.infoId = s;
        b.dragKind = 1;
        b.dragId = s;
    }
    const int by = kTabY - 24;
    btn(2, by, 120, 22, tr("Clear a button", "Очистить кнопку"), Act::ClearSlot, 0, Style::Normal, Font::Small);
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ Talents: the class's trees (talents.txt)
void GameUi::drawTalents(w3n::GameSession& g, const Input& in) {
    (void)in;
    btn(2, kTop + 2, 72, 18, tr("Book", "Книга"), Act::SubView, 0, Style::Toggle, Font::Small);
    btn(76, kTop + 2, 72, 18, tr("Talents", "Таланты"), Act::SubView, 1, Style::ToggleOn, Font::Small);
    char b[64];
    snprintf(b, sizeof b, "%s %u", tr("points:", "очки:"), (unsigned)g.talentPoints());
    list_.textRight(kTouchW - 6, kTop + 5, b, g.talentPoints() ? kGreen : kGrey);
    std::vector<const w3n::TalentTree*> trees = g.talents.treesFor(g.classId());
    if (trees.empty()) {
        window(10, kTop + 30, kTouchW - 20, 70);
        list_.textCentered(kTouchW / 2, kTop + 44, g.talents.loaded() ? tr("No talents for this class.", "Нет талантов для этого класса.")
                                                                       : tr("No talents.txt on the SD card", "Нет talents.txt на SD-карте"),
                           kText, true, Font::Normal);
        list_.textCentered(kTouchW / 2, kTop + 64, tr("(w3dconv --ui or --texts-only writes it)", "(его пишет w3dconv --ui или --texts-only)"), kGrey);
        return;
    }
    talentTree_ = std::min<int>(talentTree_, (int)trees.size() - 1);
    for (size_t i = 0; i < trees.size() && i < 3; ++i) {
        uint32_t spent = 0;
        for (const w3n::Talent* t : g.talents.talentsOf(trees[i]->id)) spent += (uint32_t)g.talentRank(*t);
        std::string label = trees[i]->name + " " + std::to_string(spent);
        btn(2 + (int)i * 64, kTop + 22, 62, 18, label, Act::TalentTree, (int)i, talentTree_ == (int)i ? Style::ToggleOn : Style::Toggle, Font::Small);
    }
    std::vector<const w3n::Talent*> ts = g.talents.talentsOf(trees[talentTree_]->id);
    uint32_t spent = 0;
    for (const w3n::Talent* t : ts) spent += (uint32_t)g.talentRank(*t);
    // tiers 0-3 or 4-6 (page_), 4 columns of 30 px
    const int tiersPer = 4, cell = 28, step = 31, gx = 4, gy = kTop + 44;
    int maxRow = 0;
    for (const w3n::Talent* t : ts) maxRow = std::max<int>(maxRow, (int)t->row);
    const int pages = maxRow / tiersPer + 1;
    page_ = std::min(page_, pages - 1);
    const w3n::Talent* sel = nullptr;
    for (const w3n::Talent* t : ts) {
        if ((int)t->row / tiersPer != page_ || t->col > 3) continue;
        const int x = gx + (int)t->col * step, y = gy + ((int)t->row % tiersPer) * step;
        const int rank = g.talentRank(*t), maxr = t->maxRank();
        bool open = spent >= t->row * 5;
        if (t->reqTalent)
            if (const w3n::Talent* req = g.talents.find(t->reqTalent)) open = open && g.talentRank(*req) >= (int)t->reqRank + 1;
        iconBox(x, y, cell, spellIcon(t->rankSpell[0]), g.spellNames.name(t->rankSpell[0]),
                rank >= maxr ? kGold : rank ? kGreen : open ? kFrame : kFrameDim, !open && !rank);
        snprintf(b, sizeof b, "%d/%d", rank, maxr);
        list_.textRight(x + cell + 1, y + cell - 9, b, rank >= maxr ? kGold : rank ? kGreen : kGrey, true);
        if ((int)t->id == talentSel_) { list_.outline(x - 2, y - 2, cell + 4, cell + 4, kGold); sel = t; }
        Button& r = region(x - 1, y - 1, cell + 2, cell + 2, Act::TalentPick, (int)t->id);
        r.infoKind = 2;
        r.infoId = t->rankSpell[std::max(0, rank - 1)];
    }
    if (pages > 1) {
        btn(gx, kTabY - 24, 60, 22, tr("Tiers ▲", "Ряды ▲"), Act::PagePrev, 0, page_ > 0 ? Style::Normal : Style::Disabled, Font::Small);
        btn(gx + 64, kTabY - 24, 60, 22, tr("Tiers ▼", "Ряды ▼"), Act::PageNext, 0, page_ + 1 < pages ? Style::Normal : Style::Disabled, Font::Small);
    }
    // the selected talent
    const int ix = 136, iw = kTouchW - ix - 4;
    window(ix, gy - 2, iw, kTabY - gy - 2);
    if (!sel) {
        list_.text(ix + 6, gy + 6, tr("Tap a talent.", "Выберите талант."), kGrey, true, Font::Normal);
        return;
    }
    const int rank = g.talentRank(*sel), maxr = sel->maxRank();
    uint32_t shown = sel->rankSpell[std::max(0, rank - 1)];
    list_.textFit(ix + 6, gy + 4, g.spellNames.name(shown), iw - 12, kGold, true, Font::Normal);
    snprintf(b, sizeof b, "%s %d/%d", tr("Rank", "Ранг"), rank, maxr);
    list_.text(ix + 6, gy + 22, b, rank ? kGreen : kText);
    int ty = gy + 36;
    if (sel->row * 5 > spent) {
        snprintf(b, sizeof b, tr("Needs %u points in %s", "Нужно %u очков в «%s»"), (unsigned)(sel->row * 5), trees[talentTree_]->name.c_str());
        for (const std::string& l : HudList::wrap(b, iw - 12)) { list_.text(ix + 6, ty, l, kRed); ty += 11; }
    }
    if (sel->reqTalent)
        if (const w3n::Talent* req = g.talents.find(sel->reqTalent))
            if (g.talentRank(*req) < (int)sel->reqRank + 1) {
                std::string l = std::string(tr("Needs: ", "Нужно: ")) + g.spellNames.name(req->rankSpell[0]);
                for (const std::string& w : HudList::wrap(l, iw - 12)) { list_.text(ix + 6, ty, w, kRed); ty += 11; }
            }
    if (rank < maxr && sel->rankSpell[rank]) {
        list_.textFit(ix + 6, ty + 2, std::string(tr("Next: ", "Далее: ")) + spellLabel(g, sel->rankSpell[rank]), iw - 12, kGrey);
    }
    const bool can = rank < maxr && g.talentPoints() > 0;
    btn(ix + 6, kTabY - 30, iw - 12, 24, tr("Learn", "Изучить"), Act::TalentLearn, 0, can ? Style::Primary : Style::Disabled);
}

// ------------------------------------------------------------------ Quests: log, one quest, reputation
void GameUi::drawQuestLog(w3n::GameSession& g, const Input& in) {
    std::vector<w3n::QuestLogEntry> log = g.questLog();
    if (logView_ == 1 && (logSel_ < 0 || logSel_ >= (int)log.size())) logView_ = 0;
    const int by = kTabY - 24;
    char b[96];
    if (logView_ == 2) {   // reputation
        btn(2, kTop + 2, 72, 18, tr("Quests", "Задания"), Act::LogBack, 0, Style::Toggle, Font::Small);
        btn(76, kTop + 2, 92, 18, tr("Reputation", "Репутация"), Act::LogReputation, 0, Style::ToggleOn, Font::Small);
        std::vector<w3n::GameSession::RepLine> reps = g.reputations();
        if (!g.factions.loaded()) list_.text(8, kTop + 28, tr("No factions.txt on the SD card.", "Нет factions.txt на SD-карте."), kGrey);
        else if (reps.empty()) list_.text(8, kTop + 28, tr("No reputations yet.", "Репутации пока нет."), kGrey);
        const int per = 6;
        int pages = std::max(1, ((int)reps.size() + per - 1) / per);
        page_ = std::min(page_, pages - 1);
        for (int r = 0; r < per; ++r) {
            int i = page_ * per + r;
            if (i >= (int)reps.size()) break;
            const w3n::GameSession::RepLine& l = reps[i];
            const int y = kTop + 24 + r * 24;
            int32_t into = l.value - w3n::rankFloor(l.rank);
            int32_t span = l.rank == w3n::Rank::Exalted ? 1000 : w3n::rankFloor((w3n::Rank)((int)l.rank + 1)) - w3n::rankFloor(l.rank);
            uint32_t c = (uint8_t)l.rank <= (uint8_t)w3n::Rank::Hostile ? kRed : l.rank == w3n::Rank::Unfriendly ? kOrange
                       : l.rank == w3n::Rank::Neutral ? kYellow : kGreen;
            list_.rect(2, y, kTouchW - 4, 22, r % 2 ? kRowAlt : kRow);
            list_.textFit(6, y + 1, l.name, 170, kText, false);
            snprintf(b, sizeof b, "%s %ld/%ld%s", w3n::rankName(l.rank), (long)into, (long)span, l.atWar ? " !" : "");
            list_.textRight(kTouchW - 6, y + 1, b, c, false);
            list_.bar(6, y + 14, kTouchW - 12, 4, span ? (float)into / span : 0, c);
        }
        pager(by, page_, pages);
        return;
    }
    if (logView_ == 1) {   // one quest
        const w3n::QuestLogEntry& e = log[logSel_];
        const w3n::QuestInfo* q = g.questInfo(e.id);
        struct L { uint32_t color; std::string text; Font f; };
        std::vector<L> body;
        std::string title = g.questTitle(e.id);
        if (q && q->level) title += " [" + std::to_string(q->level) + "]";
        for (const std::string& l : HudList::wrap(title, kTouchW - 12, Font::Normal)) body.push_back({kGold, l, Font::Normal});
        for (const std::string& p : g.questProgress(e))
            body.push_back({p == "(complete)" ? kGreen : p == "(failed)" ? kRed : kText,
                            "  " + (p == "(complete)" ? std::string(tr("(complete)", "(выполнено)")) : p == "(failed)" ? std::string(tr("(failed)", "(провалено)")) : p),
                            Font::Small});
        if (q) {
            for (const std::string& l : HudList::wrap(q->objectives, kTouchW - 12)) body.push_back({kGrey, l, Font::Small});
            if (!q->details.empty()) {
                body.push_back({kGold, tr("Description", "Описание"), Font::Small});
                for (const std::string& l : HudList::wrap(q->details, kTouchW - 12)) body.push_back({kGrey, l, Font::Small});
            }
        } else {
            body.push_back({kGrey, tr("(asking the server...)", "(запрос к серверу...)"), Font::Small});
        }
        const int top = kTop + 4, bottom = by - 4;
        int maxScroll = 0, hTotal = 0;
        for (const L& l : body) hTotal += HudList::lineHeight(l.f);
        int skip = 0, acc = 0;
        while (skip < (int)body.size() && hTotal - acc > bottom - top) acc += HudList::lineHeight(body[skip++].f);
        maxScroll = skip;
        questScroll_ = std::min(questScroll_, maxScroll);
        int y = top;
        for (int i = questScroll_; i < (int)body.size(); ++i) {
            int lh = HudList::lineHeight(body[i].f);
            if (y + lh > bottom) break;
            list_.text(6, y, body[i].text, body[i].color, true, body[i].f);
            y += lh;
        }
        btn(2, by, 58, 22, tr("Back", "Назад"), Act::LogBack, 0, Style::Normal, Font::Small);
        btn(62, by, 82, 22, tracked_ == e.id ? tr("Untrack", "Не следить") : tr("Track", "Следить"), Act::TrackQuest, 0,
            tracked_ == e.id ? Style::ToggleOn : Style::Normal, Font::Small);
        btn(146, by, 66, 22, tr("Abandon", "Отказ"), Act::LogAbandon, 0, Style::Danger, Font::Small);
        if (maxScroll) {
            btn(220, by, 46, 22, "▲", Act::QuestScroll, -3, questScroll_ ? Style::Normal : Style::Disabled);
            btn(270, by, 46, 22, "▼", Act::QuestScroll, 3, questScroll_ < maxScroll ? Style::Normal : Style::Disabled);
        }
        return;
    }
    btn(2, kTop + 2, 72, 18, tr("Quests", "Задания"), Act::LogBack, 0, Style::ToggleOn, Font::Small);
    btn(76, kTop + 2, 92, 18, tr("Reputation", "Репутация"), Act::LogReputation, 0, Style::Toggle, Font::Small);
    snprintf(b, sizeof b, "%d/%d", (int)log.size(), w3n::kQuestLogSize);
    list_.textRight(kTouchW - 6, kTop + 5, b, kGrey);
    if (log.empty()) list_.text(8, kTop + 30, tr("Your quest log is empty.", "Журнал заданий пуст."), kGrey, true, Font::Normal);
    const int per = 6;
    int pages = std::max(1, ((int)log.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    for (int k = 0; k < per; ++k) {
        int r = page_ * per + k;
        if (r >= (int)log.size()) break;
        const w3n::QuestLogEntry& e = log[r];
        const w3n::QuestInfo* q = g.questInfo(e.id);
        const int y = kTop + 22 + k * 25;
        list_.rect(2, y, kTouchW - 4, 23, k % 2 ? kRowAlt : kRow);
        if (tracked_ == e.id) list_.text(5, y + 5, "◆", kGold);
        uint32_t c = e.failed ? kRed : e.complete ? kGreen : kGold;
        list_.textFit(17, y + 3, g.questTitle(e.id), 230, c, false, Font::Normal);
        std::string st = e.failed ? tr("failed", "провал") : e.complete ? tr("done", "готово") : q && q->level ? "[" + std::to_string(q->level) + "]" : "";
        list_.textRight(kTouchW - 6, y + 6, st, e.failed ? kRed : e.complete ? kGreen : kGrey, false);
        region(2, y, kTouchW - 4, 23, Act::LogSelect, r);
    }
    pager(by, page_, pages);
    (void)in;
}

// ------------------------------------------------------------------ Social: group, guild, friends, ignore
void GameUi::drawSocial(w3n::GameSession& g, const Input& in) {
    const char* names[4] = {tr("Group", "Группа"), tr("Guild", "Гильдия"), tr("Friends", "Друзья"), tr("Ignore", "Игнор")};
    for (int i = 0; i < 4; ++i)
        btn(2 + i * 79, kTop + 2, 77, 18, names[i], Act::SubView, i, subSocial_ == i ? Style::ToggleOn : Style::Toggle, Font::Small);
    const int top = kTop + 24, rowH = 24, per = 5, by = kTabY - 24;
    auto row = [&](int k, bool sel) {
        const int y = top + k * rowH;
        list_.rect(2, y, kTouchW - 4, rowH - 2, sel ? kRowSel : k % 2 ? kRowAlt : kRow);
        if (sel) list_.outline(2, y, kTouchW - 4, rowH - 2, kGold);
        return y;
    };
    char b[96];
    if (subSocial_ == 0) {   // the group
        if (!g.party.inGroup) list_.text(8, top + 4, tr("You are not in a group.", "Вы не в группе."), kGrey, true, Font::Normal);
        for (size_t i = 0; i < g.party.members.size() && (int)i < per; ++i) {
            const w3n::PartyMember& pm = g.party.members[i];
            w3n::PartyMember v = g.memberView(pm);
            int y = row((int)i, socialSel_ == (int)i);
            list_.textFit(6, y + 1, (g.party.leader == pm.guid ? "◆ " : "") + pm.name, 150, pm.online() ? kText : kGrey, false, Font::Normal);
            snprintf(b, sizeof b, "L%u", (unsigned)v.level);
            list_.text(170, y + 3, b, kGold, false);
            if (v.maxHp) list_.bar(200, y + 8, 110, 5, (float)v.hp / v.maxHp, hudRgba(30, 200, 30));
            region(2, y, kTouchW - 4, rowH - 2, Act::SocialPick, (int)i);
        }
        int x = 2;
        btn(x, by, 84, 22, tr("Invite", "Пригласить"), Act::GroupInvite, 0, Style::Normal, Font::Small); x += 86;
        if (g.party.inGroup) { btn(x, by, 70, 22, tr("Leave", "Выйти"), Act::GroupLeave, 0, Style::Danger, Font::Small); x += 72; }
        if (socialSel_ >= 0 && socialSel_ < (int)g.party.members.size()) {
            btn(x, by, 54, 22, tr("Whisper", "Шёпот"), Act::Whisper, socialSel_, Style::Normal, Font::Small); x += 56;
            if (g.isLeader()) {
                btn(x, by, 46, 22, tr("Kick", "Искл."), Act::GroupKick, socialSel_, Style::Danger, Font::Small); x += 48;
                btn(x, by, 50, 22, tr("Leader", "Лидер"), Act::GroupPromote, socialSel_, Style::Normal, Font::Small);
            }
        }
        return;
    }
    if (subSocial_ == 1) {   // the guild
        if (!g.guildId()) {
            list_.text(8, top + 4, tr("You are not in a guild.", "Вы не в гильдии."), kGrey, true, Font::Normal);
            return;
        }
        if (!g.guild.rosterSeen && in.nowMs - g.guild.rosterMs > 3000) g.requestGuildRoster(in.nowMs);
        int online = 0;
        for (const w3n::GuildMember& m : g.guild.members) online += m.online != 0;
        snprintf(b, sizeof b, "%s  %d/%d", g.guild.name.empty() ? "..." : g.guild.name.c_str(), online, (int)g.guild.members.size());
        list_.textFit(6, top, b, kTouchW - 12, kGreen, true, Font::Normal);
        const int lt = top + 16, perG = 4;
        int pages = std::max(1, ((int)g.guild.members.size() + perG - 1) / perG);
        page_ = std::min(page_, pages - 1);
        for (int k = 0; k < perG; ++k) {
            int i = page_ * perG + k;
            if (i >= (int)g.guild.members.size()) break;
            const w3n::GuildMember& m = g.guild.members[i];
            const int y = lt + k * 24;
            list_.rect(2, y, kTouchW - 4, 22, socialSel_ == i ? kRowSel : k % 2 ? kRowAlt : kRow);
            list_.textFit(6, y + 3, m.name, 120, m.online ? classColor(m.cls) : kGrey, false, Font::Normal);
            snprintf(b, sizeof b, "%u %s", (unsigned)m.level, className(m.cls));
            list_.text(140, y + 5, b, m.online ? kText : kGrey, false);
            std::string rank = m.rank < g.guild.ranks.size() ? g.guild.ranks[m.rank] : std::string();
            list_.textRight(kTouchW - 6, y + 5, m.online ? rank : tr("offline", "не в сети"), kGrey, false);
            region(2, y, kTouchW - 4, 22, Act::SocialPick, i);
        }
        int x = 2;
        if (socialSel_ >= 0 && socialSel_ < (int)g.guild.members.size() && g.guild.members[socialSel_].online) {
            btn(x, by, 56, 22, tr("Whisper", "Шёпот"), Act::Whisper, socialSel_, Style::Normal, Font::Small); x += 58;
        }
        btn(x, by, 70, 22, tr("Invite", "Пригл."), Act::GroupInvite, 0, Style::Normal, Font::Small); x += 72;
        btn(x, by, 60, 22, tr("Leave", "Выйти"), Act::GroupLeave, 0, Style::Danger, Font::Small);
        pager(by, page_, pages);
        return;
    }
    if (subSocial_ == 2) {   // friends
        if (g.friends.empty()) list_.text(8, top + 4, tr("No friends yet.", "Друзей пока нет."), kGrey, true, Font::Normal);
        int pages = std::max(1, ((int)g.friends.size() + per - 1) / per);
        page_ = std::min(page_, pages - 1);
        for (int k = 0; k < per; ++k) {
            int i = page_ * per + k;
            if (i >= (int)g.friends.size()) break;
            const w3n::FriendEntry& f = g.friends[i];
            int y = row(k, socialSel_ == i);
            std::string nm = g.playerName(f.guid, in.nowMs);
            list_.textFit(6, y + 3, nm.empty() ? "..." : nm, 130, f.online() ? classColor(f.cls) : kGrey, false, Font::Normal);
            if (f.online()) {
                snprintf(b, sizeof b, "%u %s%s", (unsigned)f.level, className(f.cls), f.status == 2 ? " AFK" : f.status == 4 ? " DND" : "");
                list_.text(150, y + 5, b, kText, false);
            } else {
                list_.text(150, y + 5, tr("offline", "не в сети"), kGrey, false);
            }
            region(2, y, kTouchW - 4, rowH - 2, Act::SocialPick, i);
        }
        int x = 2;
        btn(x, by, 70, 22, tr("Add", "Добавить"), Act::FriendAdd, 0, Style::Normal, Font::Small); x += 72;
        if (socialSel_ >= 0 && socialSel_ < (int)g.friends.size()) {
            if (g.friends[socialSel_].online()) { btn(x, by, 56, 22, tr("Whisper", "Шёпот"), Act::Whisper, socialSel_, Style::Normal, Font::Small); x += 58; }
            btn(x, by, 62, 22, tr("Remove", "Удалить"), Act::FriendRemove, socialSel_, Style::Danger, Font::Small);
        }
        pager(by, page_, pages);
        return;
    }
    // ignore
    if (g.ignored.empty()) list_.text(8, top + 4, tr("Nobody is ignored.", "Никто не игнорируется."), kGrey, true, Font::Normal);
    int pages = std::max(1, ((int)g.ignored.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    for (int k = 0; k < per; ++k) {
        int i = page_ * per + k;
        if (i >= (int)g.ignored.size()) break;
        int y = row(k, socialSel_ == i);
        std::string nm = g.playerName(g.ignored[i], in.nowMs);
        list_.textFit(6, y + 3, nm.empty() ? "..." : nm, 200, kText, false, Font::Normal);
        region(2, y, kTouchW - 4, rowH - 2, Act::SocialPick, i);
    }
    btn(2, by, 70, 22, tr("Add", "Добавить"), Act::IgnoreAdd, 0, Style::Normal, Font::Small);
    if (socialSel_ >= 0 && socialSel_ < (int)g.ignored.size()) btn(74, by, 70, 22, tr("Remove", "Убрать"), Act::IgnoreRemove, socialSel_, Style::Danger, Font::Small);
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ Settings: controls, graphics, interface
void GameUi::drawSettings(const Input& in) {
    const char* names[3] = {tr("Controls", "Управление"), tr("Graphics/sound", "Графика/звук"), tr("Interface", "Интерфейс")};
    for (int i = 0; i < 3; ++i)
        btn(2 + i * 106, kTop + 2, 104, 18, names[i], Act::SubView, i, subSettings_ == i ? Style::ToggleOn : Style::Toggle, Font::Small);
    if (!in.cfg) return;
    const PadConfig& c = *in.cfg;
    int y = kTop + 24;
    auto label = [&](const char* t) { list_.textFit(6, y + 4, t, 110, kText, false); };
    const int bh = 19;
    auto opt = [&](int x, int w, const char* t, bool on, int arg) { btn(x, y, w, bh, t, Act::Setting, arg, on ? Style::ToggleOn : Style::Toggle, Font::Small); };
    char b[16];
    if (subSettings_ == 0) {
        label(tr("Scheme", "Схема"));
        opt(120, 96, "Forever", c.scheme == PadScheme::Forever, 0);
        opt(220, 96, "Classic", c.scheme == PadScheme::Classic, 1);
        y += 22;
        label(tr("Run from", "Бег от"));
        opt(120, 62, "0.3", fabsf(c.runThreshold - 0.3f) < 0.05f, 10);
        opt(186, 62, "0.5", fabsf(c.runThreshold - 0.5f) < 0.05f, 11);
        opt(252, 62, "0.7", fabsf(c.runThreshold - 0.7f) < 0.05f, 12);
        y += 22;
        label(tr("Face movement", "Лицом к ходу"));
        opt(120, 96, tr("On", "Вкл"), c.faceMovement, 20);
        opt(220, 96, tr("Off", "Выкл"), !c.faceMovement, 21);
        y += 22;
        label("ZL / ZR");
        opt(120, 96, tr("Hold", "Удерживать"), !c.toggleModifiers, 30);
        opt(220, 96, tr("Toggle", "Переключать"), c.toggleModifiers, 31);
        y += 22;
        label(tr("Camera", "Камера"));
        opt(120, 96, tr("Invert Y", "Инверсия Y"), c.invertY, 40);
        btn(220, y, 30, 19, "-", Act::Setting, 50, Style::Normal);
        snprintf(b, sizeof b, "%.1f", c.camSpeed);
        list_.textCentered(268, y + 4, b, kText);
        btn(286, y, 30, 19, "+", Act::Setting, 51, Style::Normal);
        y += 22;
        list_.textFit(6, y + 2, tr("SELECT + ◀/▶ turns the tabs.", "SELECT + ◀/▶ листает вкладки."), kTouchW - 12, kGrey);
    } else if (subSettings_ == 1) {
        y -= 4;   // 8 rows (stage 5.32)
        label(tr("Draw distance", "Дальность"));
        const char* d[4] = {tr("Near", "Близко"), tr("Normal", "Норм."), tr("Far", "Далеко"), tr("Max", "Макс.")};
        for (int i = 0; i < 4; ++i) opt(120 + i * 49, 47, d[i], c.drawDist == i, 70 + i);
        y += 20;
        label(tr("Crowds", "Толпы"));
        const char* cr[4] = {"10", "20", "40", tr("All", "Все")};
        for (int i = 0; i < 4; ++i) opt(120 + i * 49, 47, cr[i], c.crowd == i, 80 + i);
        y += 20;
        label(tr("World objects", "Объекты мира"));
        opt(120, 96, tr("On", "Вкл"), c.doodads, 90);
        y += 20;
        label(tr("Particles", "Частицы"));   // stage 5.32: flames, smoke, lamp glows
        const char* fxn[3] = {tr("Off", "Выкл"), tr("Low", "Мало"), tr("High", "Много")};
        for (int i = 0; i < 3; ++i) opt(120 + i * 65, 62, fxn[i], c.effects == i, 170 + i);
        y += 20;
        label(tr("Top screen", "Верх. экран"));
        opt(120, 62, tr("Normal", "Обычный"), c.topMode == 0, 95);
        opt(184, 62, "800x240", c.topMode == 1, 96);
        opt(248, 62, "3D", c.topMode == 2, 97);
        y += 20;
        const char* vn[3] = {tr("Music", "Музыка"), tr("Effects", "Эффекты"), tr("Ambience", "Окружение")};
        const int vv[3] = {c.musicVol, c.sfxVol, c.ambVol};
        for (int i = 0; i < 3; ++i) {
            label(vn[i]);
            btn(120, y, 30, bh, "-", Act::Setting, 140 + i * 10, Style::Normal);
            list_.bar(154, y + 6, 74, 7, vv[i] / 10.0f, kGold, hudRgba(0, 0, 0, 140));
            snprintf(b, sizeof b, "%d", vv[i]);
            list_.textCentered(243, y + 3, vv[i] ? b : tr("off", "выкл"), kText);
            btn(258, y, 30, bh, "+", Act::Setting, 141 + i * 10, Style::Normal);
            y += 19;
        }
    } else {
        label(tr("Language", "Язык"));
        opt(120, 96, "English", c.lang == 0, 100);
        opt(220, 96, "Русский", c.lang == 1, 101);
        y += 22;
        label(tr("Text size", "Размер текста"));
        opt(120, 96, tr("Normal", "Обычный"), !c.bigText, 110);
        opt(220, 96, tr("Large", "Крупный"), c.bigText, 110);
        y += 22;
        label(tr("Dim HUD at rest", "Тусклый HUD вне боя"));
        opt(120, 96, c.dimHud ? tr("On", "Вкл") : tr("Off", "Выкл"), c.dimHud, 120);
        y += 22;
        label(tr("Auto tabs", "Автовкладки"));
        opt(120, 96, c.autoTabs ? tr("On", "Вкл") : tr("Off", "Выкл"), c.autoTabs, 130);
        y += 22;
        label(tr("Quest on top", "Квест сверху"));
        opt(120, 96, tr("On", "Вкл"), c.questTracker, 60);
        opt(220, 96, tr("Off", "Выкл"), !c.questTracker, 61);
    }
    if (subSettings_ == 1) return;   // 8 rows since the particles (stage 5.32): Debug / Quit stay on the other pages and in START
    const int by = kTabY - 24;
    btn(2, by, 156, 22, tr("Debug page", "Отладка"), Act::MenuItem, 8, Style::Normal, Font::Small);
    btn(162, by, 154, 22, tr("Quit", "Выход"), Act::MenuItem, 9, Style::Danger, Font::Small);
}

// ------------------------------------------------------------------ START menu, the target's menu, buffs
void GameUi::drawMenu(w3n::GameSession& g) {
    (void)g;
    window(4, kTop + 2, kTouchW - 8, kContentH - 4, true);
    const char* items[10] = {tr("Chat", "Чат"), tr("Bags", "Сумки"), tr("Equipment", "Экипировка"), tr("Spells", "Книга"),
                             tr("Quests", "Задания"), tr("Buffs", "Эффекты"), tr("Settings", "Настройки"), tr("Free camera", "Своб. камера"),
                             tr("Debug page", "Отладка"), tr("Quit", "Выход")};
    for (int i = 0, k = 0; i < 10; ++i) {
        if (i == 7) continue;   // the free camera is gone (stage 5.31)
        btn(14 + (k % 2) * 148, kTop + 8 + (k / 2) * 27, 142, 24, items[i], Act::MenuItem, i, i == 9 ? Style::Danger : Style::Normal);
        ++k;
    }
    btn(kTouchW - 80, kTabY - 28, 70, 22, tr("Close", "Закрыть"), Act::ClosePanel, 0, Style::Normal, Font::Small);
}

void GameUi::drawContext(w3n::GameSession& g) {
    window(4, kTop + 2, kTouchW - 8, kContentH - 4, true);
    const w3n::WorldObject* t = g.objects.find(g.target());
    std::vector<int> items;
    if (t) {
        const bool player = t->type == w3n::ObjType::Player && !t->self;
        const bool member = g.inParty(t->guid);
        for (int i : {0, 1, 2}) items.push_back(i);
        if (player) items.push_back(3);                                                   // whisper
        if (player && !member && (!g.party.inGroup || g.isLeader())) items.push_back(5);  // invite
        if (member && g.isLeader()) { items.push_back(6); items.push_back(7); }           // uninvite, promote
        if (player) { items.push_back(9); items.push_back(10); }                         // trade, add friend
        if (player && g.guildId()) items.push_back(11);                                   // guild invite
        items.push_back(4);
    } else {
        list_.textCentered(kTouchW / 2, kTop + 20, tr("No target", "Нет цели"), kGrey, true, Font::Normal);
    }
    if (g.party.inGroup) items.push_back(8);
    const char* names[12] = {tr("Attack", "Атаковать"), tr("Interact", "Действие"), tr("Assist", "Помочь"), tr("Whisper", "Шёпот"),
                             tr("Clear target", "Сбросить цель"), tr("Invite", "В группу"), tr("Uninvite", "Исключить"),
                             tr("Make leader", "Сделать лидером"), tr("Leave party", "Покинуть группу"), tr("Trade", "Обмен"),
                             tr("Add friend", "В друзья"), tr("Guild invite", "В гильдию")};
    for (size_t k = 0; k < items.size() && k < 10; ++k) {
        int i = items[k];
        btn(14 + (int)(k % 2) * 148, kTop + (t ? 10 : 40) + (int)(k / 2) * 30, 142, 26, names[i], Act::ContextItem, i,
            i == 8 || i == 6 ? Style::Danger : i == 0 ? Style::Primary : Style::Normal);
    }
    btn(kTouchW - 80, kTabY - 28, 70, 22, tr("Close", "Закрыть"), Act::ClosePanel, 0, Style::Normal, Font::Small);
}

void GameUi::drawBuffs(w3n::GameSession& g, const Input& in) {
    window(2, kTop + 2, kTouchW - 4, kContentH - 4, true);
    const w3n::WorldObject* me = g.selfObject();
    std::vector<w3n::AuraView> av = me ? g.auras(*me, in.nowMs) : std::vector<w3n::AuraView>();
    if (av.empty()) list_.text(12, kTop + 14, tr("No buffs or debuffs.", "Нет эффектов."), kGrey, true, Font::Normal);
    bool selFound = false;
    for (size_t r = 0; r < av.size() && r < 6; ++r) {
        const w3n::AuraView& a = av[r];
        const int y = kTop + 8 + (int)r * 25;
        const bool sel = a.spell == buffSel_;
        selFound = selFound || sel;
        list_.rect(8, y, kTouchW - 16, 23, sel ? kRowSel : r % 2 ? kRowAlt : kRow);
        iconBox(10, y + 1, 21, spellIcon(a.spell), g.spellNames.name(a.spell), a.debuff ? kRed : kFrame);
        std::string nm = g.spellNames.name(a.spell);
        if (a.stacks > 1) nm += " (" + std::to_string(a.stacks) + ")";
        list_.textFit(36, y + 4, nm, 200, a.debuff ? kRed : a.cancelable ? kGreen : kYellow, false, Font::Normal);
        if (a.leftMs) list_.textRight(kTouchW - 12, y + 6, secs(a.leftMs), kText, false);
        Button& b = region(8, y, kTouchW - 16, 23, Act::BuffPick, (int)a.spell);
        b.infoKind = 3;
        b.infoId = a.spell;
    }
    if (!selFound) buffSel_ = 0;
    const int by = kTabY - 28;
    if (buffSel_) btn(10, by, 120, 22, tr("Cancel buff", "Снять эффект"), Act::BuffCancel, 0, Style::Primary, Font::Small);
    else list_.text(12, by + 6, tr("tap a buff to cancel it", "выберите эффект, чтобы снять"), kGrey);
    btn(kTouchW - 80, by, 70, 22, tr("Close", "Закрыть"), Act::ClosePanel, 0, Style::Normal, Font::Small);
}
