// channels_test — the default channels of the original client: General / Trade (capitals) / LocalDefense follow the zone,
// LookingForGroup is global and joined once; a new session starts empty.
#include <cstdio>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    WorldClient wc;
    GameSession s;
    s.attach(wc, 0);   // (no socket: sends go nowhere, the tap sees them)
    std::vector<std::string> ops;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) {
        if (dir != 1 || (op != 0x097 && op != 0x098)) return;
        Reader r(d);
        ops.push_back(std::string(op == 0x097 ? "+" : "-") + r.cstr());
    };
    s.updateDefaultChannels(0, "", 1);                   // nothing known yet
    CHECK(ops.empty(), "no zone, no channels");
    s.updateDefaultChannels(14, "Durotar", 10);
    CHECK(ops.size() == 3 && ops[0] == "+General - Durotar" && ops[1] == "+LocalDefense - Durotar" && ops[2] == "+LookingForGroup",
          "Durotar: General, LocalDefense, LFG (got %zu)", ops.size());
    ops.clear();
    s.updateDefaultChannels(14, "Durotar", 20);
    CHECK(ops.empty(), "the same zone: nothing");
    s.updateDefaultChannels(1637, "Orgrimmar", 30);
    CHECK(ops.size() == 5 && ops[0] == "-General - Durotar" && ops[1] == "-LocalDefense - Durotar" && ops[2] == "+General - Orgrimmar" &&
          ops[3] == "+Trade - Orgrimmar" && ops[4] == "+LocalDefense - Orgrimmar", "Orgrimmar: the city adds Trade; LFG stays (got %zu)", ops.size());
    ops.clear();
    s.updateDefaultChannels(14, "Durotar", 40);
    CHECK(ops.size() == 5 && ops[0] == "-General - Orgrimmar" && ops[1] == "-Trade - Orgrimmar" && ops[2] == "-LocalDefense - Orgrimmar" &&
          ops[3] == "+General - Durotar" && ops[4] == "+LocalDefense - Durotar", "back out: Trade left (got %zu)", ops.size());
    ops.clear();
    s.autoChannels = false;
    s.updateDefaultChannels(12, "Elwynn Forest", 50);
    CHECK(ops.empty(), "switched off");
    s.autoChannels = true;
    s.attach(wc, 60);   // a new session: everything again
    s.updateDefaultChannels(12, "Elwynn Forest", 70);
    CHECK(ops.size() == 3 && ops[2] == "+LookingForGroup", "new session joins again (got %zu)", ops.size());
    std::printf("DEFAULT CHANNELS: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
