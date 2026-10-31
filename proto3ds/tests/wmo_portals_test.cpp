// 6.42: the portal walk (wmo_portals.cpp, WoWee's rules) on a small building: an outdoor street group S (big box) and inside it
// the rooms A - B - C in a row, D off A's side, A's door opens onto S; plus a second copy of the building 60 yards on, turned like a
// placement. Checks when culling happens at all (indoors only), the walk, the seeds and the neighbour lookup.
#include <math.h>
#include <stdio.h>

#include <cstdio>
#include <vector>

#include "wmo_portals.hpp"

using namespace wmoportal;

static int bad = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++bad; } }

struct V3 { float x, y, z; };
static V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static W3dPortal mk(uint32_t toModel, V3 c, V3 oc, V3 mid, V3 n, float r) {
    W3dPortal p{};
    p.toModel = toModel;
    p.delta[0] = oc.x - c.x; p.delta[1] = oc.y - c.y; p.delta[2] = oc.z - c.z;
    p.plane[0] = n.x; p.plane[1] = n.y; p.plane[2] = n.z;
    p.plane[3] = dot(n, sub(mid, c));
    p.center[0] = mid.x - c.x; p.center[1] = mid.y - c.y; p.center[2] = mid.z - c.z;
    p.radius = r;
    return p;
}

struct View { float cam[3], fwd[3]; };
static bool inView(void* ctx, const float c[3], float r) {   // a half space in front of the camera
    const View* v = (const View*)ctx;
    const float d = (c[0] - v->cam[0]) * v->fwd[0] + (c[1] - v->cam[1]) * v->fwd[1] + (c[2] - v->cam[2]) * v->fwd[2];
    return d > -r;
}

int main() {
    {
        const float rot[3] = {0.3f, -0.4f, 1.1f}, v[3] = {1.5f, -2.0f, 0.7f};
        float a[3], b[3];
        rotate(rot, v, a);
        unrotate(rot, a, b);
        check(fabsf(b[0] - v[0]) < 1e-5f && fabsf(b[1] - v[1]) < 1e-5f && fabsf(b[2] - v[2]) < 1e-5f, "unrotate undoes rotate");
        const float q[3] = {0, 0, 1.5707963f}, x[3] = {1, 0, 0};
        rotate(q, x, a);
        check(fabsf(a[0]) < 1e-5f && fabsf(a[1] - 1) < 1e-5f, "a quarter turn about Z takes +x to +y");
    }
    // the building in its own frame: rooms 10 x 10 x 6 (boxes +-5, +-5, +-3 around the centres); the street S south of them (y < -5)
    const V3 cA = {0, 0, 0}, cB = {10, 0, 0}, cC = {20, 0, 0}, cD = {0, 10, 0}, cS = {10, -15, 0};
    const W3dPortal pAB = mk(2, cA, cB, {5, 0, 0}, {1, 0, 0}, 2), pAD = mk(4, cA, cD, {0, 5, 0}, {0, 1, 0}, 2), pAS = mk(5, cA, cS, {0, -5, 0}, {0, -1, 0}, 2);
    const W3dPortal pBA = mk(1, cB, cA, {5, 0, 0}, {-1, 0, 0}, 2), pBC = mk(3, cB, cC, {15, 0, 0}, {1, 0, 0}, 2);
    const W3dPortal pCB = mk(2, cC, cB, {15, 0, 0}, {-1, 0, 0}, 2), pDA = mk(1, cD, cA, {0, 5, 0}, {0, -1, 0}, 2);
    const W3dPortal pSA = mk(1, cS, cA, {0, -5, 0}, {0, 1, 0}, 2);
    const W3dPortal portalsA[] = {pAB, pAD, pAS}, portalsB[] = {pBA, pBC}, portalsC[] = {pCB}, portalsD[] = {pDA}, portalsS[] = {pSA};
    struct Room { uint32_t model; V3 c; const W3dPortal* p; uint32_t n; bool interior; float half[3]; };
    const Room rooms[] = {{1, cA, portalsA, 3, true, {5, 5, 3}}, {2, cB, portalsB, 2, true, {5, 5, 3}}, {3, cC, portalsC, 1, true, {5, 5, 3}},
                          {4, cD, portalsD, 1, true, {5, 5, 3}}, {5, cS, portalsS, 1, false, {25, 10, 8}}};
    const float yaw = 0.7f;
    std::vector<Node> nodes;
    for (int copy = 0; copy < 2; ++copy)
        for (const Room& r : rooms) {
            Node n;
            n.modelId = r.model;
            n.portals = r.p; n.nPortals = r.n; n.hasTable = true; n.interior = r.interior;
            n.rot[2] = yaw;
            const float loc[3] = {r.c.x, r.c.y, r.c.z};
            float w[3];
            rotate(n.rot, loc, w);
            n.pos[0] = 100 + copy * 60 + w[0]; n.pos[1] = 200 + w[1]; n.pos[2] = 5 + w[2];
            for (int k = 0; k < 3; ++k) { n.bmin[k] = -r.half[k]; n.bmax[k] = r.half[k]; }
            n.radius = 9;
            n.inst = (uint32_t)nodes.size();
            nodes.push_back(n);
        }
    buildGraph(nodes);
    check(nodes[0].next[0] == 1 && nodes[0].next[2] == 4, "A's portals lead to B and S of the same copy");
    check(nodes[5].next[0] == 6, "the second copy's A leads to its own B");
    check(nodes[0].comp == nodes[4].comp && nodes[0].comp != nodes[5].comp, "two buildings: one per copy");
    auto seen = [&](int copy, uint32_t model, uint32_t frame) { return nodes[copy * 5 + model - 1].stamp == frame; };
    auto worldOf = [&](V3 p, float out[3]) {
        const float loc[3] = {p.x, p.y, p.z};
        rotate(nodes[0].rot, loc, out);
        out[0] += 100; out[1] += 200; out[2] += 5;
    };
    auto dirOf = [&](V3 d, float out[3]) { const float loc[3] = {d.x, d.y, d.z}; rotate(nodes[0].rot, loc, out); };
    View v;

    // 1. in room A, looking along +x: culling in copy 0; B and C through the doors ahead, D's door is beside (half space: in view)
    worldOf({-1, 0, 1.5f}, v.cam);
    dirOf({1, 0, 0}, v.fwd);
    uint32_t cut = reach(nodes, v.cam, nullptr, inView, &v, 1);
    check(cut == nodes[0].comp, "indoors in A: the building is culled");
    check(seen(0, 1, 1) && seen(0, 2, 1) && seen(0, 3, 1), "A, B, C reached");
    check(seen(0, 5, 1), "the street (outdoor group) is always a seed");
    // 2. looking back (-x): B, C behind; A's door to the street and the street's door seed A again
    dirOf({-1, 0, 0}, v.fwd);
    reach(nodes, v.cam, nullptr, inView, &v, 2);
    check(seen(0, 1, 2) && !seen(0, 2, 2) && !seen(0, 3, 2), "turned around: B and C hidden");
    // 3. the character in B while the camera is in A looking away: B is seeded by the character
    const float viewer[3] = {0, 0, 0};
    float vw[3];
    worldOf({10, 0, 1}, vw);
    (void)viewer;
    reach(nodes, v.cam, vw, inView, &v, 3);
    check(seen(0, 2, 3), "the character's room is drawn even when the camera's view misses its door");
    // 4. on the street (outdoor group's box, no room's): nothing is culled
    worldOf({10, -15, 1.5f}, v.cam);
    check(reach(nodes, v.cam, nullptr, inView, &v, 4) == kNone, "on the street: no culling");
    // 5. outside every box: nothing is culled
    worldOf({10, -60, 1.5f}, v.cam);
    check(reach(nodes, v.cam, nullptr, inView, &v, 5) == kNone, "far outside: no culling");
    // 6. a doorway: inside A's box and the street's box at once (the street box grown to reach into A): no culling
    {
        std::vector<Node> n2 = nodes;
        n2[4].bmax[1] = 15;   // the street's box now reaches y = 0 of the building (into room A)
        worldOf({0, -3, 1.5f}, v.cam);
        check(reach(n2, v.cam, nullptr, inView, &v, 6) == kNone, "a doorway (an outdoor box holds the camera too): no culling");
    }
    // 7. a room without portals (a dead end) holding the camera: no culling
    {
        std::vector<Node> n2 = nodes;
        n2[3].nPortals = 0;
        worldOf({0, 10, 1.5f}, v.cam);
        check(reach(n2, v.cam, nullptr, inView, &v, 7) == kNone, "a dead-end room: no culling");
    }
    // 8. indoors in copy 1: copy 0 untouched (its comp is not the one returned)
    {
        float c1[3];
        worldOf({-1, 0, 1.5f}, c1);
        float d[3] = {60, 0, 0};
        v.cam[0] = c1[0] + d[0]; v.cam[1] = c1[1]; v.cam[2] = c1[2];
        dirOf({1, 0, 0}, v.fwd);
        cut = reach(nodes, v.cam, nullptr, inView, &v, 8);
        check(cut == nodes[5].comp && !seen(0, 1, 8), "indoors in the second copy: only that one is culled");
    }
    std::printf("WMO PORTALS: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
