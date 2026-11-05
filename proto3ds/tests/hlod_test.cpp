// 6.57: the client's side of the horizon on the PC (stubbed libctru): the streamer loads a pack, its map<ID>.hlod (made by
// `w3docc --hlod`) and map<ID>.pvs; hlod::frame decides which blocks are far - checked against the file decoded here: every far
// block's members marked (Instance::hblock, Tile::hlodFar), near blocks not, the PVS hiding far blocks whose members cannot be seen,
// the switch off, the tiles read by the thread.
//   hlod_test <pack.w3dp> x y   (a camera spot on the ground)
#include "../source/hlod.hpp"
#include "../source/pvs.hpp"
#include "../source/world.h"
#include "w3d_hlod.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

void pcTestIoDelay(int, uint32_t, uint32_t) {}

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 4) { std::puts("usage: hlod_test <pack.w3dp> x y"); return 2; }
    PackIndex index;
    char err[160];
    if (!index.open(argv[1], err, sizeof err) || !streamerInit(&index, err, sizeof err)) { std::printf("init: %s\n", err); return 2; }
    CHECK(hlod::load(index.path, index.buildId), "load: %s", hlod::status());
    std::printf("%s\n", hlod::status());
    const bool withPvs = pvs::load(index.path, index.buildId);
    float cam[3] = {(float)std::atof(argv[2]), (float)std::atof(argv[3]), 0};
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
    RenderSettings rs;
    rs.doodadRange = 45.0f;
    // the meshes are read by the thread: frames until the tiles around are in
    hlod::FrameInfo fi;
    for (int k = 0; k < 3000; ++k) {
        fi = hlod::frame(cam, 533.0f, 160.0f, tiles, rs);
        if (fi.tiles >= 9 && fi.blocks > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(fi.tiles >= 9, "tiles in: %u", fi.tiles);
    CHECK(rs.hlodStamp != 0, "a stamp");
    // decode the file here: per tile the blocks' boxes and members
    std::vector<uint8_t> file;
    {
        char path[160];
        std::snprintf(path, sizeof path, "%s", index.path);
        std::strcpy(std::strrchr(path, '.'), ".hlod");
        FILE* f = std::fopen(path, "rb");
        std::fseek(f, 0, SEEK_END);
        file.resize((size_t)std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        CHECK(std::fread(file.data(), 1, file.size(), f) == file.size(), "read hlod");
        std::fclose(f);
    }
    const W3dHlodHeader* h = (const W3dHlodHeader*)file.data();
    const W3dHlodTile* ht = (const W3dHlodTile*)(file.data() + h->tileTableOff);
    size_t farBlocks = 0, nearBlocks = 0, membersChecked = 0, bad = 0, drawnExpected = 0;
    for (uint32_t i = 0; i < h->tileCount; ++i) {
        const Tile* t = nullptr;
        for (Tile* x : tiles) if (x->tx == ht[i].tx && x->ty == ht[i].ty) t = x;
        if (!t) continue;
        const uint8_t* d = file.data() + ht[i].dataOff;
        CHECK(w3d_hlod_tile_valid(d, ht[i].dataSize), "tile %d_%d data", ht[i].tx, ht[i].ty);
        const W3dHlodBlock* bl = w3d_hlod_blocks(d);
        for (uint32_t b = 0; b < w3d_hlod_block_count(d); ++b) {
            float g2 = 0;
            for (int k = 0; k < 3; ++k) { const float e = std::max({0.0f, bl[b].bmin[k] - cam[k], cam[k] - bl[b].bmax[k]}); g2 += e * e; }
            const bool far = sqrtf(g2) >= 160.0f && sqrtf(g2) <= 533.0f;
            const int bi = bl[b].by * 4 + bl[b].bx;
            (far ? farBlocks : nearBlocks) += 1;
            CHECK((t->hlodFar[bi] == rs.hlodStamp) == far, "tile %d_%d block %d far %d", t->tx, t->ty, bi, (int)far);
            const uint32_t* mem = (const uint32_t*)(d + bl[b].memberOff);
            bool anySeen = !withPvs || !rs.pvsOn || t->pvsStamp != rs.pvsStamp;
            for (uint32_t k = 0; k < bl[b].memberCount; ++k) {
                const int32_t ii = mem[k] < t->plToInst.size() ? t->plToInst[mem[k]] : -1;
                if (ii < 0) continue;
                const Instance& in = t->instances[(size_t)ii];
                if (in.hblock != bi) ++bad;
                ++membersChecked;
                anySeen = anySeen || in.pvsStamp == rs.pvsStamp;
            }
            if (far && anySeen && bl[b].idxCount[sqrtf(g2) < h->levelDist[0] ? 0 : sqrtf(g2) < h->levelDist[1] ? 1 : 2]) ++drawnExpected;
        }
    }
    CHECK(bad == 0, "%zu members without their block", bad);
    CHECK(farBlocks > 0 && nearBlocks > 0, "far %zu near %zu", farBlocks, nearBlocks);
    // every instance with a block is a member of it (no stray marks)
    size_t withBlock = 0;
    for (Tile* t : tiles) for (const Instance& in : t->instances) withBlock += in.hblock >= 0;
    CHECK(withBlock == membersChecked, "instances with a block %zu, members %zu", withBlock, membersChecked);
    // the draws: the far blocks the PVS does not hide (here without the PVS's frame: rs.pvsOn false)
    CHECK(hlod::draws().size() >= drawnExpected, "draws %zu, expected at least %zu (the loaded tiles' far blocks)", hlod::draws().size(), drawnExpected);
    for (const hlod::Draw& dr : hlod::draws()) CHECK(dr.count % 3 == 0 && dr.count > 0 && dr.radius > 0, "a draw");
    CHECK(hlod::coversTile(tiles[0]->tx, tiles[0]->ty), "covers a loaded tile");
    std::printf("hlod frame: %u tiles, %u blocks drawn (%u tris), far blocks of the loaded tiles %zu, near %zu, %zu members mapped\n",
                fi.tiles, fi.blocks, fi.tris, farBlocks, nearBlocks, membersChecked);
    // with the PVS: a far block of a covered tile is drawn only when one of its members can be seen
    if (withPvs) {
        pvs::FrameInfo pf;
        for (int k = 0; k < 2000; ++k) {
            pf = pvs::frame(cam, 533.0f, tiles, true, rs);
            if (pf.on) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!pf.on) std::printf("(no PVS level at this spot, why %d: the PVS part is left out)\n", pf.why);
        const hlod::FrameInfo f2 = hlod::frame(cam, 533.0f, 160.0f, tiles, rs);
        size_t wrong = 0;
        for (const hlod::Draw& dr : hlod::draws()) {
            // find the block of this draw and check one of its members is seen
            bool ok = false;
            for (uint32_t i = 0; i < h->tileCount && !ok; ++i) {
                const Tile* t = nullptr;
                for (Tile* x : tiles) if (x->tx == ht[i].tx && x->ty == ht[i].ty) t = x;
                const uint8_t* d = file.data() + ht[i].dataOff;
                const W3dHlodBlock* bl = w3d_hlod_blocks(d);
                for (uint32_t b = 0; b < w3d_hlod_block_count(d) && !ok; ++b) {
                    if (std::memcmp(bl[b].origin, dr.origin, sizeof dr.origin)) continue;
                    if (!t || t->pvsStamp != rs.pvsStamp) { ok = true; break; }
                    const uint32_t* mem = (const uint32_t*)(d + bl[b].memberOff);
                    for (uint32_t k = 0; k < bl[b].memberCount && !ok; ++k) {
                        const int32_t ii = mem[k] < t->plToInst.size() ? t->plToInst[mem[k]] : -1;
                        ok = ii >= 0 && t->instances[(size_t)ii].pvsStamp == rs.pvsStamp;
                    }
                }
            }
            wrong += !ok;
        }
        CHECK(wrong == 0, "%zu far blocks drawn though none of their members can be seen", wrong);
        std::printf("hlod with the PVS: %u blocks drawn, %u hidden\n", f2.blocks, f2.hidden);
    }
    // off: nothing far, nothing drawn, no stamp
    RenderSettings r2 = rs;
    const hlod::FrameInfo f3 = hlod::frame(cam, 533.0f, 0.0f, tiles, r2);
    CHECK(f3.blocks == 0 && hlod::draws().empty() && r2.hlodStamp == 0, "switched off");
    hlod::unload();
    pvs::unload();
    CHECK(!hlod::loaded() && hlod::draws().empty(), "unloaded");
    streamerShutdown();
    std::printf("HLOD CLIENT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
