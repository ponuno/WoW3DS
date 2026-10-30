// The character window's numbers (UI stage 14): stats from the player's update fields, the average item level of the
// worn gear (two-hander in both hands, shirt / tabard / empty ranged slot not counted), durability.
#include <cstdio>
#include <cstring>

#include "charsheet.hpp"
#include "game.hpp"

using namespace w3n;
static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void setF(WorldObject* o, uint16_t f, uint32_t v) { if (o->fields.size() <= f) o->fields.resize(f + 1u, 0); o->fields[f] = v; }
static uint32_t asBits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }

int main() {
    GameSession s;
    const uint64_t me = 0x77;
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
    s.objects.setSelf(me);
    WorldObject* p = s.objects.find(me);
    setF(p, UF::UNIT_LEVEL, 30);
    setF(p, SF::STAT0 + 0, 120); setF(p, SF::STAT0 + 2, 150);
    setF(p, SF::POSSTAT0 + 0, 20); setF(p, SF::NEGSTAT0 + 2, (uint32_t)-5);
    setF(p, SF::RESISTANCE0, 1850); setF(p, SF::RESISTANCE0 + 2, 15);
    setF(p, SF::MINDAMAGE, asBits(34.4f)); setF(p, SF::MAXDAMAGE, asBits(61.6f));
    setF(p, SF::ATTACK_POWER, 300); setF(p, SF::ATTACK_POWER_MODS, 40u | (10u << 16));   // +40 -10
    setF(p, SF::CRIT, asBits(5.25f)); setF(p, SF::DODGE, asBits(4.5f)); setF(p, SF::BLOCK, asBits(12.0f));
    setF(p, SF::MOD_DAMAGE_DONE_POS + 3, 45);
    // gear: entry, slot, item level
    struct G { uint32_t entry; int slot; uint32_t ilvl, invType; };
    const G gear[] = {{1001, 0, 30, 1}, {1002, 4, 32, 5}, {1003, 3, 99, 4},   // head, chest, shirt (not counted)
                      {1004, 15, 40, 17}};                                     // a two-hander
    uint64_t g = 0x4000000000000100ull;
    for (const G& it : gear) {
        create(g, 1, false, it.entry);
        WorldObject* o = s.objects.find(g);
        setF(o, UF::ITEM_FIELD_DURABILITY, 50); setF(o, UF::ITEM_FIELD_MAXDURABILITY, 100);
        setF(p, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * it.slot, (uint32_t)g);
        setF(p, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * it.slot + 1, (uint32_t)(g >> 32));
        ItemInfo i;
        i.entry = it.entry; i.name = "x"; i.itemLevel = it.ilvl; i.inventoryType = it.invType;
        s.testSetItemInfo(i);
        ++g;
    }
    CharSheet c = buildCharSheet(s);
    CHECK(c.valid && c.level == 30, "level");
    CHECK(c.stat[0] == 120 && c.stat[2] == 150 && c.statBonus[0] == 20 && c.statBonus[2] == -5, "stats and their bonuses");
    CHECK(c.armor == 1850 && c.resist[1] == 15, "armor and resistances (fire is index 1 after holy) armor %d fire %d", c.armor, c.resist[1]);
    CHECK(c.dmgMin == 34 && c.dmgMax == 62, "damage %d-%d", c.dmgMin, c.dmgMax);
    CHECK(c.attackPower == 330, "attack power with its two-short mods (%d)", c.attackPower);
    CHECK(c.crit > 5.2f && c.crit < 5.3f && c.dodge > 4.4f && c.parry == 0 && c.block > 11.9f, "percentages");
    CHECK(c.spellPower == 45, "spell power (%d)", c.spellPower);
    // slots counted: head, neck, shoulder, back, chest, wrist, hands, waist, legs, feet, 2 rings, 2 trinkets, main hand, off hand = 16
    // (no ranged); the two-hander fills the off hand: (30 + 32 + 40 + 40) / 16 = 8.875 -> 9
    CHECK(c.itemsWorn == 4 && c.itemsKnown == 4, "items worn %d known %d", c.itemsWorn, c.itemsKnown);
    CHECK(c.itemLevel == 9, "average item level (%d)", c.itemLevel);
    CHECK(c.durabilityPct == 50, "durability %u", c.durabilityPct);
    std::printf("CHARSHEET: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
