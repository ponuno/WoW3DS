// ObjectTable + transports (stage 5.21): a ship created with its path progress moves along the path, the player standing on
// it is carried with it.
#include "objects.hpp"
#include "wire.hpp"
#include <cmath>
#include <cstdio>
using namespace w3n;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void fields(Writer& w, std::initializer_list<std::pair<int, uint32_t>> f) {
    uint32_t mask[4] = {0, 0, 0, 0};
    int top = 0;
    for (auto& p : f) { mask[p.first / 32] |= 1u << (p.first % 32); top = std::max(top, p.first / 32 + 1); }
    w.u8((uint8_t)top);
    for (int i = 0; i < top; ++i) w.u32(mask[i]);
    for (int b = 0; b < top; ++b)
        for (int bit = 0; bit < 32; ++bit)
            for (auto& p : f) if (p.first == b * 32 + bit) w.u32(p.second);
}

int main() {
    ObjectTable ot;
    ot.setSelf(1);
    std::string txt;
    // A -> B straight ferry on map 0: stops at 0 and 600, nodes every 50 yd along x
    int idx = 0;
    auto node = [&](float x, uint32_t fl, uint32_t delay) {
        char b[128];
        std::snprintf(b, sizeof b, "T\t9\t%d\t0\t%.1f\t0.0\t5.0\t%u\t%u\n", idx++, x, fl, delay);
        txt += b;
    };
    node(-20, 0, 0);
    node(0, 2, 10);
    for (int x = 50; x < 600; x += 50) node((float)x, 0, 0);
    node(600, 2, 10);
    for (int x = 550; x > 0; x -= 50) node((float)x, 0, 0);
    node(-5, 0, 0);
    txt += "M\t3015\t6.5\t-4.0\t-10.0\t4.0\t10.0\n";
    CHECK(ot.transports.parse(txt));
    CHECK(ot.transports.deck(3015) && std::fabs(ot.transports.deck(3015)->z - 6.5f) < 1e-4f);
    ObjectTable::GoInfo gi;
    gi.type = 15; gi.data[0] = 9; gi.data[1] = 14; gi.data[2] = 1;
    ot.setGoInfo(500, gi);

    const uint64_t ship = 0x1F10000000000001ull, pl = 77;
    {   // ship: HAS_POSITION | TRANSPORT, progress 20000 ms
        Writer w;
        w.u32(1); w.u8(0);
        w.u8(3);   // CREATE2
        w.packedGuid(ship); w.u8(5);
        w.u8(0x40 | 0x02);
        w.f32(0); w.f32(0); w.f32(5); w.f32(0);
        w.u32(20000);
        fields(w, {{3, 500}, {8, 3015}});
        CHECK(ot.parseUpdate(w.b.data(), w.b.size(), 1000));
    }
    {   // a player on it at offset (2, 1, 6.5)
        Writer w;
        w.u32(1); w.u8(0);
        w.u8(3);
        w.packedGuid(pl); w.u8(4);
        w.u8(0x20);
        w.u32(MoveFlag::OnTransport); w.u32(5000);
        w.f32(9999); w.f32(9999); w.f32(9999); w.f32(0);
        w.packedGuid(ship); w.f32(2); w.f32(1); w.f32(6.5f); w.f32(0.5f);
        w.u32(0);
        for (int i = 0; i < 6; ++i) w.f32(7.0f);
        fields(w, {{3, 0}});
        CHECK(ot.parseUpdate(w.b.data(), w.b.size(), 1000));
    }
    const WorldObject* s = ot.find(ship);
    const WorldObject* p = ot.find(pl);
    CHECK(s && p && s->hasTransportProgress && s->transportProgress == 20000);
    ot.tickTransports(1000);
    CHECK(ot.transportPose(ship) && ot.transportPose(ship)->valid);
    const TransportPose A = *ot.transportPose(ship);
    const TransportPose* a = &A;
    float wx0[3], wo;
    ot.find(ship)->positionAt(1000, wx0, &wo);
    CHECK(std::fabs(wx0[0] - a->x) < 1e-3f);
    // passenger: world = pose + rotated offset
    float pp[3];
    ot.find(pl)->positionAt(1000, pp, &wo);
    float c = std::cos(a->o), sn = std::sin(a->o);
    CHECK(std::fabs(pp[0] - (a->x + 2 * c - 1 * sn)) < 1e-2f);
    CHECK(std::fabs(pp[1] - (a->y + 2 * sn + 1 * c)) < 1e-2f);
    CHECK(std::fabs(pp[2] - (a->z + 6.5f)) < 1e-2f);
    // 30 s later the ship moved on and the passenger kept the offset
    ot.tickTransports(31000);
    CHECK(ot.transportPose(ship));
    const TransportPose B = *ot.transportPose(ship);
    const TransportPose* b = &B;
    CHECK(std::hypot(b->x - a->x, b->y - a->y) > 50.0f);
    ot.find(pl)->positionAt(31000, pp, &wo);
    c = std::cos(b->o); sn = std::sin(b->o);
    CHECK(std::fabs(pp[0] - (b->x + 2 * c - 1 * sn)) < 1e-2f);
    CHECK(std::fabs(pp[1] - (b->y + 2 * sn + 1 * c)) < 1e-2f);
    // a ship of an unknown path stays where the server put it
    ObjectTable::GoInfo gi2; gi2.type = 15; gi2.data[0] = 12345; gi2.data[1] = 14; gi2.data[2] = 1;
    ot.setGoInfo(501, gi2);
    {   // the mover: boarding sends a heartbeat carrying the transport data, leaving one without
        Mover m;
        m.reset(100, 200, 10, 1.0f, 0);
        m.setTransport(ship, 2.0f, -1.0f, 6.5f, 0.25f, 100);
        CHECK(m.onTransport() && m.queue().size() == 1);
        MovementInfo mi;
        Reader rd(m.queue()[0].data.data(), m.queue()[0].data.size());
        CHECK(readMovementInfo(rd, mi) && (mi.flags & MoveFlag::OnTransport) && mi.transportGuid == ship);
        CHECK(std::fabs(mi.tx - 2.0f) < 1e-4f && std::fabs(mi.ty + 1.0f) < 1e-4f && std::fabs(mi.tz - 6.5f) < 1e-4f);
        m.queue().clear();
        m.clearTransport(200);
        CHECK(!m.onTransport() && m.queue().size() == 1);
        Reader rd2(m.queue()[0].data.data(), m.queue()[0].data.size());
        CHECK(readMovementInfo(rd2, mi) && !(mi.flags & MoveFlag::OnTransport));
        m.setTransport(ship, 0, 0, 0, 0, 300);
        m.reset(1, 2, 3, 0, 400);
        CHECK(!m.onTransport());
    }
    printf("TRANSPORT-OBJ: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
