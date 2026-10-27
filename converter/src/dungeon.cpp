// Dungeons / instances (stage 5.24): maps that have no ADTs, only a WDT with one global WMO (WDTF_GLOBAL_WMO).
// The building is split by tile (the client streams per tile), every tile it touches gets a TERR that is only a flat
// invisible-ish quad far below the floors (the client needs a terrain tile to load a tile at all), PLCM and COLL.
#include "conv.h"

#include "core/coordinates.hpp"
#include "pipeline/wdt_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace w3d {

namespace {
constexpr float TS = wowee::core::coords::TILE_SIZE;

void eulerFromAdt(const float r[3], float out[3]) {   // same as placementEuler() in terrain.cpp
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    out[0] = r[2] * kDeg;
    out[1] = r[0] * kDeg;
    out[2] = (r[1] + 180.0f) * kDeg;
}

bool readWdt(const DataDir& data, const std::string& dir, wowee::pipeline::WDTInfo& info) {
    std::vector<uint8_t> bytes;
    if (!data.read("World\\Maps\\" + dir + "\\" + dir + ".wdt", bytes)) return false;
    info = wowee::pipeline::parseWDT(bytes);
    return info.isWMOOnly() && !info.rootWMOPath.empty();
}

bool tileOf(float x, float y, int& tx, int& ty) {
    tx = (int)std::floor(32.0f - y / TS);
    ty = (int)std::floor(32.0f - x / TS);
    return tx >= 0 && tx < 64 && ty >= 0 && ty < 64;
}

// A tile that only exists so the client loads the placements and collision of the tile: one quad at height z.
void emitDummyTerrain(const Options& o, PackWriter& pack, ConvStats& st, uint32_t placeholderTex, int tx, int ty, float z) {
    W3dTerrainHeader h{};
    h.tileX = (uint32_t)tx;
    h.tileY = (uint32_t)ty;
    h.origin[0] = (32.0f - (float)ty - 1.0f) * TS;
    h.origin[1] = (32.0f - (float)tx - 1.0f) * TS;
    h.origin[2] = z;
    h.posOffset[0] = TS * 0.5f; h.posOffset[1] = TS * 0.5f; h.posOffset[2] = 0.5f;
    h.posScale[0] = TS * 0.5f / 32767.0f; h.posScale[1] = h.posScale[0]; h.posScale[2] = 0.5f / 32767.0f;
    h.gridSize = (uint32_t)o.grid;
    std::vector<W3dTerrainVertex> qv(4);
    const int16_t lo = -32767, hi = 32767;
    const int16_t zz = -32767;   // posOffset z 0.5, posScale z 0.5/32767: -32767 is tile-local z 0
    const int16_t xs[4] = {lo, hi, lo, hi}, ys[4] = {lo, lo, hi, hi};
    for (int i = 0; i < 4; ++i) qv[i] = {xs[i], ys[i], zz, 0, 255, 255, 255, 0, 0, 0};
    const uint16_t quad[12] = {0, 1, 2, 1, 3, 2, 0, 2, 1, 1, 2, 3};   // both windings
    std::vector<uint16_t> idx;
    for (int l = 0; l < W3D_TERRAIN_LODS; ++l) idx.insert(idx.end(), quad, quad + 12);   // every LOD has its own range
    W3dTerrainBatch b{};
    b.tex0 = b.tex1 = 0;
    for (int l = 0; l < W3D_TERRAIN_LODS; ++l) { b.firstIndex[l] = (uint32_t)l * 12; b.indexCount[l] = 12; }
    b.bmin[0] = b.bmin[1] = 0; b.bmax[0] = b.bmax[1] = TS; b.bmin[2] = b.bmax[2] = 0;
    std::vector<uint32_t> texIds{placeholderTex};
    std::vector<W3dTerrainBatch> batches{b};
    h.vertexCount = 4;
    h.indexCount = (uint32_t)idx.size();
    h.batchCount = 1;
    h.texCount = 1;
    Blob blob;
    blob.put(h);
    blob.align(4);
    size_t offTex = blob.putArray(texIds);
    blob.align(4);
    size_t offB = blob.putArray(batches);
    blob.align(16);
    size_t offV = blob.putArray(qv);
    blob.align(4);
    size_t offI = blob.putArray(idx);
    blob.align(4);
    auto* hp = blob.at<W3dTerrainHeader>(0);
    hp->offTexIds = (uint32_t)offTex; hp->offBatches = (uint32_t)offB;
    hp->offVertices = (uint32_t)offV; hp->offIndices = (uint32_t)offI;
    pack.add(W3D_ENTRY_TERRAIN, w3d_tile_id(tx, ty), std::move(blob.data));
    st.terrainVerts += 4;
    ++st.tiles;
}
}  // namespace

bool isDungeonMap(const DataDir& data, const std::string& dir) {
    wowee::pipeline::WDTInfo info;
    return readWdt(data, dir, info);
}

bool convertDungeon(const Options& optIn, const DataDir& data, PackWriter& pack, ConvStats& st) {
    Options o = optIn;
    o.tileX0 = o.tileY0 = 0;
    o.tileX1 = o.tileY1 = 63;   // the building may span any tiles: do not clip its collision to the region
    wowee::pipeline::WDTInfo info;
    if (!readWdt(data, o.map, info)) {
        std::fprintf(stderr, "%s: not a WMO-only map (no global WMO in its WDT)\n", o.map.c_str());
        return false;
    }
    if (o.noWmo) { std::fputs("dungeon: --no-wmo leaves nothing to convert\n", stderr); return false; }
    const uint32_t placeholder = addPlaceholderTexture(pack, o.texFormat);
    auto modelFor = [&](const std::string& name) { return addModelFromGame(pack, data, name, o, st); };

    auto w = wowee::core::coords::adtToWorld(info.position[0], info.position[1], info.position[2]);
    float pos[3] = {w.x, w.y, w.z}, rot[3];
    eulerFromAdt(info.rotation, rot);
    std::vector<W3dPlacement> all;
    placeWmo(pack, data, info.rootWMOPath, pos, rot, info.doodadSet, o, st, modelFor, all);
    if (all.empty()) {
        std::fprintf(stderr, "%s: the building %s produced no groups\n", o.map.c_str(), info.rootWMOPath.c_str());
        return false;
    }

    std::map<uint32_t, std::vector<W3dPlacement>> byTile;
    float zmin = 1e30f;
    for (const auto& p : all) {
        int tx, ty;
        if (!tileOf(p.pos[0], p.pos[1], tx, ty)) { ++st.wmoSkipped; continue; }
        byTile[w3d_tile_id(tx, ty)].push_back(p);
        zmin = std::min(zmin, p.pos[2]);
    }
    for (auto& [tid, tris] : st.collTris)
        for (const auto& t : tris)
            for (int k = 0; k < 3; ++k) zmin = std::min(zmin, t.v[k * 3 + 2]);
    for (auto& kv : st.collTris) byTile[kv.first];   // collision-only tiles get an (empty) placement list too
    int ctx, cty;
    if (tileOf(0.0f, 0.0f, ctx, cty)) byTile[w3d_tile_id(ctx, cty)];   // the middle of the map: where entrances usually are
    if (zmin > 1e29f) zmin = 0;
    const float floorZ = zmin - 300.0f;   // terrain far below everything: falling out of the building lands there

    for (auto& [tid, list] : byTile) {
        int tx = (int)(tid / 64), ty = (int)(tid % 64);
        emitDummyTerrain(o, pack, st, placeholder, tx, ty, floorZ);
        Blob pb;
        W3dPlacementHeader ph{};
        ph.count = (uint32_t)list.size();
        pb.put(ph);
        pb.putArray(list);
        pack.add(W3D_ENTRY_PLACEMENT, tid, std::move(pb.data));
        st.placements += list.size();
        if (o.verbose) std::printf("  dungeon tile %d_%d: %zu placements\n", tx, ty, list.size());
    }
    std::printf("  dungeon %s: %s -> %zu tiles, %zu placements, floor z %.1f\n", o.map.c_str(), info.rootWMOPath.c_str(), byTile.size(),
                all.size(), floorZ);
    return true;
}

std::vector<DungeonMap> listDungeonMaps(const DataDir& data) {
    std::vector<DungeonMap> r;
    DbcTable maps;
    if (!maps.load(data, "Map.dbc") || maps.fields < 2) {
        std::fputs("dungeons: Map.dbc not found\n", stderr);
        return r;
    }
    for (uint32_t i = 0; i < maps.records; ++i) {
        std::string dir = maps.str(i, 1);
        if (dir.empty()) continue;
        if (isDungeonMap(data, dir)) r.push_back({maps.u32(i, 0), dir});
    }
    return r;
}

}  // namespace w3d
