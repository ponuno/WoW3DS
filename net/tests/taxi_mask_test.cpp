// The flight master's list (6.22): the known-nodes mask of SMSG_SHOWTAXINODES is indexed by node id - 1, as in vMaNGOS.
#include <cstdio>

#include "game.hpp"

using namespace w3n;
static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    // an orc at level 1: only Orgrimmar (TaxiNodes id 23) is known -> bit 22 of word 0
    TaxiWindow t;
    t.mask[0] = 1u << 22;
    CHECK(t.known(23), "Orgrimmar (23) must be known");
    CHECK(!t.known(22) && !t.known(24), "neighbours 22 / 24 must not be known (the old reading showed 22)");
    // The Crossroads (25) learned: bit 24
    t.mask[0] |= 1u << 24;
    CHECK(t.known(25) && !t.known(26), "Crossroads (25) known, 26 not");
    // word boundaries: node 32 is bit 31 of word 0, node 33 is bit 0 of word 1
    TaxiWindow u;
    u.mask[0] = 0x80000000u; u.mask[1] = 1u;
    CHECK(u.known(32) && u.known(33) && !u.known(31) && !u.known(34), "word boundary");
    // the ends
    TaxiWindow v;
    v.mask[7] = 0x80000000u;
    CHECK(v.known(256) && !v.known(255) && !v.known(0) && !v.known(257), "ends (0 is no node, 256 is the last bit)");
    std::printf("TAXI MASK: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
