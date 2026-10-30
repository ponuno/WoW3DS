// The far ground (6.24): the FARL entry of a converted pack against its GRND (exact heights at the chunk corners), the WDL
// tiles around it, the atlas texture, and the client's vertex / index builders (w3d_farlod.h).
//   far_test <pack.w3dp>
#include "w3d_farlod.h"
#include "w3d_ground.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = (const W3dEntry*)(b.data() + h.indexOffset);
    const uint8_t *far = nullptr, *grnd = nullptr;
    uint32_t farSize = 0;
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        if (e[i].type == W3D_ENTRY_FARLOD && e[i].id == 0) { far = b.data() + e[i].offset; farSize = e[i].size; }
        if (e[i].type == W3D_ENTRY_GROUND) grnd = b.data() + e[i].offset;
    }
    CHECK(far && w3d_far_valid(far, farSize), "a valid FARL entry");
    CHECK(grnd != nullptr, "GRND entry");
    if (!far || !grnd) { std::puts("FAR: FAIL"); return 1; }
    const W3dFarHeader* hd = (const W3dFarHeader*)far;
    CHECK(hd->tileCount == 3, "3 tiles: the converted one + 2 from the WDL (%u)", hd->tileCount);
    const W3dFarTile* t = w3d_far_tile(far, 32, 48);
    const W3dFarTile* w1 = w3d_far_tile(far, 33, 48);
    const W3dFarTile* w2 = w3d_far_tile(far, 32, 49);
    CHECK(t && t->source == W3D_FAR_FROM_ADT, "32_48 from its ADT");
    CHECK(w1 && w1->source == W3D_FAR_FROM_WDL && w2 && w2->source == W3D_FAR_FROM_WDL, "33_48 and 32_49 from the WDL");
    CHECK(!w3d_far_tile(far, 31, 48), "no tile where neither has one");
    // the converted tile: its corners agree with GRND (the detailed walker's ground) to the far ground's 0.25 yd step
    float minX, minY;
    w3d_far_tile_origin(32, 48, &minX, &minY);
    double worst = 0;
    for (int i = 0; t && i < 17; ++i)
        for (int j = 0; j < 17; ++j) {
            const float x = minX + (1.0f - i / 16.0f) * W3D_FAR_TILE_SIZE, y = minY + (1.0f - j / 16.0f) * W3D_FAR_TILE_SIZE;
            float gz;
            // (just inside the tile at its far edges: GRND covers [origin, origin + 533.33])
            if (!w3d_ground_height(grnd, std::fmin(std::fmax(x, minX + 0.01f), minX + 533.32f), std::fmin(std::fmax(y, minY + 0.01f), minY + 533.32f), &gz, nullptr)) continue;
            worst = std::fmax(worst, std::fabs(t->h[i * 17 + j] * hd->zScale - gz));
        }
    CHECK(worst <= 0.13 + 0.01, "ADT tile corners vs GRND: worst %.3f yd", worst);
    // the WDL tiles: exactly the file's heights, in the right orientation
    bool exact = true;
    for (int i = 0; w1 && i < 17; ++i)
        for (int j = 0; j < 17; ++j) exact &= std::fabs(w1->h[i * 17 + j] * hd->zScale - (100.0f + 2 * i + j)) < 0.01f;
    CHECK(exact, "WDL heights in the [i][j] reading");
    // the atlas
    const W3dEntry* tex = nullptr;
    for (uint32_t i = 0; i < h.entryCount; ++i)
        if (e[i].type == W3D_ENTRY_TEXTURE && e[i].id == hd->texId) tex = &e[i];
    CHECK(tex != nullptr, "atlas texture in the pack");
    if (tex) {
        W3dTexHeader th;
        std::memcpy(&th, b.data() + tex->offset, sizeof th);
        CHECK(th.width == hd->atlasW && th.height == hd->atlasH && th.format == W3D_TEX_ETC1 && th.mipCount == 1,
              "atlas %ux%u fmt %u mips %u", th.width, th.height, th.format, th.mipCount);
    }
    // the client's builders: positions back in render space, z, uvs inside the cell, CCW from above
    std::vector<W3dTerrainVertex> v(W3D_FAR_VERTS);
    w3d_far_build_vertices(hd, t, v.data());
    double posErr = 0;
    bool uvIn = true, lightOk = true;
    for (int i = 0; i < 17; ++i)
        for (int j = 0; j < 17; ++j) {
            const W3dTerrainVertex& q = v[i * 17 + j];
            const float x = minX + q.x * W3D_FAR_XY_SCALE, y = minY + q.y * W3D_FAR_XY_SCALE;
            posErr = std::fmax(posErr, std::fabs(x - (minX + (1.0f - i / 16.0f) * W3D_FAR_TILE_SIZE)));
            posErr = std::fmax(posErr, std::fabs(y - (minY + (1.0f - j / 16.0f) * W3D_FAR_TILE_SIZE)));
            const float u = q.u * W3D_FAR_UV_SCALE * hd->atlasW, tv = (1.0f - q.v * W3D_FAR_UV_SCALE) * hd->atlasH;
            uvIn &= u > t->cellX + 0.4f && u < t->cellX + 15.6f && tv > t->cellY + 0.4f && tv < t->cellY + 15.6f;
            lightOk &= q.r >= 127 && q.a == 0;
        }
    CHECK(posErr < 0.02, "vertex positions (worst %.4f yd)", posErr);
    CHECK(uvIn, "uvs on the tile's own atlas cell (texel centres)");
    CHECK(lightOk, "baked light in r (>= the ambient), a = 0");
    std::vector<uint16_t> idx(W3D_FAR_INDICES);
    w3d_far_build_indices(idx.data());
    int ccw = 0;
    for (int k = 0; k < W3D_FAR_INDICES; k += 3) {
        const W3dTerrainVertex &a = v[idx[k]], &c = v[idx[k + 1]], &d = v[idx[k + 2]];
        const double cz = (double)(c.x - a.x) * (d.y - a.y) - (double)(c.y - a.y) * (d.x - a.x);
        if (cz > 0) ++ccw;
    }
    CHECK(ccw == W3D_FAR_INDICES / 3, "every triangle counter-clockwise from above (%d of %d)", ccw, W3D_FAR_INDICES / 3);
    float z0;
    CHECK(w3d_far_height(far, minX + 100.0f, minY + 100.0f, &z0) && std::fabs(z0 - 80.0f) < 12.0f, "w3d_far_height inside (%.2f)", z0);
    std::printf("FAR: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
