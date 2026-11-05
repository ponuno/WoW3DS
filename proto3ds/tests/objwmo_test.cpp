// objwmo_test — 6.59: a game object whose model is a building, end to end on the PC (stubbed libctru): the converter's OWMO entry
// (converter/tests/run_worldobj_tests.sh: gen_wmo_data.py's house as GameObjectDisplayInfo 4009) read by the streamer, its parts'
// models loaded, the object placed turned and moved, and the walker's queries against it:
//   floor  - inside the room the room's floor (z 0.2 in the house), under the eaves nothing of the room, far away nothing;
//   walls  - a walk from the middle of the room towards its wall stops in front of it (the turn applied both ways);
//   ceiling- the room's ceiling (z 6) over the feet;
//   scale  - the same at scale 2 (the floor 0.4 over the origin);
//   placed - nothing once the frame forgets the object.
//   objwmo_test <pack.w3dp>
#include "../source/world.h"
#include "w3d_format.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

void pcTestIoDelay(int, uint32_t, uint32_t) {}

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: objwmo_test <pack.w3dp>"); return 2; }
    PackIndex index;
    char err[160];
    if (!index.open(argv[1], err, sizeof err) || !streamerInit(&index, err, sizeof err)) { std::printf("init: %s\n", err); return 2; }
    const uint32_t did = 0x80000000u | 4009u;
    CHECK(streamerHasObjWmo(did) && !streamerHasDisplay(did), "the house object: OWMO, no CDSP");
    CHECK(!streamerHasObjWmo(0x80000000u | 4001u), "the elevator is no building");
    // far from every tile of the pack (no terrain, no tile collision): only the object answers
    const float P[3] = {-3000.0f, 2500.0f, 50.0f};
    const float yaw = 0.5236f;   // 30 degrees
    const ObjWmo* w = streamerObjWmo(did, P, 1000);
    CHECK(w && w->parts.size() == 3 && !w->collBlob.empty(), "parts %zu, collision %zu bytes", w ? w->parts.size() : 0,
          w ? w->collBlob.size() : 0);
    if (!w) { std::printf("OBJECT WMO: FAIL\n"); return 1; }
    CHECK(streamerObjWmo(did, P, 1100) == w, "cached");
    // the parts' models come in through the reader as any model
    float cam[3] = {P[0], P[1], P[2]};
    bool ready = false;
    for (int f = 0; f < 3000 && !ready; ++f) {
        streamerUpdate(cam, 300.0f, true);
        streamerUploadTextures();
        ready = true;
        for (const ObjWmoPart& p : w->parts) ready = ready && p.model->state == AssetState::Ready;
        streamerEndFrame();
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    CHECK(ready, "the parts' models loaded");
    const float c = cosf(yaw), s = sinf(yaw);
    auto world = [&](float lx, float ly, float sc, float out[2]) { out[0] = P[0] + (c * lx - s * ly) * sc; out[1] = P[1] + (s * lx + c * ly) * sc; };
    for (float sc : {1.0f, 2.0f}) {
        streamerObjWmoTick(2000);
        streamerObjWmoPlace(w, P, yaw, sc);
        float at[2], z = 0;
        bool onFloor = false;
        world(0, 0, sc, at);
        CHECK(streamerFloorHeight(at[0], at[1], P[2] + 1.0f * sc, &z, &onFloor) && std::fabs(z - (P[2] + 0.2f * sc)) < 0.02f * sc && onFloor,
              "scale %.0f: the room's floor %.3f (want %.3f)", sc, z, P[2] + 0.2f * sc);
        float ceil = 0;
        CHECK(streamerCeilingAbove(at[0], at[1], P[2] + 0.7f * sc, &ceil) && std::fabs(ceil - (P[2] + 6.0f * sc)) < 0.02f * sc,
              "scale %.0f: the room's ceiling %.3f", sc, ceil);
        float far[2];
        world(40, 40, sc, far);
        CHECK(!streamerFloorHeight(far[0], far[1], P[2] + 1.0f, &z), "scale %.0f: nothing far from the house", sc);
        // walk from the middle towards the wall at local y = -4 (the room box): stopped in front of it
        float to[2];
        world(0.5f, -7.0f, sc, to);
        float tx = to[0], ty = to[1];
        const bool hit = streamerCollideMove(at[0], at[1], P[2] + 0.2f * sc, &tx, &ty);
        const float lx = (c * (tx - P[0]) + s * (ty - P[1])) / sc, ly = (-s * (tx - P[0]) + c * (ty - P[1])) / sc;
        CHECK(hit && ly > -4.0f && ly < -3.3f && std::fabs(lx - 0.5f) < 0.3f, "scale %.0f: the wall stops the walk at local (%.2f, %.2f)", sc,
              lx, ly);
    }
    {   // 7.08: whose floor holds the feet (a ship's own floor boards the walker)
        const uint64_t ship = 0xF120000000000ABCull;
        streamerObjWmoTick(2500);
        streamerObjWmoPlace(w, P, yaw, 1.0f, ship);
        float at[2], z = 0;
        world(0, 0, 1, at);
        CHECK(streamerObjFloorOwner(at[0], at[1], P[2] + 0.2f) == ship, "standing on its floor: the owner");
        CHECK(streamerObjFloorOwner(at[0], at[1], P[2] + 3.0f) == 0, "3 yd above it (a jump): not standing on it");
        CHECK(streamerObjFloorOf(ship, at[0], at[1], P[2] + 3.0f, 12.0f, &z) && std::fabs(z - (P[2] + 0.2f)) < 0.02f, "in the air: its floor below");
        CHECK(!streamerObjFloorOf(ship + 1, at[0], at[1], P[2] + 0.2f, 12.0f, &z), "another object's floor is not it");
        float far[2];
        world(40, 40, 1, far);
        CHECK(streamerObjFloorOwner(far[0], far[1], P[2] + 0.2f) == 0, "far away: nobody's");
    }
    // the next frame without the object: nothing
    streamerObjWmoTick(3000);
    {
        float z = 0, at[2];
        world(0, 0, 1, at);
        CHECK(!streamerFloorHeight(at[0], at[1], P[2] + 1.0f, &z), "forgotten with the frame");
    }
    // let go after 30 s unused
    streamerObjWmoTick(40000);
    for (int f = 0; f < 10; ++f) { streamerUpdate(cam, 300.0f, true); streamerEndFrame(); }
    streamerShutdown();
    std::printf("OBJECT WMO: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
