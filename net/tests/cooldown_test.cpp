// cooldown_test — UI stage 12: our cooldowns are started by the client when a cast goes off (the 1.12 server sends none
// for an ordinary cast): Spell.dbc numbers from spells.txt ("@cd"), shared categories (all potions), the item's own numbers
// over the spell's, "on event" spells waiting for SMSG_COOLDOWN_EVENT, SMSG_ITEM_COOLDOWN, SMSG_CLEAR_COOLDOWN.
#include <cstdio>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    GameSession s;
    const uint64_t me = 0x77;
    // ourselves (SELF) and a potion item object of entry 118
    auto create = [&](uint64_t guid, uint8_t type, bool self, uint32_t entry) {
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(type);
        if (type == 4) {
            w.u8(self ? 0x71 : 0x70); w.u32(0); w.u32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0);
            for (int k = 0; k < 6; ++k) w.f32(7);
            w.u32(1);
        } else {
            w.u8(0x10); w.u32(1);
        }
        w.u8(1);
        w.u32(0xF);
        w.u32((uint32_t)guid); w.u32((uint32_t)(guid >> 32)); w.u32(type == 4 ? 0x19u : 0x3u); w.u32(entry);
        s.handle(0x0A9, w.b, 1);
    };
    create(me, 4, true, 0);
    create(0x4000000000000501ull, 1, false, 118);
    s.objects.setSelf(me);
    s.spellNames.addCooldown(100, {15000, 0, 0, false});        // Charge
    s.spellNames.addCooldown(439, {0, 4, 120000, false});       // a healing potion's spell: category 4, 2 min
    s.spellNames.addCooldown(1784, {10000, 0, 0, true});        // Stealth: on event
    auto go = [&](uint64_t item, uint32_t spell, uint32_t ms) {   // SMSG_SPELL_GO of ours
        Writer w;
        w.packedGuid(item ? item : me); w.packedGuid(me); w.u32(spell); w.u16(0); w.u8(0); w.u8(0); w.u16(0);
        s.handle(0x132, w.b, ms);
    };
    go(0, 100, 1000);
    CHECK(s.cooldownLeftMs(100, 1000) == 15000 && s.cooldownTotalMs(100) == 15000, "Charge 15 s from its GO (%u)", s.cooldownLeftMs(100, 1000));
    CHECK(s.cooldownLeftMs(100, 16000) == 0, "Charge ready after 15 s");
    CHECK(s.gcdLeftMs(1000) == kGlobalCooldownMs, "a spell starts the GCD");
    {   // the item's query answer: spell 439, own cooldown -1 (the spell's), category 4 with 60 s
        ItemInfo i;
        i.entry = 118;
        i.name = "Minor Healing Potion";
        i.spellId[0] = 439; i.spellTrigger[0] = 0; i.spellCooldown[0] = -1; i.spellCategory[0] = 4; i.spellCatCooldown[0] = 60000;
        s.testSetItemInfo(i);
    }
    go(0x4000000000000501ull, 439, 2000);
    CHECK(s.itemCooldownLeftMs(118, 2000) == 60000 && s.itemCooldownTotalMs(118) == 60000, "potion: the item's 60 s over the spell's 120 s (%u)",
          s.itemCooldownLeftMs(118, 2000));
    CHECK(s.cooldownLeftMs(439, 32000) == 30000, "the category is shared by the spell");
    CHECK(s.gcdLeftMs(2000) == 0 || s.gcdLeftMs(2000) <= kGlobalCooldownMs - 1000, "an item does not start the GCD");
    // on event: nothing at GO, the time runs from SMSG_COOLDOWN_EVENT
    go(0, 1784, 3000);
    CHECK(s.cooldownLeftMs(1784, 3000) == 0, "Stealth: no cooldown while it lasts");
    { Writer w; w.u32(1784); w.u64(me); s.handle(0x135, w.b, 9000); }
    CHECK(s.cooldownLeftMs(1784, 9000) == 10000, "Stealth: 10 s from the event");
    // an equipped trinket: SMSG_ITEM_COOLDOWN = 30 s on its spell; SMSG_CLEAR_COOLDOWN ends one
    { Writer w; w.u64(0x4000000000000999ull); w.u32(23723); s.handle(0x0B0, w.b, 10000); }
    CHECK(s.cooldownLeftMs(23723, 10000) == 30000, "item cooldown 30 s");
    { Writer w; w.u32(100); w.u64(me); s.handle(0x1DE, w.b, 10500); }
    go(0, 100, 10000);
    { Writer w; w.u32(100); w.u64(me); s.handle(0x1DE, w.b, 10500); }
    CHECK(s.cooldownLeftMs(100, 10500) == 0, "cleared by the server");
    std::printf("COOLDOWNS: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
