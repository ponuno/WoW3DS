// Stage 5.28: area triggers (dungeon doors, the Deeprun Tram): areatriggers.txt, the zone geometry (sphere / turned box),
// "reported once per walk-in", the CMSG_AREATRIGGER packet and the server's SMSG_AREA_TRIGGER_MESSAGE.
//   areatrigger_test <areatriggers.txt>
#include "../game.hpp"

#include <cmath>
#include <cstdio>

using namespace w3n;
static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++bad; } } while (0)

int main(int argc, char** argv) {
    // the file the converter wrote (gen_ui_data.py: a sphere 2173 on map 369 and a box 45 on map 0 turned by 90 degrees)
    AreaTriggerTable t;
    if (argc > 1) {
        CHECK(t.load(argv[1]));
        CHECK(t.all().size() == 2 && t.all()[0].id == 2173 && t.all()[0].map == 369 && std::fabs(t.all()[0].radius - 3.5f) < 1e-4f);
        CHECK(t.all()[1].id == 45 && std::fabs(t.all()[1].yaw - 1.5708f) < 1e-4f && std::fabs(t.all()[1].length - 10) < 1e-4f);
    } else {
        AreaTrigger a; a.id = 2173; a.map = 369; a.x = -4; a.y = 5; a.z = 6; a.radius = 3.5f; t.add(a);
        AreaTrigger b; b.id = 45; b.map = 0; b.x = 100; b.y = 200; b.z = 30; b.length = 10; b.width = 4; b.height = 6; b.yaw = 1.5708f; t.add(b);
    }
    const AreaTrigger& sph = t.all()[0];
    const AreaTrigger& box = t.all()[1];
    { float p[3] = {-4, 5, 6};        CHECK(AreaTriggerTable::inside(sph, p)); }
    { float p[3] = {-1, 5, 6};        CHECK(AreaTriggerTable::inside(sph, p)); }        // 3 yards
    { float p[3] = {0.6f, 5, 6};      CHECK(!AreaTriggerTable::inside(sph, p)); }       // 4.6 yards
    { float p[3] = {-4, 5, 10};       CHECK(!AreaTriggerTable::inside(sph, p)); }       // above it: a sphere, not a column
    // the box is 10 long (along its own x) and 4 wide, turned 90 degrees: long along world y
    { float p[3] = {100, 204, 30};    CHECK(AreaTriggerTable::inside(box, p)); }
    { float p[3] = {101.5f, 200, 30}; CHECK(AreaTriggerTable::inside(box, p)); }
    { float p[3] = {103, 200, 30};    CHECK(!AreaTriggerTable::inside(box, p)); }       // outside the width
    { float p[3] = {100, 206, 30};    CHECK(!AreaTriggerTable::inside(box, p)); }       // outside the length
    { float p[3] = {100, 200, 34};    CHECK(!AreaTriggerTable::inside(box, p)); }       // outside the height (+-3)

    // the watch: once per walk-in, again after leaving; a teleport into a zone does not fire it
    AreaTriggerWatch w;
    std::vector<uint32_t> got;
    float far[3] = {500, 500, 0}, in[3] = {-4, 5, 6};
    w.update(t, 369, far, got);                      CHECK(got.empty());
    w.update(t, 369, in, got);                       CHECK(got.size() == 1 && got[0] == 2173);
    got.clear(); w.update(t, 369, in, got);          CHECK(got.empty());                 // still inside
    w.update(t, 369, far, got);                      CHECK(got.empty());
    w.update(t, 369, in, got);                       CHECK(got.size() == 1);             // walked in again
    got.clear();
    w.rearm();                                       // the server moved us
    float in2[3] = {-3, 5, 6};
    w.update(t, 369, in2, got);                      CHECK(got.empty());                 // appeared inside: not entered
    w.update(t, 369, far, got); w.update(t, 369, in, got);   CHECK(got.size() == 1);    // leave and come back: fires
    got.clear();
    w.update(t, 0, in, got);                         CHECK(got.empty());                 // another map: the zone is not there
    w.update(t, 369, in, got);                       CHECK(got.empty());                 // map change = silent re-arm

    // the session: CMSG_AREATRIGGER (0x0B4, u32 id), SMSG_AREA_TRIGGER_MESSAGE (0x2B8, u32 len, string) -> a chat line
    GameSession g;
    g.areaTriggers = t;
    std::vector<std::pair<uint16_t, Bytes>> sent;
    g.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) sent.push_back({op, d}); };
    CHECK(g.checkAreaTriggers(far, 1000) == 0);
    CHECK(g.checkAreaTriggers(far, 1010) == 0);       // (no map yet: map 0 -> box 45 is on map 0)
    float inBox[3] = {100, 202, 30};
    CHECK(g.checkAreaTriggers(inBox, 1020) == 1);
    CHECK(sent.size() == 1 && sent[0].first == 0x0B4 && sent[0].second.size() == 4 && sent[0].second[0] == 45 && sent[0].second[1] == 0);
    CHECK(g.checkAreaTriggers(inBox, 1030) == 0 && sent.size() == 1);
    CHECK(g.stats.areaTriggers == 1);
    Writer m;
    const std::string msg = "You must be level 55 to enter.";
    m.u32((uint32_t)msg.size() + 1);
    m.cstr(msg);
    CHECK(g.handle(0x2B8, m.b, 2000));
    CHECK(!g.chat.empty() && g.chat.back().text == msg);
    std::printf("AREA TRIGGERS: %s\n", bad ? "FAIL" : "PASS");
    return bad != 0;
}
