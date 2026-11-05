// blend_test — 6.83: a renewed server path (a chasing monster) starts where the server says, not where the unit was drawn: the
// difference fades out over WorldObject::kBlendMs instead of making the unit jump; a teleport is not smoothed.
//   blend_test
#include "../movement.hpp"
#include "../objects.hpp"
#include "../wire.hpp"

#include <cmath>
#include <cstdio>

using namespace w3n;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const uint64_t kG = 0xF130000000000061ull;

static Bytes create(float x) {
    Writer w;
    w.u32(1); w.u8(0);
    w.u8(2); w.packedGuid(kG); w.u8(3);
    w.u8(0x20 | 0x10);
    w.u32(0); w.u32(1000);
    w.f32(x); w.f32(0); w.f32(0); w.f32(1.0f);
    w.u32(0);
    for (int i = 0; i < 6; ++i) w.f32(i == 1 ? 7.0f : 2.5f);
    w.u32(0); w.u8(0);
    return w.b;
}
static Bytes move(float sx, float dx, uint32_t dur) {
    Writer m;
    m.packedGuid(kG);
    m.f32(sx); m.f32(0); m.f32(0);
    m.u32(1); m.u8(0); m.u32(0); m.u32(dur); m.u32(1);
    m.f32(dx); m.f32(0); m.f32(0);
    return m.b;
}

int main() {
    ObjectTable t;
    t.handle(ObjOp::SMSG_UPDATE_OBJECT, create(0), 1000);
    t.handle(ObjOp::SMSG_MONSTER_MOVE, move(0, 70, 10000), 1000);
    const WorldObject* u = t.find(kG);
    float p[3];
    u->positionAt(2000, p);
    CHECK(std::fabs(p[0] - 7.0f) < 0.05f, "drawn at 7 after 1 s (%.2f)", p[0]);
    // the server renews the path 2 yards behind where we drew it
    t.handle(ObjOp::SMSG_MONSTER_MOVE, move(5, 75, 10000), 2000);
    u = t.find(kG);
    u->positionAt(2000, p);
    CHECK(std::fabs(p[0] - 7.0f) < 0.05f, "no jump at the renewal (%.2f)", p[0]);
    u->positionAt(2000 + WorldObject::kBlendMs / 2, p);
    const float half = 5.0f + 7.0f * (WorldObject::kBlendMs / 2000.0f);
    CHECK(p[0] > half && p[0] < half + 2.0f, "halfway the difference is partly gone (%.2f, path alone %.2f)", p[0], half);
    u->positionAt(2000 + WorldObject::kBlendMs + 50, p);
    const float end = 5.0f + 7.0f * ((WorldObject::kBlendMs + 50) / 1000.0f);
    CHECK(std::fabs(p[0] - end) < 0.02f, "after kBlendMs it is on the server's path (%.2f vs %.2f)", p[0], end);
    // a teleport (50 yd away) is not smoothed
    t.handle(ObjOp::SMSG_MONSTER_MOVE, move(80, 120, 5000), 4000);
    u = t.find(kG);
    u->positionAt(4000, p);
    CHECK(std::fabs(p[0] - 80.0f) < 0.05f, "a big difference is taken at once (%.2f)", p[0]);
    // a stop where the server says
    t.handle(ObjOp::SMSG_MONSTER_MOVE, [] { Writer m; m.packedGuid(kG); m.f32(84); m.f32(0); m.f32(0); m.u32(2); m.u8(1); return m.b; }(), 4300);
    u = t.find(kG);
    u->positionAt(4300, p);
    CHECK(u->pathActive == false && std::fabs(p[0] - (80.0f + 8.0f * 0.3f)) < 0.2f, "a stop keeps the drawn place for now (%.2f)", p[0]);
    u->positionAt(4300 + WorldObject::kBlendMs + 10, p);
    CHECK(std::fabs(p[0] - 84.0f) < 0.02f, "and ends on the server's (%.2f)", p[0]);
    std::printf("BLEND: %d checks, %d failed -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
