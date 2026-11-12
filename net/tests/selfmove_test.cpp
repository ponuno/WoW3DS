// selfmove_test — 7.13: the server moves our own character along a spline (Charge, taxi). GameSession exposes it while it
// runs and puts the mover at its exact end when it stops; before this the client drew us at the last position we sent.
#include <algorithm>
#include <cmath>
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
    const uint64_t me = 0x77;
    auto create = [&](uint32_t flags) {
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(me); w.u8(4);
        w.u8(0x71); w.u32(0); w.u32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0);
        for (int k = 0; k < 6; ++k) w.f32(7);
        w.u32(1);
        values(w, {{0, (uint32_t)me}, {1, (uint32_t)(me >> 32)}, {2, 0x19u}, {UF::UNIT_HEALTH, 100},
                   {UF::UNIT_MAXHEALTH, 100}, {UF::UNIT_FLAGS, flags}});
        s.handle(0x0A9, w.b, 1);
    };
    auto move = [&](float sx, float dx, uint32_t dur) {
        Writer m;
        m.packedGuid(me);
        m.f32(sx); m.f32(0); m.f32(0);
        m.u32(1); m.u8(0); m.u32(0); m.u32(dur); m.u32(1);
        m.f32(dx); m.f32(0); m.f32(0);
        s.handle(ObjOp::SMSG_MONSTER_MOVE, m.b, 1000);
    };
    create(0);
    s.objects.setSelf(me);
    move(0, 20, 10000);

    float p[3], yaw = 0;
    bool taxi = true;
    CHECK(s.selfServerMove(2000, p, &yaw, &taxi), "a self spline is followed");
    CHECK(!taxi, "a charge is not a taxi");
    CHECK(std::fabs(p[0] - 2.0f) < 0.05f, "at 2 s of 10 (%.2f)", p[0]);
    CHECK(s.selfServerMove(6000, p, &yaw, &taxi) && std::fabs(p[0] - 10.0f) < 0.05f, "at 5 s of the spline (%.2f)", p[0]);

    const float last[3] = {999, 999, 999};
    s.endSelfServerMove(last, 0, 11000);
    CHECK(std::fabs(s.mover.info().x - 20.0f) < 0.05f, "the mover ends at the spline's end (%.2f)", s.mover.info().x);
    CHECK(!s.selfServerMove(12000, p, &yaw, &taxi), "the spline is over");
    const WorldObject* self = s.objects.find(me);
    CHECK(self && !self->pathActive, "the self path is cleared");

    // a taxi has the same spline but asks for the mount camera
    create(0x00100000u);
    move(0, 10, 5000);
    CHECK(s.selfServerMove(1500, p, &yaw, &taxi) && taxi, "taxi flag is reported");

    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
