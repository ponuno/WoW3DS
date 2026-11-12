// weaponicon_test — 7.13: only actual weapon swings use the weapon's action-bar icon. The server marks every ranged
// ability as ranged for the shooting animation (Arcane Shot, Serpent Sting ...); their own SpellIcon must stay.
#include <cstdio>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    SpellNames n;
    int w = -1;
    CHECK(weaponSwingSpell(n, kSpellAttack, &w) && w == 0, "Attack -> main hand");

    n.add(75, "Auto Shot", "");
    n.addCombat(75, SpellNames::kCombatRanged);
    CHECK(weaponSwingSpell(n, 75, &w) && w == 1, "Auto Shot -> ranged weapon");

    n.add(3044, "Arcane Shot", "Rank 1");
    n.addCombat(3044, SpellNames::kCombatRanged);
    CHECK(!weaponSwingSpell(n, 3044, &w), "Arcane Shot keeps its own icon (got %d)", w);

    n.add(1978, "Serpent Sting", "Rank 1");
    n.addCombat(1978, SpellNames::kCombatRanged);
    CHECK(!weaponSwingSpell(n, 1978, &w), "Serpent Sting keeps its own icon");

    // a custom server's shot id: the spell book name decides
    n.add(9999, "Shoot Bow", "");
    n.addCombat(9999, SpellNames::kCombatRanged);
    CHECK(weaponSwingSpell(n, 9999, &w) && w == 1, "a named Shoot variant -> ranged weapon");

    n.add(9998, "Throw Weapon", "");
    CHECK(weaponSwingSpell(n, 9998, &w) && w == 1, "a named Throw variant -> ranged weapon");

    n.add(78, "Heroic Strike", "Rank 1");
    n.addCombat(78, SpellNames::kCombatMelee);
    CHECK(!weaponSwingSpell(n, 78, &w), "a melee ability does not take the weapon icon");

    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
