// farobj_test — the far buildings (6.29): the FARO entry of a pack converted from gen_wmo_data.py's world.
//   farobj_test <pack.w3dp> <expected objects>
// Every far object names a building placed in its tile's PLCM (its first group: model and position, which the client uses to
// hide the shell once the real building is shown); the house and tower shells lie inside their buildings' boxes and cover them;
// the client's tile builder (w3d_farobj_build_tile) puts the vertices where w3d_farobj_place does; the palette is an RGB565 texture.
#include "w3d_farobj.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    const uint32_t expectObjects = (uint32_t)std::atoi(argv[2]);
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = (const W3dEntry*)(b.data() + h.indexOffset);
    const uint8_t* fo = nullptr;
    uint32_t foSize = 0;
    std::vector<const W3dEntry*> plcm;
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        if (e[i].type == W3D_ENTRY_FAROBJ && e[i].id == 0) { fo = b.data() + e[i].offset; foSize = e[i].size; }
        if (e[i].type == W3D_ENTRY_PLACEMENT) plcm.push_back(&e[i]);
    }
    if (expectObjects == 0) {
        CHECK(!fo, "no FARO entry");
        std::printf("FAR OBJECTS: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
        return g_fail != 0;
    }
    CHECK(fo && w3d_farobj_valid(fo, foSize), "a valid FARO entry");
    if (!fo || !w3d_farobj_valid(fo, foSize)) { std::puts("FAR OBJECTS: FAIL"); return 1; }
    const W3dFarObjHeader* hd = (const W3dFarObjHeader*)fo;
    CHECK(hd->objectCount == expectObjects, "%u objects (expected %u)", hd->objectCount, expectObjects);
    const W3dFarShell* shells = w3d_farobj_shells(fo);
    const W3dFarShellVert* sv = w3d_farobj_verts(fo);
    const W3dFarObject* objs = w3d_farobj_objects(fo);
    // palette
    bool palOk = false;
    for (uint32_t i = 0; i < h.entryCount; ++i)
        if (e[i].type == W3D_ENTRY_TEXTURE && e[i].id == hd->paletteTexId) {
            W3dTexHeader th;
            std::memcpy(&th, b.data() + e[i].offset, sizeof th);
            palOk = th.format == W3D_TEX_RGB565 && th.width == hd->palW && th.height == hd->palH && th.mipCount == 1;
        }
    CHECK(palOk, "the palette: RGB565 %ux%u", hd->palW, hd->palH);
    int matched = 0, house = 0, tower = 0;
    for (uint32_t i = 0; i < hd->objectCount; ++i) {
        const W3dFarObject& o = objs[i];
        // its tile: the one whose PLCM has the first group there (the tower's origin is outside the converted region: the
        // neighbour that lists it writes its groups)
        uint32_t k = 0;
        while (k < 64 * 64 && !(i >= hd->tileStart[k] && i < hd->tileStart[k + 1])) ++k;
        const int tx = (int)(k % 64), ty = (int)(k / 64);
        CHECK(k < 64 * 64, "object %u listed under a tile", i);
        bool found = false;
        for (const W3dEntry* pe : plcm) {
            if (pe->id != (uint32_t)(tx * 64 + ty)) continue;
            const W3dPlacementHeader* ph = (const W3dPlacementHeader*)(b.data() + pe->offset);
            const W3dPlacement* pl = (const W3dPlacement*)(ph + 1);
            for (uint32_t q = 0; q < ph->count; ++q)
                if (pl[q].modelId == o.groupModel && std::fabs(pl[q].pos[0] - o.groupPos[0]) < 1e-3f &&
                    std::fabs(pl[q].pos[1] - o.groupPos[1]) < 1e-3f && std::fabs(pl[q].pos[2] - o.groupPos[2]) < 1e-3f &&
                    std::fabs(pl[q].rot[2] - o.rot[2]) < 1e-5f)
                    found = true;
        }
        CHECK(found, "object %u: its first group is placed in tile %d_%d", i, tx, ty);
        matched += found;
        // the shell in its own space: inside the building and covering it
        const W3dFarShell& s = shells[o.shell];
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (uint32_t v = 0; v < s.vertCount; ++v) {
            const float p[3] = {sv[s.firstVert + v].x / W3D_FAROBJ_POS_UNIT, sv[s.firstVert + v].y / W3D_FAROBJ_POS_UNIT, sv[s.firstVert + v].z / W3D_FAROBJ_POS_UNIT};
            for (int q = 0; q < 3; ++q) { lo[q] = std::min(lo[q], p[q]); hi[q] = std::max(hi[q], p[q]); }
        }
        const float ex = hi[0] - lo[0], ey = hi[1] - lo[1], ez = hi[2] - lo[2];
        const float t = 0.1f;
        if (ex > 15.0f) {   // the house: the exterior group, box (-10, -6, 0) .. (10, 6, 8)
            ++house;
            CHECK(lo[0] >= -10 - t && hi[0] <= 10 + t && lo[1] >= -6 - t && hi[1] <= 6 + t && lo[2] >= -t && hi[2] <= 8 + t,
                  "house shell inside its box: %.2f..%.2f %.2f..%.2f %.2f..%.2f", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            CHECK(ex >= 18 && ey >= 10.5f && ez >= 7, "house shell covers it: %.1f x %.1f x %.1f", ex, ey, ez);
        } else if (ez > 15.0f) {   // the tower: (-3, -3, 0) .. (3, 3, 20)
            ++tower;
            CHECK(lo[0] >= -3 - t && hi[0] <= 3 + t && lo[1] >= -3 - t && hi[1] <= 3 + t && lo[2] >= -t && hi[2] <= 20 + t,
                  "tower shell inside its box: %.2f..%.2f %.2f..%.2f %.2f..%.2f", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            CHECK(ex >= 5 && ey >= 5 && ez >= 18, "tower shell covers it: %.1f x %.1f x %.1f", ex, ey, ez);
        }
        CHECK(s.indexCount >= 12 && s.indexCount % 3 == 0, "shell %u: %u indices", o.shell, (unsigned)s.indexCount);
        CHECK(s.radius > 0 && s.radius < 40, "shell %u radius %.1f", o.shell, s.radius);
    }
    CHECK(house == 1 && tower == 1, "the house and the tower among them (%d, %d)", house, tower);
    // the client's tile builder against w3d_farobj_place
    int tilesBuilt = 0;
    for (int k = 0; k < 64 * 64; ++k) {
        const uint32_t n = hd->tileStart[k + 1] - hd->tileStart[k];
        if (!n) continue;
        const int tx = k % 64, ty = k / 64;
        std::vector<W3dTerrainVertex> vb(4096);
        std::vector<uint16_t> ib(16384);
        std::vector<uint32_t> start(n + 1);
        uint32_t ni = 0;
        const uint32_t nv = w3d_farobj_build_tile(fo, tx, ty, 0.25f, vb.data(), (uint32_t)vb.size(), ib.data(), (uint32_t)ib.size(), &ni, start.data());
        uint32_t want = 0;
        for (uint32_t i = 0; i < n; ++i) want += shells[objs[hd->tileStart[k] + i].shell].vertCount;
        CHECK(nv == want, "tile %d_%d: %u vertices (%u)", tx, ty, nv, want);
        CHECK(start[n] == ni && ni % 3 == 0, "tile %d_%d: index ranges", tx, ty);
        bool idxOk = true;
        for (uint32_t q = 0; q < ni; ++q) idxOk &= ib[q] < nv;
        CHECK(idxOk, "tile %d_%d: indices within the vertices", tx, ty);
        float minX, minY;
        w3d_far_tile_origin(tx, ty, &minX, &minY);
        float worst = 0;
        uint32_t at = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const W3dFarObject& o = objs[hd->tileStart[k] + i];
            const W3dFarShell& s = shells[o.shell];
            for (uint32_t v = 0; v < s.vertCount; ++v, ++at) {
                float p[3];
                w3d_farobj_place(&o, &sv[s.firstVert + v], p, nullptr);
                const W3dTerrainVertex& d = vb[at];
                const float q[3] = {minX + d.x * W3D_FAROBJ_XY_SCALE, minY + d.y * W3D_FAROBJ_XY_SCALE, d.z * 0.25f};
                worst = std::max({worst, std::fabs(q[0] - p[0]), std::fabs(q[1] - p[1]), std::fabs(q[2] - p[2])});
                CHECK(d.a == 0 && d.r == d.b && d.r >= 127, "vertex colour: a 0, r = b = light");
            }
        }
        CHECK(worst <= 0.13f, "tile %d_%d: built vertices where the shell is placed (worst %.3f yd)", tx, ty, worst);
        ++tilesBuilt;
    }
    CHECK(tilesBuilt >= 1, "tiles with far buildings: %d", tilesBuilt);
    std::printf("far buildings: %u objects of %u shells, %u vertices, %u indices, palette %ux%u; %d matched their placements\n",
                hd->objectCount, hd->shellCount, hd->vertCount, hd->indexCount, hd->palW, hd->palH, matched);
    std::printf("FAR OBJECTS: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
