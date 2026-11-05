// hlod.cpp — 6.57: the horizon (see hlod.hpp, common/w3d_hlod.h).
#include "hlod.hpp"

#include "world.h"
#include "w3d_farlod.h"
#include "w3d_hlod.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>

namespace hlod {
namespace {
FILE* g_file = nullptr;
uint32_t g_fileSize = 0;
W3dHlodHeader g_h{};
bool g_open = false;
std::vector<W3dHlodTile> g_tiles;    // sorted by (tx, ty)
char g_status[256] = "no horizon";
GpuTex* g_pal = nullptr;
uint32_t g_stamp = 0, g_frame = 0, g_gen = 0;
std::vector<Draw> g_draws;

// a tile's meshes in linear memory (the GPU reads them there)
struct In { uint8_t* blob = nullptr; uint32_t size = 0; uint32_t gen = 0; };
std::vector<uint8_t> g_state;        // per tile of the table: 0 not asked, 1 asked, 2 in, 3 failed
std::unordered_map<uint32_t, In> g_in;   // by table index
uint32_t g_bytesIn = 0;
struct Freed { uint8_t* p; uint32_t frame; };
std::vector<Freed> g_freed;          // (the GPU may still read a dropped tile's meshes for a frame or two)
constexpr uint32_t kMaxBytes = 12u << 20;

// the reader thread: one read per tile
struct Done { uint32_t index; std::vector<uint8_t> data; bool ok; };
std::deque<uint32_t> g_queue;
std::vector<Done> g_done;
LightLock g_lock;
LightEvent g_wake;
Thread g_thread = nullptr;
std::atomic<bool> g_quit{false};

bool readAt(uint32_t off, void* dst, uint32_t n) {
    return std::fseek(g_file, (long)off, SEEK_SET) == 0 && std::fread(dst, 1, n, g_file) == n;
}

void readerMain(void*) {
    while (!g_quit) {
        LightEvent_Wait(&g_wake);
        for (;;) {
            LightLock_Lock(&g_lock);
            const bool any = !g_queue.empty();
            const uint32_t idx = any ? g_queue.front() : 0;
            if (any) g_queue.pop_front();
            LightLock_Unlock(&g_lock);
            if (!any || g_quit) break;
            Done d;
            d.index = idx;
            const W3dHlodTile& t = g_tiles[idx];
            d.data.resize(t.dataSize);
            d.ok = t.dataSize && t.dataSize <= (8u << 20) && readAt(t.dataOff, d.data.data(), t.dataSize) &&
                   w3d_hlod_tile_valid(d.data.data(), t.dataSize);
            LightLock_Lock(&g_lock);
            g_done.push_back(std::move(d));
            LightLock_Unlock(&g_lock);
        }
    }
}

void dropTile(uint32_t index) {
    auto it = g_in.find(index);
    if (it == g_in.end()) return;
    g_freed.push_back({it->second.blob, g_frame});
    g_bytesIn -= it->second.size;
    g_in.erase(it);
    g_state[index] = 0;
}

float rectDist(int tx, int ty, const float cam[3]) {   // 2D distance from the camera to the tile's square
    float minX, minY;
    w3d_far_tile_origin(tx, ty, &minX, &minY);
    const float gx = std::max({0.0f, minX - cam[0], cam[0] - (minX + W3D_FAR_TILE_SIZE)});
    const float gy = std::max({0.0f, minY - cam[1], cam[1] - (minY + W3D_FAR_TILE_SIZE)});
    return sqrtf(gx * gx + gy * gy);
}
}  // namespace

void unload() {
    if (g_thread) {
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    g_quit = false;
    for (auto& [i, in] : g_in) linearFree(in.blob);
    g_in.clear();
    for (const Freed& f : g_freed) linearFree(f.p);
    g_freed.clear();
    g_bytesIn = 0;
    g_queue.clear();
    g_done.clear();
    g_draws.clear();
    if (g_pal) {
        C3D_TexDelete(&g_pal->tex);
        delete g_pal;
        g_pal = nullptr;
    }
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    g_open = false;
    g_tiles.clear();
    g_state.clear();
    std::snprintf(g_status, sizeof g_status, "no horizon");
}

bool loaded() { return g_open; }
const char* status() { return g_status; }
const std::vector<Draw>& draws() { return g_draws; }
GpuTex* palette() { return g_pal; }

bool coversTile(int tx, int ty) {
    if (!g_open) return false;
    for (const auto& [i, in] : g_in)
        if (g_tiles[i].tx == tx && g_tiles[i].ty == ty) return true;
    return false;
}

bool load(const char* packPath, uint32_t buildId) {
    unload();
    char path[160];
    std::snprintf(path, sizeof path, "%s", packPath);
    char* dot = std::strrchr(path, '.');
    char* slash = std::strrchr(path, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    if (std::strlen(path) + 6 >= sizeof path) return false;
    std::strcat(path, ".hlod");
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::snprintf(g_status, sizeof g_status, "no %.150s", path); return false; }
    g_file = f;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    auto fail = [&](const char* why) {
        unload();
        std::snprintf(g_status, sizeof g_status, "%.150s: %s - not used", path, why);
        return false;
    };
    if (n < (long)sizeof(W3dHlodHeader)) return fail("too short");
    g_fileSize = (uint32_t)n;
    W3dHlodHeader h;
    if (!readAt(0, &h, sizeof h)) return fail("cannot read");
    if (h.magic != W3D_HLOD_MAGIC || h.version != W3D_HLOD_VERSION) return fail("not a horizon of this version (make it again with this w3docc)");
    if (h.tileCount > 4096 || (uint64_t)h.tileTableOff + (uint64_t)h.tileCount * sizeof(W3dHlodTile) > (uint64_t)n ||
        h.palW != 256 || h.palH != 256 || h.palBytes != 256u * 256u * 2u || (uint64_t)h.palOff + h.palBytes > (uint64_t)n)
        return fail("damaged");
    if (h.buildId && buildId && h.buildId != buildId) return fail("made for another conversion of the packs");
    std::vector<W3dHlodTile> tiles(h.tileCount);
    if (h.tileCount && !readAt(h.tileTableOff, tiles.data(), h.tileCount * (uint32_t)sizeof(W3dHlodTile))) return fail("cannot read its tiles");
    for (const W3dHlodTile& t : tiles)
        if ((uint64_t)t.dataOff + t.dataSize > (uint64_t)n) return fail("damaged tile table");
    // the palette: 128 KB of RGB565 in linear memory (not VRAM: it is tight, and the few texels sampled do not need it)
    {
        std::vector<uint8_t> px(h.palBytes);
        if (!readAt(h.palOff, px.data(), h.palBytes)) return fail("cannot read its colours");
        GpuTex* t = new GpuTex;
        t->refs = 1;
        C3D_TexInitParams p{};
        p.width = h.palW;
        p.height = h.palH;
        p.maxLevel = 0;
        p.format = GPU_RGB565;
        p.type = GPU_TEX_2D;
        p.onVram = false;
        if (!C3D_TexInitWithParams(&t->tex, NULL, p)) { delete t; return fail("no memory for its colours"); }
        C3D_TexLoadImage(&t->tex, px.data(), GPU_TEXFACE_2D, 0);
        C3D_TexFlush(&t->tex);
        C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        t->bytes = h.palBytes;
        t->state = AssetState::Ready;
        g_pal = t;
    }
    g_h = h;
    g_tiles.swap(tiles);
    g_state.assign(g_tiles.size(), 0);
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    g_quit = false;
    {   // the reader: core 2 on the New 3DS (a lower priority than the main thread), else core 1 / 0 (as the PVS's)
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        if (n3ds) g_thread = threadCreate(readerMain, nullptr, 16 * 1024, prio + 1, 2, false);
        if (!g_thread) g_thread = threadCreate(readerMain, nullptr, 16 * 1024, prio + 1, 1, false);
        if (!g_thread) g_thread = threadCreate(readerMain, nullptr, 16 * 1024, prio + 1, 0, false);
    }
    if (!g_thread) return fail("no thread to read it");
    g_open = true;
    std::snprintf(g_status, sizeof g_status, "%.120s: %lu tiles, %lu KB, switch %.0f yd (levels from %.0f / %.0f)", path,
                  (unsigned long)h.tileCount, (unsigned long)(n / 1024), h.switchDist, h.levelDist[0], h.levelDist[1]);
    return true;
}

FrameInfo frame(const float cam[3], float reach, float switchDist, const std::vector<Tile*>& tiles, RenderSettings& rs) {
    FrameInfo fi;
    g_draws.clear();
    ++g_frame;
    rs.hlodStamp = 0;
    if (!g_open) return fi;
    // what was freed a few frames ago (the GPU is done with it)
    for (size_t i = 0; i < g_freed.size();) {
        if (g_frame - g_freed[i].frame >= 3) { linearFree(g_freed[i].p); g_freed[i] = g_freed.back(); g_freed.pop_back(); }
        else ++i;
    }
    // the tiles read: into linear memory
    {
        std::vector<Done> done;
        LightLock_Lock(&g_lock);
        done.swap(g_done);
        LightLock_Unlock(&g_lock);
        for (Done& d : done) {
            if (d.index >= g_state.size() || g_state[d.index] != 1) continue;
            if (!d.ok) { g_state[d.index] = 3; continue; }
            uint8_t* p = (uint8_t*)linearAlloc(d.data.size());
            if (!p) { g_state[d.index] = 0; continue; }   // (asked again later)
            std::memcpy(p, d.data.data(), d.data.size());
            GSPGPU_FlushDataCache(p, d.data.size());
            In in;
            in.blob = p;
            in.size = (uint32_t)d.data.size();
            in.gen = ++g_gen;
            g_in[d.index] = in;
            g_bytesIn += in.size;
            g_state[d.index] = 2;
        }
    }
    if (switchDist <= 0) return fi;   // (off: nothing far)
    // ask for the tiles in reach, drop those well out of it
    {
        bool asked = false;
        // (nearest first: a town's tiles are megabytes, the cap must not go to the far ones)
        std::vector<std::pair<float, uint32_t>> want;
        for (uint32_t i = 0; i < g_tiles.size(); ++i) {
            if (g_state[i] != 0) continue;
            const float d = rectDist(g_tiles[i].tx, g_tiles[i].ty, cam);
            if (d <= reach + 300.0f) want.push_back({d, i});
        }
        std::sort(want.begin(), want.end());
        uint32_t pending = 0;
        for (uint32_t i = 0; i < g_tiles.size(); ++i) if (g_state[i] == 1) pending += g_tiles[i].dataSize;
        LightLock_Lock(&g_lock);
        for (const auto& [d, i] : want) {
            if (g_bytesIn + pending + g_tiles[i].dataSize > kMaxBytes) break;
            g_state[i] = 1;
            g_queue.push_back(i);
            pending += g_tiles[i].dataSize;
            asked = true;
        }
        LightLock_Unlock(&g_lock);
        if (asked) { LightEvent_Signal(&g_wake); fi.read = true; }
        std::vector<uint32_t> out;
        for (const auto& [i, in] : g_in)
            if (rectDist(g_tiles[i].tx, g_tiles[i].ty, cam) > reach + 700.0f) out.push_back(i);
        for (uint32_t i : out) dropTile(i);
    }
    if (++g_stamp == 0) g_stamp = 1;
    const uint32_t stamp = g_stamp;
    rs.hlodStamp = stamp;
    for (auto& [index, in] : g_in) {
        const W3dHlodTile& ht = g_tiles[index];
        Tile* t = nullptr;
        for (Tile* x : tiles) if (x->tx == ht.tx && x->ty == ht.ty) { t = x; break; }
        // the loaded tile's instances know their block (rebuilt when the tile's instances or these meshes changed)
        if (t && !t->plToInst.empty() &&
            (t->hlodGen != in.gen || t->hlodInst != t->instances.size() || t->hlodPl != t->plToInst.size())) {
            for (Instance& x : t->instances) x.hblock = -1;
            const W3dHlodBlock* bl = w3d_hlod_blocks(in.blob);
            for (uint32_t b = 0; b < w3d_hlod_block_count(in.blob); ++b) {
                const uint32_t* mem = (const uint32_t*)(in.blob + bl[b].memberOff);
                const int bi = bl[b].by * W3D_HLOD_BLOCKS + bl[b].bx;
                for (uint32_t k = 0; k < bl[b].memberCount; ++k)
                    if (mem[k] < t->plToInst.size() && t->plToInst[mem[k]] >= 0 && (size_t)t->plToInst[mem[k]] < t->instances.size())
                        t->instances[(size_t)t->plToInst[mem[k]]].hblock = (int8_t)bi;
            }
            t->hlodGen = in.gen;
            t->hlodInst = (uint32_t)t->instances.size();
            t->hlodPl = (uint32_t)t->plToInst.size();
        }
        const bool mapped = t && t->hlodGen == in.gen;
        const W3dHlodBlock* bl = w3d_hlod_blocks(in.blob);
        for (uint32_t b = 0; b < w3d_hlod_block_count(in.blob); ++b) {
            const W3dHlodBlock& B = bl[b];
            const float gx = std::max({0.0f, B.bmin[0] - cam[0], cam[0] - B.bmax[0]});
            const float gy = std::max({0.0f, B.bmin[1] - cam[1], cam[1] - B.bmax[1]});
            const float gz = std::max({0.0f, B.bmin[2] - cam[2], cam[2] - B.bmax[2]});
            const float d = sqrtf(gx * gx + gy * gy + gz * gz);
            if (d > reach) continue;
            if (t && d < switchDist) continue;   // near: the real buildings (a tile not loaded: its mesh even near - not a hole)
            const int bi = B.by * W3D_HLOD_BLOCKS + B.bx;
            if (mapped) t->hlodFar[bi] = stamp;
            // the PVS: a covered tile's block whose members cannot be seen from here is not drawn at all
            if (rs.pvsOn && mapped && t->pvsStamp == rs.pvsStamp && d <= rs.pvsMaxDist) {
                bool any = false;
                const uint32_t* mem = (const uint32_t*)(in.blob + B.memberOff);
                for (uint32_t k = 0; k < B.memberCount && !any; ++k) {
                    const int32_t ii = mem[k] < t->plToInst.size() ? t->plToInst[mem[k]] : -1;
                    any = ii >= 0 && (size_t)ii < t->instances.size() && t->instances[(size_t)ii].pvsStamp == rs.pvsStamp;
                }
                if (!any) { ++fi.hidden; continue; }
            }
            if (rs.pvsOn && rs.pvsFarHidden && d > rs.pvsFarR) { ++fi.hidden; continue; }
            const int l = d < g_h.levelDist[0] ? 0 : d < g_h.levelDist[1] ? 1 : 2;
            if (!B.idxCount[l]) continue;
            Draw dr;
            dr.verts = (const W3dTerrainVertex*)(in.blob + B.vertOff[l]);
            dr.idx = (const uint16_t*)(in.blob + B.idxOff[l]);
            dr.count = B.idxCount[l];
            float r2 = 0;
            for (int k = 0; k < 3; ++k) {
                dr.origin[k] = B.origin[k];
                dr.center[k] = 0.5f * (B.bmin[k] + B.bmax[k]);
                r2 += 0.25f * (B.bmax[k] - B.bmin[k]) * (B.bmax[k] - B.bmin[k]);
            }
            dr.radius = sqrtf(r2);
            g_draws.push_back(dr);
            ++fi.blocks;
            fi.tris += dr.count / 3;
        }
    }
    fi.tiles = (uint16_t)g_in.size();
    fi.bytes = g_bytesIn;
    return fi;
}

}  // namespace hlod
