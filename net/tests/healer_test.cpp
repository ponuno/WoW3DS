// healer_test — 7.08: a ghost talking to the spirit healer. The server answers CMSG_GOSSIP_HELLO with a gossip menu
// ("Return me to life"); the client picks it at once (CMSG_GOSSIP_SELECT_OPTION) instead of opening a gossip window the death
// page hid. A menu without options asks for the confirmation directly. Not a ghost / not a healer: the menu opens as before.
#include <cstdio>
#include <initializer_list>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void fields(Writer& w, std::initializer_list<std::pair<int, uint32_t>> f) {
    uint32_t mask[8] = {};
    int top = 0;
    for (auto& p : f) { mask[p.first / 32] |= 1u << (p.first % 32); top = std::max(top, p.first / 32 + 1); }
    w.u8((uint8_t)top);
    for (int i = 0; i < top; ++i) w.u32(mask[i]);
    for (int b = 0; b < top; ++b)
        for (int bit = 0; bit < 32; ++bit)
            for (auto& p : f) if (p.first == b * 32 + bit) w.u32(p.second);
}

static void create(GameSession& s, uint64_t guid, uint8_t type, bool self, std::initializer_list<std::pair<int, uint32_t>> f) {
    Writer w;
    w.u32(1); w.u8(0); w.u8(3); w.packedGuid(guid); w.u8(type);
    w.u8(self ? 0x61 : 0x60);
    w.u32(0); w.u32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0); w.u32(0);
    for (int k = 0; k < 6; ++k) w.f32(7);
    fields(w, f);
    s.handle(0x0A9, w.b, 100);
}

static Bytes gossip(uint64_t npc, int options) {
    Writer w;
    w.u64(npc); w.u32(0); w.u32((uint32_t)options);
    for (int i = 0; i < options; ++i) { w.u32((uint32_t)i); w.u8(0); w.u8(0); w.cstr("Return me to life."); }
    w.u32(0);
    return w.b;
}

int main() {
    const uint64_t me = 0x77, healer = 0xF130001955000001ull, vendor = 0xF130000001000002ull;
    for (int ghost = 0; ghost < 2; ++ghost) {
        GameSession s;
        std::vector<std::pair<uint16_t, Bytes>> sent;
        s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) sent.push_back({op, d}); };
        s.objects.setSelf(me);
        create(s, me, 4, true, {{UF::UNIT_HEALTH, ghost ? 1u : 100u}, {UF::UNIT_MAXHEALTH, 100}, {UF::PLAYER_FLAGS, ghost ? 0x10u : 0u}});
        create(s, healer, 3, false, {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}, {UF::UNIT_NPC_FLAGS, 0x20}});
        create(s, vendor, 3, false, {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}, {UF::UNIT_NPC_FLAGS, 0x1 | 0x4}});
        CHECK((s.life() == Life::Ghost) == (ghost != 0), "life %d", ghost);
        sent.clear();
        s.handle(0x17D, gossip(healer, 1), 200);
        bool selected = false;
        for (auto& p : sent) if (p.first == 0x17C) { Reader r(p.second); selected = r.u64() == healer && r.u32() == 0; }
        if (ghost) CHECK(selected && !s.gossip.open, "ghost: the healer's option is chosen at once, no hidden window");
        else CHECK(!selected && s.gossip.open, "alive: the menu opens as before");
        if (ghost) {
            s.handle(0x17D, gossip(healer, 0), 300);
            CHECK(s.spiritHealerConfirm == healer && !s.gossip.open, "no options: straight to Accept / Decline");
            s.spiritHealerConfirm = 0;
            sent.clear();
            s.handle(0x17D, gossip(vendor, 1), 400);
            CHECK(s.gossip.open, "another NPC's menu is left alone");
            bool sel = false;
            for (auto& p : sent) sel = sel || p.first == 0x17C;
            CHECK(!sel, "nothing chosen for another NPC");
        }
    }
    std::printf("SPIRIT HEALER: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
