// 6.42: the portal walk (wmo_portals.cpp) on a small building: rooms A - B - C in a row, D off A's side, E the outside hall with a
// door into A; the whole building turned and moved like a placement. Checks the seeds, the facing test, the view test, the
// neighbour lookup (two copies of the building side by side) and the rotation helpers.
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

// a portal of the group with centre c to the group with centre oc: the polygon's middle mid, normal n (out of c's room), radius r
static W3dPortal mk(uint32_t toModel, V3 c, V3 oc, V3 mid, V3 n, float r, uint32_t flags = 0) {
    W3dPortal p{};
    p.toModel = toModel;
    p.delta[0] = oc.x - c.x; p.delta[1] = oc.y - c.y; p.delta[2] = oc.z - c.z;
    p.plane[0] = n.x; p.plane[1] = n.y; p.plane[2] = n.z;
    p.plane[3] = dot(n, sub(mid, c));
    p.center[0] = mid.x - c.x; p.center[1] = mid.y - c.y; p.center[2] = mid.z - c.z;
    p.radius = r;
    p.flags = flags;
    return p;
}

struct View { float cam[3], fwd[3]; };
static bool inView(void* ctx, const float c[3], float r) {   // a half space in front of the camera
    const View* v = (const View*)ctx;
    const float d = (c[0] - v->cam[0]) * v->fwd[0] + (c[1] - v->cam[1]) * v->fwd[1] + (c[2] - v->cam[2]) * v->fwd[2];
    return d > -r;
}

int main() {
    // rotation helpers: unrotate(rotate(v)) = v, and a quarter turn about Z
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

    // the building in its own frame
    const V3 cA = {0, 0, 0}, cB = {10, 0, 0}, cC = {20, 0, 0}, cD = {0, 10, 0}, cE = {-10, 0, 0};
    const W3dPortal pAB = mk(2, cA, cB, {5, 0, 0}, {1, 0, 0}, 2), pAD = mk(4, cA, cD, {0, 5, 0}, {0, 1, 0}, 2), pAE = mk(5, cA, cE, {-5, 0, 0}, {-1, 0, 0}, 2);
    const W3dPortal pBA = mk(1, cB, cA, {5, 0, 0}, {-1, 0, 0}, 2), pBC = mk(3, cB, cC, {15, 0, 0}, {1, 0, 0}, 2);
    const W3dPortal pCB = mk(2, cC, cB, {15, 0, 0}, {-1, 0, 0}, 2), pDA = mk(1, cD, cA, {0, 5, 0}, {0, -1, 0}, 2);
    const W3dPortal pEA = mk(1, cE, cA, {-5, 0, 0}, {1, 0, 0}, 2);
    const W3dPortal portalsA[] = {pAB, pAD, pAE}, portalsB[] = {pBA, pBC}, portalsC[] = {pCB}, portalsD[] = {pDA}, portalsE[] = {pEA};
    struct Room { uint32_t model; V3 c; const W3dPortal* p; uint32_t n; bool interior; };
    const Room rooms[] = {{1, cA, portalsA, 3, true}, {2, cB, portalsB, 2, true}, {3, cC, portalsC, 1, true},
                          {4, cD, portalsD, 1, true}, {5, cE, portalsE, 1, false}};

    // two copies: yaw 0.7 at (100, 200, 5) and the same turn 60 yards further on
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
            n.bmin[0] = -5; n.bmin[1] = -5; n.bmin[2] = -3; n.bmax[0] = 5; n.bmax[1] = 5; n.bmax[2] = 3;
            n.radius = 9;
            n.inst = (uint32_t)nodes.size();
            nodes.push_back(n);
        }
    const std::vector<uint32_t> idx = buildIndex(nodes);
    auto stampOf = [&](int copy, uint32_t model, uint32_t frame) {
        for (const Node& n : nodes)
            if (n.modelId == model && (n.pos[0] > 130) == (copy == 1)) return n.stamp == frame;
        return false;
    };
    auto worldOf = [&](V3 p, float out[3]) {   // WMO frame -> world (copy 0)
        const float loc[3] = {p.x, p.y, p.z};
        rotate(nodes[0].rot, loc, out);
        out[0] += 100; out[1] += 200; out[2] += 5;
    };
    auto dirOf = [&](V3 d, float out[3]) { const float loc[3] = {d.x, d.y, d.z}; rotate(nodes[0].rot, loc, out); };

    // 1. the camera in room A (copy 0), looking along +x of the building: A, B, C and the way out (E) are in view; D is to the side
    //    but its portal (at +y, perpendicular) is in the half space in front only when the view plane is at the camera: it is not
    View v;
    worldOf({-1, 0, 1.5f}, v.cam);
    dirOf({1, 0, 0}, v.fwd);
    uint32_t n1 = reach(nodes, idx, v.cam, inView, &v, 200.0f, 1);
    check(stampOf(0, 1, 1), "A (the camera's room) is reached");
    check(stampOf(0, 2, 1), "B through the portal ahead");
    check(stampOf(0, 3, 1), "C through B");
    check(!stampOf(0, 5, 1), "E (behind the camera) is not reached");
    check(!stampOf(1, 1, 1) && !stampOf(1, 2, 1) && !stampOf(1, 3, 1), "the other copy of the building is untouched");
    check(n1 >= 3, "the count includes the three rooms");

    // 2. looking the other way: B and C are behind, E (the hall) in front; D's portal (+y) is in the half space only at 90 degrees
    dirOf({-1, 0, 0}, v.fwd);
    reach(nodes, idx, v.cam, inView, &v, 200.0f, 2);
    check(stampOf(0, 1, 2) && stampOf(0, 5, 2), "turned around: A and the hall E");
    check(!stampOf(0, 2, 2) && !stampOf(0, 3, 2), "turned around: B and C are behind the camera");

    // 3. the camera in room C, looking at A: B on the way, A beyond; the facing test lets the camera look back through C's portal
    worldOf({21, 0, 1.5f}, v.cam);
    dirOf({-1, 0, 0}, v.fwd);
    reach(nodes, idx, v.cam, inView, &v, 200.0f, 3);
    check(stampOf(0, 3, 3) && stampOf(0, 2, 3) && stampOf(0, 1, 3), "from C: B, A reached in a row");
    check(stampOf(0, 4, 3), "from C: D is reached through A (the view test is the half space only)");

    // 4. outside, in front of the hall's door (the camera in no room's box): the exterior group E is the seed, its door leads to A
    worldOf({-25, 0, 1.5f}, v.cam);
    dirOf({1, 0, 0}, v.fwd);
    reach(nodes, idx, v.cam, inView, &v, 200.0f, 4);
    check(stampOf(0, 5, 4) && stampOf(0, 1, 4), "outside: the hall's door shows A");
    check(stampOf(0, 2, 4), "outside: and B beyond A");

    // 5. outside, back to the door: the camera is behind E's portal (the facing test), nothing but E is reached
    dirOf({-1, 0, 0}, v.fwd);
    reach(nodes, idx, v.cam, inView, &v, 200.0f, 5);
    check(!stampOf(0, 1, 5) && !stampOf(0, 2, 5), "outside, looking away: the interior is not reached");

    // 6. far away: beyond the draw distance nothing is a seed
    worldOf({-25, 0, 1.5f}, v.cam);
    dirOf({1, 0, 0}, v.fwd);
    reach(nodes, idx, v.cam, inView, &v, 5.0f, 6);
    check(!stampOf(0, 1, 6), "a short draw distance: nothing reached");

    // 7. a portal whose plane points the wrong way (a camera on its "far" side fails the facing test); the NOFACE flag turns the test off
    for (int flag = 0; flag < 2; ++flag) {
        std::vector<Node> n2 = nodes;
        W3dPortal wrong[] = {mk(1, cE, cA, {-5, 0, 0}, {-1, 0, 0}, 2, flag ? W3D_PORTAL_NOFACE : 0u)};
        for (Node& n : n2) if (n.modelId == 5) { n.portals = wrong; n.nPortals = 1; }
        worldOf({-8, 0, 1.5f}, v.cam);   // inside E, looking at the door to A
        dirOf({1, 0, 0}, v.fwd);
        reach(n2, idx, v.cam, inView, &v, 200.0f, 7);
        bool a = false;
        for (const Node& n : n2) if (n.modelId == 1 && n.pos[0] < 130 && n.stamp == 7) a = true;
        check(a == (flag == 1), flag ? "NOFACE: a portal without a known side is passed" : "the facing test refuses a camera on the far side");
    }

    std::printf("WMO PORTALS: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
