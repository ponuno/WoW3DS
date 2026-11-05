// lootroll_test — 6.88: group loot rolls. SMSG_LOOT_START_ROLL in vMaNGOS' layout (no vote mask) and cMaNGOS' (a vote mask
// byte), the choice going out as CMSG_LOOT_ROLL (guid, slot, vote), the chat lines of the others' choices and throws, the
// winner, "everyone passed", a roll sent twice, and the window leaving when the countdown is over.
#include <cstdio>
#include <string>
#include <vector>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    GameSession s;
    std::vector<std::pair<uint16_t, Bytes>> out;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) out.push_back({op, d}); };
    const uint64_t me = 0x77, mate = 0x88, wolf = 0xF130000045000101ull;
    {   // ourselves: the session learns its own guid from the object created with the SELF flag
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(me); w.u8(4);
        w.u8(0x71); w.u32(0); w.u32(0); for (int k = 0; k < 5; ++k) w.f32(0);
        for (int k = 0; k < 6; ++k) w.f32(7);
        w.u32(1);
        w.u8(1); w.u32(0x7); w.u32((uint32_t)me); w.u32(0); w.u32(0x19);
        s.handle(0x0A9, w.b, 1);
        s.update(2);
    }
    {   // Hogger's loot: the item's query answer so the lines carry its name
        ItemInfo i; i.entry = 1959; i.name = "Cold Iron Pick"; i.quality = 2; i.bonding = 2;
        s.testSetItemInfo(i);
    }
    auto start = [&](uint32_t slot, uint32_t item, bool mask, uint8_t m, uint32_t ms) {
        Writer w;
        w.u64(wolf); w.u32(slot); w.u32(item); w.u32(0); w.u32(0); w.u32(60000);
        if (mask) w.u8(m);
        s.handle(0x2A1, w.b, ms);
    };
    auto lastChat = [&]() { return s.chat.empty() ? std::string() : s.lineText(s.chat.back()); };
    start(0, 1959, false, 0, 1000);
    CHECK(s.lootRolls.size() == 1 && s.lootRolls[0].itemId == 1959 && s.lootRolls[0].voteMask == 7 && s.lootRolls[0].countdownMs == 60000,
          "a roll from vMaNGOS (no vote mask): all three choices");
    start(0, 1959, false, 0, 1500);
    CHECK(s.lootRolls.size() == 1, "the same roll sent again is one window");
    start(1, 2000, true, 0x5, 2000);   // cMaNGOS: pass + greed only
    CHECK(s.lootRolls.size() == 2 && s.lootRolls[1].voteMask == 5, "vote mask read when there (%u)", s.lootRolls.size() > 1 ? s.lootRolls[1].voteMask : 0);
    // our choice: Need on the first
    s.rollLoot(0, kRollNeed, 3000);
    CHECK(s.lootRolls.size() == 1 && s.lootRolls[0].slot == 1, "answered roll leaves the list");
    CHECK(!out.empty() && out.back().first == 0x2A0 && out.back().second.size() == 13 && out.back().second[8] == 0 && out.back().second[12] == kRollNeed,
          "CMSG_LOOT_ROLL guid + slot 0 + need");
    {
        uint64_t g = 0; for (int k = 7; k >= 0; --k) g = g << 8 | out.back().second[k];
        CHECK(g == wolf, "the looted guid");
    }
    // the others: a mate chose Greed (number 0), then the throws, then the winner
    auto roll = [&](uint64_t who, uint8_t number, uint8_t type, uint32_t ms) {
        Writer w;
        w.u64(wolf); w.u32(0); w.u64(who); w.u32(1959); w.u32(0); w.u32(0); w.u8(number); w.u8(type);
        s.handle(0x2A2, w.b, ms);
    };
    roll(me, 0, kRollNeed, 3100);
    CHECK(lastChat() == "You have selected Need for: [Cold Iron Pick]", "own choice: '%s'", lastChat().c_str());
    roll(mate, 0, kRollPass, 3200);
    CHECK(lastChat().find("passed on: [Cold Iron Pick]") != std::string::npos, "a pass: '%s'", lastChat().c_str());
    roll(me, 87, kRollNeed, 4000);
    CHECK(lastChat() == "Need Roll - 87 for [Cold Iron Pick] by You", "the throw: '%s'", lastChat().c_str());
    {
        Writer w;
        w.u64(wolf); w.u32(0); w.u32(1959); w.u32(0); w.u32(0); w.u64(me); w.u8(87); w.u8(kRollNeed);
        s.handle(0x29F, w.b, 4100);
    }
    CHECK(lastChat() == "You won: [Cold Iron Pick] (Need 87)", "won: '%s'", lastChat().c_str());
    // everyone passed on the second item: its window goes
    {
        Writer w;
        w.u64(wolf); w.u32(1); w.u32(2000); w.u32(0); w.u32(0);
        s.handle(0x29E, w.b, 5000);
    }
    CHECK(s.lootRolls.empty() && lastChat() == "Everyone passed on: [Item 2000]", "all passed: '%s'", lastChat().c_str());
    // a roll nobody answers is gone when the countdown ends (the server passes for us)
    start(2, 1959, false, 0, 10000);
    s.update(69000);
    CHECK(s.lootRolls.size() == 1 && s.lootRolls[0].leftMs(69000) == 1000, "still there 1 s before the end");
    s.update(70001);
    CHECK(s.lootRolls.empty(), "timed out");
    // a truncated packet is ignored
    { Writer w; w.u64(wolf); w.u32(3); s.handle(0x2A1, w.b, 71000); }
    CHECK(s.lootRolls.empty(), "short START_ROLL ignored");
    std::printf("LOOT ROLL: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
