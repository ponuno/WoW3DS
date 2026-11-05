// autoattack_test — 6.87: auto-attack as the original client drives it. A melee ability at an enemy turns it on
// (spells.txt "@combat id 1"), Gouge-like spells turn the wish off ("@combat id 4"), right-click (interact) on an enemy
// attacks it, the attack follows a new enemy target and ends on a friend / no target, the fight ends when the victim
// dies even when SMSG_ATTACKSTOP came before the update that killed it, and a spell at a unit out of its range
// ("@range id min max") is refused before it is sent.
#include <cstdio>
#include <map>
#include <vector>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    GameSession s;
    std::vector<std::pair<uint16_t, Bytes>> out;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) out.push_back({op, d}); };
    auto sent = [&](uint16_t op) { size_t n = 0; for (auto& p : out) n += p.first == op; return n; };
    auto lastGuid = [&](uint16_t op) -> uint64_t {   // the u64 of the last such packet
        for (auto it = out.rbegin(); it != out.rend(); ++it)
            if (it->first == op && it->second.size() >= 8) { uint64_t g = 0; for (int k = 7; k >= 0; --k) g = g << 8 | it->second[k]; return g; }
        return 0;
    };
    const uint64_t me = 0x77, wolf = 0xF130000045000101ull, boar = 0xF130000045000102ull, friendNpc = 0xF130000045000103ull;
    auto values = [&](Writer& w, const std::map<uint16_t, uint32_t>& f) {
        uint16_t top = 0;
        for (auto& kv : f) top = std::max<uint16_t>(top, kv.first);
        const uint8_t blocks = (uint8_t)(top / 32 + 1);
        w.u8(blocks);
        std::vector<uint32_t> mask(blocks, 0);
        for (auto& kv : f) mask[kv.first / 32] |= 1u << (kv.first % 32);
        for (uint32_t m : mask) w.u32(m);
        for (auto& kv : f) w.u32(kv.second);
    };
    auto fl = [](float v) { uint32_t u; memcpy(&u, &v, 4); return u; };
    auto create = [&](uint64_t guid, uint8_t type, bool self, float x, std::map<uint16_t, uint32_t> f) {
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(type);
        w.u8(self ? 0x71 : 0x70); w.u32(0); w.u32(0); w.f32(x); w.f32(0); w.f32(0); w.f32(0); w.f32(0);
        for (int k = 0; k < 6; ++k) w.f32(7);
        w.u32(1);
        f[0] = (uint32_t)guid; f[1] = (uint32_t)(guid >> 32); f[2] = type == 4 ? 0x19u : 0x9u;
        f[UF::UNIT_BOUNDINGRADIUS] = fl(0.5f);
        values(w, f);
        s.handle(0x0A9, w.b, 1);
    };
    create(me, 4, true, 0, {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}});
    s.objects.setSelf(me);
    create(wolf, 3, false, 4, {{UF::UNIT_HEALTH, 50}, {UF::UNIT_MAXHEALTH, 50}});
    create(boar, 3, false, 50, {{UF::UNIT_HEALTH, 60}, {UF::UNIT_MAXHEALTH, 60}});
    create(friendNpc, 3, false, 6, {{UF::UNIT_HEALTH, 60}, {UF::UNIT_NPC_FLAGS, 1}});
    s.spellNames.addCombat(1752, SpellNames::kCombatMelee);    // Sinister Strike
    s.spellNames.addCombat(1776, SpellNames::kCombatStops);    // Gouge
    s.spellNames.addCombat(3044, SpellNames::kCombatRanged);   // Arcane Shot
    s.spellNames.addRange(3044, 8, 35);
    const uint16_t CAST = 0x12E, SWING = 0x141, STOP = 0x142;

    // a melee ability at the wolf starts auto-attack; a second one does not ask again
    s.setTarget(wolf, 100);
    CHECK(s.castSpell(1752, 200) && sent(CAST) == 1 && sent(SWING) == 1 && lastGuid(SWING) == wolf && s.attackWanted(), "Sinister Strike starts auto-attack");
    s.castSpell(1752, 2000);
    CHECK(sent(SWING) == 1, "already attacking: no second ATTACKSWING (%zu)", sent(SWING));
    // Gouge: the server stops the attack; we forget the wish
    s.castSpell(1776, 4000);
    CHECK(!s.attackWanted() && sent(SWING) == 1, "Gouge ends the wish to attack");
    // a ranged ability does not start melee
    out.clear();
    s.castSpell(3044, 6000);
    CHECK(sent(SWING) == 0, "Arcane Shot does not start melee");

    // range: the wolf is 4 yd away (3 between the spheres): too close for Arcane Shot (8-35); the boar 50 yd: out of range
    out.clear();
    s.castSpell(3044, 8000);
    CHECK(sent(CAST) == 0, "too close: refused before sending");
    s.setTarget(boar, 9000);
    out.clear();
    s.castSpell(3044, 10000);
    CHECK(sent(CAST) == 0, "out of range: refused before sending");

    // right-click on an enemy attacks it; the attack follows a new enemy target, a friend ends it
    out.clear();
    float pos[3] = {0, 0, 0};
    s.interact(wolf, pos, 12000);
    CHECK(sent(SWING) == 1 && lastGuid(SWING) == wolf && s.target() == wolf, "interact with an enemy = attack");
    s.setTarget(boar, 13000);
    CHECK(sent(SWING) == 2 && lastGuid(SWING) == boar && s.attackWanted(), "the attack follows the new enemy");
    s.setTarget(friendNpc, 14000);
    CHECK(sent(STOP) == 1 && !s.attackWanted(), "a friendly target ends it");

    // the fight ends with the kill even when ATTACKSTOP comes before the update that says the wolf is dead
    s.setTarget(wolf, 15000);
    s.castSpell(kSpellAttack, 15100);
    CHECK(s.attackWanted(), "Attack");
    { Writer w; w.u64(me); w.u64(wolf); s.handle(0x143, w.b, 15200); }   // SMSG_ATTACKSTART
    { Writer w; w.packedGuid(me); w.packedGuid(wolf); w.u32(0); s.handle(0x144, w.b, 16000); }   // SMSG_ATTACKSTOP: wolf still alive here
    s.update(16010);
    CHECK(s.inCombat(), "still fighting until the wolf is known dead");
    { Writer w; w.u32(1); w.u8(0); w.u8(0); w.packedGuid(wolf); values(w, {{UF::UNIT_HEALTH, 0}}); s.handle(0x0A9, w.b, 16050); }
    s.update(16100);
    CHECK(!s.inCombat() && !s.attackWanted(), "combat mode ends after the kill");
    std::printf("AUTO-ATTACK: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
