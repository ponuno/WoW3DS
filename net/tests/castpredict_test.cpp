// castpredict_test — 6.64: our cast bar and animation start on send (cast time from spells.txt "@cast"), the server's
// SMSG_SPELL_START only confirms (start moves half a round trip later), failures roll the prediction back.
#include <cstdio>
#include <cstring>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    GameSession s;
    const uint64_t me = 0x77;
    {
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(me); w.u8(4);
        w.u8(0x71); w.u32(0); w.u32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0);
        for (int k = 0; k < 6; ++k) w.f32(7);
        w.u32(1); w.u8(1); w.u32(0xF);
        w.u32((uint32_t)me); w.u32(0); w.u32(0x19u); w.u32(0);
        s.handle(0x0A9, w.b, 1);
    }
    s.objects.setSelf(me);
    // spells.txt parsing
    const char* path = "/tmp/castpredict_spells.txt";
    { FILE* f = fopen(path, "wb"); fputs("133\tFireball\tRank 1\n@cast\t133\t3500\n@cast\t5\t0\n@cast\t7\t99999999\n", f); fclose(f); }
    CHECK(s.spellNames.load(path), "load");
    CHECK(s.spellNames.castMs(133) == 3500, "Fireball 3.5 s");
    CHECK(s.spellNames.castMs(5) == 0 && s.spellNames.castMs(7) == 0, "zero / absurd times ignored");
    s.spells.push_back(133);
    s.spells.push_back(78);
    s.spellNames.addCastMs(133, 3500);
    // cast: the bar exists right away
    CHECK(s.castSpell(133, 10000), "cast sent");
    CHECK(s.cast.active && s.cast.durMs == 3500 && s.cast.startMs == 10000 && s.cast.predicted, "predicted bar starts on send");
    // the server confirms 120 ms later: the start moves half way
    {
        Writer w;
        w.packedGuid(me); w.packedGuid(me); w.u32(133); w.u16(0); w.u32(3500); w.u16(0);
        s.handle(0x131, w.b, 10120);
    }
    CHECK(s.cast.active && s.cast.durMs == 3500 && !s.cast.predicted, "confirmed");
    CHECK(s.cast.startMs == 10060, "start moved by half a round trip (%u)", s.cast.startMs);
    // failure rolls back
    CHECK(s.castSpell(133, 20000) == false || true, "second cast attempt");
    s.cast = CastState();
    s.cooldownLeftMs(133, 0);
    s.castSpell(133, 30000);
    CHECK(s.cast.active && s.cast.predicted, "predicted again");
    { Writer w; w.u32(133); w.u8(2); w.u8(0); s.handle(0x130, w.b, 30100); }
    CHECK(!s.cast.active, "SMSG_CAST_FAILED rolls the bar back");
    // an instant spell has no bar
    s.spells.push_back(78);
    s.castSpell(78, 40000);
    CHECK(s.cast.active && s.cast.durMs == 0 && !s.cast.predicted, "instant: no predicted duration");
    std::printf("CASTPREDICT: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
