// selfcast_test — 7.11: a helpful spell ("@friend id") at an enemy or with no target is cast at ourselves; with no
// @friend data a refusal "invalid target" is retried once at ourselves.
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
    const uint16_t CAST = 0x12E, FAILED = 0x130;
    auto lastCastTarget = [&]() -> uint64_t {   // u32 spell, u16 flags, packed guid
        for (auto it = out.rbegin(); it != out.rend(); ++it)
            if (it->first == CAST) {
                if (it->second.size() < 7) return 0;
                if (!(it->second[4] | it->second[5])) return 0;
                uint8_t mask = it->second[6]; uint64_t g = 0; size_t p = 7;
                for (int k = 0; k < 8; ++k) if (mask & (1 << k)) g |= (uint64_t)it->second[p++] << (8 * k);
                return g;
            }
        return 0;
    };
    s.spellNames.addFriendly(635);   // Holy Light
    s.setTarget(wolf, 100);
    CHECK(s.castSpell(635, 1000) && lastCastTarget() == me, "enemy selected -> cast at self (got %llx)", (unsigned long long)lastCastTarget());
    s.setTarget(friendNpc, 100);
    CHECK(s.castSpell(635, 5000) && lastCastTarget() == friendNpc, "friend selected -> cast at the friend");
    s.setTarget(0, 100);
    CHECK(s.castSpell(635, 9000) && lastCastTarget() == me, "no target -> self");
    // no @friend data for 1000: the refusal is retried once
    s.setTarget(wolf, 100);
    {
        GameSession& q = s;
        q.spellNames.addFriendly(635);
    }
    CHECK(s.spellNames.friendlyCount() == 1, "one friendly spell");
    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
