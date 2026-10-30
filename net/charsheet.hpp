// charsheet.hpp — what the character window (the top screen of the Equipment tab, UI stage 14) shows: the stats of the
// player object (1.12.1 update fields, vMaNGOS UpdateFields_1_12_1.h) and the average item level of what is worn.
// Header-only on purpose: the PC tests that link a fixed list of net sources keep building.
#pragma once
#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <vector>

#include "game.hpp"
#include "objects.hpp"

namespace w3n {

namespace SF {   // update fields not in objects.hpp's UF (OBJECT_END 6 + the header's offsets)
enum : uint16_t {
    MINDAMAGE = 134, MAXDAMAGE = 135,
    STAT0 = 150,                  // strength, agility, stamina, intellect, spirit: the total with auras and gear
    RESISTANCE0 = 155,            // armor, then holy, fire, nature, frost, shadow, arcane
    ATTACK_POWER = 165, ATTACK_POWER_MODS = 166,                // TWO_SHORT: + / -
    RANGED_ATTACK_POWER = 168, RANGED_ATTACK_POWER_MODS = 169,
    MINRANGEDDAMAGE = 171, MAXRANGEDDAMAGE = 172,
    BLOCK = 1106, DODGE = 1107, PARRY = 1108, CRIT = 1109, RANGED_CRIT = 1110,   // floats (percent)
    POSSTAT0 = 1177, NEGSTAT0 = 1182,                           // what gear / auras added to / took from each stat
    MOD_DAMAGE_DONE_POS = 1201,                                 // 7 schools: physical, holy, fire, nature, frost, shadow, arcane
};
}  // namespace SF

struct CharSheet {
    int level = 0;
    int stat[5] = {0, 0, 0, 0, 0};       // strength, agility, stamina, intellect, spirit (total)
    int statBonus[5] = {0, 0, 0, 0, 0};  // + gear / buffs, - debuffs (colours the number)
    int armor = 0;
    int resist[6] = {0, 0, 0, 0, 0, 0};  // holy, fire, nature, frost, shadow, arcane
    int dmgMin = 0, dmgMax = 0;          // main hand, per swing
    int attackPower = 0, rangedAttackPower = 0;
    int rangedMin = 0, rangedMax = 0;
    float crit = 0, rangedCrit = 0, dodge = 0, parry = 0, block = 0;   // percent
    int spellPower = 0;                  // the best of the magic schools' bonus damage
    // the average item level of the worn gear (Classic Era style: head, neck, shoulders, back, chest, wrists, hands,
    // waist, legs, feet, two rings, two trinkets, main hand, off hand, ranged; a two-hander counts for both hands;
    // shirt and tabard do not count; the ranged slot only when something is in it)
    int itemLevel = 0;                   // rounded, 0 = nothing known yet
    int itemsWorn = 0, itemsKnown = 0;   // worn items counted / of them with known data (the rest are still being asked for)
    uint32_t durabilityPct = 100;
    bool valid = false;
};

inline int charsheetTwoShortSum(uint32_t v) { return (int)(int16_t)(v & 0xFFFF) - (int)(int16_t)(v >> 16); }

inline CharSheet buildCharSheet(const GameSession& g) {
    CharSheet c;
    const WorldObject* me = g.selfObject();
    if (!me) return c;
    c.valid = true;
    c.level = (int)me->level();
    auto F = [&](uint16_t f) { return (int)me->field(f); };
    for (int i = 0; i < 5; ++i) {
        c.stat[i] = F(SF::STAT0 + i);
        c.statBonus[i] = F(SF::POSSTAT0 + i) + F(SF::NEGSTAT0 + i);   // (the negative part is already negative)
    }
    c.armor = F(SF::RESISTANCE0);
    for (int i = 0; i < 6; ++i) c.resist[i] = F(SF::RESISTANCE0 + 1 + i);
    c.dmgMin = (int)lroundf(me->fieldF(SF::MINDAMAGE));
    c.dmgMax = (int)lroundf(me->fieldF(SF::MAXDAMAGE));
    c.attackPower = F(SF::ATTACK_POWER) + charsheetTwoShortSum(me->field(SF::ATTACK_POWER_MODS));
    c.rangedAttackPower = F(SF::RANGED_ATTACK_POWER) + charsheetTwoShortSum(me->field(SF::RANGED_ATTACK_POWER_MODS));
    c.rangedMin = (int)lroundf(me->fieldF(SF::MINRANGEDDAMAGE));
    c.rangedMax = (int)lroundf(me->fieldF(SF::MAXRANGEDDAMAGE));
    c.crit = me->fieldF(SF::CRIT);
    c.rangedCrit = me->fieldF(SF::RANGED_CRIT);
    c.dodge = me->fieldF(SF::DODGE);
    c.parry = me->fieldF(SF::PARRY);
    c.block = me->fieldF(SF::BLOCK);
    for (int s = 1; s < 7; ++s) c.spellPower = std::max(c.spellPower, F(SF::MOD_DAMAGE_DONE_POS + s));

    // item level
    uint32_t entryAt[19] = {0};
    uint32_t dur = 0, durMax = 0;
    for (const InvItem& it : g.inventory()) {
        if (it.bag != kBagSelf || it.slot >= 19) continue;
        entryAt[it.slot] = it.entry;
        dur += it.durability;
        durMax += it.maxDurability;
    }
    c.durabilityPct = durMax ? (uint32_t)(dur * 100ull / durMax) : 100;
    auto levelOf = [&](uint32_t entry, bool* known, uint32_t* invType) {
        const ItemInfo* info = entry ? g.itemInfoIfKnown(entry) : nullptr;
        if (!info || info->missing) { *known = false; return 0u; }
        *known = true;
        if (invType) *invType = info->inventoryType;
        return info->itemLevel;
    };
    uint64_t sum = 0;
    int slots = 0;
    static const int kCounted[] = {0, 1, 2, 14, 4, 8, 9, 5, 6, 7, 10, 11, 12, 13, 15, 16, 17};
    for (int s : kCounted) {
        const uint32_t e = entryAt[s];
        if (s == 17 && !e) continue;                  // no ranged weapon: the slot is not counted
        uint32_t useEntry = e;
        if (s == 16 && !e && entryAt[15]) {           // a two-hander fills both hands
            bool known;
            uint32_t inv = 0;
            levelOf(entryAt[15], &known, &inv);
            if (known && inv == 17) useEntry = entryAt[15];
        }
        ++slots;
        if (!useEntry) continue;                      // an empty slot counts as 0
        ++c.itemsWorn;
        bool known;
        const uint32_t lv = levelOf(useEntry, &known, nullptr);
        if (known) { sum += lv; ++c.itemsKnown; }
        // (unknown ones are asked for by the Equipment tab of the touch screen, which draws every worn item)
    }
    if (slots && c.itemsKnown) c.itemLevel = (int)((sum + (uint64_t)slots / 2) / (uint64_t)slots);
    return c;
}

}  // namespace w3n
