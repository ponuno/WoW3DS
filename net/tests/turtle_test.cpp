// turtle_test — 6.58: the Turtle WoW clone server's movement layouts (as WoWee's TurtlePacketParsers reads them) against the
// 1.12 ones: a unit on a transport with Turtle's u32 time after the transport position, the same unit in 1.12's layout (the
// fallback), a block after each still aligned; the spline under TBC's flag 0x08000000; SMSG_MONSTER_MOVE with the Animation
// part and zlib-compressed + wrapped. A vanilla table must read 1.12's layout exactly as before.
//   turtle_test
#include "../crypto.hpp"
#include "../movement.hpp"
#include "../objects.hpp"
#include "../wire.hpp"

#include <cmath>
#include <cstdio>

using namespace w3n;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// a CREATE block of a unit at (x, y, z); flags: movement flags; turtleTime: write Turtle's transport time
static void unitCreate(Writer& w, uint64_t guid, float x, float y, float z, uint32_t flags, bool turtleTime, bool splineTbc = false) {
    w.u8(2);   // CREATE
    w.packedGuid(guid);
    w.u8(3);   // unit
    w.u8(0x20 | 0x10);   // LIVING | ALL
    w.u32(flags | (splineTbc ? 0x08000000u : 0));
    w.u32(1000);
    w.f32(x); w.f32(y); w.f32(z); w.f32(1.0f);
    if (flags & MoveFlag::OnTransport) {
        w.packedGuid(0x1F00000000000001ull);
        w.f32(1); w.f32(2); w.f32(3); w.f32(0);
        if (turtleTime) w.u32(123456);
    }
    w.u32(0);   // fall time
    for (int i = 0; i < 6; ++i) w.f32(i == 1 ? 7.0f : 2.5f);
    if (splineTbc) {   // a spline: flags, passed, duration, id, 1 point, end
        w.u32(0); w.u32(0); w.u32(2000); w.u32(9); w.u32(1);
        w.f32(x + 5); w.f32(y); w.f32(z);
        w.f32(x + 10); w.f32(y); w.f32(z);
    }
    w.u32(0);   // ALL
    w.u8(0);    // no field mask blocks
}

static Bytes update(const std::vector<Bytes>& blocks) {
    Writer w;
    w.u32((uint32_t)blocks.size());
    w.u8(0);
    for (const Bytes& b : blocks) w.raw(b);
    return w.b;
}

static Bytes block(void (*f)(Writer&)) { Writer w; f(w); return w.b; }

int main() {
    const uint32_t onT = MoveFlag::OnTransport;
    // 1. Turtle layout on a Turtle table: both units in place
    {
        ObjectTable t;
        t.turtle = true;
        Writer a, b;
        unitCreate(a, 0xF130000000000011ull, 100, 200, 30, onT, true);
        unitCreate(b, 0xF130000000000012ull, 110, 210, 31, 0, true);
        t.handle(ObjOp::SMSG_UPDATE_OBJECT, update({a.b, b.b}), 1000);
        const WorldObject* u1 = t.find(0xF130000000000011ull);
        const WorldObject* u2 = t.find(0xF130000000000012ull);
        CHECK(u1 && std::fabs(u1->mv.x - 100) < 1e-3f && u1->speeds.run == 7.0f, "turtle unit on a transport");
        CHECK(u2 && std::fabs(u2->mv.y - 210) < 1e-3f && u2->speeds.run == 7.0f, "the next block aligned");
        CHECK(t.stats.failedPackets == 0 && t.stats.turtleFallbacks == 0, "no failure / fallback (%u / %u)", t.stats.failedPackets,
              t.stats.turtleFallbacks);
    }
    // 2. 1.12 layout on a Turtle table: the fallback reads it
    {
        ObjectTable t;
        t.turtle = true;
        Writer a, b;
        unitCreate(a, 0xF130000000000021ull, 100, 200, 30, onT, false);
        unitCreate(b, 0xF130000000000022ull, 110, 210, 31, 0, false);
        t.handle(ObjOp::SMSG_UPDATE_OBJECT, update({a.b, b.b}), 1000);
        const WorldObject* u1 = t.find(0xF130000000000021ull);
        const WorldObject* u2 = t.find(0xF130000000000022ull);
        CHECK(u1 && u1->speeds.run == 7.0f && u2 && std::fabs(u2->mv.x - 110) < 1e-3f, "1.12 layout through the fallback");
        CHECK(t.stats.failedPackets == 0, "no failed packet");
    }
    // 3. a vanilla table reads 1.12 as before, and does not take Turtle's
    {
        ObjectTable t;
        Writer a, b;
        unitCreate(a, 0xF130000000000031ull, 100, 200, 30, onT, false);
        unitCreate(b, 0xF130000000000032ull, 110, 210, 31, 0, false);
        t.handle(ObjOp::SMSG_UPDATE_OBJECT, update({a.b, b.b}), 1000);
        CHECK(t.find(0xF130000000000032ull) && t.stats.failedPackets == 0, "vanilla unchanged");
    }
    // 4. the spline under TBC's bit (Turtle)
    {
        ObjectTable t;
        t.turtle = true;
        Writer a, b;
        unitCreate(a, 0xF130000000000041ull, 100, 200, 30, 0, true, true);
        unitCreate(b, 0xF130000000000042ull, 110, 210, 31, 0, true);
        t.handle(ObjOp::SMSG_UPDATE_OBJECT, update({a.b, b.b}), 1000);
        const WorldObject* u1 = t.find(0xF130000000000041ull);
        CHECK(u1 && u1->pathActive && u1->path.size() == 3, "spline under 0x08000000 read (%zu points)", u1 ? u1->path.size() : 0);
        CHECK(t.find(0xF130000000000042ull) != nullptr, "aligned after the spline");
    }
    // 5. SMSG_MONSTER_MOVE: Animation part, then zlib-compressed and wrapped
    {
        ObjectTable t;
        t.turtle = true;
        Writer c;
        unitCreate(c, 0xF130000000000051ull, 0, 0, 0, 0, true);
        t.handle(ObjOp::SMSG_UPDATE_OBJECT, update({c.b}), 1000);
        Writer m;
        m.packedGuid(0xF130000000000051ull);
        m.f32(10); m.f32(20); m.f32(5);
        m.u32(77);              // spline id
        m.u8(0);                // normal
        m.u32(0x00400000u);     // Animation
        m.u8(3); m.u32(500);    // its type, start
        m.u32(1500);            // duration
        m.u32(1);               // one point: the destination
        m.f32(30); m.f32(20); m.f32(5);
        t.handle(ObjOp::SMSG_MONSTER_MOVE, m.b, 2000);
        const WorldObject* u = t.find(0xF130000000000051ull);
        CHECK(u && u->pathActive && u->pathDurMs == 1500 && std::fabs(u->path.back().x - 30) < 1e-3f, "monster move with Animation");
        // compressed + wrapped: u32 size, zlib(u8 size, u16 opcode, body)
        Writer inner;
        inner.u8((uint8_t)(m.b.size() + 2));
        inner.u16(ObjOp::SMSG_MONSTER_MOVE);
        inner.raw(m.b);
        const Bytes z = zlibStored(inner.b);
        Writer pk;
        pk.u32((uint32_t)inner.b.size());
        pk.raw(z);
        const uint32_t before = t.stats.monsterMoves;
        t.handle(ObjOp::SMSG_MONSTER_MOVE, pk.b, 3000);
        CHECK(t.stats.monsterMoves == before + 1 && t.stats.turtleCompressedMoves == 1, "compressed monster move (%u)", t.stats.monsterMoveFails);
        // a vanilla table: an ordinary monster move unchanged
        ObjectTable v;
        Writer c2;
        unitCreate(c2, 0xF130000000000052ull, 0, 0, 0, 0, false);
        v.handle(ObjOp::SMSG_UPDATE_OBJECT, update({c2.b}), 1000);
        Writer m2;
        m2.packedGuid(0xF130000000000052ull);
        m2.f32(10); m2.f32(20); m2.f32(5);
        m2.u32(1); m2.u8(0); m2.u32(0); m2.u32(900); m2.u32(1);
        m2.f32(30); m2.f32(20); m2.f32(5);
        v.handle(ObjOp::SMSG_MONSTER_MOVE, m2.b, 2000);
        CHECK(v.stats.monsterMoves == 1 && v.stats.monsterMoveFails == 0, "vanilla monster move unchanged");
    }
    std::printf("TURTLE PROTOCOL: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
