// w3dlogin — PC test client for the network code: logs into a vanilla 1.12.1
// server exactly like the 3DS client (same sources), enters the world with a
// character and (stage 5) walks, tracks world objects and records packets.
//
//   w3dlogin <server[:3724]> <account> [password] [--realm N] [--char N] [--stay SEC]
//            [--protocol 3|8] [--ping SEC] [--walk] [--log FILE] [--objects N] [--out FILE]
//   w3dlogin --replay FILE [--objects N]     parse a recorded session offline
//   --walk     walk a closed ~10 yd route at server speed (watch it from a second client)
//   --fight N  target the nearest living unit whose name contains N, walk up to it, auto-attack until it
//              dies, loot it
//   --life     stage-5.10 script against fake_server --life: bags, loot window, equip/use/destroy, gossip,
//              three deaths (corpse run, spirit healer, resurrection by a player)
//   --trade    stage-5.12..5.14 script against fake_server --trade: faction reactions, quests, a vendor
//   --party    stage-5.17 script against fake_server --party: invitation, members, leader, kick, leave
//   --survey   after 6 s: reactions, attackability and quest marks of everything within 45 yd, reputations, quest log
//              (use with --factions / --spells on a real server: it changes nothing)
//   --factions F  factions.txt of w3dconv (reactions: hostile / neutral / friendly)
//   --say TEXT send a chat line after 1 s (repeatable; "/w Name text", "/y text" ... as on the 3DS)
//   --log F    record the in-world packets (both directions) for --replay
//   --out F    also write everything printed to F
//   --create NAME:race:class:gender[:skin:face:hairstyle:haircolour:facial]   create a character first (stage 5.29)
//   --delete NAME   delete that character first
#include "../game.hpp"

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

using namespace w3n;

// Console output, optionally duplicated into a file (--out), so a batch file can
// keep the log while the password prompt stays visible.
static FILE* g_tee = nullptr;
static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    if (g_tee) {
        va_start(a, fmt);
        std::vfprintf(g_tee, fmt, a);
        va_end(a);
        std::fflush(g_tee);
    }
}
static void sayLine(const char* s) { say("%s\n", s); }

static bool waitUntil(const std::function<bool()>& done, const std::function<void()>& poll) {
    while (!done()) { poll(); sleepMs(5); }
    return true;
}

static void printObjects(const GameSession& s, uint32_t nowMs, int maxList) {
    const ObjectTable& t = s.objects;
    const ObjectStats& st = t.stats;
    say("objects: %zu total | players %zu units %zu gameobjects %zu items %zu containers %zu dynobj %zu corpses %zu\n",
                t.all().size(), t.count(ObjType::Player), t.count(ObjType::Unit), t.count(ObjType::GameObject),
                t.count(ObjType::Item), t.count(ObjType::Container), t.count(ObjType::DynamicObject), t.count(ObjType::Corpse));
    say("  update packets %u (+%u compressed, %llu -> %llu bytes inflated), failed %u%s%s\n", st.updatePackets,
                st.compressedPackets, (unsigned long long)st.inflatedIn, (unsigned long long)st.inflatedOut, st.failedPackets,
                st.lastError.empty() ? "" : ", last error: ", st.lastError.c_str());
    say("  blocks: values %u movement %u create %u create2 %u out-of-range %u near %u | destroy %u\n", st.blocks[0],
                st.blocks[1], st.blocks[2], st.blocks[3], st.blocks[4], st.blocks[5], st.destroys);
    say("  monster moves %u (failed %u, compressed batches %u), player moves %u, answers: names %u creatures %u gameobjects %u\n",
                st.monsterMoves, st.monsterMoveFails, st.compressedMoves, st.playerMoves, st.nameAnswers, st.creatureAnswers,
                st.gameobjectAnswers);
    if (maxList <= 0) return;
    const MovementInfo& me = s.mover.info();
    struct Row { float d; const WorldObject* o; float p[3]; };
    std::vector<Row> rows;
    for (auto& [g, o] : t.all()) {
        if (o.self || !o.hasPos || !(o.isUnitLike() || o.type == ObjType::GameObject)) continue;
        Row r{0, &o, {0, 0, 0}};
        o.positionAt(nowMs, r.p);
        r.d = std::sqrt((r.p[0] - me.x) * (r.p[0] - me.x) + (r.p[1] - me.y) * (r.p[1] - me.y) + (r.p[2] - me.z) * (r.p[2] - me.z));
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
    for (int i = 0; i < (int)rows.size() && i < maxList; ++i) {
        const WorldObject& o = *rows[i].o;
        char hp[32] = "";
        if (o.isUnitLike() && o.maxHealth()) std::snprintf(hp, sizeof hp, " hp %u/%u", o.health(), o.maxHealth());
        say("   %6.1f yd  %-10s %-24.24s entry %5u display %5u%s%s%s%s\n", rows[i].d, objTypeName(o.type),
                    o.name.empty() ? "?" : o.name.c_str(), o.entry(), o.displayId(),
                    o.isUnitLike() ? (" lvl " + std::to_string(o.level())).c_str() : "", hp,
                    o.pathActive ? " moving(spline)" : "", (o.mv.flags & MoveFlag::MaskMoving) ? " moving(keys)" : "");
    }
}

static void printInventory(GameSession& s, const char* title) {
    say("%s: %s money, %u free bag slots\n", title, moneyText(s.money()).c_str(), s.freeBagSlots());
    for (const InvItem& it : s.inventory()) {
        const ItemInfo* i = s.itemInfoIfKnown(it.entry);
        char where[32];
        if (it.bag == kBagSelf) std::snprintf(where, sizeof where, "%s %u", equipSlotName(it.slot), it.slot);
        else std::snprintf(where, sizeof where, "bag %u slot %u", it.bag, it.slot);
        say("  %-14s %-22s x%u%s%s\n", where, s.itemName(it.entry).c_str(), it.count,
            i && i->missing ? " (unknown item)" : i ? (std::string(" (") + itemQualityName(i->quality) + (i->inventoryType ? std::string(", ") + inventoryTypeName(i->inventoryType) : std::string()) +
                 (i->usable() ? ", use" : "") + ")").c_str() : "",
            it.maxDurability ? (" " + std::to_string(it.durability) + "/" + std::to_string(it.maxDurability)).c_str() : "");
    }
}

// --life: one step of the stage-5.10 script per frame; returns the movement keys for the frame.
static uint32_t lifeStep(GameSession& g, float pos[3], float& face, float dt, uint32_t ms) {
    static int step = 0;
    static uint32_t stepMs = 0;
    static bool printedDist = false;
    auto next = [&](const char* what) {
        say("LIFE step %d done: %s\n", step, what);
        ++step;
        stepMs = ms;
    };
    auto find = [&](uint32_t entry, int bag = -1) -> const InvItem* {
        static std::vector<InvItem> inv;
        inv = g.inventory();
        for (const InvItem& it : inv)
            if (it.entry == entry && (bag < 0 || it.bag == bag)) return &it;
        return nullptr;
    };
    auto unitWith = [&](auto pred) -> uint64_t {
        for (auto& [guid, o] : g.objects.all())
            if (!o.self && o.isUnitLike() && pred(o)) return guid;
        return 0;
    };
    auto walkTo = [&](float x, float y, float want) -> uint32_t {   // forward at server speed
        float dx = x - pos[0], dy = y - pos[1], d = std::sqrt(dx * dx + dy * dy);
        if (d <= want) return 0;
        face = normalizeAngle(std::atan2(dy, dx));
        float step = std::min(g.mover.speeds.forFlags(MoveFlag::Forward | (g.mover.info().flags & ~MoveFlag::MaskMoving)) * dt,
                              d - want + 0.05f);
        pos[0] += std::cos(face) * step;
        pos[1] += std::sin(face) * step;
        return MoveFlag::Forward;
    };
    const bool slow = ms - stepMs > 6000;
    if (slow && step < 99) { say("LIFE step %d TIMED OUT\n", step); step = 99; }
    switch (step) {
        case 0: {
            auto inv = g.inventory();
            bool named = inv.size() >= 7;
            for (const InvItem& it : inv) named = named && g.itemInfoIfKnown(it.entry);
            if (named) { printInventory(g, "INVENTORY"); next("inventory with names"); }
            return 0;
        }
        case 1: {
            uint64_t w = unitWith([](const WorldObject& o) { return o.dead() && (o.field(UF::UNIT_DYNAMIC_FLAGS) & 1); });
            if (!w) return 0;
            g.setTarget(w, ms);
            std::string e = g.interact(w, pos, ms);
            if (!e.empty()) say("interact: %s\n", e.c_str());
            next("loot request");
            return 0;
        }
        case 2: {
            if (!g.lootWindow.open) return 0;
            bool named = true;
            for (const LootSlot& l : g.lootWindow.items) named = named && g.itemInfoIfKnown(l.itemId);
            if (!named && ms - stepMs < 3000) return 0;
            say("LOOT WINDOW: %s", moneyText(g.lootWindow.money).c_str());
            size_t meat = 0;
            for (size_t i = 0; i < g.lootWindow.items.size(); ++i) {
                const LootSlot& l = g.lootWindow.items[i];
                const ItemInfo* info = g.itemInfoIfKnown(l.itemId);
                say(" | [%u] %s x%u%s", l.slot, g.itemName(l.itemId).c_str(), l.count, info && info->missing ? " (unknown item)" : "");
                if (l.itemId == 750) meat = i;
            }
            say("\n");
            g.takeLoot(meat, ms);
            next("took the meat");
            return 0;
        }
        case 3:
            if (g.lootWindow.items.size() != 2) return 0;
            g.takeAllLoot(ms);
            next("take all");
            return 0;
        case 4: {
            if (g.lootOpen() || g.money() != 1284) return 0;
            const InvItem* h = find(6125);
            if (!h) return 0;
            g.equipItem(*h, ms);
            next("loot window closed, money in, equip the harness");
            return 0;
        }
        case 5: {
            const InvItem* h = find(6125);
            if (!h || !(h->bag == kBagSelf && h->slot == 4)) return 0;
            if (const InvItem* b = find(117)) g.useItem(*b, ms);
            next("harness on the chest, eat bread");
            return 0;
        }
        case 6: {
            const InvItem* b = find(117);
            if (!b || b->count != 3) return 0;
            if (const InvItem* hs = find(6948)) g.equipItem(*hs, ms);
            next("3 breads left, try to wear the hearthstone");
            return 0;
        }
        case 7: {
            if (!g.lastInventoryError) return 0;
            if (const InvItem* l = find(2589, 19)) g.destroyItem(*l, ms);
            next("equip error seen, destroy the linen in the pouch");
            return 0;
        }
        case 8: {
            if (find(2589, 19)) return 0;
            if (const InvItem* sh = find(38)) g.unequipItem(*sh, ms);
            next("linen gone, take off the shirt");
            return 0;
        }
        case 9: {
            const InvItem* sh = find(38);
            if (!sh || sh->slot < kSlotBackpackFirst) return 0;
            printInventory(g, "INVENTORY AFTER");
            uint64_t m = unitWith([](const WorldObject& o) { return o.entry() == 197; });
            g.setTarget(m, ms);
            std::string e = g.interact(m, pos, ms);
            if (!e.empty()) say("interact: %s\n", e.c_str());
            next("shirt in the backpack, talk to the marshal");
            return 0;
        }
        case 10: {
            if (!g.gossip.open || g.gossip.text.empty()) return 0;
            say("GOSSIP: \"%s\"", g.gossip.text.c_str());
            for (const GossipOption& o : g.gossip.options) say(" | option %u: %s", o.index, o.text.c_str());
            for (const GossipQuest& q : g.gossip.quests) say(" | quest %u: %s (level %u)", q.id, q.title.c_str(), q.level);
            say("\n");
            g.selectGossip(1, ms);
            next("gossip option 1");
            return 0;
        }
        case 11:
            if (g.gossip.open) return 0;
            next("gossip closed by the server");
            return 0;
        case 12: case 16: {
            static int deadShown = -1;
            if (g.life() != Life::Dead) return 0;
            if (deadShown != step) { deadShown = step; stepMs = ms; say("LIFE: dead (A says: %s)\n", g.interact(g.target(), pos, ms).c_str()); }
            if (ms - stepMs < 300) return 0;
            g.releaseSpirit(ms);
            printedDist = false;
            next("released the spirit");
            return 0;
        }
        case 13: {
            float d = g.corpseDistance(pos);
            if (g.life() != Life::Ghost || d < 45) return 0;   // the graveyard teleport has happened
            say("LIFE: ghost at the graveyard, corpse %.1f yd away, reclaim in %u ms, water walking %d\n", d,
                g.reclaimDelayLeftMs(ms), (g.mover.info().flags & MoveFlag::WaterWalking) != 0);
            next("ghost at the graveyard");
            return 0;
        }
        case 14: {
            if (!printedDist) { say("interact as ghost: %s\n", g.interact(0, pos, ms).c_str()); printedDist = true; }
            if (g.canReclaim(pos, ms)) {
                std::string e = g.interact(0, pos, ms);
                next(e.empty() ? "reclaim the corpse" : e.c_str());
                return 0;
            }
            return walkTo(g.corpse.x, g.corpse.y, 30.0f);
        }
        case 15: case 20: case 22:
            if (g.life() != Life::Alive || g.resurrections < g.deaths) return 0;
            say("LIFE: alive again (deaths %u, resurrections %u)\n", g.deaths, g.resurrections);
            next("alive");
            if (step == 23) {
                say("LIFE: deaths %u, resurrections %u, gossip menus %u, item queries %u/%u, flag ACKs %u, last inventory error %u (%s)\n",
                    g.deaths, g.resurrections, g.gossipMenus, g.itemQueries, g.itemAnswers, g.stats.flagAcks,
                    g.lastInventoryError, inventoryResultText(g.lastInventoryError).c_str());
                step = 100;
            }
            return 0;
        case 17: {
            if (g.life() != Life::Ghost) return 0;
            uint64_t h = unitWith([](const WorldObject& o) { return (o.field(UF::UNIT_NPC_FLAGS) & 0x20) != 0; });
            const WorldObject* ho = g.objects.find(h);
            if (!ho || g.corpseDistance(pos) < 45) return 0;
            if (g.target() != h) g.setTarget(h, ms);
            float hp[3];
            ho->positionAt(ms, hp);
            uint32_t d = walkTo(hp[0], hp[1], 2.5f);
            if (d) return d;
            std::string e = g.interact(h, pos, ms);
            next(e.empty() ? "talk to the spirit healer" : e.c_str());
            return 0;
        }
        case 18:
            if (!g.gossip.open) return 0;
            say("SPIRIT HEALER: %zu option(s), first \"%s\"\n", g.gossip.options.size(),
                g.gossip.options.empty() ? "" : g.gossip.options[0].text.c_str());
            g.selectGossip(0, ms);
            next("chose to return to life");
            return 0;
        case 19:
            if (!g.spiritHealerConfirm) return 0;
            g.acceptSpiritHealer(ms);
            next("accepted the spirit healer");
            return 0;
        case 21:
            if (g.life() != Life::Dead || !g.resurrectOffer.active) return 0;
            say("LIFE: %s offers a resurrection\n", g.resurrectOffer.name.c_str());
            g.answerResurrect(true, ms);
            next("accepted the resurrection");
            return 0;
        default:
            return 0;
    }
}

// --trade: one step of the stage-5.12..5.14 script per frame (fake_server --trade): reactions, quests, a vendor.
static uint32_t tradeStep(GameSession& g, float pos[3], float& face, float dt, uint32_t ms) {
    static int step = 0;
    static uint32_t stepMs = 0;
    static int sub = 0;
    auto next = [&](const char* what) {
        say("TRADE step %d done: %s\n", step, what);
        ++step;
        sub = 0;
        stepMs = ms;
    };
    auto unitNamed = [&](const char* n) -> uint64_t {
        for (auto& [guid, o] : g.objects.all())
            if (!o.self && o.isUnitLike() && o.name == n) return guid;
        return 0;
    };
    auto find = [&](uint32_t entry) -> const InvItem* {
        static std::vector<InvItem> inv;
        inv = g.inventory();
        for (const InvItem& it : inv)
            if (it.entry == entry) return &it;
        return nullptr;
    };
    auto talk = [&](const char* n) {
        uint64_t u = unitNamed(n);
        g.setTarget(u, ms);
        std::string e = g.interact(u, pos, ms);
        if (!e.empty()) say("interact %s: %s\n", n, e.c_str());
    };
    if (ms - stepMs > 6000 && step < 99) { say("TRADE step %d TIMED OUT (sub %d)\n", step, sub); step = 99; }
    switch (step) {
        case 0: {   // reactions of everyone around once the names and the reputation list are in
            static const char* kNames[] = {"Booty Bay Bruiser", "Brother Danil", "Defias Bandit", "Deputy Willem", "Hordeguy",
                                           "Invisible Stalker", "Kobold Vermin", "Marshal McBride", "Pvpguy", "Rabbit",
                                           "Stormwind City Guard", "Young Wolf"};
            if (!g.rep.initialized) return 0;
            for (const char* n : kNames) if (!unitNamed(n)) return 0;
            say("REACTIONS:");
            for (const char* n : kNames) {
                const WorldObject* o = g.objects.find(unitNamed(n));
                say(" %s=%s/%s", n, rankName(g.shownReaction(*o)), g.canAttack(*o) ? "attack" : "no");
            }
            say("\n");
            {   // stage 5.16: auras of ours and of the wolf, the wolf's target
                const WorldObject* me = g.selfObject();
                const WorldObject* wolf = g.objects.find(unitNamed("Young Wolf"));
                say("AURAS:");
                for (const AuraView& a : g.auras(*me, ms))
                    say(" %s slot %u%s%s %us |", g.spellNames.name(a.spell).c_str(), a.slot, a.debuff ? " debuff" : "",
                        a.cancelable ? " cancelable" : "", (unsigned)((a.leftMs + 999) / 1000));
                for (const AuraView& a : g.auras(*wolf, ms)) say(" wolf: %s x%u |", g.spellNames.name(a.spell).c_str(), a.stacks);
                say(" wolf targets %s\n", g.targetOf(wolf->guid) == g.selfGuid() ? "us" : "someone else");
                bool debuff = g.cancelAura(6788, ms), buff = g.cancelAura(1127, ms);
                say("cancel debuff sent %d, buff sent %d\n", (int)debuff, (int)buff);
            }
            g.setTarget(unitNamed("Stormwind City Guard"), ms);
            g.startAttack(ms);   // refused by the client itself
            next("reactions, refused attack on the guard");
            return 0;
        }
        case 1: {
            uint64_t w = unitNamed("Deputy Willem"), m = unitNamed("Marshal McBride");
            if (g.questGiverStatus(w) != QuestMark::Available || g.questGiverStatus(m) != QuestMark::Available) return 0;
            if (g.auras(*g.selfObject(), ms).size() != 1) return 0;   // Well Fed gone, the debuff stays
            say("AURAS NOW: %zu (%s)\n", g.auras(*g.selfObject(), ms).size(), g.spellNames.name(g.auras(*g.selfObject(), ms)[0].spell).c_str());
            say("MARKS: Willem %s McBride %s\n", questMarkGlyph(g.questGiverStatus(w)), questMarkGlyph(g.questGiverStatus(m)));
            talk("Deputy Willem");
            next("marks seen, talk to Willem");
            return 0;
        }
        case 2:
            if (!g.gossip.open || g.gossip.quests.empty()) return 0;
            say("WILLEM GOSSIP: %zu quest(s), first %u \"%s\" icon %u\n", g.gossip.quests.size(), g.gossip.quests[0].id,
                g.gossip.quests[0].title.c_str(), g.gossip.quests[0].icon);
            g.selectGossipQuest(0, ms);
            next("chose the quest");
            return 0;
        case 3: {
            const QuestDialog& d = g.questDialog;
            if (d.kind != QuestDialog::Kind::Details) return 0;
            bool named = true;
            for (const QuestItemLine& l : d.choices) named = named && g.itemInfoIfKnown(l.item);
            if (!named && ms - stepMs < 3000) return 0;
            std::string text = d.text;
            for (char& c : text) if (c == '\n') c = '|';
            say("DETAILS: %u \"%s\" | %s | %s | choices:", d.quest, d.title.c_str(), text.c_str(), d.objectives.c_str());
            for (const QuestItemLine& l : d.choices) say(" %s", g.itemName(l.item).c_str());
            say(" | money %s\n", moneyText(d.money).c_str());
            g.acceptQuest(ms);
            next("accepted");
            return 0;
        }
        case 4: {
            auto log = g.questLog();
            if (log.empty() || !g.questInfoIfKnown(log[0].id)) return 0;
            if (!sub) { say("LOG: slot %u quest %u \"%s\"\n", log[0].slot, log[0].id, g.questTitle(log[0].id).c_str()); sub = 1; }
            if (!log[0].complete) return 0;
            if (g.questGiverStatus(unitNamed("Deputy Willem")) != QuestMark::RewardRep) return 0;
            say("PROGRESS:");
            for (const std::string& l : g.questProgress(log[0])) say(" %s |", l.c_str());
            say(" Willem now %s\n", questMarkGlyph(g.questGiverStatus(unitNamed("Deputy Willem"))));
            talk("Deputy Willem");
            next("quest complete, back to Willem");
            return 0;
        }
        case 5:
            if (!g.gossip.open || g.gossip.quests.empty()) return 0;
            say("WILLEM GOSSIP 2: quest %u icon %u\n", g.gossip.quests[0].id, g.gossip.quests[0].icon);
            g.selectGossipQuest(0, ms);
            next("asked to complete");
            return 0;
        case 6:
            if (g.questDialog.kind != QuestDialog::Kind::RequestItems) return 0;
            say("REQUEST ITEMS: \"%s\" wants %s x%u, completable %d\n", g.questDialog.text.c_str(),
                g.questDialog.required.empty() ? "-" : g.itemName(g.questDialog.required[0].item).c_str(),
                g.questDialog.required.empty() ? 0 : g.questDialog.required[0].count, (int)g.questDialog.completable);
            g.continueQuest(ms);
            next("continue");
            return 0;
        case 7:
            if (g.questDialog.kind != QuestDialog::Kind::OfferReward) return 0;
            say("OFFER REWARD: \"%s\" %zu choices, money %s\n", g.questDialog.text.c_str(), g.questDialog.choices.size(),
                moneyText(g.questDialog.money).c_str());
            g.completeQuest(1, ms);
            next("took the second reward");
            return 0;
        case 8: {
            if (!g.questLog().empty() || !find(1394) || g.repChanges < 2) return 0;
            say("REPUTATION:");
            for (const GameSession::RepLine& l : g.reputations())
                say(" %s %s %d%s |", l.name.c_str(), rankName(l.rank), (int)l.value, l.atWar ? " at war" : "");
            const WorldObject* b = g.objects.find(unitNamed("Booty Bay Bruiser"));
            say(" Bruiser now %s/%s\n", rankName(g.shownReaction(*b)), g.canAttack(*b) ? "attack" : "no");
            talk("Marshal McBride");
            next("rewarded, talk to McBride");
            return 0;
        }
        case 9: {
            const QuestDialog& d = g.questDialog;
            if (d.kind == QuestDialog::Kind::List && sub == 0) {
                say("MCBRIDE LIST: \"%s\" |", d.text.c_str());
                for (const GossipQuest& q : d.list) say(" %u %s (%u) |", q.id, q.title.c_str(), q.icon);
                say("\n");
                g.chooseQuest(d.npc, d.list[1], ms);
                sub = 1;
            } else if (d.kind == QuestDialog::Kind::Details && sub == 1) {
                say("DECLINE: %s\n", d.title.c_str());
                g.closeQuestDialog(ms);
                sub = 2;
                stepMs = ms;
            } else if (sub == 2 && !d.open() && ms - stepMs > 300) {
                talk("Marshal McBride");
                sub = 3;
            } else if (d.kind == QuestDialog::Kind::List && sub == 3) {
                g.chooseQuest(d.npc, d.list[0], ms);
                sub = 4;
            } else if (d.kind == QuestDialog::Kind::Details && sub == 4) {
                g.acceptQuest(ms);
                sub = 5;
            } else if (sub == 5) {
                auto log = g.questLog();
                if (log.empty()) return 0;
                say("ABANDON: slot %u quest %u\n", log[0].slot, log[0].id);
                g.abandonQuest(log[0].slot, ms);
                next("declined one quest, accepted and abandoned another");
            }
            return 0;
        }
        case 10:
            if (!g.questLog().empty()) return 0;
            talk("Brother Danil");
            next("log empty again, talk to the vendor");
            return 0;
        case 11:
            if (!g.gossip.open) return 0;
            say("DANIL GOSSIP: option 0 \"%s\" icon %u\n", g.gossip.options[0].text.c_str(), g.gossip.options[0].icon);
            g.selectGossip(0, ms);
            next("browse goods");
            return 0;
        case 12: {
            if (!g.vendor.open) return 0;
            bool named = true;
            for (const VendorItem& v : g.vendor.items) named = named && g.itemInfoIfKnown(v.entry);
            if (!named && ms - stepMs < 3000) return 0;
            say("VENDOR: repair %d |", (int)g.vendor.canRepair);
            for (const VendorItem& v : g.vendor.items)
                say(" %u %s %s x%u stock %s |", v.slot, g.itemName(v.entry).c_str(), moneyText(v.price).c_str(), v.buyCount,
                    v.unlimited() ? "-" : std::to_string(v.stock).c_str());
            say(" money %s\n", moneyText(g.money()).c_str());
            g.buyItem(0, 1, ms);
            next("buy water");
            return 0;
        }
        case 13: {
            const InvItem* w = find(159);
            if (!w) return 0;
            if (sub == 0) { g.buyItem(1, 1, ms); sub = 1; return 0; }
            if (sub == 1 && g.vendor.items[1].stock == 1) { g.buyItem(1, 1, ms); sub = 2; return 0; }
            if (sub == 2 && g.vendor.items[1].stock == 0) {
                bool third = g.buyItem(1, 1, ms), rich = g.buyItem(2, 1, ms);
                say("VENDOR BUY: water x%u, knives sold out (third buy sent %d), blade too dear (sent %d), money %s\n", w->count,
                    (int)third, (int)rich, moneyText(g.money()).c_str());
                const InvItem* l = find(2589);
                if (l) g.sellItem(*l, ms);
                next("bought, sell the linen");
            }
            return 0;
        }
        case 14: {
            if (find(2589) || g.buybackItems().empty()) return 0;
            if (const InvItem* h = find(6948)) say("sell hearthstone sent %d\n", (int)g.sellItem(*h, ms));
            if (const InvItem* sh = find(38)) g.sellItem(*sh, ms);
            next("sold the linen, try hearthstone and shirt");
            return 0;
        }
        case 15: {
            if (g.lastSellError == 0xFF) return 0;
            auto bb = g.buybackItems();
            say("BUYBACK: %zu item(s), first %s x%u for %s\n", bb.size(), g.itemName(bb[0].entry).c_str(), bb[0].count,
                moneyText(bb[0].price).c_str());
            g.buybackItem(bb[0], ms);
            next("shirt refused, buy the linen back");
            return 0;
        }
        case 16: {
            if (!find(2589) || !g.buybackItems().empty()) return 0;
            g.repairAll(ms);
            next("linen back, repair");
            return 0;
        }
        case 17: {
            const InvItem* sw = find(25);
            if (!sw || sw->durability != sw->maxDurability || g.repairs == 0 || ms - stepMs < 200) return 0;
            say("REPAIRED: sword %u/%u, money %s\n", sw->durability, sw->maxDurability, moneyText(g.money()).c_str());
            g.closeVendor();
            talk("Llane Beshere");
            next("repaired, talk to the trainer");
            return 0;
        }
        case 18:
            if (!g.gossip.open) return 0;
            g.selectGossip(0, ms);
            next("training please");
            return 0;
        case 19: {
            if (!g.trainer.open) return 0;
            say("TRAINER: \"%s\" |", g.trainer.greeting.c_str());
            for (const TrainerSpell& t : g.trainer.spells)
                say(" %s (%s) state %u L%u %s |", g.spellNames.name(t.spell).c_str(), g.spellNames.rank(t.spell).c_str(), t.state,
                    t.reqLevel, moneyText(t.cost).c_str());
            say("\n");
            bool parry = g.trainSpell(1, ms), known = g.trainSpell(2, ms);
            say("train parry sent %d, known sent %d\n", (int)parry, (int)known);
            g.trainSpell(0, ms);
            next("train Heroic Strike rank 2");
            return 0;
        }
        case 20:
            if (!g.knowsSpell(284) || g.trained == 0 || g.trainer.spells.empty() || g.trainer.spells[0].state != 2) return 0;
            say("TRAINED: knows 284 %d, list again with state %u, money %s\n", (int)g.knowsSpell(284), g.trainer.spells[0].state,
                moneyText(g.money()).c_str());
            g.closeTrainer();
            talk("Innkeeper Farley");
            next("learned, talk to the innkeeper");
            return 0;
        case 21:
            if (!g.gossip.open) return 0;
            g.selectGossip(0, ms);
            next("make this inn my home");
            return 0;
        case 22:
            if (!g.binderConfirm) return 0;
            g.acceptBinder(ms);
            next("confirmed, the quest chest next");
            return 0;
        case 23: {   // stage 5.18: the quest chest (an object: used, not selected), then its loot window
            const WorldObject* chest = nullptr;
            for (auto& [guid, o] : g.objects.all())
                if (o.type == ObjType::GameObject && o.name == "Milly's Harvest") chest = &o;
            if (!chest) return 0;
            uint64_t before = g.target();
            const char* verb = g.useVerb(*chest);
            std::string e = g.interact(chest->guid, pos, ms);
            say("OBJECT: %s verb %s, interact \"%s\", target unchanged %d\n", chest->name.c_str(), verb ? verb : "-", e.c_str(),
                (int)(g.target() == before));
            next("used the chest");
            return 0;
        }
        case 24:
            if (!g.lootWindow.open || !g.itemInfoIfKnown(11119)) return 0;
            say("CHEST LOOT: %s x%u\n", g.itemName(g.lootWindow.items[0].itemId).c_str(), g.lootWindow.items[0].count);
            g.takeAllLoot(ms);
            next("took the harvest");
            return 0;
        case 25: {
            if (sub == 0 && (g.lootWindow.open || !find(11119))) return 0;
            if (sub == 0) {   // stage 5.15: open the clam in the backpack (CMSG_OPEN_ITEM), take its meat
                const InvItem* c = find(5523);
                const ItemInfo* ci = c ? g.itemInfoIfKnown(5523) : nullptr;
                if (!c || !ci) return 0;
                say("CLAM: openable %d, usable %d\n", (int)ci->openable(), (int)ci->usable());
                g.openItem(*c, ms);
                sub = 1;
                return 0;
            }
            if (sub == 1) {
                if (!g.lootWindow.open || !g.itemInfoIfKnown(5503)) return 0;
                say("CLAM LOOT: %s x%u\n", g.itemName(g.lootWindow.items[0].itemId).c_str(), g.lootWindow.items[0].count);
                g.takeAllLoot(ms);
                sub = 2;
                return 0;
            }
            if (sub == 2 && (find(5523) || !find(5503))) return 0;
            uint64_t poster = 0;
            for (auto& [guid, o] : g.objects.all())
                if (o.type == ObjType::GameObject && o.name == "Wanted Poster") poster = guid;
            if (!poster || g.questGiverStatus(poster) != QuestMark::Available) return 0;
            say("POSTER: mark %s, verb %s\n", questMarkGlyph(g.questGiverStatus(poster)), g.useVerb(*g.objects.find(poster)));
            std::string e = g.interact(poster, pos, ms);
            if (!e.empty()) say("poster: %s\n", e.c_str());
            next("read the poster");
            return 0;
        }
        case 26:
            if (g.questDialog.kind != QuestDialog::Kind::List) return 0;
            say("POSTER LIST: \"%s\" | %s\n", g.questDialog.text.c_str(), g.questDialog.list.empty() ? "-" : g.questDialog.list[0].title.c_str());
            g.closeQuestDialog(ms);
            talk("Brother Danil");
            next("poster closed, open the vendor again");
            return 0;
        case 27:
            if (!g.gossip.open) return 0;
            g.selectGossip(0, ms);
            next("vendor again before walking away");
            return 0;
        case 28:
            if (!g.vendor.open) return 0;
            next("vendor open, walk away");
            return 0;
        case 29: {   // walk 15 yd away: the vendor window closes by itself
            float tx = -8949.95f - 18.0f, ty = -132.49f;
            float dx = tx - pos[0], dy = ty - pos[1], d = std::sqrt(dx * dx + dy * dy);
            if (d > 0.5f) {
                face = normalizeAngle(std::atan2(dy, dx));
                float st = std::min(g.mover.speeds.forFlags(MoveFlag::Forward) * dt, d);
                pos[0] += std::cos(face) * st;
                pos[1] += std::sin(face) * st;
                return MoveFlag::Forward;
            }
            say("TRADE: vendor open %d after walking away | bought %u, sold %u, buybacks %u, repairs %u, quests accepted %u, "
                "completed %u, quest queries %u, updates %u, reputation changes %u, trained %u, money %s\n", (int)g.vendor.open,
                g.bought, g.sold, g.buybacks, g.repairs, g.questsAccepted, g.questsCompleted, g.questQueries, g.questUpdates,
                g.repChanges, g.trained, moneyText(g.money()).c_str());
            step = 100;
            return 0;
        }
        default:
            return 0;
    }
}

// --party: the stage-5.17 script against fake_server --party.
static uint32_t partyStep(GameSession& g, uint32_t ms) {
    static int step = 0;
    static uint32_t stepMs = 0;
    auto next = [&](const char* what) { say("PARTY step %d done: %s\n", step, what); ++step; stepMs = ms; };
    if (ms - stepMs > 6000 && step < 99) { say("PARTY step %d TIMED OUT\n", step); step = 99; }
    auto member = [&](const char* n) -> const PartyMember* {
        for (const PartyMember& m : g.party.members) if (m.name == n) return &m;
        return nullptr;
    };
    switch (step) {
        case 0:
            if (g.groupInviteFrom.empty()) return 0;
            say("INVITE from %s\n", g.groupInviteFrom.c_str());
            g.answerGroupInvite(true, ms);
            next("accepted");
            return 0;
        case 1: {
            const PartyMember *o = member("Otherguy"), *f = member("Farguy");
            if (!o || !f || !f->statsKnown) return 0;
            PartyMember ov = g.memberView(*o), fv = g.memberView(*f);
            say("PARTY: leader %s | Otherguy L%u %u/%u online %d (in range) | Farguy L%u %u/%u zone %u online %d | leader us %d\n",
                g.objects.nameOf(g.party.leader).empty() ? "?" : g.objects.nameOf(g.party.leader).c_str(), ov.level, ov.hp, ov.maxHp,
                (int)ov.online(), fv.level, fv.hp, fv.maxHp, fv.zone, (int)fv.online(), (int)g.isLeader());
            next("members seen");
            return 0;
        }
        case 2: {
            const PartyMember* f = member("Farguy");
            if (!f || f->hp != 150) return 0;
            say("FARGUY HURT: %u/%u\n", f->hp, f->maxHp);
            next("partial stats");
            return 0;
        }
        case 3:
            if (!g.isLeader()) return 0;
            say("LEADER: us\n");
            g.sendTyped("/uninvite Farguy", ms);
            next("kick Farguy");
            return 0;
        case 4:
            if (g.party.members.size() != 1) return 0;
            g.sendTyped("/invite Nobody", ms);
            next("Farguy gone, invite nobody");
            return 0;
        case 5:
            if (ms - stepMs < 400) return 0;
            g.sendTyped("/leaveparty", ms);
            next("leave");
            return 0;
        case 6:
            if (g.party.inGroup || g.groupInviteFrom.empty()) return 0;
            say("SECOND INVITE from %s while not in a group: declined\n", g.groupInviteFrom.c_str());
            g.answerGroupInvite(false, ms);
            say("PARTY DONE: lists %u, stats %u\n", g.partyLists, g.partyStats);
            step = 100;
            return 0;
        default:
            return 0;
    }
}

// --survey (stages 5.12-5.18, real servers): after 6 s, every unit and usable object within 45 yd with its reaction,
// whether we may attack it and its quest mark, then our reputation list and quest log. Changes nothing on the server
// except the quest giver status queries every client sends.
static void printSurvey(GameSession& g, const float pos[3], uint32_t ms) {
    struct Row { float d; const WorldObject* o; };
    std::vector<Row> rows;
    for (auto& [guid, o] : g.objects.all()) {
        if (o.self || !o.hasPos || !(o.isUnitLike() || (o.type == ObjType::GameObject && g.useVerb(o)))) continue;
        float p[3];
        o.positionAt(ms, p);
        float d = std::sqrt((p[0] - pos[0]) * (p[0] - pos[0]) + (p[1] - pos[1]) * (p[1] - pos[1]));
        if (d <= 45.0f) rows.push_back({d, &o});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
    say("SURVEY: factions.txt %s (%zu templates), reputation list %s, %zu objects within 45 yd\n",
        g.factions.loaded() ? "loaded" : "MISSING", g.factions.templateCount(), g.rep.initialized ? "received" : "not received", rows.size());
    for (const Row& r : rows) {
        const WorldObject& o = *r.o;
        const char* verb = g.useVerb(o);
        if (o.type == ObjType::GameObject) {
            say("  %5.1f yd  object %-26.26s type %2u  %s%s\n", r.d, o.name.empty() ? "?" : o.name.c_str(), o.field(UF::GAMEOBJECT_TYPE_ID),
                verb ? verb : "", g.questGiverStatus(o.guid) ? (std::string(" mark ") + questMarkGlyph(g.questGiverStatus(o.guid))).c_str() : "");
            continue;
        }
        uint32_t st = g.questGiverStatus(o.guid);
        say("  %5.1f yd  %-6s %-26.26s L%-2u template %4u  %-10s %s%s%s%s\n", r.d, o.type == ObjType::Player ? "player" : "unit",
            o.name.empty() ? "?" : o.name.c_str(), o.level(), o.field(UF::UNIT_FACTIONTEMPLATE), rankName(g.shownReaction(o)),
            g.canAttack(o) ? "attackable" : "-", verb ? (std::string("  [") + verb + "]").c_str() : "",
            st ? (std::string("  mark ") + (questMarkGlyph(st)[0] ? questMarkGlyph(st) : "?") + std::to_string(st)).c_str() : "",
            o.dead() ? "  dead" : "");
    }
    say("REPUTATION:");
    for (const GameSession::RepLine& l : g.reputations()) say(" %s %s %d%s |", l.name.c_str(), rankName(l.rank), (int)l.value, l.atWar ? " war" : "");
    say("\nQUEST LOG:");
    for (const QuestLogEntry& e : g.questLog()) {
        say(" [%u] %s", e.slot, g.questTitle(e.id).c_str());
        for (const std::string& l : g.questProgress(e)) say(" / %s", l.c_str());
        say(" |");
    }
    const WorldObject* me = g.selfObject();
    say("\nAURAS:");
    if (me) for (const AuraView& a : g.auras(*me, ms)) say(" %s%s %us |", g.spellNames.name(a.spell).c_str(), a.debuff ? " (debuff)" : "", (unsigned)(a.leftMs / 1000));
    say("\nPARTY: %s, %zu other member(s)\n", g.party.inGroup ? "in a group" : "not in a group", g.party.members.size());
}

static int replay(const char* path, int maxList) {
    std::vector<LoggedPacket> pk;
    if (!readPacketLog(path, pk)) say("warning: log truncated or unreadable after %zu packets\n", pk.size());
    GameSession s;
    // The first outgoing movement packet tells us where the player was.
    uint32_t in = 0, out = 0, unhandled = 0, lastMs = 0;
    std::map<uint16_t, uint32_t> unknown;
    for (const LoggedPacket& p : pk) {
        lastMs = p.ms;
        if (p.dir == 1) {
            ++out;
            if (MoveOp::isRelayedMove(p.opcode) || p.opcode == MoveOp::Heartbeat) {
                Reader r(p.data);
                MovementInfo m;
                if (readMovementInfo(r, m)) s.mover.reset(m.x, m.y, m.z, m.o, p.ms);
            }
            continue;
        }
        ++in;
        if (p.opcode == 0x236 && p.data.size() >= 20) {   // SMSG_LOGIN_VERIFY_WORLD
            Reader r(p.data);
            r.u32();
            float x = r.f32(), y = r.f32(), z = r.f32(), o = r.f32();
            s.mover.reset(x, y, z, o, p.ms);
        }
        if (!s.handle(p.opcode, p.data, p.ms)) { ++unhandled; unknown[p.opcode]++; }
        s.objects.takeCreated();
    }
    say("replayed %s: %u server packets, %u client packets, %.1f s\n", path, in, out, lastMs / 1000.0);
    printObjects(s, lastMs, maxList);
    say("chat/combat lines: %u (melee swings %zu, xp %u, kills %u)\n", s.chatCount, s.hits.size(), s.xpGained, s.kills);
    for (const ChatLine& l : s.chat) say("  chat [%s] %s\n", Chat::typeName(l.type), s.lineText(l).c_str());
    if (!s.spells.empty()) {
        say("spells: %zu in the book, %u casts of ours, action bar:", s.spells.size(), s.spellCount);
        for (uint32_t id : s.barSpells()) say(" %s(%u)", s.spellNames.name(id).c_str(), id);
        say("\n");
    }
    {   // stage 5.10: what we carry (entries only: no item queries offline) and the look fields
        auto inv = s.inventory();
        say("inventory: %zu items, %s money:", inv.size(), moneyText(s.money()).c_str());
        for (const InvItem& it : inv) say(" %u/%u=%u%s", it.bag, it.slot, it.entry, it.count > 1 ? ("x" + std::to_string(it.count)).c_str() : "");
        say("\n");
        if (const WorldObject* me = s.selfObject()) {
            uint32_t b = me->field(UF::PLAYER_BYTES), b2 = me->field(UF::PLAYER_BYTES_2);
            say("look: skin %u face %u hair %u colour %u facial %u | life %s\n", b & 0xFF, (b >> 8) & 0xFF, (b >> 16) & 0xFF,
                b >> 24, b2 & 0xFF, s.life() == Life::Alive ? "alive" : s.life() == Life::Dead ? "dead" : "ghost");
        }
    }
    say("server opcodes not handled by the stage-5 layer (%u packets):", unhandled);
    for (auto& [op, n] : unknown) say(" 0x%03X x%u", op, n);
    say("\n");
    return s.objects.stats.failedPackets ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--replay") {
        int maxList = 15;
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--objects") maxList = atoi(argv[i + 1]);
            if (std::string(argv[i]) == "--out") g_tee = std::fopen(argv[i + 1], "w");
        }
        return replay(argv[2], maxList);
    }
    if (argc < 3) {
        sayLine("usage: w3dlogin <server[:port]> <account> [password] [--realm N] [--char N] [--stay SEC]\n"
                  "                [--protocol 3|8] [--ping SEC] [--walk] [--fight NAME] [--say TEXT]... [--cast ID]... [--spells FILE] [--log FILE]\n"
                  "                [--factions FILE] [--life] [--trade] [--party] [--survey]\n"
                  "                [--objects N] [--out FILE]\n"
                  "       w3dlogin --replay FILE [--objects N] [--out FILE]");
        return 1;
    }
    std::string server = argv[1], account = argv[2], password, logPath, outPath;
    int realmIdx = -1, charIdx = -1, stay = 20, pingSec = 30, maxList = 12;
    bool walk = false, fight = false, autoLoot = true, lifeTest = false, tradeTest = false, partyTest = false, survey = false;
    bool surveyed = false;
    std::string factionsFile;         // --factions FILE (factions.txt of w3dconv)
    std::vector<std::string> sayLines;
    std::vector<uint32_t> castList;   // --cast ID (repeatable): one cast per second once the previous one ended
    std::string spellsFile;           // --spells FILE (spells.txt of w3dconv)
    std::string createSpec, deleteName;   // --create / --delete (stage 5.29)
    bool relog = false;
    std::string fightName;
    ClientVersion ver;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--realm") realmIdx = atoi(next().c_str());
        else if (a == "--char") charIdx = atoi(next().c_str());
        else if (a == "--stay") stay = atoi(next().c_str());
        else if (a == "--ping") pingSec = atoi(next().c_str());
        else if (a == "--protocol") ver.authProtocol = (uint8_t)atoi(next().c_str());
        else if (a == "--walk") walk = true;
        else if (a == "--fight") { fight = true; fightName = next(); }
        else if (a == "--life") { lifeTest = true; autoLoot = false; }
        else if (a == "--trade") { tradeTest = true; autoLoot = false; }
        else if (a == "--party") partyTest = true;
        else if (a == "--survey") survey = true;
        else if (a == "--factions") factionsFile = next();
        else if (a == "--say") sayLines.push_back(next());
        else if (a == "--cast") castList.push_back((uint32_t)std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--spells") spellsFile = next();
        else if (a == "--log") logPath = next();
        else if (a == "--objects") maxList = atoi(next().c_str());
        else if (a == "--out") outPath = next();
        else if (a == "--relog") relog = true;   // log out to the character list, then enter the world again
        else if (a == "--create") createSpec = next();
        else if (a == "--delete") deleteName = next();
        else if (password.empty()) password = a;
    }
    if (password.empty()) {
        std::printf("password: ");
        std::fflush(stdout);
        std::getline(std::cin, password);
    }
    if (!outPath.empty()) g_tee = std::fopen(outPath.c_str(), "w");

    // ---- realmd
    AuthClient auth;
    say("connecting to login server %s ...\n", server.c_str());
    if (!auth.start(server, account, password, ver)) { say("FAILED: %s\n", auth.error().c_str()); return 2; }
    password.assign(password.size(), '\0');
    waitUntil([&] { return auth.state() == AuthClient::State::Done || auth.state() == AuthClient::State::Failed; },
              [&] { auth.update(); });
    if (auth.state() == AuthClient::State::Failed) { say("FAILED: %s\n", auth.error().c_str()); return 2; }
    const Timings& at = auth.timings();
    say("authenticated: connect %.0f ms, SRP %.1f ms, login total %.0f ms, realm list %.0f ms\n",
                at.authConnect, at.srp, at.authTotal, at.realmList);
    for (size_t i = 0; i < auth.realms().size(); ++i) {
        const Realm& r = auth.realms()[i];
        say("  [%zu] %-24s %-22s pop %.1f chars %u%s\n", i, r.name.c_str(), r.address.c_str(), r.population,
                    r.characters, (r.flags & 0x02) ? " OFFLINE" : "");
    }
    if (realmIdx < 0) realmIdx = 0;
    if (realmIdx >= (int)auth.realms().size()) { sayLine("no such realm"); return 3; }
    const Realm& realm = auth.realms()[realmIdx];

    // ---- world
    WorldClient world;
    world.pingIntervalMs = (uint32_t)pingSec * 1000;
    say("connecting to realm '%s' at %s ...\n", realm.name.c_str(), realm.address.c_str());
    if (!world.start(realm.address, auth.account(), auth.sessionKey(), ver)) {
        say("FAILED: %s\n", world.error().c_str());
        return 4;
    }
    uint32_t lastQueue = 0;
    waitUntil([&] {
        auto s = world.state();
        return s == WorldClient::State::CharSelect || s == WorldClient::State::Failed;
    }, [&] {
        world.update();
        if (world.state() == WorldClient::State::InQueue && world.queuePosition() != lastQueue)
            say("in queue, position %u\n", lastQueue = world.queuePosition());
    });
    if (world.state() == WorldClient::State::Failed) { say("FAILED: %s\n", world.error().c_str()); return 4; }
    const Timings& wt = world.timings();
    say("world session: connect %.0f ms, auth %.0f ms, character list %.0f ms\n", wt.worldConnect, wt.worldAuth,
                wt.charEnum);
    auto listChars = [&] {
        const auto& cs = world.characters();
        for (size_t i = 0; i < cs.size(); ++i) {
            const Character& c = cs[i];
            say("  [%zu] %-12s level %2u %s %s, map %u (%.1f, %.1f, %.1f) look %u %u %u %u %u %u", i, c.name.c_str(), c.level,
                raceName(c.race), className(c.cls), c.map, c.x, c.y, c.z, c.gender, c.skin, c.face, c.hairStyle, c.hairColor,
                c.facialHair);
            for (int k = 0; k < 20; ++k)
                if (c.equipDisplay[k]) say(" eq%d=%u/%u", k, c.equipDisplay[k], c.equipInv[k]);
            say("\n");
        }
    };
    listChars();
    auto waitList = [&] {
        waitUntil([&] { return world.state() == WorldClient::State::CharSelect || world.state() == WorldClient::State::Failed; },
                  [&] { world.update(); });
    };
    if (!createSpec.empty()) {   // NAME:race:class:gender[:skin:face:hs:hc:fh]
        CharCreateInfo ci;
        std::vector<std::string> f;
        size_t a = 0;
        for (size_t b; (b = createSpec.find(':', a)) != std::string::npos; a = b + 1) f.push_back(createSpec.substr(a, b - a));
        f.push_back(createSpec.substr(a));
        ci.name = f[0];
        uint8_t* fld[] = {&ci.race, &ci.cls, &ci.gender, &ci.skin, &ci.face, &ci.hairStyle, &ci.hairColor, &ci.facialHair};
        for (size_t k = 1; k < f.size() && k <= 8; ++k) *fld[k - 1] = (uint8_t)atoi(f[k].c_str());
        world.createCharacter(ci);
        int res = -1;
        waitUntil([&] { return (res = world.takeCreateResult()) >= 0 || world.state() == WorldClient::State::Failed; },
                  [&] { world.update(); });
        say("create %s: %s (0x%02X)\n", ci.name.c_str(), charCreateResultText((uint8_t)res), res);
        waitList();
        listChars();
    }
    if (!deleteName.empty()) {
        uint64_t g = 0;
        for (const Character& c : world.characters()) if (c.name == deleteName) g = c.guid;
        world.deleteCharacter(g);
        int res = -1;
        waitUntil([&] { return (res = world.takeDeleteResult()) >= 0 || world.state() == WorldClient::State::Failed; },
                  [&] { world.update(); });
        say("delete %s: %s (0x%02X)\n", deleteName.c_str(), charDeleteResultText((uint8_t)res), res);
        waitList();
        listChars();
    }
    const auto& chars = world.characters();
    if (chars.empty()) { sayLine("no characters on this realm - create one with the PC client first"); world.close(); return 0; }
    if (charIdx < 0) { sayLine("(pass --char N to enter the world)"); world.close(); return 0; }
    if (charIdx >= (int)chars.size()) { sayLine("no such character"); return 5; }

    // Record from the moment we ask to enter: the update objects arrive right after LOGIN_VERIFY_WORLD.
    PacketLog log;
    const uint64_t t0 = nowMs();
    auto rel = [&] { return (uint32_t)(nowMs() - t0); };
    if (!logPath.empty()) {
        if (log.open(logPath.c_str())) say("recording packets to %s\n", logPath.c_str());
        else say("cannot write %s\n", logPath.c_str());
    }
    GameSession game;
    bool attached = false;
    // Until attach() the session is not hooked; record what arrives anyway.
    world.onPacket = [&](const Packet& p) { log.write(rel(), 0, p.opcode, p.data); };

    world.login(chars[charIdx].guid);
    waitUntil([&] { return world.state() == WorldClient::State::InWorld || world.state() == WorldClient::State::Failed; },
              [&] { world.update(); });
    if (world.state() == WorldClient::State::Failed) { say("FAILED: %s\n", world.error().c_str()); return 6; }
    const float* p = world.position();
    say("in world: map %u at (%.2f, %.2f, %.2f) facing %.2f, enter-world %.0f ms\n", world.mapId(), p[0], p[1],
                p[2], world.orientation(), world.timings().enterWorld);
    say("  tile %d_%d (the 3DS pack must contain this tile to draw the spot)\n",
                (int)(32.0f - p[1] / 533.33333f), (int)(32.0f - p[0] / 533.33333f));
    {   // LOGIN_VERIFY_WORLD itself is consumed by WorldClient; log a copy for replays.
        Writer w;
        w.u32(world.mapId());
        w.f32(p[0]); w.f32(p[1]); w.f32(p[2]); w.f32(world.orientation());
        log.write(rel(), 0, 0x236, w.b);
    }
    game.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { log.write(rel(), dir, op, d); };
    game.attach(world, rel());
    game.autoLoot = autoLoot;
    attached = true;

    // Walk: a 3 s pattern repeated (forward, turn, strafe, backward), at server speed
    // and constant height, so a second client can watch the character move.
    float pos[3] = {p[0], p[1], p[2]}, face = world.orientation();
    uint64_t until = nowMs() + (uint64_t)stay * 1000, lastFrame = nowMs(), lastPrint = nowMs();
    uint32_t walkSegment = 99;
    uint64_t segStart = nowMs();
    // --fight / --say state
    const uint64_t inWorldAt = nowMs();
    bool said = false, looted = false, fightDone = false;
    size_t castNext = 0;
    uint32_t castAt = 2000;
    if (!spellsFile.empty()) {
        bool loaded = game.spellNames.load(spellsFile.c_str());
        say("spell names: %s (%zu)\n", loaded ? "loaded" : "NOT FOUND", game.spellNames.size());
    }
    if (!factionsFile.empty()) {
        bool loaded = game.factions.load(factionsFile.c_str());
        say("factions: %s (%zu templates, %zu factions)\n", loaded ? "loaded" : "NOT FOUND", game.factions.templateCount(),
            game.factions.factionCount());
    }
    uint64_t victim = 0;
    uint32_t chatShown = 0, swings = 0, lootAskedMs = 0;
    while (nowMs() < until && world.state() == WorldClient::State::InWorld) {
        world.update();
        uint64_t now = nowMs();
        float dt = (float)(now - lastFrame) / 1000.0f;
        lastFrame = now;
        float tp[3], to;
        if (game.takeTeleport(tp, &to)) {
            say("server teleported us to (%.2f, %.2f, %.2f)\n", tp[0], tp[1], tp[2]);
            std::memcpy(pos, tp, sizeof pos);
            face = to;
        }
        uint32_t dir = 0;
        if (walk) {
            // Closed route at server speeds: a 10.5 yd square (turning 90 deg before each side),
            // strafe right and back left, backward and forward again, then a pause.
            // Stage 5.11: then the same 3 s forward and back in walk mode (the controller's half-pushed stick),
            // and a diagonal forward + strafe (the Forever scheme's strafing in combat).
            struct Seg { uint32_t dir; uint32_t ms; bool turn; bool walkMode; };
            static const Seg kRoute[] = {
                {MoveFlag::Forward, 1500, false, false}, {MoveFlag::Forward, 1500, true, false}, {MoveFlag::Forward, 1500, true, false},
                {MoveFlag::Forward, 1500, true, false}, {MoveFlag::StrafeRight, 1500, true, false}, {MoveFlag::StrafeLeft, 1500, false, false},
                {MoveFlag::Backward, 1500, false, false}, {MoveFlag::Forward, 964, false, false},
                {MoveFlag::Forward, 1500, false, true}, {MoveFlag::Backward, 1500, false, true},
                {MoveFlag::Forward | MoveFlag::StrafeLeft, 1000, false, false}, {MoveFlag::Backward | MoveFlag::StrafeRight, 1000, false, false},
                {0, 1500, false, false}};
            const uint32_t nSeg = sizeof kRoute / sizeof kRoute[0];
            if (walkSegment == 99 || now - segStart >= kRoute[walkSegment].ms) {
                walkSegment = walkSegment == 99 ? 0 : (walkSegment + 1) % nSeg;
                segStart = now;
                if (kRoute[walkSegment].turn) face = normalizeAngle(face + 1.5707963f);
            }
            dir = kRoute[walkSegment].dir;
            if (dir) game.mover.setWalk(kRoute[walkSegment].walkMode, rel());
            float speed = game.mover.speeds.forFlags(dir | (game.mover.info().flags & ~MoveFlag::MaskMoving));
            float fx = (dir & MoveFlag::Forward) ? 1.f : (dir & MoveFlag::Backward) ? -1.f : 0.f;
            float sx = (dir & MoveFlag::StrafeLeft) ? 1.f : (dir & MoveFlag::StrafeRight) ? -1.f : 0.f;
            if (fx != 0 || sx != 0) {
                float a = face + std::atan2(sx, fx);
                pos[0] += std::cos(a) * speed * dt;
                pos[1] += std::sin(a) * speed * dt;
            }
        }
        if (fight && !fightDone && now - inWorldAt > 1000) {
            float ty;
            if (game.takeFacingRequest(&ty)) face = ty;
            if (!victim) {   // the nearest living unit with that name (names arrive with the creature queries)
                float best = 40.0f;
                for (auto& [g, o] : game.objects.all()) {
                    if (o.self || !o.hasPos || !o.isUnitLike() || o.dead() || o.name.find(fightName) == std::string::npos) continue;
                    float q[3];
                    o.positionAt(rel(), q);
                    float d = std::sqrt((q[0] - pos[0]) * (q[0] - pos[0]) + (q[1] - pos[1]) * (q[1] - pos[1]));
                    if (d < best) { best = d; victim = g; }
                }
                const WorldObject* v = game.objects.find(victim);
                if (v) {
                    say("fight: target %s (entry %u, %u hp)\n", v->name.empty() ? "?" : v->name.c_str(), v->entry(), v->health());
                    game.setTarget(victim, rel());
                    game.startAttack(rel());
                } else {
                    victim = 0;
                }
            }
            if (const WorldObject* v = victim ? game.objects.find(victim) : nullptr) {
                float vp[3];
                v->positionAt(rel(), vp);
                float dx = vp[0] - pos[0], dy = vp[1] - pos[1], d = std::sqrt(dx * dx + dy * dy);
                bool lootable = (v->field(UF::UNIT_DYNAMIC_FLAGS) & 1) != 0;
                float want = v->dead() ? 2.5f : 3.0f;
                if (d > want) {   // walk up to it at server speed
                    face = normalizeAngle(std::atan2(dy, dx));
                    dir = MoveFlag::Forward;
                    float speed = game.mover.speeds.forFlags(dir);
                    float step = std::min(speed * dt, d - want + 0.05f);
                    pos[0] += std::cos(face) * step;
                    pos[1] += std::sin(face) * step;
                } else if (v->dead()) {
                    if (lootable && !looted && rel() - lootAskedMs > 2000) {
                        lootAskedMs = rel();
                        game.loot(victim, rel());
                    }
                    if (game.lootedCopper || game.lootedItems || (!lootable && rel() - lootAskedMs > 2000)) looted = true;
                    if (looted && !game.lootOpen()) {
                        fightDone = true;
                        for (const MeleeHit& h : game.hits) swings += h.attacker == game.selfGuid();
                        say("FIGHT: %s killed, %u swings, kills %u, xp %u, loot %u copper + %u items, attack on %d\n",
                            v->name.empty() ? "?" : v->name.c_str(), swings, game.kills, game.xpGained,
                            game.lootedCopper, game.lootedItems, (int)game.attacking());
                    }
                }
            }
        }
        if (lifeTest && now - inWorldAt > 800) dir = lifeStep(game, pos, face, dt, rel());
        if (tradeTest && now - inWorldAt > 800) dir = tradeStep(game, pos, face, dt, rel());
        if (partyTest && now - inWorldAt > 500) partyStep(game, rel());
        if (survey && !surveyed && now - inWorldAt > 6000) { surveyed = true; printSurvey(game, pos, rel()); }
        if (!sayLines.empty() && !said && now - inWorldAt > 1000) {
            said = true;
            for (const std::string& l : sayLines) game.sendTyped(l, rel());
        }
        if (castNext < castList.size() && rel() >= castAt && !game.cast.active) {
            game.castSpell(castList[castNext++], rel());
            castAt = rel() + 1000;
        }
        game.mover.update(dir, pos[0], pos[1], pos[2], face, rel());
        game.update(rel());
        while (chatShown < game.chatCount) {   // print new chat / combat lines
            size_t back = game.chatCount - chatShown;
            if (back <= game.chat.size()) {
                const ChatLine& l = game.chat[game.chat.size() - back];
                say("chat [%s] %s\n", Chat::typeName(l.type), game.lineText(l).c_str());
            }
            ++chatShown;
        }
        if (now - lastPrint >= 10000) {
            lastPrint = now;
            say("--- t=%.0f s, at (%.1f, %.1f, %.1f), movement packets sent %u (heartbeats %u)\n", rel() / 1000.0,
                        pos[0], pos[1], pos[2], game.mover.packetsSent, game.mover.heartbeats);
            printObjects(game, rel(), maxList);
        }
        sleepMs(walk || fight ? 15 : 5);
    }
    if (walk) {   // stop where we are
        game.mover.stopAll(rel());
        game.update(rel());
    }
    if (world.state() == WorldClient::State::Failed) { say("FAILED while in world: %s\n", world.error().c_str()); return 7; }
    say("Warden anticheat: %s\n", world.wardenSeen() ? "YES - expect to be disconnected" : "not seen");
    say("stayed %d s: %llu bytes received, %zu distinct opcodes, latency %u ms\n", stay,
                (unsigned long long)world.bytesReceived(), world.opcodeCounts().size(), world.lastLatencyMs());
    if (!castList.empty())
        say("SPELLS: book %zu, bar %zu, casts that went off %u, last failure code %d, rage/power %u/%u\n", game.spells.size(),
            game.barSpells().size(), game.spellCount, game.lastCastFail == 0xFFFFFFFFu ? -1 : (int)game.lastCastFail,
            game.selfPower().cur, game.selfPower().max);
    say("movement packets sent %u (heartbeats %u), teleports %u, forced speed changes %u, roots %u, queries %u\n",
                game.mover.packetsSent, game.mover.heartbeats, game.stats.teleports, game.stats.speedChanges,
                game.stats.roots, game.stats.queries);
    printObjects(game, rel(), maxList);
    int shown = 0;
    for (auto& [op, n] : world.opcodeCounts())
        if (shown++ < 60) say("    0x%03X x%u\n", op, n);
    if (relog) {   // the START menu's "Characters": logout keeps the connection, the list comes again
        if (attached) { game.detach(); attached = false; }
        world.logout(true);
        waitUntil([&] { return world.state() == WorldClient::State::CharSelect || world.state() == WorldClient::State::Failed ||
                               world.state() == WorldClient::State::Done; }, [&] { world.update(); });
        if (world.state() != WorldClient::State::CharSelect) { say("RELOG FAILED: state %d %s\n", (int)world.state(), world.error().c_str()); return 8; }
        say("RELOG: back at the character list (%zu characters)\n", world.characters().size());
        world.login(chars[charIdx].guid);
        waitUntil([&] { return world.state() == WorldClient::State::InWorld || world.state() == WorldClient::State::Failed; },
                  [&] { world.update(); });
        if (world.state() != WorldClient::State::InWorld) { say("RELOG FAILED to enter again: %s\n", world.error().c_str()); return 8; }
        say("RELOG: entered the world again, map %u\n", world.mapId());
    }
    sayLine("logging out ...");
    if (attached) game.detach();
    world.logout();
    waitUntil([&] { return world.state() == WorldClient::State::Done || world.state() == WorldClient::State::Failed; },
              [&] { world.update(); });
    sayLine(world.state() == WorldClient::State::Done ? "logged out OK" : world.error().c_str());
    log.close();
    return 0;
}
