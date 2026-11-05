// 6.42: the portal tables the converter wrote for the synthetic house (gen_wmo_data.py): one door between the outer shell (group 0)
// and the room (group 1), stored in both groups; the target models, centres, plane and sphere as worked out by hand.
//   portal_pack_test <pack.w3dp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "w3d_format.h"

static int bad = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++bad; } }
static bool near(float a, float b, float e = 0.02f) { return std::fabs(a - b) < e; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = (const W3dEntry*)(b.data() + h.indexOffset);
    struct G { uint32_t id; const W3dPortalTable* t; const W3dModelHeader* mh; };
    std::vector<G> gs;
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        if (e[i].type != W3D_ENTRY_MODEL) continue;
        const uint8_t* p = b.data() + e[i].offset;
        if (const W3dPortalTable* t = w3d_model_portals(p, e[i].size)) gs.push_back({e[i].id, t, (const W3dModelHeader*)p});
    }
    check(gs.size() == 2, "two groups hold portals");
    if (gs.size() != 2) { std::printf("PORTAL PACK: FAIL\n"); return 1; }
    for (const G& g : gs) {
        check(g.t->count == 1, "one portal in each group");
        const W3dPortal& p = w3d_portals_of(g.t)[0];
        const G& other = gs[0].id == g.id ? gs[1] : gs[0];
        check(p.toModel == other.id, "the portal leads to the other group's model");
        check(near(p.plane[0], 0) && near(p.plane[1], -1, 0.001f) && near(p.plane[2], 0) || near(p.plane[1], 1, 0.001f), "the plane is y = const");
        check(near(p.radius, 1.72f), "the sphere around the 2 x 2.8 yard door");
        check(p.flags == W3D_PORTAL_NOFACE, "both centres on one side of the door's plane: no facing test");
        // the model space is around the group's centre: the door is 4 yards in front of it, (0, -4, 1.6) - centre
        const bool shell = g.mh->bmax[0] > 9.0f;   // the 20-yard wide outer group
        check(near(p.center[0], 0) && near(p.center[1], -4) && near(p.center[2], shell ? -2.4f : -1.5f), "the door's middle relative to the group's centre");
        check(near(p.delta[2], shell ? -0.9f : 0.9f), "the other group's centre relative to this one's");
    }
    std::printf("PORTAL PACK: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
