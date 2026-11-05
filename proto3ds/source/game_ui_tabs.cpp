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
        case Chat::ChannelNotice: return hudRgba(255, 170, 170);
        case Chat::Afk: case Chat::Dnd: case Chat::Ignored: return hudRgba(255, 128, 255);
        case Chat::Emote: case Chat::TextEmote: case Chat::MonsterEmote: return hudRgba(255, 128, 64);
        case Chat::Local: return kCyan;
        case Chat::Loot: return hudRgba(0, 170, 0);
        default: return kText;
    }
}

// Chat tabs: 0 all, 1 say (and other nearby speech), 2 group, 3 guild, 4 whispers, 5 channels, 6 system / combat / loot
bool chatInChannel(uint8_t type, int ch) {
    using namespace w3n;
    switch (ch) {
        case 1: return type == Chat::Say || type == Chat::Yell || type == Chat::Emote || type == Chat::TextEmote ||
                       type == Chat::MonsterSay || type == Chat::MonsterYell || type == Chat::MonsterEmote;
        case 2: return type == Chat::Party || type == Chat::Raid || type == Chat::RaidLeader || type == Chat::RaidWarning;
        case 3: return type == Chat::Guild || type == Chat::Officer;
        case 4: return type == Chat::Whisper || type == Chat::WhisperInform || type == Chat::MonsterWhisper || type == Chat::Afk ||
                       type == Chat::Dnd || type == Chat::Ignored;
        case 5: return type == Chat::Channel || type == Chat::ChannelNotice;
        case 6: return type == Chat::System || type == Chat::Local || type == Chat::Loot || type == Chat::Skill;
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
    const int stripH = g.pet.active ? 36 : 0;   // UI stage 13: the pet's buttons take the bottom
    const int limitY = kTabY - stripH;
    rows_.clear();
    list_.text(L, kTop + 2, tr("Group and nearby", "Группа и рядом"), kGold);
    int y = kTop + 14;
    // party members first
    for (const w3n::PartyMember& pm : g.party.members) {
        if (y > limitY - 26) break;
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
        if (y > limitY - 24) break;
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
        for (int s = 0; s < kPadSlots && ry + 20 <= limitY; ++s) {
            const ActionSlot& a = barsView_->at(s);
            if (a.type == ActionSlot::Empty || a.type > ActionSlot::Item) continue;
            const uint32_t key = a.type == ActionSlot::Spell ? a.id : (a.id | 0x80000000u);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            const bool item = a.type == ActionSlot::Item;   // UI stage 12: items too
            uint32_t cd = item ? g.itemCooldownLeftMs(a.id, in.nowMs) : g.cooldownLeftMs(a.id, in.nowMs);
            if (cd <= 1500) continue;   // the global cooldown is not news
            const std::string nm = item ? g.itemName(a.id) : g.spellNames.name(a.id);
            iconBox(R + 1, ry + 1, 16, item ? itemIcon(g, a.id) : spellIcon(a.id), nm, kFrameDim, true);
            list_.textFit(R + 22, ry + 2, nm, RW - 52, kText, false);
            list_.textRight(R + RW - 2, ry + 2, secs(cd), kYellow, false);
            ry += 20;
            ++shown;
        }
    }
    if (!shown) list_.text(R + 2, ry + 2, tr("all ready", "всё готово"), kGrey, false);
    if (g.cast.active) btn(R, limitY - 24, RW, 22, tr("Cancel the cast", "Прервать заклинание"), Act::CancelCast, 0, Style::Danger, Font::Small);
    petStrip(g, in);
}

// ------------------------------------------------------------------ the pet's buttons (UI stage 13)
// Orders (attack / follow / stay), stances (passive / defensive / aggressive), the pet's castable spells (the server's bar
// slots 3..6) and the button that opens the pet window. Tap = do it; drag onto the crossbars = bind it.
int GameUi::petStrip(w3n::GameSession& g, const Input& in) {
    if (!g.pet.active) return 0;
    const int y = kTabY - 31, cell = 26, gap = 2;
    list_.rect(2, y - 4, kTouchW - 4, 34, kRowAlt);
    static const uint32_t kCmds[6] = {kPetCmdAttack, kPetCmdFollow, kPetCmdStay, kPetCmdPassive, kPetCmdDefensive, kPetCmdAggressive};
    int x = 5;
    for (int i = 0; i < 6; ++i) {
        const uint32_t id = kCmds[i];
        const ActionSlot a{ActionSlot::PetCmd, id};
        iconBox(x, y, cell, actionIcon(g, a), petCmdName(id), petCmdActive(g, id) ? kGold : kFrameDim, !g.pet.enabled);
        Button& b = region(x, y, cell, cell, Act::PetCommand, (int)id);
        b.dragKind = 3;
        b.dragId = id;
        x += cell + gap + (i == 2 ? 4 : 0);
    }
    x += 4;
    for (int slot = 3; slot < 7 && x + cell < kTouchW - 34; ++slot) {
        const uint32_t sp = w3n::PetState::spellOf(g.pet.bar[slot]);
        const uint8_t ty = w3n::PetState::typeOf(g.pet.bar[slot]);
        if (!sp || ty == w3n::PetAct::Command || ty == w3n::PetAct::Reaction) continue;
        const uint32_t cd = g.petCooldownLeftMs(sp, in.nowMs);
        iconBox(x, y, cell, spellIcon(sp), g.spellNames.name(sp), ty == w3n::PetAct::Enabled ? kGreen : kFrameDim, cd > 1500 || ty == w3n::PetAct::Passive || !g.pet.enabled);
        if (cd > 1500) list_.textCentered(x + cell / 2, y + 8, secs(cd), kYellow, true);
        if (ty == w3n::PetAct::Enabled) list_.rect(x + cell - 6, y + 1, 5, 5, kYellow);   // auto-cast on
        if (ty != w3n::PetAct::Passive) {
            Button& b = region(x, y, cell, cell, Act::PetCast, (int)sp);
            b.dragKind = 4;
            b.dragId = sp;
            b.infoKind = 2;
            b.infoId = sp;
        }
        x += cell + gap;
    }
    btn(kTouchW - 34, y, 30, cell, "...", Act::PetOpen, 0, Style::Normal, Font::Small);
    return 36;
}


// ------------------------------------------------------------------ Map: the zone (or continent) with us, units, quest givers
void GameUi::drawMap(w3n::GameSession& g, const Input& in) {
    const int mx = 2, my = kTop + 2, mw = 258, mh = 172;
    // 6.69: our zone / our continent / the whole world / what a tap picked (mapMode_)
    ui::MapView mv;
    bool worldView = false;
    switch (mapMode_) {
        case 1: mv = assets().continent(in.mapId); break;
        case 2: mv = assets().world(); worldView = true; break;
        case 3: mv = mapPickArea_ ? assets().zoneByArea(mapPickArea_) : assets().continent(mapPickMap_); break;
        default: mv = assets().zoneAt(in.mapId, in.pos[0], in.pos[1], in.zone); break;
    }
    const bool worldStandIn = worldView && !mv.ok();   // an older pack: no picture of the world, two buttons on a plain panel
    list_.rect(mx, my, mw, mh, hudRgba(20, 26, 30));
    float pu = 0.5f, pv = 0.5f;
    auto worldToUv = [&](float x, float y, float* u, float* v) {
        if (!mv.ok() || mv.m->mapId != in.mapId) return false;   // (the world and the other continent show none of our position)
        *u = (mv.m->left - y) / (mv.m->left - mv.m->right);
        *v = (mv.m->top - x) / (mv.m->top - mv.m->bottom);
        return true;
    };
    // the visible window of the picture (zoom around us)
    float u0 = 0, v0 = 0, span = 1.0f / (float)mapZoom_;
    const bool onMap = mv.ok() && worldToUv(in.pos[0], in.pos[1], &pu, &pv) && pu >= 0 && pu <= 1 && pv >= 0 && pv <= 1;
    if (mv.ok()) {
        if (mapZoom_ > 1) {
            if (mapPanned_) { u0 = mapCu_ - span / 2; v0 = mapCv_ - span / 2; }   // 6.69: moved by the stylus
            else if (onMap) { u0 = pu - span / 2; v0 = pv - span / 2; }
            else { u0 = 0.5f - span / 2; v0 = 0.5f - span / 2; }
            u0 = std::clamp(u0, 0.0f, 1.0f - span);
            v0 = std::clamp(v0, 0.0f, 1.0f - span);
        }
        list_.imagePart(mx, my, mw, mh, mv.image, u0, v0, u0 + span, v0 + span);
    } else if (worldStandIn) {
        list_.rect(mx + mw / 2, my, 1, mh, hudRgba(60, 70, 80));
    } else {   // no map in the pack: a schematic around us, 60 yd to the edge, north up
        for (int k = 1; k < 4; ++k) {
            list_.rect(mx, my + mh * k / 4, mw, 1, hudRgba(40, 50, 56));
            list_.rect(mx + mw * k / 4, my, 1, mh, hudRgba(40, 50, 56));
        }
        char why[96];
        if (!assets().loaded())
            snprintf(why, sizeof why, "%s", tr("no ui.w3dp", "нет ui.w3dp"));
        else if (!assets().mapCount())
            snprintf(why, sizeof why, "%s", tr("ui.w3dp has no maps (log_ui: maps)", "в ui.w3dp нет карт (log_ui: maps)"));
        else
            snprintf(why, sizeof why, tr("no map #%u in ui.w3dp (%u maps)", "нет карты #%u в ui.w3dp (карт: %u)"),
                     (unsigned)in.mapId, (unsigned)assets().mapCount());
        list_.textCentered(mx + mw / 2, my + mh - 12, why, kGrey, false);
    }
    list_.outline(mx - 1, my - 1, mw + 2, mh + 2, kFrame);
    mapShown_ = mv.ok() ? mv.m : nullptr;
    mapU0_ = u0; mapV0_ = v0; mapSpan_ = mv.ok() ? span : 1.0f;
    mapRectX_ = mx; mapRectY_ = my; mapRectW_ = mw; mapRectH_ = mh;
    if (mv.ok() || worldStandIn) region(mx, my, mw, mh, Act::MapTap, 0);   // 6.69: taps open zones, a drag moves the picture
    // world -> screen
    const float yardsPerPx = 60.0f * 2 / mh;
    auto toScreen = [&](float x, float y, int* sx, int* sy) {
        if (mv.ok()) {
            float u = u0, v = v0;
            if (!worldToUv(x, y, &u, &v)) return false;
            *sx = mx + (int)((u - u0) / span * mw);
            *sy = my + (int)((v - v0) / span * mh);
        } else {   // north (+x) up, west (+y) left
            *sx = mx + mw / 2 - (int)((y - in.pos[1]) / yardsPerPx);
            *sy = my + mh / 2 - (int)((x - in.pos[0]) / yardsPerPx);
        }
        return *sx >= mx && *sx < mx + mw && *sy >= my && *sy < my + mh;
    };
    list_.clip(mx, my, mw, mh);
    // 6.63: the areas of the followed quests (Questie's data, questdb.bin): translucent discs in the quest's colour over what its
    // objectives ask for; a finished quest shows where to hand it in
    if (mapQuests_ && !watch_.ids().empty()) {
        if (!qdbTried_) { qdbTried_ = true; qdb_.open("sdmc:/3ds/w3dproto/questdb.bin"); }
        auto zoneToWorld = [&](const ui::MapView& zm, float px, float py, float* wx, float* wy) {
            *wy = zm.m->left - (px / 10000.0f) * (zm.m->left - zm.m->right);
            *wx = zm.m->top - (py / 10000.0f) * (zm.m->top - zm.m->bottom);
        };
        std::vector<std::pair<uint32_t, ui::MapView>> zones;   // (the lookups of this frame)
        auto zoneOf = [&](uint32_t area) -> const ui::MapView* {
            for (auto& z : zones) if (z.first == area) return z.second.ok() ? &z.second : nullptr;
            zones.push_back({area, assets().zoneByArea(area)});
            return zones.back().second.ok() ? &zones.back().second : nullptr;
        };
        auto disc = [&](int cx, int cy, int r, uint32_t col) {
            if (cx + r < mx || cx - r > mx + mw || cy + r < my || cy - r > my + mh) return;
            if (r <= 2) { list_.rect(cx - 1, cy - 1, 3, 3, col); return; }
            for (int dy = -r; dy <= r; dy += 2) {
                const int hw = (int)sqrtf((float)(r * r - dy * dy));
                list_.rect(cx - hw, cy + dy, 2 * hw + 1, 2, col);
            }
        };
        int drawn = 0;
        for (uint32_t qid : watch_.ids()) {
            const questdb::Quest* qd = questAreas(qid);
            if (!qd) continue;
            const int slot = watch_.slot(qid);
            const w3n::QuestLogEntry* le = nullptr;
            std::vector<w3n::QuestLogEntry> log = g.questLog();
            for (const w3n::QuestLogEntry& e : log) if (e.id == qid) { le = &e; break; }
            if (!le) continue;
            const w3n::QuestInfo* qi = g.questInfoIfKnown(qid);
            // which of the quest's own objectives (server slots 0..3) is this Questie objective, and is it done. By id first; when the ids
            // differ (a kill credit of another creature, a variant of the object) by position among the objectives of the same kind
            // (6.76: before, an objective that was not found by id counted as not done - so a finished stage kept its areas on the map)
            auto objectiveDone = [&](const questdb::Objective& ob) {
                if (le->complete) return true;
                if (!qi) return false;
                int slotOf = -1;
                std::vector<int> sameKind;   // the server's objectives of this kind, in slot order
                for (int j = 0; j < 4; ++j) {
                    const w3n::QuestInfo::Objective& so = qi->obj[j];
                    if (ob.kind == questdb::kItem) {
                        if (!so.item || !so.itemCount) continue;
                        sameKind.push_back(j);
                        if (so.item == ob.id) slotOf = j;
                    } else if (so.creatureOrGo && so.count) {
                        const bool go = (so.creatureOrGo & 0x80000000u) != 0;
                        if (go != (ob.kind == questdb::kObject)) continue;
                        sameKind.push_back(j);
                        if ((so.creatureOrGo & 0x7FFFFFFFu) == ob.id) slotOf = j;
                    }
                }
                if (slotOf < 0) {   // by position among Questie's objectives of the same kind
                    size_t idx = 0;
                    for (const questdb::Objective& o2 : qd->obj) {
                        if (&o2 == &ob) break;
                        if (o2.kind == ob.kind) ++idx;
                    }
                    if (idx < sameKind.size() && ob.kind != questdb::kExplore) slotOf = sameKind[idx];
                }
                if (slotOf < 0) return false;
                const w3n::QuestInfo::Objective& so = qi->obj[slotOf];
                if (ob.kind == questdb::kItem) return g.itemCount(so.item) >= so.itemCount;
                return le->kills[slotOf] >= so.count;
            };
            auto markAt = [&](const questdb::Pt& p, const char* glyph, uint32_t col) {
                float wx, wy; int sx, sy;
                if (qd->world) {   // version 2: the world position itself
                    if (p.area != in.mapId) return;
                    wx = p.x; wy = p.y;
                } else {
                    const ui::MapView* zm = zoneOf(p.area);
                    if (!zm || zm->m->mapId != in.mapId) return;
                    zoneToWorld(*zm, p.x, p.y, &wx, &wy);
                }
                if (toScreen(wx, wy, &sx, &sy)) { list_.rect(sx - 5, sy - 5, 11, 11, hudRgba(0, 0, 0, 170)); list_.textCentered(sx, sy - 6, glyph, col, true, Font::Normal); }
            };
            if (le->complete) {   // hand it in
                for (const questdb::Pt& p : qd->end) markAt(p, "?", kGold);
                continue;
            }
            for (const questdb::Objective& ob : qd->obj) {
                if (objectiveDone(ob)) continue;
                for (const questdb::Blob& b : ob.blobs) {
                    if (++drawn > 500) break;
                    float wx, wy, rYards; int sx, sy, ex, ey;
                    if (qd->world) {
                        if (b.area != in.mapId) continue;
                        wx = b.x; wy = b.y; rYards = b.r;
                    } else {
                        const ui::MapView* zm = zoneOf(b.area);
                        if (!zm || zm->m->mapId != in.mapId) continue;
                        zoneToWorld(*zm, b.x, b.y, &wx, &wy);
                        rYards = (b.r / 10000.0f) * (zm->m->left - zm->m->right);
                    }
                    toScreen(wx, wy, &sx, &sy);
                    toScreen(wx, wy - rYards, &ex, &ey);
                    const int r = std::clamp((int)lroundf(sqrtf((float)((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy)))), 2, 40);
                    disc(sx, sy, r, questSlotColor(slot, 85));
                    if (ob.kind != questdb::kKill || r <= 5) list_.rect(sx - 1, sy - 1, 3, 3, questSlotColor(slot));   // (a dot marks the middle)
                }
            }
        }
    }
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
        } else if (mapUnits_ || !mv.ok()) {   // 6.63: the creatures' dots only when asked for (the Units button)
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
    // our corpse (the place of death): a little skull, while dead or a ghost
    if ((g.life() == w3n::Life::Ghost || g.life() == w3n::Life::Dead) && g.corpse.known && g.corpse.map == in.mapId) {
        int sx, sy;
        if (toScreen(g.corpse.x, g.corpse.y, &sx, &sy)) {
            static const char* const kSkull[8] = {
                "..XXXXX..", ".XXXXXXX.", ".XOOXOOX.", ".XOOXOOX.",
                ".XXXXXXX.", "..XXOXX..", "..XXXXX..", "..X.X.X.."};
            list_.rect(sx - 5, sy - 5, 11, 10, kBlack);
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 9; c++) {
                    char ch = kSkull[r][c];
                    if (ch == 'X') list_.rect(sx - 4 + c, sy - 4 + r, 1, 1, kWhite);
                    else if (ch == 'O') list_.rect(sx - 4 + c, sy - 4 + r, 1, 1, kBlack);
                }
        }
    }
    // 6.82: the spirit healer (a gold cross) while a ghost
    if (g.life() == w3n::Life::Ghost && g.healer.known && g.healer.map == in.mapId) {
        int sx, sy;
        if (toScreen(g.healer.x, g.healer.y, &sx, &sy)) {
            list_.rect(sx - 4, sy - 1, 9, 3, kGold);
            list_.rect(sx - 1, sy - 4, 3, 9, kGold);
            list_.outline(sx - 5, sy - 5, 11, 11, kBlack);
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
    // 6.69: the world: the two continents' names (ours in gold) over the picture, the stylus on one lights it
    if (worldView) {
        const bool down = press_.active && press_.onButton && press_.button.act == Act::MapTap;
        const int px = down ? (press_.x < mx + mw / 2 ? 0 : 1) : -1;
        const char* nm[2] = {tr("Kalimdor", "Калимдор"), tr("Eastern Kingdoms", "Восточные королевства")};
        const uint32_t mine = in.mapId == 1 ? 0 : 1;
        for (int k = 0; k < 2; ++k) {
            const int cx = mx + (k == 0 ? mw / 4 : mw * 3 / 4);
            if (px == k) list_.rect(mx + (k == 0 ? 0 : mw / 2), my, mw / 2, mh, hudRgba(255, 220, 120, 50));
            const int w = HudList::width(nm[k], Font::Normal);
            list_.rect(cx - w / 2 - 4, my + mh / 2 - 8, w + 8, 16, hudRgba(0, 0, 0, 150));
            list_.textCentered(cx, my + mh / 2 - 7, nm[k], (uint32_t)k == mine && (in.mapId == 0 || in.mapId == 1) ? kGold : kText, true, Font::Normal);
        }
    } else if (mv.ok() && mv.m->areaId == 0) {
        // a continent: while the stylus is down on it, the zone under it is outlined with its name
        if (press_.active && press_.onButton && press_.button.act == Act::MapTap && !press_.moved) {
            const float u = u0 + (float)(press_.x - mx) / mw * span, v = v0 + (float)(press_.y - my) / mh * span;
            const float wy = mv.m->left - u * (mv.m->left - mv.m->right), wx = mv.m->top - v * (mv.m->top - mv.m->bottom);
            uint32_t shapeZone = 0;
            ui::MapView z = zoneUnder(in, mv.m->mapId, wx, wy, &shapeZone);
            if (z.ok() && z.m->areaId && shapeZone) {
                // 7.04: the zone as it is shaped (as the original lights it): the map's pixels whose terrain belongs to it,
                // sampled every 2 px and joined into runs per row, with a gold edge where the zone ends
                const int st = 2, cols = (mw + st - 1) / st, rows = (mh + st - 1) / st;
                std::vector<uint8_t> in2((size_t)cols * rows, 0);
                for (int r = 0; r < rows; ++r) {
                    const float vv = v0 + (r * st + st * 0.5f) / mh * span;
                    const float px = mv.m->top - vv * (mv.m->top - mv.m->bottom);
                    for (int c = 0; c < cols; ++c) {
                        const float uu = u0 + (c * st + st * 0.5f) / mw * span;
                        const float py = mv.m->left - uu * (mv.m->left - mv.m->right);
                        in2[(size_t)r * cols + c] = in.zoneAtPoint(mv.m->mapId, px, py) == shapeZone;
                    }
                }
                auto at = [&](int c, int r) { return c >= 0 && r >= 0 && c < cols && r < rows && in2[(size_t)r * cols + c]; };
                for (int r = 0; r < rows; ++r)
                    for (int c = 0; c < cols;) {
                        if (!at(c, r)) { ++c; continue; }
                        int e = c;
                        while (at(e, r)) ++e;
                        list_.rect(mx + c * st, my + r * st, (e - c) * st, st, hudRgba(255, 220, 120, 60));
                        for (int k = c; k < e; ++k)   // the edge: a cell with a neighbour outside the zone
                            if (!at(k - 1, r) || !at(k + 1, r) || !at(k, r - 1) || !at(k, r + 1))
                                list_.rect(mx + k * st, my + r * st, st, st, kGold);
                        c = e;
                    }
            } else if (z.ok() && z.m->areaId) {   // the terrain's areas are not known here: the zone's rectangle, as before
                auto edge = [&](float zx, float zy, int* sx, int* sy) {
                    *sx = mx + (int)(((mv.m->left - zy) / (mv.m->left - mv.m->right) - u0) / span * mw);
                    *sy = my + (int)(((mv.m->top - zx) / (mv.m->top - mv.m->bottom) - v0) / span * mh);
                };
                int ax, ay, bx2, by2;
                edge(z.m->top, z.m->left, &ax, &ay);
                edge(z.m->bottom, z.m->right, &bx2, &by2);
                list_.rect(ax, ay, bx2 - ax, by2 - ay, hudRgba(255, 220, 120, 45));
                list_.outline(ax, ay, bx2 - ax, by2 - ay, kGold);
            }
            if (z.ok() && z.m->areaId) {
                const int w = HudList::width(z.name, Font::Normal);
                list_.rect(mx + mw / 2 - w / 2 - 4, my + mh - 18, w + 8, 16, hudRgba(0, 0, 0, 170));
                list_.textCentered(mx + mw / 2, my + mh - 17, z.name, kGold, true, Font::Normal);
            }
        }
    }
    list_.noClip();
    list_.clip(0, kTop, kTouchW, kContentH);
    // zone name and the tracked quest over the map
    if (mv.ok() || worldStandIn) {
        std::string title = mv.ok() ? mv.name : std::string("World");
        if (worldView) title = tr("World", "Мир");
        else if (mv.m->areaId == 0 && mv.m->mapId == 0) title = tr("Eastern Kingdoms", "Восточные королевства");
        else if (mv.m->areaId == 0 && mv.m->mapId == 1) title = tr("Kalimdor", "Калимдор");
        int w = HudList::width(title, Font::Normal);
        list_.rect(mx, my, w + 8, 16, hudRgba(0, 0, 0, 140));
        list_.text(mx + 4, my + 1, title, kGold, true, Font::Normal);
    }
    if (mapQuests_ && !watch_.ids().empty()) {   // 6.63: the legend of the followed quests
        const int n = (int)watch_.ids().size();
        const int ty = my + mh - n * 12 - 2;
        list_.rect(mx, ty - 1, mw, n * 12 + 3, hudRgba(0, 0, 0, 150));
        int k = 0;
        for (uint32_t qid : watch_.ids()) {
            bool done = false;
            for (const w3n::QuestLogEntry& e : g.questLog()) if (e.id == qid) done = e.complete;
            list_.rect(mx + 3, ty + 12 * k + 2, 8, 8, questSlotColor(k));
            list_.textFit(mx + 15, ty + 12 * k, g.questTitleKnown(qid), mw - 20, done ? kGreen : kText, true);
            ++k;
        }
    }
    // controls
    const int bx = 266, bw = 50;
    // 6.69: Zone (ours) / Continent / World; a tap on the world picks a continent, a tap on a continent a zone; "Zone" comes back
    btn(bx, kTop + 2, bw, 19, tr("Zone", "Зона"), Act::MapMode, 0, mapMode_ == 0 ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(bx, kTop + 22, bw, 19, tr("Contin.", "Контин."), Act::MapMode, 1, mapMode_ == 1 || (mapMode_ == 3 && !mapPickArea_) ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(bx, kTop + 42, bw, 19, tr("World", "Мир"), Act::MapMode, 2, mapMode_ == 2 ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(bx, kTop + 63, bw, 20, "+", Act::MapZoom, mapZoom_ * 2, mapZoom_ < 4 ? Style::Normal : Style::Disabled, Font::Large);
    btn(bx, kTop + 84, bw, 20, "-", Act::MapZoom, mapZoom_ / 2, mapZoom_ > 1 ? Style::Normal : Style::Disabled, Font::Large);
    char b[32];
    if (onMap) snprintf(b, sizeof b, "%.1f %.1f", pu * 100, pv * 100);
    else if (mv.ok() && mv.m->mapId == in.mapId) snprintf(b, sizeof b, "%.0f %.0f", in.pos[0], in.pos[1]);
    else b[0] = 0;
    list_.textCentered(bx + bw / 2, kTop + 107, b, kText);
    snprintf(b, sizeof b, mapZoom_ > 1 ? tr("x%d drag", "x%d тяни") : "x%d", mapZoom_);
    list_.textCentered(bx + bw / 2, kTop + 118, b, kGrey);
    // 6.63: what the map shows: the creatures' dots (off: they hid the map), the followed quests' areas
    btn(bx, kTop + 130, bw, 20, tr("Mobs", "Мобы"), Act::MapUnits, mapUnits_ ? 0 : 1, mapUnits_ ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(bx, kTop + 152, bw, 20, tr("Quests", "Задан."), Act::MapQuests, mapQuests_ ? 0 : 1, mapQuests_ ? Style::ToggleOn : Style::Toggle, Font::Small);
    if (g.newMail) {
        const int w = HudList::width(tr("Mail!", "Почта!"), Font::Normal);
        list_.rect(mx + mw - w - 8, my, w + 8, 16, hudRgba(0, 0, 0, 140));
        list_.text(mx + mw - w - 4, my + 1, tr("Mail!", "Почта!"), kYellow, true, Font::Normal);
    }
}

// ------------------------------------------------------------------ Chat: channel tabs, lines, keyboard, quick phrases
void GameUi::drawChat(w3n::GameSession& g, const Input& in) {
    (void)in;
    const char* names[7] = {tr("All", "Все"), tr("Say", "Рядом"), tr("Group", "Группа"), tr("Guild", "Гильд."), tr("Whisp.", "Шёпот"),
                            tr("Chan.", "Кан."), tr("System", "Систем.")};
    for (int i = 0; i < 7; ++i)
        btn(2 + i * 45, kTop + 2, 43, 18, names[i], Act::ChatChannel, i, chatChannel_ == i ? Style::ToggleOn : Style::Toggle, Font::Small);
    const Font f = listFont();
    const int lh = HudList::lineHeight(f), top = kTop + 24, bottom = kTabY - 28, rows = (bottom - top) / lh;
    // newest lines first, wrapped
    struct Line { uint32_t color; std::string text; uint32_t item = 0; };
    std::vector<Line> lines;
    const int want = rows + chatScroll_;
    for (size_t i = g.chat.size(); i-- > 0 && (int)lines.size() < want;) {
        const w3n::ChatLine& l = g.chat[i];
        if (!chatInChannel(l.type, chatChannel_)) continue;
        // 6.91: the line's items - links (|Hitem:N|h[Name]|h, shown without their codes) or our own loot lines (the entry kept with them)
        const std::string raw = g.lineText(l);
        std::vector<w3n::ItemLink> links = w3n::itemLinks(raw);
        if (links.empty() && l.type == w3n::Chat::Loot && !l.channel.empty()) {
            w3n::ItemLink k;
            k.entry = (uint32_t)strtoul(l.channel.c_str(), nullptr, 10);
            const size_t a = raw.find('['), b = raw.find(']', a == std::string::npos ? 0 : a);
            if (a != std::string::npos && b != std::string::npos) k.shown = raw.substr(a, b - a + 1);
            if (k.entry) links.push_back(k);
        }
        std::vector<std::string> w = HudList::wrap(trText(w3n::stripWowCodes(raw)), kTouchW - 10, f);
        // which item a wrapped row belongs to: the first link whose name (or its start / its end, when it is cut by the wrap) is on it
        auto itemOfRow = [&](const std::string& row) -> uint32_t {
            if (links.size() == 1) return links[0].entry;   // 6.95: one item in the line: every row of it opens that item (a wrap cannot lose it)
            for (const w3n::ItemLink& k : links) {
                if (k.shown.empty()) continue;
                const size_t n = std::min<size_t>(k.shown.size(), 6);
                if (row.find(k.shown) != std::string::npos || row.find(k.shown.substr(0, n)) != std::string::npos ||
                    row.find(k.shown.substr(k.shown.size() - n)) != std::string::npos)
                    return k.entry;
            }
            return 0;
        };
        for (size_t k = w.size(); k-- > 0 && (int)lines.size() < want;) lines.push_back({chatColor(l.type), (k ? "  " : "") + w[k], itemOfRow(w[k])});
    }
    const int maxScroll = std::max(0, (int)lines.size() - rows);
    chatScroll_ = std::min(chatScroll_, maxScroll);
    list_.rect(2, top - 2, kTouchW - 4, bottom - top + 4, hudRgba(0, 0, 0, 90));
    for (int r = 0; r < rows; ++r) {
        int k = chatScroll_ + (rows - 1 - r);
        if (k < (int)lines.size()) {
            list_.text(5, top + r * lh, lines[k].text, lines[k].color, true, f);
            if (lines[k].item) {   // 6.91: a row with an item: underlined like a link, a tap shows the item
                list_.rect(5, top + r * lh + lh - 2, std::min(kTouchW - 10, HudList::width(lines[k].text, f)), 1, kCyan);
                region(2, top + r * lh, kTouchW - 4, lh, Act::ItemCard, (int)lines[k].item);
            }
        }
    }
    if (lines.empty()) list_.text(8, top + 4, tr("No messages.", "Сообщений нет."), kGrey);
    const int by = kTabY - 24;
    {   // the destination of Write (tap: the next one) - if it went away (left the group, the channel), say
        const std::vector<std::string> ds = chatDests(g);
        if (std::find(ds.begin(), ds.end(), chatDest_) == ds.end()) chatDest_ = "s";
    }
    btn(2, by, 64, 22, tr("Write", "Писать"), Act::ChatWrite, chatChannel_, Style::Primary, Font::Small);
    btn(68, by, 100, 22, chatDestLabel(g, chatDest_), Act::ChatDest, 0, Style::Normal, Font::Small);
    btn(170, by, 56, 22, tr("More", "Ещё"), Act::ChatPhrases, 0, phrases_ ? Style::ToggleOn : Style::Normal, Font::Small);
    btn(230, by, 42, 22, "▲", Act::ChatScroll, 3, chatScroll_ < maxScroll ? Style::Normal : Style::Disabled);
    btn(274, by, 42, 22, "▼", Act::ChatScroll, -3, chatScroll_ > 0 ? Style::Normal : Style::Disabled);
    if (phrases_) {   // quick phrases over the lines
        window(20, kTop + 22, kTouchW - 40, 128, true);
        // two pages: phrases, and (the animation stage) emotes (CMSG_TEXT_EMOTE) with sitting down / standing up
        btn(28, kTop + 27, 80, 18, tr("Phrases", "Фразы"), Act::EmotePick, 2000, phrasePage_ == 0 ? Style::ToggleOn : Style::Toggle, Font::Small);
        btn(112, kTop + 27, 80, 18, tr("Emotes", "Эмоции"), Act::EmotePick, 2001, phrasePage_ == 1 ? Style::ToggleOn : Style::Toggle, Font::Small);
        btn(196, kTop + 27, 80, 18, tr("Channels", "Каналы"), Act::EmotePick, 2002, phrasePage_ == 2 ? Style::ToggleOn : Style::Toggle, Font::Small);
        if (phrasePage_ == 2) {   // stage 5.46: the joined channels: write, list who is in, leave; and join another
            const std::vector<std::string>& ch = g.channelSlots();
            int row = 0;
            for (size_t i = 0; i < ch.size() && row < 4; ++i) {
                if (ch[i].empty()) continue;
                const int y = kTop + 49 + row * 20, n = (int)i + 1;
                std::string nm = ch[i];
                size_t dash = nm.find(" - ");
                if (dash != std::string::npos) nm.resize(dash);
                list_.text(30, y + 3, std::to_string(n) + ". " + nm, kText, true, Font::Small);
                btn(138, y, 48, 18, tr("Write", "Писать"), Act::ChannelSay, n, Style::Normal, Font::Small);
                btn(190, y, 40, 18, tr("Who", "Кто"), Act::ChannelList, n, Style::Normal, Font::Small);
                btn(234, y, 42, 18, tr("Leave", "Выйти"), Act::ChannelLeave, n, Style::Normal, Font::Small);
                ++row;
            }
            if (!row) list_.text(30, kTop + 56, tr("Not in any channel.", "Вы не в каналах."), kGrey, true, Font::Small);
            btn(28, kTop + 131, 120, 18, tr("Join channel...", "Войти в канал..."), Act::ChannelJoin, 0, Style::Primary, Font::Small);
        } else if (phrasePage_ == 0) {
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
    btn(164, kTop + 2, 74, 18, bagLabel(g, bagFilter_), Act::SubView, 10, bagFilter_ >= 0 ? Style::ToggleOn : Style::Toggle, Font::Small);   // 6.95
    if (sel) {
        const w3n::ItemInfo* info = g.itemInfoIfKnown(sel->entry);
        list_.textFit(242, kTop + 5, g.itemName(sel->entry), kTouchW - 244, qualityColor(info ? info->quality : 1));
    }
    if (items.empty()) list_.text(10, kTop + 30, tr("Your bags are empty.", "Ваши сумки пусты."), kGrey, true, Font::Normal);
    for (int k = 0; k < per; ++k) {
        int idx = page_ * per + k;
        if (idx >= (int)items.size()) break;
        const w3n::InvItem& it = items[idx];
        const int x = 6 + (k % cols) * cell, y = kTop + 24 + (k / cols) * cell;
        const w3n::ItemInfo* info = g.itemInfo(it.entry);
        iconBox(x + 1, y + 1, cell - 4, itemIcon(g, it.entry), g.itemName(it.entry), qualityColor(info ? info->quality : 1));
        if (const uint32_t cd = g.itemCooldownLeftMs(it.entry, in.nowMs)) {   // UI stage 12: potions, stones, trinkets
            const uint32_t total = std::max(cd, g.itemCooldownTotalMs(it.entry));
            const int h = (int)((cell - 4) * (float)cd / (float)total + 0.5f);
            list_.rect(x + 1, y + 1 + (cell - 4 - h), cell - 4, h, hudRgba(0, 0, 0, 160));
            list_.textCentered(x + cell / 2 - 1, y + cell / 2 - 7, secs(cd), cd <= 5000 ? kRed : kYellow, true);
        }
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
void GameUi::spellTabs(int active) {
    btn(2, kTop + 2, 72, 18, tr("Book", "Книга"), Act::SubView, 0, active == 0 ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(76, kTop + 2, 72, 18, tr("Talents", "Таланты"), Act::SubView, 1, active == 1 ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(150, kTop + 2, 72, 18, tr("Skills", "Навыки"), Act::SubView, 2, active == 2 ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(224, kTop + 2, 78, 18, tr("Crafting", "Ремёсла"), Act::SubView, 3, active == 3 ? Style::ToggleOn : Style::Toggle, Font::Small);
}

void GameUi::drawSpells(w3n::GameSession& g, const Input& in) {
    spellTabs(0);   // (7.03: the bar page is written on the bottom row - up here the Crafting tab covered it)
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
        const bool pass = g.spellNames.passive(s);
        if (pass && rk.empty()) rk = tr("Passive", "Пассивное");
        else if (pass) rk += std::string(" · ") + tr("Passive", "Пассивное");
        if (f == Font::Small) list_.text(x + 27, y + 11, cd > 1500 ? secs(cd) : rk, cd > 1500 ? kYellow : kGrey, false);
        Button& b = region(x, y, 156, rowH - 2, Act::BindSpell, (int)s);
        b.infoKind = 2;
        b.infoId = s;
        if (!pass) { b.dragKind = 1; b.dragId = s; }   // 6.95: no dragging a passive onto a bar
    }
    const int by = kTabY - 24;
    btn(2, by, 120, 22, tr("Clear a button", "Очистить кнопку"), Act::ClearSlot, 0, Style::Normal, Font::Small);
    if (barsView_) {
        char b[48];
        snprintf(b, sizeof b, "%s %d/%d", tr("bar", "панель"), barsView_->page + 1, ActionLayout::kPages);
        list_.textCentered(pages > 1 ? 175 : 220, by + 6, b, kGrey);
    }
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ Talents: the class's trees (talents.txt)
void GameUi::drawTalents(w3n::GameSession& g, const Input& in) {
    (void)in;
    spellTabs(1);
    char b[64];
    snprintf(b, sizeof b, "%s %u", tr("points:", "очки:"), (unsigned)g.talentPoints());
    list_.textRight(kTouchW - 6, kTop + 25, b, g.talentPoints() ? kGreen : kGrey);   // (7.03: beside the trees, not under the tabs)
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
    {   // 6.20: what the current rank does (rank 1 when none is learned yet), then the next rank's text
        const std::string d = g.spellDesc.text(shown);
        for (const std::string& l : HudList::wrap(d, iw - 12, Font::Small)) {
            if (ty + 10 > kTabY - 50) break;
            list_.text(ix + 6, ty, l, kText, false, Font::Small);
            ty += HudList::lineHeight(Font::Small);
        }
        ty += 2;
    }
    if (rank < maxr && sel->rankSpell[rank]) {
        list_.textFit(ix + 6, ty + 2, std::string(tr("Next: ", "Далее: ")) + spellLabel(g, sel->rankSpell[rank]), iw - 12, kGrey);
        ty += 12;
        const std::string d = rank > 0 ? g.spellDesc.text(sel->rankSpell[rank]) : std::string();
        for (const std::string& l : HudList::wrap(d, iw - 12, Font::Small)) {
            if (ty + 10 > kTabY - 34) break;
            list_.text(ix + 6, ty, l, kGrey, false, Font::Small);
            ty += HudList::lineHeight(Font::Small);
        }
    }
    const bool can = rank < maxr && g.talentPoints() > 0;
    btn(ix + 6, kTabY - 30, iw - 12, 24, tr("Learn", "Изучить"), Act::TalentLearn, 0, can ? Style::Primary : Style::Disabled);
}

// ------------------------------------------------------------------ Skills (6.19): weapon skills, professions, languages ... like the original's page
// Groups in the original's order; a header folds its group away. A bar per skill: value / maximum (+ bonus), the rank title of a
// profession ("Journeyman"); a tap selects a skill and shows its description; a primary profession can be unlearned.
void GameUi::drawSkills(w3n::GameSession& g, const Input& in) {
    (void)in;
    spellTabs(2);
    struct Group { int cat; const char* en; const char* ru; };
    static const Group kGroups[] = {
        {7, "Class Skills", "Классовые навыки"}, {11, "Professions", "Профессии"}, {9, "Secondary Skills", "Вторичные навыки"},
        {6, "Weapon Skills", "Оружейные навыки"}, {8, "Armor Proficiencies", "Владение доспехами"}, {10, "Languages", "Языки"},
    };
    const std::vector<w3n::GameSession::SkillState> mine = g.playerSkills();
    if (!g.skills.loaded()) {
        window(10, kTop + 30, kTouchW - 20, 70);
        list_.textCentered(kTouchW / 2, kTop + 44, tr("No skills.txt on the SD card", "Нет skills.txt на SD-карте"), kText, true, Font::Normal);
        list_.textCentered(kTouchW / 2, kTop + 64, tr("(w3dconv --ui or --texts-only writes it)", "(его пишет w3dconv --ui или --texts-only)"), kGrey);
        return;
    }
    struct Row { bool header; int cat; int count; const w3n::GameSession::SkillState* s; const w3n::SkillLine* line; };
    std::vector<Row> rows;
    for (const Group& gr : kGroups) {
        std::vector<Row> items;
        for (const w3n::GameSession::SkillState& s : mine) {
            const w3n::SkillLine* l = g.skills.find(s.id);
            if (l && l->category == gr.cat) items.push_back({false, gr.cat, 0, &s, l});
        }
        if (items.empty()) continue;
        rows.push_back({true, gr.cat, (int)items.size(), nullptr, nullptr});
        if (!(skillFolded_ & (1u << gr.cat))) rows.insert(rows.end(), items.begin(), items.end());
    }
    if (rows.empty()) {
        list_.text(8, kTop + 30, tr("No skills yet.", "Навыков пока нет."), kGrey);
        return;
    }
    const int per = 6, rowH = 14, top = kTop + 22;
    const int pages = std::max(1, ((int)rows.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    const w3n::GameSession::SkillState* sel = nullptr;
    const w3n::SkillLine* selLine = nullptr;
    char b[64];
    for (int k = 0; k < per; ++k) {
        const int idx = page_ * per + k;
        if (idx >= (int)rows.size()) break;
        const Row& r = rows[idx];
        const int y = top + k * rowH;
        if (r.header) {
            const Group* gr = nullptr;
            for (const Group& c : kGroups) if (c.cat == r.cat) gr = &c;
            const bool folded = (skillFolded_ & (1u << r.cat)) != 0;
            list_.rect(2, y, kTouchW - 4, rowH - 1, kRowSel);
            snprintf(b, sizeof b, "%s %s (%d)", folded ? "+" : "-", tr(gr->en, gr->ru), r.count);
            list_.text(6, y + 2, b, kGold, true, Font::Small);
            region(2, y, kTouchW - 4, rowH - 1, Act::SkillHeader, r.cat);
            continue;
        }
        const bool isSel = skillSel_ == r.s->id;
        if (isSel) { sel = r.s; selLine = r.line; }
        list_.rect(2, y, kTouchW - 4, rowH - 1, isSel ? kRowSel : (k % 2 ? kRowAlt : kRow));
        list_.textFit(8, y + 2, r.line->name, 112, kText, false, Font::Small);
        const int bx = 124, bw = kTouchW - 4 - bx - 2;
        if (r.s->max > 1) {
            const int bonus = r.s->tempBonus + r.s->permBonus;
            const bool maxed = r.s->value >= r.s->max;
            const float fill = std::min(1.0f, (float)r.s->value / (float)std::max<int>(1, r.s->max));
            list_.bar(bx, y + 1, bw, rowH - 3, fill, maxed ? hudRgba(40, 120, 50) : hudRgba(36, 84, 150));
            const int tier = w3n::skillRankTier(r.s->max);
            static const char* kTierEn[] = {"", "Apprentice", "Journeyman", "Expert", "Artisan", "Master"};
            static const char* kTierRu[] = {"", "Ученик", "Подмастерье", "Умелец", "Искусник", "Мастер"};
            const bool titled = (r.cat == 11 || r.cat == 9) && tier;
            std::string num = std::to_string(r.s->value);
            if (bonus) num += (bonus > 0 ? " (+" : " (") + std::to_string(bonus) + ")";
            num += " / " + std::to_string(r.s->max);
            if (titled) list_.text(bx + 4, y + 2, tr(kTierEn[tier], kTierRu[tier]), kText, true, Font::Small);
            list_.textRight(bx + bw - 4, y + 2, num, bonus > 0 ? kGreen : kText, true, Font::Small);
        }
        region(2, y, kTouchW - 4, rowH - 1, Act::SkillPick, (int)r.s->id);
    }
    // the selected skill: its description
    const int dy = top + per * rowH + 2, by = kTabY - 24;
    window(2, dy, kTouchW - 4, by - dy - 2);
    if (sel && selLine) {
        std::string head = selLine->name;
        if (sel->max > 1) head += "  " + std::to_string(sel->value) + " / " + std::to_string(sel->max);
        list_.textFit(7, dy + 3, head, kTouchW - 16, kGold, true, Font::Small);
        int ty = dy + 15;
        const std::string d = selLine->description.empty() ? std::string(tr("No description.", "Нет описания.")) : selLine->description;
        for (const std::string& l : HudList::wrap(d, kTouchW - 16, Font::Small)) {
            if (ty + 10 > by - 2) break;
            list_.text(7, ty, l, kText, false, Font::Small);
            ty += 11;
        }
    } else {
        list_.text(7, dy + 5, tr("Tap a skill for its description. Tap a title to fold the group.", "Нажмите навык — описание. Заголовок сворачивает группу."),
                   kGrey, false, Font::Small);
    }
    if (selLine && selLine->category == 11) btn(2, by, 110, 22, tr("Unlearn", "Забыть навык"), Act::SkillAbandon, 0, Style::Danger, Font::Small);
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ Crafting (6.38): the profession window
// The professions we have recipes for (recipes.txt) as chips, the known recipes of the chosen one coloured by difficulty, and
// for the chosen recipe: what it makes, the tools and the reagents with what the bags hold. Create casts the recipe once, Create
// All as often as the reagents allow (one cast after another); an enchant asks which item to put it on.
namespace {
uint32_t craftColour(int difficulty) {
    static const uint32_t c[4] = {hudRgba(255, 128, 64), hudRgba(255, 210, 0), hudRgba(64, 192, 64), hudRgba(150, 150, 150)};
    return c[difficulty & 3];
}
}  // namespace

// short names of the equipment slots for the enchant picker (under a 42 px icon)
const char* equipSlotShort(uint8_t slot) {
    static const char* en[19] = {"Head", "Neck", "Shoul", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrist", "Hands",
                                 "Ring", "Ring", "Trink", "Trink", "Back", "Main", "Off", "Range", "Tabard"};
    static const char* ru[19] = {"Голова", "Шея", "Плечи", "Рубаха", "Грудь", "Пояс", "Ноги", "Ступни", "Запяст", "Кисти",
                                 "Кольцо", "Кольцо", "Аксес", "Аксес", "Спина", "Пр.рук", "Лев.рук", "Дальн", "Герб"};
    return slot < 19 ? tr(en[slot], ru[slot]) : "";
}

// 6.90: what an enchant can go on: the worn items first (the original lets you click them on the character), then the bags;
// only those its spell fits (a weapon enchant: weapons; "Enchant Boots": feet ...). Bags (18) and ammunition are never offered.
std::vector<w3n::InvItem> enchantTargets(w3n::GameSession& g, const w3n::Recipe* r) {
    std::vector<w3n::InvItem> worn, bags;
    for (const w3n::InvItem& it : g.inventory()) {
        if (it.isBagSlot()) continue;
        const w3n::ItemInfo* info = g.itemInfo(it.entry);
        if (!info || !info->equippable() || info->inventoryType == 18 || info->inventoryType == 24) continue;
        if (r && !r->fits(info->cls, info->subclass, info->inventoryType)) continue;
        (it.equipped() ? worn : bags).push_back(it);
    }
    std::stable_sort(worn.begin(), worn.end(), [](const w3n::InvItem& a, const w3n::InvItem& b) { return a.slot < b.slot; });
    worn.insert(worn.end(), bags.begin(), bags.end());
    return worn;
}

bool craftHasMaterials(w3n::GameSession& g, const w3n::Recipe& r) {
    for (const w3n::Recipe::Part& p : r.reagents)
        if (g.itemCount(p.entry) < p.count) return false;
    for (const w3n::Recipe::Part& t : r.tools)   // tools are carried, or worn (a fishing pole in hand, a skinning knife ...)
        if (!g.itemCount(t.entry)) {
            bool worn = false;
            for (const w3n::InvItem& it : g.inventory()) worn = worn || (it.equipped() && it.entry == t.entry);
            if (!worn) return false;
        }
    return true;
}

bool GameUi::craftCast(w3n::GameSession& g, const Input& in, uint32_t spell, uint64_t itemGuid) {
    craftCastMs_ = in.nowMs ? in.nowMs : 1;
    craftLastMs_ = craftCastMs_;
    return itemGuid ? g.castSpellOnItem(spell, itemGuid, in.nowMs) : g.castSpell(spell, in.nowMs);
}

void GameUi::craftTick(w3n::GameSession& g, const Input& in) {
    if (craftLeft_ <= 0) return;
    const w3n::Recipe* r = g.recipes.find(craftSel_);
    bool stop = !r || r->kind != 0 || g.life() != w3n::Life::Alive;
    if (!stop && g.lastCastFailMs && g.lastCastFailMs >= craftCastMs_) stop = true;   // the last piece failed (moved, no reagents ...)
    if (!stop && g.cast.active) return;                                               // still casting
    if (!stop && in.nowMs - craftLastMs_ < 250) return;
    if (!stop)
        for (const w3n::Recipe::Part& p : r->reagents)
            if (g.itemCount(p.entry) < p.count) stop = true;
    if (!stop && !craftCast(g, in, r->spell, 0)) stop = true;
    if (stop) { craftLeft_ = 0; dirty_ = true; return; }
    --craftLeft_;
    dirty_ = true;
}

void GameUi::drawCraft(w3n::GameSession& g, const Input& in) {
    spellTabs(3);
    if (!g.recipes.loaded()) {
        window(10, kTop + 30, kTouchW - 20, 70);
        list_.textCentered(kTouchW / 2, kTop + 44, tr("No recipes.txt on the SD card", "Нет recipes.txt на SD-карте"), kText, true, Font::Normal);
        list_.textCentered(kTouchW / 2, kTop + 64, tr("(w3dconv --ui or --texts-only writes it)", "(его пишет w3dconv --ui или --texts-only)"), kGrey);
        return;
    }
    std::vector<w3n::GameSession::SkillState> lines;
    for (const w3n::GameSession::SkillState& s : g.playerSkills())
        if (g.recipes.anyFor(s.id) && !g.knownRecipes(s.id).empty()) lines.push_back(s);
    if (lines.empty()) {
        list_.text(8, kTop + 30, tr("You know no crafting recipes.", "Вы не знаете ни одного рецепта."), kGrey, true, Font::Normal);
        list_.text(8, kTop + 46, tr("Learn a profession from a trainer.", "Выучите профессию у учителя."), kGrey, false, Font::Small);
        return;
    }
    bool have = false;
    for (const auto& s : lines) have = have || s.id == craftSkill_;
    if (!have) { craftSkill_ = lines[0].id; craftSel_ = 0; page_ = 0; }
    const w3n::GameSession::SkillState* sk = nullptr;
    const int chipW = (kTouchW - 4) / (int)std::min<size_t>(lines.size(), 5);
    for (size_t i = 0; i < lines.size() && i < 5; ++i) {
        const w3n::SkillLine* l = g.skills.find(lines[i].id);
        const bool on = lines[i].id == craftSkill_;
        if (on) sk = &lines[i];
        btn(2 + (int)i * chipW, kTop + 22, chipW - 2, 18, l ? l->name : std::to_string(lines[i].id), Act::CraftSkill, (int)lines[i].id,
            on ? Style::ToggleOn : Style::Toggle, Font::Small);
    }
    if (!sk) return;
    const uint32_t skillValue = sk->value + (uint32_t)std::max<int>(0, sk->tempBonus + sk->permBonus);
    std::vector<const w3n::Recipe*> rs = g.knownRecipes(craftSkill_);
    if (craftHaveMats_)   // 6.90
        rs.erase(std::remove_if(rs.begin(), rs.end(), [&](const w3n::Recipe* r) { return !craftHasMaterials(g, *r); }), rs.end());
    std::stable_sort(rs.begin(), rs.end(), [&](const w3n::Recipe* a, const w3n::Recipe* b) {
        const int da = a->difficulty(skillValue), db = b->difficulty(skillValue);
        if (da != db) return da < db;
        return g.spellNames.name(a->spell) < g.spellNames.name(b->spell);
    });
    const int by = kTabY - 24;
    // the item picker of an enchant
    if (craftPick_) {
        const w3n::Recipe* r = g.recipes.find(craftSel_);
        list_.textFit(6, kTop + 44, (r ? g.spellNames.name(r->spell) : std::string()) + " - " + tr("pick an item", "выберите вещь"), kTouchW - 12, kGold, true, Font::Small);
        std::vector<w3n::InvItem> items = enchantTargets(g, r);
        const int cols = 7, per = cols * 2, cell = 42;
        const int pages = std::max(1, ((int)items.size() + per - 1) / per);
        page_ = std::min(page_, pages - 1);
        if (items.empty()) list_.text(10, kTop + 70, tr("Nothing this enchant fits.", "Нет вещей, подходящих для этого чара."), kGrey, true, Font::Normal);
        for (int k = 0; k < per; ++k) {
            const int idx = page_ * per + k;
            if (idx >= (int)items.size()) break;
            const w3n::InvItem& it = items[idx];
            const int x = 6 + (k % cols) * cell, y = kTop + 58 + (k / cols) * cell;
            const w3n::ItemInfo* info = g.itemInfo(it.entry);
            iconBox(x + 1, y + 1, cell - 4, itemIcon(g, it.entry), g.itemName(it.entry), qualityColor(info ? info->quality : 1));
            if (it.equipped()) {   // worn: a gold corner and the slot's name under the icon
                list_.rect(x + 1, y + 1, 9, 9, kGold);
                list_.textCentered(x + cell / 2 - 1, y + cell - 12, equipSlotShort(it.slot), kGold, true, Font::Small);
            }
            Button& b = region(x - 1, y - 1, cell, cell, Act::CraftItem, idx);
            b.infoKind = 4;
            b.infoId = it.entry;
            b.guid = it.guid;
        }
        btn(2, by, 70, 22, tr("Back", "Назад"), Act::CraftPick, 0, Style::Normal, Font::Small);
        if (pages > 1) pager(by, page_, pages);
        return;
    }
    // left: the recipes, right: the chosen one
    const int per = 5, rowH = 21, top = kTop + 43, lw = 152;
    const int pages = std::max(1, ((int)rs.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    const w3n::Recipe* sel = nullptr;
    for (const w3n::Recipe* r : rs)
        if (r->spell == craftSel_) sel = r;
    if (!sel && !rs.empty()) { sel = rs[0]; craftSel_ = sel->spell; }
    for (int k = 0; k < per; ++k) {
        const int idx = page_ * per + k;
        if (idx >= (int)rs.size()) break;
        const w3n::Recipe* r = rs[idx];
        const int y = top + k * rowH;
        list_.rect(2, y, lw, rowH - 1, r == sel ? kRowSel : (k % 2 ? kRowAlt : kRow));
        const uint32_t icon = r->kind == 0 ? r->item : 0;
        if (icon) iconBox(4, y + 1, rowH - 3, itemIcon(g, icon), g.spellNames.name(r->spell), kFrameDim);
        else iconBox(4, y + 1, rowH - 3, spellIcon(r->spell), g.spellNames.name(r->spell), kFrameDim);
        bool can = true;
        for (const w3n::Recipe::Part& p : r->reagents) can = can && g.itemCount(p.entry) >= p.count;
        list_.textFit(4 + rowH, y + 5, g.spellNames.name(r->spell), lw - rowH - 8, can ? craftColour(r->difficulty(skillValue)) : hudAlpha(craftColour(r->difficulty(skillValue)), 150),
                      false, Font::Small);
        Button& b = region(2, y, lw, rowH - 1, Act::CraftPick, (int)r->spell);   // arg 0 (Back) closes the item picker
        b.infoKind = 2;
        b.infoId = r->spell;
    }
    // the chosen recipe
    const int rx = lw + 6, rw = kTouchW - rx - 2;
    window(rx, top - 1, rw, by - top - 2);
    char b[96];
    if (!sel) {
        list_.text(rx + 6, top + 6, tr("No recipes.", "Нет рецептов."), kGrey);
    } else {
        int y = top + 3;
        const int diff = sel->difficulty(skillValue);
        if (sel->kind == 0 && sel->item) {
            const w3n::ItemInfo* info = g.itemInfo(sel->item);
            iconBox(rx + 4, y, 22, itemIcon(g, sel->item), g.itemName(sel->item), qualityColor(info ? info->quality : 1));
            snprintf(b, sizeof b, sel->count > 1 ? "%s x%u" : "%s", g.itemName(sel->item).c_str(), (unsigned)sel->count);
            list_.textFit(rx + 30, y + 1, b, rw - 34, qualityColor(info ? info->quality : 1), true, Font::Small);
        } else {
            iconBox(rx + 4, y, 22, spellIcon(sel->spell), g.spellNames.name(sel->spell), kFrameDim);
            list_.textFit(rx + 30, y + 1, g.spellNames.name(sel->spell), rw - 34, kGold, true, Font::Small);
        }
        static const char* kDiffEn[] = {"Difficult", "Medium", "Easy", "Trivial"};
        static const char* kDiffRu[] = {"Сложно", "Средне", "Легко", "Тривиально"};
        list_.text(rx + 30, y + 12, tr(kDiffEn[diff], kDiffRu[diff]), craftColour(diff), false, Font::Small);
        y += 27;
        if (!sel->tools.empty()) {
            std::string t = tr("Tools: ", "Инструменты: ");
            for (size_t i = 0; i < sel->tools.size(); ++i) t += (i ? ", " : "") + g.itemName(sel->tools[i].entry);
            list_.textFit(rx + 6, y, t, rw - 10, kGrey, false, Font::Small);
            y += 11;
        }
        list_.text(rx + 6, y, tr("Reagents:", "Реагенты:"), kText, false, Font::Small);
        y += 11;
        for (const w3n::Recipe::Part& p : sel->reagents) {
            if (y + 10 > by - 4) break;
            const uint32_t have = g.itemCount(p.entry);
            snprintf(b, sizeof b, "%u/%u", (unsigned)std::min<uint32_t>(have, 999), (unsigned)p.count);
            list_.textRight(rx + rw - 5, y, b, have >= p.count ? kGreen : kRed, false, Font::Small);
            list_.textFit(rx + 6, y, g.itemName(p.entry), rw - 46, have >= p.count ? kText : kGrey, false, Font::Small);
            y += 11;
        }
    }
    // buttons: Create, Create All / Stop
    bool can = sel != nullptr && !g.cast.active;
    uint32_t most = 0;
    if (sel) {
        most = 9999;
        for (const w3n::Recipe::Part& p : sel->reagents) most = std::min<uint32_t>(most, g.itemCount(p.entry) / p.count);
        if (sel->reagents.empty()) most = 1;
    }
    if (craftLeft_ > 0) {
        snprintf(b, sizeof b, "%s (%d)", tr("Stop", "Стоп"), craftLeft_);
        btn(2, by, 90, 22, b, Act::CraftStop, 0, Style::Danger, Font::Small);
    } else {
        btn(2, by, 62, 22, sel && sel->kind == 1 ? tr("Enchant", "Зачаровать") : tr("Create", "Создать"), Act::CraftMake, 0,
            can && (most > 0 || (sel && sel->kind == 1)) ? Style::Primary : Style::Disabled, Font::Small);
        if (sel && sel->kind == 0) {
            snprintf(b, sizeof b, "%s %u", tr("All", "Все"), (unsigned)(most < 999 ? most : 999));
            btn(66, by, 66, 22, b, Act::CraftAll, 0, can && most > 1 ? Style::Normal : Style::Disabled, Font::Small);
        }
    }
    btn(136, by, 88, 22, tr("Have mats", "Есть реагенты"), Act::CraftHaveMats, 0, craftHaveMats_ ? Style::ToggleOn : Style::Toggle, Font::Small);   // 6.90
    if (rs.empty() && craftHaveMats_) list_.text(8, top + 4, tr("Nothing you can make now.", "Сейчас ничего не сделать."), kGrey, false, Font::Small);
    if (pages > 1) pager(by, page_, pages);
    (void)in;
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
        struct L { uint32_t color; std::string text; Font f; uint32_t item = 0; };
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
            // 6.94: the rewards, as in the quest window: the ones to choose from, the fixed ones, the money (a tap shows the item)
            bool anyChoice = false, anyFixed = false;
            for (int i = 0; i < 6; ++i) anyChoice |= q->choiceItem[i] != 0;
            for (int i = 0; i < 4; ++i) anyFixed |= q->rewardItem[i] != 0;
            auto itemLine = [&](uint32_t id, uint32_t count) {
                const w3n::ItemInfo* info = g.itemInfo(id);
                L l{qualityColor(info ? info->quality : 1), g.itemName(id) + (count > 1 ? " x" + std::to_string(count) : std::string()), Font::Normal};
                l.item = id;
                body.push_back(l);
            };
            if (anyChoice) {
                body.push_back({kGold, tr("Choose one reward:", "Награда на выбор:"), Font::Normal});
                for (int i = 0; i < 6; ++i) if (q->choiceItem[i]) itemLine(q->choiceItem[i], q->choiceCount[i]);
            }
            if (anyFixed) {
                body.push_back({kGold, anyChoice ? tr("You will also receive:", "А также:") : tr("You will receive:", "Вы получите:"), Font::Normal});
                for (int i = 0; i < 4; ++i) if (q->rewardItem[i]) itemLine(q->rewardItem[i], q->rewardCount[i]);
            }
            if (q->money) body.push_back({kText, std::string(tr("Money: ", "Деньги: ")) + w3n::moneyText(q->money), Font::Small});
        } else {
            body.push_back({kGrey, tr("(asking the server...)", "(запрос к серверу...)"), Font::Small});
        }
        const int top = kTop + 4, bottom = by - 4;
        auto lineH = [&](const L& l) { return l.item ? 24 : HudList::lineHeight(l.f); };
        int maxScroll = 0, hTotal = 0;
        for (const L& l : body) hTotal += lineH(l);
        int skip = 0, acc = 0;
        while (skip < (int)body.size() && hTotal - acc > bottom - top) acc += lineH(body[skip++]);
        maxScroll = skip;
        questScroll_ = std::min(questScroll_, maxScroll);
        qsOn_ = true; qsX_ = 0; qsY_ = top; qsW_ = kTouchW; qsH_ = bottom - top;   // 6.94: a finger drag scrolls the text
        int y = top;
        for (int i = questScroll_; i < (int)body.size(); ++i) {
            const L& l = body[i];
            const int lh = lineH(l);
            if (y + lh > bottom) break;
            if (l.item) {
                list_.rect(4, y, kTouchW - 8, 22, kRow);
                iconBox(6, y + 1, 20, itemIcon(g, l.item), l.text, l.color);
                list_.textFit(32, y + 3, l.text, kTouchW - 40, l.color, true, l.f);
                Button& bt = region(4, y, kTouchW - 8, 22, Act::ItemInfo, 999);
                bt.infoKind = 1;
                bt.infoId = l.item;
            } else {
                list_.text(6, y, l.text, l.color, true, l.f);
            }
            y += lh;
        }
        btn(2, by, 58, 22, tr("Back", "Назад"), Act::LogBack, 0, Style::Normal, Font::Small);
        btn(62, by, 82, 22, watch_.tracked(e.id) ? tr("Untrack", "Не следить") : tr("Track", "Следить"), Act::TrackQuest, 0,
            watch_.tracked(e.id) ? Style::ToggleOn : Style::Normal, Font::Small);
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
        if (watch_.tracked(e.id)) list_.rect(4, y + 7, 9, 9, questSlotColor(watch_.slot(e.id)));   // 6.63: the colour its area has on the map
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
        y += 21;
        label(tr("Run from", "Бег от"));
        opt(120, 62, "0.3", fabsf(c.runThreshold - 0.3f) < 0.05f, 10);
        opt(186, 62, "0.5", fabsf(c.runThreshold - 0.5f) < 0.05f, 11);
        opt(252, 62, "0.7", fabsf(c.runThreshold - 0.7f) < 0.05f, 12);
        y += 21;
        label(tr("Face movement", "Лицом к ходу"));
        opt(120, 96, tr("On", "Вкл"), c.faceMovement, 20);
        opt(220, 96, tr("Off", "Выкл"), !c.faceMovement, 21);
        y += 21;
        label("ZL / ZR");
        opt(120, 96, tr("Hold", "Удерживать"), !c.toggleModifiers, 30);
        opt(220, 96, tr("Toggle", "Переключать"), c.toggleModifiers, 31);
        y += 21;
        label(tr("Camera", "Камера"));
        opt(120, 96, tr("Invert Y", "Инверсия Y"), c.invertY, 40);
        btn(220, y, 30, 19, "-", Act::Setting, 50, Style::Normal);
        snprintf(b, sizeof b, "%.1f", c.camSpeed);
        list_.textCentered(268, y + 4, b, kText);
        btn(286, y, 30, 19, "+", Act::Setting, 51, Style::Normal);
        y += 21;
        label(tr("Ground spells", "Спеллы на землю"));   // UI stage 10
        opt(120, 96, tr("Aim", "Прицел"), c.groundCast == 0, 190);
        opt(220, 96, tr("At target", "В цель"), c.groundCast == 1, 191);
        // (SELECT + ◀/▶ turns the tabs: no room for that line since the ground spells' row)
    } else if (subSettings_ == 1) {
        y -= 4;   // 8 rows (stage 5.32)
        label(tr("Draw distance", "Дальность"));
        const char* d[4] = {tr("Near", "Близко"), tr("Normal", "Норм."), tr("Far", "Далеко"), tr("Max", "Макс.")};
        for (int i = 0; i < 4; ++i) opt(120 + i * 39, 37, d[i], c.drawDist == i, 70 + i);
        opt(120 + 4 * 39, 37, tr("Auto", "Авто"), c.autoDetail, 75);   // 6.72: shorter by itself in heavy scenes
        y += 20;
        label(tr("Crowds", "Толпы"));
        const char* cr[4] = {"10", "20", "40", tr("All", "Все")};
        for (int i = 0; i < 4; ++i) opt(120 + i * 49, 47, cr[i], c.crowd == i, 80 + i);
        y += 20;
        label(tr("World objects", "Объекты мира"));
        opt(120, 96, tr("On", "Вкл"), c.doodads, 90);
        opt(220, 96, c.fadeIn ? tr("Fade in: on", "Плавно: вкл") : tr("Fade in: off", "Плавно: выкл"), c.fadeIn, 205);   // stage 6.11
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
        y -= 4;   // 8 rows (6.34b: the horizon)
        label(tr("Language", "Язык"));
        opt(120, 96, "English", c.lang == 0, 100);
        opt(220, 96, "Русский", c.lang == 1, 101);
        y += 20;
        label(tr("Text size", "Размер текста"));
        opt(120, 96, tr("Normal", "Обычный"), !c.bigText, 110);
        opt(220, 96, tr("Large", "Крупный"), c.bigText, 110);
        y += 20;
        label(tr("Dim HUD / bubbles", "Тусклый HUD / облачка"));
        opt(120, 96, c.dimHud ? tr("Dim: on", "Туск.: вкл") : tr("Dim: off", "Туск.: выкл"), c.dimHud, 120);
        opt(220, 96, c.chatBubbles ? tr("Bubbles: on", "Облачка: вкл") : tr("Bubbles: off", "Облачка: выкл"), c.chatBubbles, 121);   // 6.67
        y += 20;
        label(tr("Auto tabs", "Автовкладки"));
        opt(120, 96, c.autoTabs ? tr("On", "Вкл") : tr("Off", "Выкл"), c.autoTabs, 130);
        y += 20;
        label(tr("Quest on top", "Квест сверху"));
        opt(120, 96, tr("On", "Вкл"), c.questTracker, 60);
        opt(220, 96, tr("Off", "Выкл"), !c.questTracker, 61);
        y += 20;
        label(tr("FPS / ping", "FPS / пинг"));   // 5.35e: small, at the bottom of the top screen
        opt(120, 96, tr("On", "Вкл"), c.showStats, 180);
        opt(220, 96, tr("Off", "Выкл"), !c.showStats, 181);
        y += 20;
        label(tr("Log / GPU / Frame", "Лог / GPU / Кадр"));   // stage 6: perf.csv; 6.3: the early GPU kick; 6.4: vsync (each button toggles)
        opt(120, 64, c.perfLog ? tr("Log", "Лог") : tr("no log", "без лога"), c.perfLog, 202);
        opt(186, 64, c.gpuKick ? tr("GPU early", "GPU рано") : tr("GPU end", "GPU конец"), c.gpuKick, 203);
        opt(252, 64, c.pace == 2 ? tr("Auto 60/30", "Авто 60/30") : c.pace == 1 ? tr("Steady 30", "Ровно 30") : tr("Free", "Свободно"), c.pace != 0, 204);   // 6.71
        y += 20;
        label(tr("Horizon", "Горизонт"));   // 6.34b: the ground and far buildings past the draw distance (costs GPU time)
        const char* hz[3] = {tr("Off", "Выкл"), "800", "1200"};
        for (int i = 0; i < 3; ++i) opt(120 + i * 65, 62, hz[i], c.horizon == i, 210 + i);
    }
    // 8 rows on Graphics/sound since the particles (stage 5.32), 7 on Interface since the perf log (stage 6): Debug / Quit
    // stay on the Controls page and in START
    if (subSettings_ != 0) return;
    const int by = kTabY - 24;
    btn(2, by, 156, 22, tr("Debug page", "Отладка"), Act::MenuItem, 8, Style::Normal, Font::Small);
    btn(162, by, 154, 22, tr("Quit", "Выход"), Act::MenuItem, 9, Style::Danger, Font::Small);
}

// ------------------------------------------------------------------ START menu, the target's menu, buffs
void GameUi::drawMenu(w3n::GameSession& g) {
    window(4, kTop + 2, kTouchW - 8, kContentH - 4, true);
    const char* items[12] = {tr("Chat", "Чат"), tr("Bags", "Сумки"), tr("Equipment", "Экипировка"), tr("Spells", "Книга"),
                             tr("Quests", "Задания"), tr("Buffs", "Эффекты"), tr("Settings", "Настройки"), tr("Free camera", "Своб. камера"),
                             tr("Debug page", "Отладка"), tr("Quit", "Выход"), tr("Pet", "Питомец"), tr("Characters", "Персонажи")};
    // the action of each button: 10 is the pet (menu action 12), 11 the character list (menu action 13)
    static const int kAction[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13};
    static const int kOrder[11] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 11, 9};   // the free camera is gone (stage 5.31); Quit last
    int k = 0;
    for (int i : kOrder) {
        if (i == 10 && !g.petGuid()) continue;   // UI stage 13: only with a pet
        btn(14 + (k % 2) * 148, kTop + 8 + (k / 2) * 27, 142, 24, items[i], Act::MenuItem, kAction[i], i == 9 ? Style::Danger : Style::Normal);
        ++k;
    }
    btn(kTouchW - 80, kTabY - 28, 70, 22, tr("Close", "Закрыть"), Act::ClosePanel, 0, Style::Normal, Font::Small);
}

void GameUi::drawContext(w3n::GameSession& g) {
    // the original's unit menu (UnitPopup): for a player Whisper, Inspect, Invite, Trade, Follow, Duel, Add friend, Ignore ...
    window(4, kTop + 2, kTouchW - 8, kContentH - 4, true);
    const w3n::WorldObject* t = g.objects.find(g.target());
    std::vector<int> items;
    if (t) {
        const bool player = t->type == w3n::ObjType::Player && !t->self;
        const bool member = g.inParty(t->guid);
        items.push_back(0);
        if (!player) items.push_back(1);
        items.push_back(2);
        if (player) {
            items.push_back(3);                                                        // whisper
            items.push_back(12);                                                       // inspect
            if (!member && (!g.party.inGroup || g.isLeader())) items.push_back(5);    // invite
            items.push_back(9);                                                        // trade
            items.push_back(g.following == t->guid ? 16 : 13);                         // follow / stop following
            if (!g.duel.active()) items.push_back(14);                                 // duel
            items.push_back(10);                                                       // add friend
            if (g.guildId()) items.push_back(11);                                      // guild invite
            items.push_back(15);                                                       // ignore
        } else if (t->self) {
            items.push_back(12);                                                       // our own gear and honour
        } else if (t->isUnitLike()) {
            items.push_back(g.following == t->guid ? 16 : 13);
        }
        if (member && g.isLeader()) { items.push_back(6); items.push_back(7); }        // uninvite, promote
        items.push_back(4);
    } else {
        list_.textCentered(kTouchW / 2, kTop + 20, tr("No target", "Нет цели"), kGrey, true, Font::Normal);
        if (g.following) items.push_back(16);
    }
    if (g.party.inGroup) items.push_back(8);
    const char* names[17] = {tr("Attack", "Атаковать"), tr("Interact", "Действие"), tr("Assist", "Помочь"), tr("Whisper", "Шёпот"),
                             tr("Clear target", "Сбросить цель"), tr("Invite", "В группу"), tr("Uninvite", "Исключить"),
                             tr("Make leader", "Сделать лидером"), tr("Leave party", "Покинуть группу"), tr("Trade", "Обмен"),
                             tr("Add friend", "В друзья"), tr("Guild invite", "В гильдию"), tr("Inspect", "Осмотреть"),
                             tr("Follow", "Следовать"), tr("Duel", "Дуэль"), tr("Ignore", "Игнорировать"),
                             tr("Stop following", "Не следовать")};
    // three columns of buttons (up to 18)
    const int cols = 3, bw = (kTouchW - 20 - (cols - 1) * 4) / cols, bh = 24, y0 = kTop + (t ? 8 : 40);
    for (size_t k = 0; k < items.size() && k < 18; ++k) {
        const int i = items[k];
        btn(10 + (int)(k % cols) * (bw + 4), y0 + (int)(k / cols) * (bh + 4), bw, bh, names[i], Act::ContextItem, i,
            i == 8 || i == 6 || i == 15 ? Style::Danger : i == 0 || i == 14 ? Style::Primary : Style::Normal, Font::Small);
    }
    btn(kTouchW - 80, kTabY - 28, 70, 22, tr("Close", "Закрыть"), Act::ClosePanel, 0, Style::Normal, Font::Small);
}

// UI stage 9: another player's gear (their visible items: what everybody sees of them) and honour, like the original's
// InspectFrame; a tap on an item shows its tooltip.
void GameUi::drawInspect(w3n::GameSession& g) {
    window(2, kTop + 2, kTouchW - 4, kContentH - 4, true);
    const w3n::WorldObject* o = g.objects.find(inspect_);
    if (!o || o->type != w3n::ObjType::Player) {
        list_.textCentered(kTouchW / 2, kTop + 60, tr("Out of range", "Слишком далеко"), kGrey, true, Font::Normal);
        btn(kTouchW - 80, kTabY - 28, 70, 22, tr("Close", "Закрыть"), Act::ClosePanel, 0, Style::Normal, Font::Small);
        return;
    }
    auto cellAt = [&](int slot, int x, int y, int size) {
        const uint32_t entry = g.visibleItem(*o, slot);
        if (entry) {
            const w3n::ItemInfo* info = g.itemInfo(entry);
            iconBox(x, y, size, itemIcon(g, entry), g.itemName(entry), qualityColor(info ? info->quality : 1));
            Button& b = region(x - 1, y - 1, size + 2, size + 2, Act::InspectSlot, (int)entry);
            b.infoKind = 1;
            b.infoId = entry;
        } else {
            ui::Image bg = assets().namedIcon(("slot_" + std::to_string(slot)).c_str());
            if (bg.ok()) list_.image(x, y, size, size, bg, hudRgba(150, 150, 150));
            else {
                list_.rect(x, y, size, size, kRowAlt);
                list_.textCentered(x + size / 2, y + size / 2 - 5, HudList::fit(slotShortName(slot), size - 2), hudRgba(110, 104, 92), false);
            }
            list_.outline(x - 1, y - 1, size + 2, size + 2, kFrameDim);
        }
    };
    static const int left[8] = {0, 1, 2, 14, 4, 3, 18, 8}, right[8] = {9, 5, 6, 7, 10, 11, 12, 13};
    const int cs = 32, step = 36, y0 = kTop + 8;
    for (int i = 0; i < 8; ++i) {
        cellAt(left[i], 8 + (i % 2) * step, y0 + (i / 2) * step, cs);
        cellAt(right[i], kTouchW - 8 - cs - (1 - i % 2) * step, y0 + (i / 2) * step, cs);
    }
    const int cx0 = 8 + 2 * step + 4, cx1 = kTouchW - 8 - 2 * step - 4, cw = cx1 - cx0, mid = cx0 + cw / 2;
    const std::string nm = o->self ? std::string() : (o->name.empty() ? g.objects.nameOf(o->guid) : o->name);
    list_.textCentered(mid, y0, HudList::fit(nm.empty() ? std::string(tr("You", "Вы")) : nm, cw, Font::Normal), kGold, true, Font::Normal);
    const uint32_t b0 = o->field(w3n::UF::UNIT_BYTES_0), race = b0 & 0xFF, cls = (b0 >> 8) & 0xFF;
    static const char* kRaceEn[11] = {"", "Human", "Orc", "Dwarf", "Night Elf", "Undead", "Tauren", "Gnome", "Troll", "Goblin", "High Elf"};
    static const char* kRaceRu[11] = {"", "Человек", "Орк", "Дворф", "Ночной эльф", "Нежить", "Таурен", "Гном", "Тролль", "Гоблин", "Высший эльф"};
    char b[80];
    snprintf(b, sizeof b, "%s %u", tr("Level", "Уровень"), (unsigned)o->level());
    list_.textCentered(mid, y0 + 16, b, kText);
    snprintf(b, sizeof b, "%s %s", race < 11 ? (ui::g_lang ? kRaceRu[race] : kRaceEn[race]) : "", className(cls));
    list_.textCentered(mid, y0 + 28, HudList::fit(b, cw), classColor(cls));
    // honour (MSG_INSPECT_HONOR_STATS)
    const w3n::GameSession::HonorView& h = g.honor;
    if (h.known && h.guid == o->guid) {
        const bool horde = race == 2 || race == 5 || race == 6 || race == 8;
        static const char* kRuA[14] = {"Рядовой", "Капрал", "Сержант", "Старший сержант", "Старшина", "Рыцарь", "Рыцарь-лейтенант",
            "Рыцарь-капитан", "Рыцарь-защитник", "Лейтенант-командор", "Командор", "Маршал", "Фельдмаршал", "Главнокомандующий"};
        static const char* kRuH[14] = {"Разведчик", "Рубака", "Сержант", "Старший сержант", "Старшина", "Каменный страж",
            "Кровавый страж", "Легионер", "Центурион", "Защитник", "Генерал-лейтенант", "Генерал", "Полководец", "Верховный полководец"};
        const char* rank = w3n::pvpRankName(h.highestRank, horde);
        if (rank[0] && ui::g_lang) rank = (horde ? kRuH : kRuA)[h.highestRank - 5];
        int y = y0 + 46;
        if (rank[0]) { list_.textCentered(mid, y, HudList::fit(rank, cw), kYellow); y += 12; }
        snprintf(b, sizeof b, "%s: %u", tr("Honorable kills", "Почётные победы"), (unsigned)h.lifetimeHK);
        list_.textCentered(mid, y, HudList::fit(b, cw), kGrey);
        y += 12;
        if (h.todayKills) { snprintf(b, sizeof b, "%s: %u", tr("Today", "Сегодня"), (unsigned)h.todayKills); list_.textCentered(mid, y, b, kGrey); }
    }
    for (int k = 0; k < 3; ++k) cellAt(15 + k, cx0 + (cw - 3 * step) / 2 + k * step + 2, y0 + 3 * step, cs);
    list_.text(10, kTabY - 22, tr("tap an item for its tooltip", "нажмите на предмет - описание"), kGrey);
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
