// 6.54: the client's side of the PVS on the PC (stubbed libctru): the streamer loads a pack and its map<ID>.pvs (made by
// `w3docc --pvs`), pvs::frame marks what the camera's cell / level lists - checked against the file decoded here independently
// and against the pack's PLCM (Tile::plToInst points at the instance of that placement) - and switches itself off where it must.
//   pvs_test <pack.w3dp> x y   (a camera spot in the computed area, on the ground)
#include "../source/pvs.hpp"
#include "../source/world.h"
#include "w3d_pvs.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

void pcTestIoDelay(int, uint32_t, uint32_t) {}   // (streamer.cpp's hook for an SD card's speed: none here)

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 4) { std::puts("usage: pvs_test <pack.w3dp> x y"); return 2; }
    PackIndex index;
    char err[160];
    if (!index.open(argv[1], err, sizeof err) || !streamerInit(&index, err, sizeof err)) { std::printf("init: %s\n", err); return 2; }
    CHECK(pvs::load(index.path, index.buildId), "load: %s", pvs::status());
    std::printf("%s\n", pvs::status());
    float cam[3] = {(float)std::atof(argv[2]), (float)std::atof(argv[3]), 0};
    // the tiles around: in, every building shown
    bool ready = false;
    for (int f = 0; f < 4000 && !ready; ++f) {
        streamerUpdate(cam, 533.0f, true);
        streamerUploadTextures();
        float g;
        if (streamerGroundHeight(cam[0], cam[1], &g)) cam[2] = g + 2.5f;
        int good = 0;
        for (Tile* t : streamerReadyTiles()) {
            bool modelsIn = true;
            for (const Instance& in : t->instances) modelsIn = modelsIn && in.model->state == AssetState::Ready;
            good += t->bldgsClosed == 0 && modelsIn && !t->plToInst.empty();
        }
        ready = good >= 9;
        streamerEndFrame();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    CHECK(ready, "the 3 x 3 tiles never came in");
    const std::vector<Tile*>& tiles = streamerReadyTiles();
    // plToInst against the pack's placements
    size_t plChecked = 0;
    for (Tile* t : tiles) {
        const W3dEntry* e = index.find(W3D_ENTRY_PLACEMENT, w3d_tile_id((uint32_t)t->tx, (uint32_t)t->ty));
        if (!e) continue;
        std::vector<uint8_t> b(e->size);
        FILE* f = std::fopen(index.files[0], "rb");
        std::fseek(f, (long)e->offset, SEEK_SET);
        CHECK(std::fread(b.data(), 1, b.size(), f) == b.size(), "read PLCM");
        std::fclose(f);
        W3dPlacementHeader ph;
        std::memcpy(&ph, b.data(), sizeof ph);
        const W3dPlacement* p = (const W3dPlacement*)(b.data() + sizeof ph);
        CHECK(t->plToInst.size() == ph.count, "plToInst size %zu vs %u", t->plToInst.size(), ph.count);
        for (uint32_t i = 0; i < ph.count && i < t->plToInst.size(); ++i) {
            if (t->plToInst[i] < 0) continue;
            const Instance& in = t->instances[(size_t)t->plToInst[i]];
            CHECK(std::memcmp(in.pos, p[i].pos, sizeof in.pos) == 0 && in.model->id == p[i].modelId,
                  "plToInst %u of tile %d_%d: instance %.2f %.2f %.2f model %08x, placement %.2f %.2f %.2f model %08x", i, t->tx, t->ty,
                  in.pos[0], in.pos[1], in.pos[2], (unsigned)in.model->id, p[i].pos[0], p[i].pos[1], p[i].pos[2], (unsigned)p[i].modelId);
            ++plChecked;
        }
    }
    // the frame: on, and exactly the decoded set marked
    RenderSettings rs;
    rs.doodadRange = 45.0f;
    pvs::FrameInfo fi = pvs::frame(cam, 533.0f, tiles, true, rs);
    CHECK(!fi.on && fi.why == 6 && fi.read, "the first frame in a cell: off while it is read (why %d)", fi.why);   // 6.54c
    for (int k = 0; k < 2000 && !fi.on; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        fi = pvs::frame(cam, 533.0f, tiles, true, rs);
    }
    CHECK(fi.on, "PVS off at the camera (why %d)", fi.why);
    // decode the file here
    std::set<std::pair<const Tile*, uint32_t>> wantDood, wantTerr;
    std::set<const Tile*> covered;
    bool wantFarHidden = true;
    {
        char path[160];
        std::snprintf(path, sizeof path, "%s", index.path);
        std::strcpy(std::strrchr(path, '.'), ".pvs");
        FILE* f = std::fopen(path, "rb");
        std::fseek(f, 0, SEEK_END);
        std::vector<uint8_t> d((size_t)std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        CHECK(std::fread(d.data(), 1, d.size(), f) == d.size(), "read pvs");
        std::fclose(f);
        const W3dPvsHeader* h = (const W3dPvsHeader*)d.data();
        const W3dPvsTile* tt = (const W3dPvsTile*)(d.data() + h->tileTableOff);
        const Tile* ct = nullptr;
        for (Tile* t : tiles) {
            const float* o = t->th->origin;
            if (cam[0] >= o[0] && cam[0] < o[0] + 533.33333f && cam[1] >= o[1] && cam[1] < o[1] + 533.33333f) ct = t;
        }
        for (uint32_t i = 0; i < h->tileCount && ct; ++i) {
            if (tt[i].tx != ct->tx || tt[i].ty != ct->ty) continue;
            const int cx = (int)std::floor((cam[0] - tt[i].origin[0]) / h->cellSize), cy = (int)std::floor((cam[1] - tt[i].origin[1]) / h->cellSize);
            uint32_t off;
            std::memcpy(&off, d.data() + tt[i].cellTableOff + 4 * (size_t)(cx + cy * (int)h->cellsPerSide), 4);
            W3dPvsLevel lv[16];
            const int nl = off ? w3d_pvs_levels(d.data(), (uint32_t)d.size(), off, lv, 16) : 0;
            for (int k = 0; k < nl; ++k) {
                if (cam[2] < lv[k].zlo || cam[2] > lv[k].zhi) continue;
                const uint8_t* p = lv[k].data;
                const uint8_t* end = p + lv[k].bytes;
                const uint32_t flags = w3d_pvs_varint(&p, end);
                if (!(flags & W3D_PVS_FAR_HIDDEN)) wantFarHidden = false;
                const uint32_t groups = w3d_pvs_varint(&p, end);
                for (uint32_t g = 0; g < groups; ++g) {
                    const int tx = ct->tx + (int8_t)p[0], ty = ct->ty + (int8_t)p[1];
                    p += 2;
                    const Tile* t = nullptr;
                    for (Tile* x : tiles) if (x->tx == tx && x->ty == ty) t = x;
                    if (t) covered.insert(t);
                    for (int kind = 0; kind < 3; ++kind) {
                        const uint32_t n = w3d_pvs_varint(&p, end);
                        uint32_t prev = 0;
                        for (uint32_t q = 0; q < n; ++q) {
                            const uint32_t v = w3d_pvs_varint(&p, end);
                            const uint32_t idx = q ? prev + 1 + v : v;
                            prev = idx;
                            if (t) (kind == 1 ? wantTerr : wantDood).insert({t, idx});   // (kind 2: WMO groups, by PLCM index like doodads)
                        }
                    }
                }
            }
        }
    }
    size_t marked = 0, terrMarked = 0, mismatch = 0;
    for (Tile* t : tiles) {
        CHECK((t->pvsStamp == rs.pvsStamp) == (covered.count(t) > 0), "tile %d_%d covered", t->tx, t->ty);
        for (uint32_t i = 0; i < t->plToInst.size(); ++i) {
            if (t->plToInst[i] < 0) continue;
            const bool m = t->instances[(size_t)t->plToInst[i]].pvsStamp == rs.pvsStamp;
            marked += m;
            if (m != (wantDood.count({t, i}) > 0)) ++mismatch;
        }
        for (uint32_t b = 0; b < t->pvsTerr.size(); ++b) {
            const bool m = t->pvsTerr[b] == rs.pvsStamp;
            terrMarked += m;
            if (m != (wantTerr.count({t, b}) > 0)) ++mismatch;
        }
    }
    CHECK(mismatch == 0, "%zu marks differ from the file", mismatch);
    CHECK(rs.pvsFarHidden == wantFarHidden, "far hidden %d, the file says %d", (int)rs.pvsFarHidden, (int)wantFarHidden);
    size_t bldgMarked = 0, bldgTotal = 0;
    for (Tile* t : tiles)
        for (const Instance& in : t->instances)
            if (in.model->building) { ++bldgTotal; bldgMarked += in.pvsStamp == rs.pvsStamp; }
    CHECK(marked > 0, "nothing marked");
    size_t totalDood = 0;
    for (Tile* t : tiles) for (const Instance& in : t->instances) totalDood += !in.model->building;
    std::printf("pvs frame: on, %u levels, %zu of %zu doodads + WMO groups (%zu of %zu WMO groups) and %zu ground batches marked, %zu tiles covered,"
                " far %s; plToInst %zu placements checked\n", fi.levels, marked, totalDood + bldgTotal, bldgMarked, bldgTotal, terrMarked, covered.size(),
                rs.pvsFarHidden ? "hidden" : "drawn", plChecked);
    // where it must be off
    RenderSettings r2 = rs;
    r2.doodadRange = 200.0f;
    CHECK(pvs::frame(cam, 533.0f, tiles, true, r2).why == 3 && !r2.pvsOn, "a doodad range over maxRange");
    r2 = rs;
    r2.doodadRange = 10.0f;
    CHECK(pvs::frame(cam, 533.0f, tiles, true, r2).why == 3, "a doodad range under occRange");
    CHECK(pvs::frame(cam, 800.0f, tiles, true, r2).why == 3, "a draw distance over maxDist");
    r2 = rs;
    CHECK(pvs::frame(cam, 533.0f, tiles, false, r2).why == 2 && !r2.pvsOn, "switched off");
    const float high[3] = {cam[0], cam[1], cam[2] + 40.0f};
    CHECK(pvs::frame(high, 533.0f, tiles, true, r2).why == 4, "40 yd up: no level");
    const float far[3] = {cam[0] + 4000.0f, cam[1], cam[2]};
    CHECK(pvs::frame(far, 533.0f, tiles, true, r2).why == 4, "off the map");
    // a stamp is per frame: the next frame's marks do not leak the old ones
    const uint32_t s1 = rs.pvsStamp;
    pvs::frame(cam, 533.0f, tiles, true, rs);
    CHECK(rs.pvsStamp != s1, "a new stamp each frame");
    pvs::unload();
    RenderSettings r3;
    CHECK(pvs::frame(cam, 533.0f, tiles, true, r3).why == 1 && !r3.pvsOn, "unloaded");
    streamerShutdown();
    std::printf("PVS CLIENT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
