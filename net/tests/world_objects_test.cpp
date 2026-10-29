// World objects (stage 5.41): elevators / trams (GO type 11, TransportAnimation), tilted objects (GAMEOBJECT_ROTATION), door states,
// dynamic objects (ground spell areas) and corpses, through ObjectTable with hand-built update blocks.
#include "objects.hpp"
#include "wire.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace w3n;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
#define NEAR(a, b, e) do { if (!(std::fabs((a) - (b)) <= (e))) { printf("FAIL line %d: %s = %g, want %g\n", __LINE__, #a, (double)(a), (double)(b)); ++fails; } } while (0)

static uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

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

static bool createGo(ObjectTable& ot, uint64_t guid, uint8_t updFlags, const float pos[4], bool transport, uint32_t progress,
                     std::initializer_list<std::pair<int, uint32_t>> f, uint32_t nowMs) {
    Writer w;
    w.u32(1); w.u8(0);
    w.u8(3);   // CREATE2
    w.packedGuid(guid); w.u8(5);
    w.u8(updFlags);
    w.f32(pos[0]); w.f32(pos[1]); w.f32(pos[2]); w.f32(pos[3]);
    if (transport) w.u32(progress);
    fields(w, f);
    return ot.parseUpdate(w.b.data(), w.b.size(), nowMs);
}

static bool valuesBlock(ObjectTable& ot, uint64_t guid, std::initializer_list<std::pair<int, uint32_t>> f, uint32_t nowMs) {
    Writer w;
    w.u32(1); w.u8(0);
    w.u8(0);   // VALUES
    w.packedGuid(guid);
    fields(w, f);
    return ot.parseUpdate(w.b.data(), w.b.size(), nowMs);
}

int main() {
    // ---------------------------------------------------------------- elevator
    {
        ObjectTable ot;
        ot.setSelf(1);
        // entry 20650: rests 0..4000 at the top, goes down 40 yd in 6 s, rests, comes back (period 20 s); the x offset makes the rotation matter
        std::string txt = "A\t20650\t0\t0.0\t0.0\t0.0\nA\t20650\t4000\t0.0\t0.0\t0.0\nA\t20650\t10000\t10.0\t0.0\t-40.0\n"
                          "A\t20650\t14000\t10.0\t0.0\t-40.0\nA\t20650\t20000\t0.0\t0.0\t0.0\n";
        CHECK(ot.transports.parse(txt));
        const ElevatorAnim* ea = ot.transports.elevator(20650);
        CHECK(ea && ea->valid() && ea->total() == 20000 && ea->size() == 5);
        const float yaw = 0.5f;
        const float st[4] = {-1000.0f, 200.0f, 50.0f, yaw};
        const float qz = std::sin(yaw / 2), qw = std::cos(yaw / 2);
        const uint64_t lift = 0x1F10000000000002ull;
        CHECK(createGo(ot, lift, 0x40 | 0x02, st, true, 7000,
                       {{3, 20650}, {8, 455}, {10, fbits(0)}, {11, fbits(0)}, {12, fbits(qz)}, {13, fbits(qw)}, {14, 1}, {21, 11}}, 1000));
        const WorldObject* o = ot.find(lift);
        CHECK(o && o->hasTransportProgress && o->transportProgress == 7000 && o->goType() == 11);
        CHECK(o && o->stationary[0] == st[0] && o->stationary[3] == yaw);
        // t = 1000: progress 7000 = halfway down (4000..10000): offset (5, 0, -20)
        ot.tickTransports(1000);
        const TransportPose* p = ot.transportPose(lift);
        CHECK(p && p->valid);
        auto expect = [&](float ox, float oy, float oz, float out[3]) {   // independent: rotate by -yaw, flip y, add the resting place
            const float c = std::cos(yaw), s = std::sin(yaw);
            const float vx = c * ox + s * oy, vy = -s * ox + c * oy;
            out[0] = st[0] + vx; out[1] = st[1] - vy; out[2] = st[2] + oz;
        };
        float e[3];
        expect(5, 0, -20, e);
        if (p) { NEAR(p->x, e[0], 1e-3f); NEAR(p->y, e[1], 1e-3f); NEAR(p->z, e[2], 1e-3f); NEAR(p->o, yaw, 1e-6f); }
        // the object itself follows
        float pos[3], ori;
        o->positionAt(1000, pos, &ori);
        NEAR(pos[2], 30.0f, 1e-3f);
        // 3 s later: 10000 ms -> bottom
        ot.tickTransports(4000);
        p = ot.transportPose(lift);
        expect(10, 0, -40, e);
        if (p) { NEAR(p->x, e[0], 1e-3f); NEAR(p->y, e[1], 1e-3f); NEAR(p->z, e[2], 1e-3f); }
        // wraps at the period: 7000 + 20000 -> same as 7000
        ot.tickTransports(21000);
        p = ot.transportPose(lift);
        expect(5, 0, -20, e);
        if (p) { NEAR(p->z, e[2], 1e-3f); NEAR(p->x, e[0], 1e-3f); }
        // at progress 0 (very start) the offset is the first node
        ElevatorAnim a2;
        a2.add({0, 1, 2, 3}); a2.add({1000, 1, 2, 3}); a2.add({2000, 5, 2, 3});
        float off[3];
        CHECK(a2.offsetAt(0, off) && off[0] == 1);
        CHECK(a2.offsetAt(1500, off) && std::fabs(off[0] - 3.0f) < 1e-5f);
        CHECK(a2.offsetAt(3500, off) && std::fabs(off[0] - 3.0f) < 1e-5f);   // modulo 2000
        // a passenger on the cabin is carried with it (the generic transport code, a different transport kind)
        const uint64_t pl = 77;
        {
            Writer w;
            w.u32(1); w.u8(0);
            w.u8(3);
            w.packedGuid(pl); w.u8(4);
            w.u8(0x20);
            w.u32(MoveFlag::OnTransport); w.u32(5000);
            w.f32(9999); w.f32(9999); w.f32(9999); w.f32(0);
            w.packedGuid(lift); w.f32(1.5f); w.f32(-0.5f); w.f32(0.25f); w.f32(0.0f);
            w.u32(0);
            for (int i = 0; i < 6; ++i) w.f32(7.0f);
            fields(w, {{3, 0}});
            CHECK(ot.parseUpdate(w.b.data(), w.b.size(), 21000));
        }
        ot.tickTransports(21000);
        float pp[3], po;
        CHECK(ot.find(pl));
        ot.find(pl)->positionAt(21000, pp, &po);
        const TransportPose P = *ot.transportPose(lift);
        NEAR(pp[0], P.x + 1.5f * std::cos(yaw) + 0.5f * std::sin(yaw), 1e-3f);
        NEAR(pp[1], P.y + 1.5f * std::sin(yaw) - 0.5f * std::cos(yaw), 1e-3f);
        NEAR(pp[2], P.z + 0.25f, 1e-3f);
        // an elevator of an unknown entry stays where the server put it, and is not a pose
        const uint64_t lift2 = 0x1F10000000000003ull;
        CHECK(createGo(ot, lift2, 0x40 | 0x02, st, true, 0, {{3, 99999}, {8, 456}, {21, 11}}, 1000));
        ot.tickTransports(1000);
        CHECK(!ot.transportPose(lift2));
    }

    // ---------------------------------------------------------------- tilt and door state
    {
        ObjectTable ot;
        ot.setSelf(1);
        const float pos[4] = {10, 20, 30, 1.0f};
        const uint64_t log1 = 0x1F10000000000010ull, door = 0x1F10000000000011ull, flat = 0x1F10000000000012ull;
        // a log lying at ~30 degrees about x, facing 1 rad: quaternion = Rz(1) * Rx(0.52)
        const float ax = 0.52f, yaw = 1.0f;
        const float qx0 = std::sin(ax / 2), qw0 = std::cos(ax / 2);
        const float cz = std::cos(yaw / 2), sz = std::sin(yaw / 2);
        const float qx = cz * qx0, qy = sz * qx0, qz = sz * qw0, qw = cz * qw0;   // z-rot * x-rot
        CHECK(createGo(ot, log1, 0x40, pos, false, 0, {{3, 1}, {8, 7}, {10, fbits(qx)}, {11, fbits(qy)}, {12, fbits(qz)}, {13, fbits(qw)}, {21, 5}}, 100));
        float q[4];
        const WorldObject* o = ot.find(log1);
        CHECK(o && o->goTilt(q));
        NEAR(q[0], qx, 1e-5f); NEAR(q[1], qy, 1e-5f); NEAR(q[2], qz, 1e-5f); NEAR(q[3], qw, 1e-5f);
        // an upright object (only z / w set): rotation known, but not a tilt
        const float fz = std::sin(0.4f), fw = std::cos(0.4f);
        CHECK(createGo(ot, flat, 0x40, pos, false, 0, {{3, 2}, {8, 8}, {10, fbits(0)}, {11, fbits(0)}, {12, fbits(fz)}, {13, fbits(fw)}, {21, 5}}, 100));
        o = ot.find(flat);
        CHECK(o && o->goRotation(q) && !o->goTilt(q));
        // all-zero rotation (not sent): neither
        CHECK(createGo(ot, door, 0x40, pos, false, 0, {{3, 3}, {8, 9}, {14, 1}, {21, 0}}, 100));
        o = ot.find(door);
        CHECK(o && !o->goRotation(q) && !o->goTilt(q));
        // state: created closed, no change seen; then it opens
        CHECK(o && o->goType() == 0 && o->goState() == 1 && o->stateChangeMs == 0);
        CHECK(valuesBlock(ot, door, {{14, 0}}, 5000));
        o = ot.find(door);
        CHECK(o->goState() == 0 && o->stateChangeMs == 5000);
        // the same value again is not a change
        CHECK(valuesBlock(ot, door, {{14, 0}}, 6000));
        CHECK(ot.find(door)->stateChangeMs == 5000);
        CHECK(valuesBlock(ot, door, {{14, 1}}, 9000));
        CHECK(ot.find(door)->goState() == 1 && ot.find(door)->stateChangeMs == 9000);
        // a malformed rotation (NaN, huge) is refused
        CHECK(valuesBlock(ot, door, {{10, 0x7FC00000u}, {11, fbits(0)}, {12, fbits(0)}, {13, fbits(1)}}, 9500));
        CHECK(!ot.find(door)->goRotation(q));
    }

    // ---------------------------------------------------------------- dynamic object and corpse
    {
        ObjectTable ot;
        ot.setSelf(1);
        const uint64_t dyn = 0x2000000000000005ull;
        {   // a Blizzard-like area: HAS_POSITION, fields caster, spell, radius
            Writer w;
            w.u32(1); w.u8(0);
            w.u8(3);
            w.packedGuid(dyn); w.u8(6);
            w.u8(0x40);
            w.f32(100); w.f32(-50); w.f32(12); w.f32(0);
            fields(w, {{6, 0x1234}, {7, 0}, {9, 10187}, {10, fbits(8.0f)}, {11, fbits(100)}, {12, fbits(-50)}, {13, fbits(12)}});
            CHECK(ot.parseUpdate(w.b.data(), w.b.size(), 100));
        }
        const WorldObject* d = ot.find(dyn);
        CHECK(d && d->type == ObjType::DynamicObject && d->hasPos);
        CHECK(d && d->dynCaster() == 0x1234ull && d->dynSpell() == 10187);
        NEAR(d->dynRadius(), 8.0f, 1e-6f);
        CHECK(ot.count(ObjType::DynamicObject) == 1);
        const uint64_t cor = 0x3000000000000006ull;
        {
            Writer w;
            w.u32(1); w.u8(0);
            w.u8(3);
            w.packedGuid(cor); w.u8(7);
            w.u8(0x40);
            w.f32(5); w.f32(6); w.f32(7); w.f32(2.0f);
            fields(w, {{6, 0x99}, {7, 0x10000}, {12, 1478}, {13, 1234u | (7u << 24)}, {20, 55u | (4u << 24)}, {32, 0x03020201u}, {33, 0x00010402u},
                       {35, 0x5}});
            CHECK(ot.parseUpdate(w.b.data(), w.b.size(), 100));
        }
        const WorldObject* c = ot.find(cor);
        CHECK(c && c->type == ObjType::Corpse && c->hasPos);
        CHECK(c && c->corpseOwner() == (0x99ull | (0x10000ull << 32)) && c->corpseDisplay() == 1478);
        CHECK(c && c->field(UF::CORPSE_FIELD_BYTES_1) == 0x03020201u && c->corpseBones() && (c->corpseFlags() & 4));
        CHECK(c && (c->field(UF::CORPSE_FIELD_ITEM) & 0xFFFFFF) == 1234 && (c->field(UF::CORPSE_FIELD_ITEM) >> 24) == 7);
        // gone with an out-of-range block
        Writer w;
        w.u32(1); w.u8(0);
        w.u8(4); w.u32(1); w.packedGuid(dyn);
        CHECK(ot.parseUpdate(w.b.data(), w.b.size(), 200));
        CHECK(!ot.find(dyn) && ot.find(cor));
    }
    printf("WORLD-OBJECTS: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
