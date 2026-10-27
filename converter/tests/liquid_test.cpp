// Stage 5.26: checks the liquids of a converted pack against expect.txt from gen_water_data.py.
//   liquid_test <pack.w3dp> <expect.txt>
// Q lines: the LIQD query (w3d_liquid_at, as the 3DS uses it); F: frames of each liquid's WATR batches;
// L: the LIQD look (Light.dbc colours); T: WATR triangles of a liquid in a tile. Also: every WATR vertex inside its
// tile's quantisation range, every batch's frames packed, and a random sweep that never crashes the query.
#include "w3d_format.h"
#include "w3d_liquid.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::puts("usage: liquid_test pack expect.txt"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = reinterpret_cast<const W3dEntry*>(b.data() + h.indexOffset);
    std::map<std::pair<uint32_t, uint32_t>, const W3dEntry*> by;
    for (uint32_t i = 0; i < h.entryCount; ++i) by[{e[i].type, e[i].id}] = &e[i];
    const float TS = 533.33333f;
    int fails = 0, checks = 0;
    auto bad = [&](const std::string& m) { std::printf("FAIL %s\n", m.c_str()); ++fails; };
    std::ifstream ex(argv[2]);
    std::string line;
    std::map<int, int> wantFrames;
    while (std::getline(ex, line)) {
        std::istringstream s(line);
        std::string k;
        s >> k;
        if (k == "Q") {
            float x, y, level; int wet, flags;
            s >> x >> y >> wet >> flags >> level;
            uint32_t tx = (uint32_t)std::floor(32.0f - y / TS), ty = (uint32_t)std::floor(32.0f - x / TS);
            auto it = by.find({W3D_ENTRY_LIQUID, w3d_tile_id(tx, ty)});
            W3dLiquidHit hit{};
            int got = 0;
            if (it != by.end()) {
                const uint8_t* p = b.data() + it->second->offset;
                if (!w3d_liquid_valid(p, it->second->size)) { bad("LIQD invalid"); continue; }
                got = w3d_liquid_at(p, x, y, &hit);
            }
            ++checks;
            char m[200];
            if (got != wet) { std::snprintf(m, sizeof m, "query %.2f %.2f: wet %d, expected %d (flags %u)", x, y, got, wet, hit.flags); bad(m); }
            else if (wet && (hit.flags != flags || std::fabs(hit.level - level) > 0.05f)) {
                std::snprintf(m, sizeof m, "query %.2f %.2f: flags %u level %.3f, expected %d %.3f", x, y, hit.flags, hit.level, flags, level);
                bad(m);
            }
        } else if (k == "F") {
            int liq, n; s >> liq >> n; wantFrames[liq] = n;
        } else if (k == "L") {
            unsigned tx, ty, kind, v[9]; float end;
            s >> tx >> ty >> kind;
            for (unsigned& q : v) s >> q;
            s >> end;
            auto it = by.find({W3D_ENTRY_LIQUID, w3d_tile_id(tx, ty)});
            ++checks;
            if (it == by.end()) { bad("no LIQD for L line"); continue; }
            W3dLiquidHeader lh; std::memcpy(&lh, b.data() + it->second->offset, sizeof lh);
            const W3dLiquidLook& lk = lh.look[kind];
            const uint8_t got[9] = {lk.close[0], lk.close[1], lk.close[2], lk.far_[0], lk.far_[1], lk.far_[2], lk.fog[0], lk.fog[1], lk.fog[2]};
            for (int q = 0; q < 9; ++q)
                if (std::abs((int)got[q] - (int)v[q]) > 1) { bad("look of tile " + std::to_string(tx) + "_" + std::to_string(ty) + " channel " + std::to_string(q)); break; }
            if (std::fabs(lk.fogEnd - end) > 0.5f) bad("fog end of tile " + std::to_string(tx) + "_" + std::to_string(ty));
        } else if (k == "T") {
            unsigned tx, ty, liq, tris; s >> tx >> ty >> liq >> tris;
            auto it = by.find({W3D_ENTRY_WATER, w3d_tile_id(tx, ty)});
            ++checks;
            if (it == by.end()) { bad("no WATR for T line"); continue; }
            const uint8_t* p = b.data() + it->second->offset;
            W3dWaterHeader wh; std::memcpy(&wh, p, sizeof wh);
            const W3dWaterBatch* wb = reinterpret_cast<const W3dWaterBatch*>(p + wh.offBatches);
            unsigned n = 0;
            for (uint32_t i = 0; i < wh.batchCount; ++i) if (wb[i].liquid == liq) n += wb[i].indexCount / 3;
            if (n != tris) bad("tile " + std::to_string(tx) + "_" + std::to_string(ty) + " liquid " + std::to_string(liq) + ": " +
                               std::to_string(n) + " triangles, expected " + std::to_string(tris));
        }
    }
    // every WATR: frames as expected, textures packed, positions decode inside the bounds
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        if (e[i].type != W3D_ENTRY_WATER) continue;
        const uint8_t* p = b.data() + e[i].offset;
        W3dWaterHeader wh; std::memcpy(&wh, p, sizeof wh);
        const W3dWaterBatch* wb = reinterpret_cast<const W3dWaterBatch*>(p + wh.offBatches);
        const uint32_t* ids = reinterpret_cast<const uint32_t*>(p + wh.offTexIds);
        const W3dTerrainVertex* v = reinterpret_cast<const W3dTerrainVertex*>(p + wh.offVertices);
        for (uint32_t k = 0; k < wh.batchCount; ++k) {
            ++checks;
            if (wantFrames.count(wb[k].liquid) && wb[k].frames != wantFrames[wb[k].liquid])
                bad("liquid " + std::to_string(wb[k].liquid) + " has " + std::to_string(wb[k].frames) + " frames");
            for (uint32_t fr = 0; fr < std::max<uint32_t>(1, wb[k].frames); ++fr)
                if (!by.count({W3D_ENTRY_TEXTURE, ids[wb[k].tex + fr]})) bad("frame texture not packed");
        }
        for (uint32_t k = 0; k < wh.vertexCount; ++k) {
            float x = wh.origin[0] + v[k].x * wh.posScale[0] + wh.posOffset[0];
            float y = wh.origin[1] + v[k].y * wh.posScale[1] + wh.posOffset[1];
            if (x < wh.origin[0] - 60 || x > wh.origin[0] + TS + 60 || y < wh.origin[1] - 60 || y > wh.origin[1] + TS + 60) { bad("water vertex far outside its tile"); break; }
        }
    }
    // random sweep over every LIQD: in range, no crash (ASan), levels finite
    std::mt19937 rng(7);
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        if (e[i].type != W3D_ENTRY_LIQUID) continue;
        const uint8_t* p = b.data() + e[i].offset;
        W3dLiquidHeader lh; std::memcpy(&lh, p, sizeof lh);
        std::uniform_real_distribution<float> d(-20.0f, TS + 20.0f);
        for (int k = 0; k < 20000; ++k) {
            W3dLiquidHit hit;
            if (w3d_liquid_at(p, lh.maxX - d(rng), lh.maxY - d(rng), &hit) && !std::isfinite(hit.level)) { bad("level not finite"); break; }
        }
    }
    std::printf("LIQUIDS: %s (%d checks, %d failed)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
