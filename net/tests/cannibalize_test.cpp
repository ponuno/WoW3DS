// cannibalize_test — 7.13: Cannibalize (20577) works on a corpse. The client must not soft-target a living enemy instead,
// and the cast goes without a unit target: vMaNGOS Spell::EffectDummy 20577 finds an edible corpse near the caster itself.
#include <algorithm>
#include <cstdio>
#include <cstring>
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
    auto create = [&](uint64_t guid, uint8_t type, bool self, float x, std::map<uint16_t, uint32_t> f) {
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(type);
        w.u8(self ? 0x71 : 0x70); w.u32(0); w.u32(0); w.f32(x); w.f32(0); w.f32(0); w.f32(0); w.f32(0);
        for (int k = 0; k < 6; ++k) w.f32(7);
        w.u32(1);
        f[0] = (uint32_t)guid; f[1] = (uint32_t)(guid >> 32); f[2] = type == 4 ? 0x19u : 0x9u;
        values(w, f);
        s.handle(0x0A9, w.b, 1);
    };
    const uint64_t me = 0x77, dead = 0xF130000045000301ull, wolf = 0xF130000045000302ull;
    create(me, 4, true, 0, {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}});
    s.objects.setSelf(me);
    create(dead, 3, false, 1, {{UF::UNIT_HEALTH, 0}, {UF::UNIT_MAXHEALTH, 60}});
    create(wolf, 3, false, 3, {{UF::UNIT_HEALTH, 50}, {UF::UNIT_MAXHEALTH, 60}});
    s.spellNames.add(20577, "Cannibalize", "");
    CHECK(s.corpseSpell(20577), "20577 is a corpse spell");
    CHECK(!s.corpseSpell(78), "Heroic Strike is not");

    auto lastMask = [&]() -> int {
        for (auto it = out.rbegin(); it != out.rend(); ++it)
            if (it->first == 0x12E && it->second.size() >= 6)
                return (int)(it->second[4] | (it->second[5] << 8));
        return -1;
    };
    s.setTarget(dead, 1000);
    out.clear();
    CHECK(s.castSpell(20577, 1000), "cast at a dead target");
    CHECK(lastMask() == 0, "no unit target: the server finds the corpse (mask %d)", lastMask());

    s.setTarget(wolf, 2000);
    out.clear();
    CHECK(s.castSpell(20577, 4000), "cast with a living enemy selected");
    CHECK(lastMask() == 0, "the living enemy is not sent (mask %d)", lastMask());

    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
