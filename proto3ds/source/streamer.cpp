// streamer.cpp — background tile streaming.
//
// Threads:
//  * main thread: decides what to load, allocates every buffer (the libctru
//    linear/VRAM allocators are not thread-safe), parses finished blobs,
//    uploads textures under a per-frame time budget, frees memory with a
//    two-frame delay so the GPU never reads freed memory;
//  * IO worker (core 2 on New 3DS): only fread()s into buffers it was handed
//    and builds the tile height field, then reports the job as done.
//
// Memory: terrain blobs live in one contiguous pool of equal slots (no
// fragmentation from the largest, most frequently recycled allocations);
// models, water and texture staging use the linear heap; textures go to VRAM
// first and fall back to linear memory.
#include "world.h"
#include "vram_guard.hpp"
#include "w3d_collision.h"
#include "w3d_ground.h"
#include "w3d_liquid.h"
#include "w3d_farlod.h"
#include "w3d_farobj.h"
#include "crash.hpp"

#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <unordered_map>

extern "C" {
extern u32 __ctru_heap_size;   // (global scope: inside the anonymous namespace GCC took it for a namespace member - 7r link error)
}
#ifdef W3D_PC_TEST
void pcTestIoDelay(int file, uint32_t offset, uint32_t size);   // tests/streamer_pc.cpp: an SD card's speed (SD_KBPS)
#endif

// ================================================================== index
namespace {
// One file's header, index and PINF (absent: a standalone pack, info.kind 0, buildId 0).
bool readPackIndex(const char* p, std::vector<W3dEntry>& out, W3dPackInfo* info, char* err, size_t errLen) {
    FILE* f = fopen(p, "rb");
    if (!f) { snprintf(err, errLen, "cannot open %s", p); return false; }
    W3dPackHeader h;
    bool ok = fread(&h, 1, sizeof h, f) == sizeof h && h.magic == W3DP_MAGIC;
    if (!ok) snprintf(err, errLen, "%s: not a W3DP pack", p);
    else if (h.version != W3DP_VERSION) {
        snprintf(err, errLen, "pack version %lu, need %u: re-run the converter", (unsigned long)h.version, W3DP_VERSION);
        ok = false;
    }
    if (ok) {
        out.resize(h.entryCount);
        ok = fseek(f, h.indexOffset, SEEK_SET) == 0 && fread(out.data(), sizeof(W3dEntry), h.entryCount, f) == h.entryCount;
        if (!ok) snprintf(err, errLen, "%s: bad pack index", p);
    }
    *info = W3dPackInfo{};
    if (ok)
        for (const W3dEntry& e : out)
            if (e.type == W3D_ENTRY_PACKINFO && e.size >= sizeof(W3dPackInfo)) {
                if (fseek(f, e.offset, SEEK_SET) != 0 || fread(info, sizeof *info, 1, f) != 1) *info = W3dPackInfo{};
                break;
            }
    fclose(f);
    return ok;
}
}  // namespace

bool PackIndex::open(const char* p, char* err, size_t errLen) {
    snprintf(path, sizeof path, "%s", p);
    entries.clear();
    fileOf.clear();
    fileCount = 0;
    buildId = 0;
    poolParts = 0;
    W3dPackInfo info;
    std::vector<W3dEntry> own;
    if (!readPackIndex(p, own, &info, err, errLen)) return false;
    snprintf(files[fileCount++], sizeof files[0], "%s", p);
    struct Tagged { W3dEntry e; uint8_t file; };
    std::vector<Tagged> all;
    all.reserve(own.size());
    for (const W3dEntry& e : own) all.push_back({e, 0});
    if (info.kind == W3D_PACK_MAP) {   // its assets: the pool next to it, every part of the same conversion
        buildId = info.buildId;
        char dir[108];   // room for "common<k>.w3dp" in a 128-byte path
        if (snprintf(dir, sizeof dir, "%s", p) >= (int)sizeof dir) { snprintf(err, errLen, "path too long: %s", p); return false; }
        char* slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else dir[0] = 0;
        int parts = 1;
        for (int k = 0; k < parts; ++k) {
            if (fileCount >= kMaxFiles) { snprintf(err, errLen, "the pool has more than %d parts", kMaxFiles - 1); return false; }
            char cp[128];
            if (k) snprintf(cp, sizeof cp, "%scommon%d.w3dp", dir, k % 10);
            else snprintf(cp, sizeof cp, "%scommon.w3dp", dir);
            std::vector<W3dEntry> part;
            W3dPackInfo pi;
            if (!readPackIndex(cp, part, &pi, err, errLen)) {
                snprintf(err, errLen, "%s needs %s (copy every common*.w3dp of the same conversion)", p, cp);
                return false;
            }
            if (pi.kind != W3D_PACK_POOL || pi.buildId != buildId) {
                snprintf(err, errLen, "%s is from another conversion than %s: copy all files of one w3dconv --all run", cp, p);
                return false;
            }
            if (k == 0) parts = std::max<int>(1, pi.partCount);
            const uint8_t fi = (uint8_t)fileCount;
            snprintf(files[fileCount++], sizeof files[0], "%s", cp);
            for (const W3dEntry& e : part)
                if (e.type != W3D_ENTRY_PACKINFO) all.push_back({e, fi});
        }
        poolParts = parts;
    }
    // sorted by (type, id); the map pack's own entry wins a tie (it comes first, the sort is stable)
    std::stable_sort(all.begin(), all.end(), [](const Tagged& a, const Tagged& b) {
        return a.e.type != b.e.type ? a.e.type < b.e.type : a.e.id < b.e.id;
    });
    entries.reserve(all.size());
    fileOf.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        if (i && all[i].e.type == all[i - 1].e.type && all[i].e.id == all[i - 1].e.id) continue;
        entries.push_back(all[i].e);
        fileOf.push_back(all[i].file);
    }
    for (const W3dEntry& e : entries) {
        if (e.type != W3D_ENTRY_TERRAIN) continue;
        maxTerrainSize = std::max(maxTerrainSize, e.size);
        int tx = (int)(e.id / 64), ty = (int)(e.id % 64);
        minTileX = std::min(minTileX, tx); maxTileX = std::max(maxTileX, tx);
        minTileY = std::min(minTileY, ty); maxTileY = std::max(maxTileY, ty);
    }
    if (!maxTerrainSize) { snprintf(err, errLen, "pack has no terrain"); return false; }
    return true;
}

const W3dEntry* PackIndex::find(uint32_t type, uint32_t id) const {
    W3dEntry key{type, id, 0, 0};
    auto it = std::lower_bound(entries.begin(), entries.end(), key, [](const W3dEntry& a, const W3dEntry& b) {
        return a.type != b.type ? a.type < b.type : a.id < b.id;
    });
    return (it != entries.end() && it->type == type && it->id == id) ? &*it : nullptr;
}

bool CreatureDisplay::ready() const {
    if (failed || state != AssetState::Ready || !model || model->state != AssetState::Ready || !model->h) return false;
    for (GpuTex* t : tex)
        if (t && t->state != AssetState::Ready) return false;
    for (GpuTex* t : model->slotTex)
        if (t && t->state != AssetState::Ready) return false;
    return true;
}

bool CreatureModel::fxReady() const {
    for (const GpuTex* t : fxTex) if (t && t->state != AssetState::Ready) return false;
    return true;
}

bool Model::texturesReady() const {
    for (GpuTex* t : tex)
        if (t->state != AssetState::Ready) return false;
    return true;
}

// ================================================================== internals
namespace {

enum class JobKind : uint8_t { Terrain, Water, Placement, Collision, Model, Texture, Display, Creature, CharInfo, CharImg, Item, Liquid, Ground };

struct Job {
    JobKind kind;
    uint8_t file;                    // PackIndex::files (0: the map pack, 1..: the pool parts)
    uint32_t offset, size;
    uint8_t* dst;
    void* owner;
    bool ok = false;
    // stage 5.30: what the read is for and where (the queue is ordered by readKey every frame)
    float anchor[3] = {0, 0, 0};
    uint8_t cls = 0;
    float key = 0;
    // perf log (stage 6): the IO thread's time on it - the read from the SD card, then the cache flush / height field
    uint32_t readUs = 0, cpuUs = 0;
};

// Stage 5.30: the read order. Every queued read carries the spot it is needed at and a class; each frame the queue is
// sorted by distance from the camera weighted by the class and by whether the spot is in front of the camera, reads of
// equal rank in file order (the SD card reads faster forwards). Reads whose owner was let go meanwhile are dropped.
enum : uint8_t {
    kClsUnit = 0,        // creatures, players, worn items, the login scenes: wanted now, near the camera
    kClsTile = 1,        // terrain, water, liquids, placements of a tile (small, everything else waits for them)
    kClsColl = 2,        // floors and walls near the camera
    kClsTerrainTex = 3,  // the ground's textures: the tile is drawn when they are in
    kClsObject = 4,      // a placed model (building or doodad: not known before it is read)
    kClsBuilding = 5,    // a building's textures
    kClsDoodad = 6,      // a doodad's textures
    kClsBldModel = 7,    // stage 6.11: a placed model that is probably a building (its placement shares its transform with another)
};
struct Anchor { float p[3]; uint8_t cls; };
Anchor g_anchor{{0, 0, 0}, kClsUnit};   // what submit() / acquire*() record; set around the calls that know better
struct AnchorScope {
    Anchor saved;
    AnchorScope(const float p[3], uint8_t cls) : saved(g_anchor) { g_anchor = Anchor{{p[0], p[1], p[2]}, cls}; }
    ~AnchorScope() { g_anchor = saved; }
};
// Tuning of the read order (the PC test can try other values: W3D_BAND, W3D_GROW, W3D_TILEW, W3D_TILES).
float tunable(const char* name, float def) {
#ifdef W3D_PC_TEST
    if (const char* v = getenv(name)) return (float)atof(v);
#else
    (void)name;
#endif
    return def;
}
float g_cam[3] = {0, 0, 0};
float g_fwd[2] = {0, 0};
bool g_haveFwd = false, g_boost = false;
bool g_worldWanted = true;   // 6.37: streamerSetWorldWanted
bool groundClass(uint8_t cls) { return cls == kClsTile || cls == kClsColl || cls == kClsTerrainTex; }
float readKey(const float a[3], uint8_t cls) {
    if (cls == kClsUnit) return 0.0f;
    if (cls == kClsTile || cls == kClsTerrainTex) {
        // anchored at the tile's centre: the distance to the tile itself (0 for the one under the camera; its centre can be
        // 370 yd away, which put the ground under the player behind the doodads of the next tile - report: ground 3.4 s)
        const float h = kTileSize * 0.5f;
        const float dx = fmaxf(fabsf(a[0] - g_cam[0]) - h, 0.0f), dy = fmaxf(fabsf(a[1] - g_cam[1]) - h, 0.0f);
        // x2: a neighbour tile's ground does not overtake the models standing near the camera (tests/run_streaming.sh:
        // at a tile corner the first ground 1.0 s instead of 1.4 s, in the middle of a tile as before)
        static const float w = tunable("W3D_TILEW", 2.0f);
        return sqrtf(dx * dx + dy * dy) * w;
    }
    const float dx = a[0] - g_cam[0], dy = a[1] - g_cam[1], dz = a[2] - g_cam[2];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    float front = 1.0f;
    if (g_haveFwd && d > 40.0f) {   // behind the camera: later
        const float c = (dx * g_fwd[0] + dy * g_fwd[1]) / d;
        front = c > 0.4f ? 1.0f : c > -0.3f ? 1.35f : 1.8f;
    }
    switch (cls) {
        case kClsColl: return d * 0.4f;
        // stage 6.11: buildings first (their model, then their textures), small models after both
        case kClsBldModel: return 4.0f + d * front * 0.85f;
        case kClsBuilding: return 8.0f + d * front * 0.9f;
        case kClsObject: return 14.0f + d * front * 1.1f;
        default: return 40.0f + d * front * 1.6f;
    }
}

struct CollLoad;
struct TileExt : Tile {
    bool doomed = false;             // released while jobs were in flight
    uint32_t waterSize = 0;
    // COLL (floors + walls) is loaded only for tiles near the camera, not with the tile: a city tile
    // can hold 5 MB of it (Stormwind), and only the ground under the walker needs it.
    CollLoad* collLoad = nullptr;    // read in flight
    bool collLinear = false;         // t->coll came from the linear heap (the normal heap was full)
};
// One COLL read; outlives a tile released while it is in flight (tile = nullptr then).
struct CollLoad {
    TileExt* tile;
    uint8_t* buf;
    uint32_t size;
    bool linear;
};
constexpr float kCollLoadDist = 200.0f, kCollDropDist = 320.0f;   // yards from the camera to the tile's edge
// 6.31: the normal heap's reserves (see StreamStats::heapFree)
constexpr u32 kCollHeapReserve = 6u << 20;   // COLL goes to the heap only if this much stays free after it (else linear memory)
constexpr u32 kHeapLow = 3u << 20;           // under this the wanted ring shrinks to 3 x 3 tiles
constexpr u32 kHeapOk = 5u << 20;            // ... and grows back above this
static u32 heapFreeNow() {
#ifdef W3D_PC_TEST
    static const u32 fake = (u32)tunable("W3D_HEAP_FREE_MB", 64.0f) << 20;   // (tests: the heap left, as asked)
    return fake;
#else
    const struct mallinfo mi = mallinfo();
    return __ctru_heap_size > (u32)mi.uordblks ? __ctru_heap_size - (u32)mi.uordblks : 0;
#endif
}

void freeColl(uint8_t* p, bool linear) {
    if (linear) linearFree(p);
    else free(p);
}

PackIndex* g_index = nullptr;
// One descriptor per pack file, used by the IO thread only. Stage 6.11: plain open()/lseek()/read(), no stdio. newlib's
// buffered fread after an fseek refills its whole buffer from an aligned offset (a 10 KB texture cost a 64-128 KB read
// from the card); the sdmc device's read() is one FSFILE_Read of exactly the size asked.
int g_ioFiles[PackIndex::kMaxFiles] = {};   // -1: not open (see streamerInit)
bool readExact(int fd, uint32_t offset, uint8_t* dst, uint32_t size) {
    if (fd < 0 || lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) return false;
    while (size) {
        const ssize_t n = read(fd, dst, size);
        if (n <= 0) return false;
        dst += n;
        size -= (uint32_t)n;
    }
    return true;
}

Thread g_thread = nullptr;
std::atomic<bool> g_quit{false};
LightLock g_lock;
LightEvent g_wake;
std::vector<Job*> g_requests;   // guarded by g_lock; sorted so that the worker takes the back (the most wanted)
std::deque<Job*> g_done;          // guarded by g_lock
std::vector<Job*> g_incoming;     // main thread: submitted since the last sort

std::unordered_map<uint32_t, GpuTex*> g_tex;
std::unordered_map<uint32_t, Model*> g_models;
std::unordered_map<uint32_t, CreatureDisplay*> g_displays;
std::unordered_map<uint32_t, CreatureModel*> g_cmodels;

// Players (stage 5.5). CHRS entries stay resident once read (<= 16, small); CIMG layers are
// refcounted CPU data, dropped as soon as the body texture is composited.
struct CharInfo { uint32_t id = 0; uint8_t* blob = nullptr; uint32_t size = 0; AssetState state = AssetState::Loading; bool ok = false; };
struct CharImg { uint32_t id = 0; int refs = 0; uint8_t* data = nullptr; uint32_t size = 0; AssetState state = AssetState::Loading; bool ok = false; };
std::unordered_map<uint32_t, CharInfo*> g_chars;
std::unordered_map<uint32_t, CharImg*> g_charImgs;
std::unordered_map<uint32_t, ItemDisplay*> g_items;   // worn items (ITEM entries, small, refcounted)
std::unordered_map<uint64_t, CreatureDisplay*> g_playerDisplays;
std::vector<CreatureDisplay*> g_pendingPlayers;
uint8_t g_charRgb[W3D_CHAR_ATLAS * W3D_CHAR_ATLAS * 3];   // composite scratch
std::vector<TileExt*> g_tiles;
std::vector<Tile*> g_ready;
std::deque<GpuTex*> g_uploads;
std::vector<GpuTex*> g_retry;   // textures that found no memory: white now, loaded when some frees

uint8_t* g_pool = nullptr;
uint32_t g_slotSize = 0;
std::vector<int> g_freeSlots;
int g_slotCount = 0;

enum class FreeKind : uint8_t { Linear, Slot, Texture, ModelStruct, CreatureStruct, DisplayStruct };
struct Deferred { FreeKind kind; void* p; int slot; u32 frame; };
std::vector<Deferred> g_deferred;
u32 g_frame = 0;

GpuTex g_white;
StreamStats g_stats;

// ------------------------------------------------------------------ IO side
void buildHeightField(Tile* t) {
    const W3dTerrainHeader* th = (const W3dTerrainHeader*)t->terr;
    t->height.assign(kHeightN * kHeightN, -1e9f);
    const W3dTerrainVertex* v = (const W3dTerrainVertex*)(t->terr + th->offVertices);
    for (u32 i = 0; i < th->vertexCount; ++i) {
        float x = v[i].x * th->posScale[0] + th->posOffset[0];
        float y = v[i].y * th->posScale[1] + th->posOffset[1];
        float z = v[i].z * th->posScale[2] + th->posOffset[2];
        int cx = (int)lroundf(x / kTileSize * (kHeightN - 1)), cy = (int)lroundf(y / kTileSize * (kHeightN - 1));
        cx = std::clamp(cx, 0, kHeightN - 1);
        cy = std::clamp(cy, 0, kHeightN - 1);
        float& h = t->height[cy * kHeightN + cx];
        h = std::max(h, z);
    }
    // Coarser grids / holes leave gaps: fill from neighbours.
    for (int pass = 0; pass < 8; ++pass) {
        bool gaps = false;
        for (int y = 0; y < kHeightN; ++y)
            for (int x = 0; x < kHeightN; ++x) {
                float& h = t->height[y * kHeightN + x];
                if (h > -1e8f) continue;
                gaps = true;
                static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& o : d) {
                    int nx = x + o[0], ny = y + o[1];
                    if (nx >= 0 && ny >= 0 && nx < kHeightN && ny < kHeightN && t->height[ny * kHeightN + nx] > -1e8f) {
                        h = t->height[ny * kHeightN + nx];
                        break;
                    }
                }
            }
        if (!gaps) break;
    }
    // 6.17: which sample points the drawn ground (LOD 0) covers; the rest are in holes
    t->covered.assign(kHeightN * kHeightN, 0);
    const W3dTerrainBatch* b = (const W3dTerrainBatch*)(t->terr + th->offBatches);
    const uint16_t* idx = (const uint16_t*)(t->terr + th->offIndices);
    const float sc = (kHeightN - 1) / kTileSize;
    for (u32 bi = 0; bi < th->batchCount; ++bi)
        for (u32 k = b[bi].firstIndex[0]; k + 2 < b[bi].firstIndex[0] + b[bi].indexCount[0]; k += 3) {
            float px[3], py[3];
            bool bad = false;
            for (int c = 0; c < 3; ++c) {
                const u32 vi = idx[k + c];
                if (vi >= th->vertexCount) { bad = true; break; }
                px[c] = (v[vi].x * th->posScale[0] + th->posOffset[0]) * sc;
                py[c] = (v[vi].y * th->posScale[1] + th->posOffset[1]) * sc;
            }
            if (bad) continue;
            const float den = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
            if (fabsf(den) < 1e-6f) continue;   // a vertical skirt
            const int x0 = std::max(0, (int)floorf(std::min({px[0], px[1], px[2]}) - 0.01f));
            const int x1 = std::min(kHeightN - 1, (int)ceilf(std::max({px[0], px[1], px[2]}) + 0.01f));
            const int y0 = std::max(0, (int)floorf(std::min({py[0], py[1], py[2]}) - 0.01f));
            const int y1 = std::min(kHeightN - 1, (int)ceilf(std::max({py[0], py[1], py[2]}) + 0.01f));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float w0 = ((py[1] - py[2]) * (x - px[2]) + (px[2] - px[1]) * (y - py[2])) / den;
                    const float w1 = ((py[2] - py[0]) * (x - px[2]) + (px[0] - px[2]) * (y - py[2])) / den;
                    const float eps = -0.02f;   // (edges and corners count: neighbouring triangles leave no cracks)
                    if (w0 >= eps && w1 >= eps && 1.0f - w0 - w1 >= eps) t->covered[y * kHeightN + x] = 1;
                }
        }
}

// Stage 6.13: a terrain batch's LOD 2 may cover less ground than its LOD 1 (older packs: a 16-yd block touching a hole was
// dropped whole): such a batch is drawn no coarser than the finer level. 6.30: and the regions the LOD is picked by.
void terrainLodCaps(Tile* t) {
    const W3dTerrainHeader* th = (const W3dTerrainHeader*)t->terr;
    t->batchMaxLod.assign(th->batchCount, W3D_TERRAIN_LODS - 1);
    t->batchRegion.assign(th->batchCount, 0);
    w3d_terrain_batch_max_lod(t->terr, t->batchMaxLod.data());
    w3d_terrain_regions(t->terr, t->batchMaxLod.data(), t->batchRegion.data(), t->regions);
}

void runJob(Job* j) {
    const int f = j->file < PackIndex::kMaxFiles ? g_ioFiles[j->file] : -1;
    const u64 t0 = svcGetSystemTick();
    j->ok = readExact(f, j->offset, j->dst, j->size);
#ifdef W3D_PC_TEST
    pcTestIoDelay(j->file, j->offset, j->size);
#endif
    const u64 t1 = svcGetSystemTick();
    j->readUs = (uint32_t)(ticksToMs(t1 - t0) * 1000.0);
    if (j->ok && j->kind != JobKind::Placement && j->kind != JobKind::Collision && j->kind != JobKind::Display &&
        j->kind != JobKind::CharInfo && j->kind != JobKind::CharImg && j->kind != JobKind::Item &&
        j->kind != JobKind::Liquid && j->kind != JobKind::Ground) {   // CPU-only data needs no flush
        // Write the data back from THIS core's L1 cache: ARM11 MPCore cache
        // maintenance is per-core, and the GPU/GX engine read memory directly,
        // so a flush issued later on the main core may not see these lines.
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)j->dst, j->size);
    }
    if (j->ok && j->kind == JobKind::Terrain) {
        buildHeightField((Tile*)j->owner);
        terrainLodCaps((Tile*)j->owner);
    }
    j->cpuUs = (uint32_t)(ticksToMs(svcGetSystemTick() - t1) * 1000.0);
}

void workerMain(void*) {
#ifndef W3D_PC_TEST
    crashInstall();   // 6.31: crash.txt if this thread crashes
#endif
    while (!g_quit) {
        LightEvent_Wait(&g_wake);
        for (;;) {
            LightLock_Lock(&g_lock);
            Job* j = nullptr;
            if (!g_requests.empty()) { j = g_requests.back(); g_requests.pop_back(); }
            LightLock_Unlock(&g_lock);
            if (!j) break;
            if (g_quit) {   // hand it back unrun so shutdown can free it
                LightLock_Lock(&g_lock);
                g_done.push_back(j);
                LightLock_Unlock(&g_lock);
                break;
            }
            runJob(j);
            LightLock_Lock(&g_lock);
            g_done.push_back(j);
            LightLock_Unlock(&g_lock);
        }
    }
}

// 'e' must be an entry of g_index (its file comes from its position there); sizeOverride reads less than the entry holds.
void submit(JobKind kind, const W3dEntry* e, uint8_t* dst, void* owner, uint32_t sizeOverride = 0) {
    Job* j = new Job{kind, (uint8_t)g_index->fileIndex(e), e->offset, sizeOverride ? sizeOverride : e->size, dst, owner};
    memcpy(j->anchor, g_anchor.p, sizeof j->anchor);
    j->cls = g_anchor.cls;
    j->key = readKey(j->anchor, j->cls);
    // an idle worker starts at once; otherwise the read joins the queue at the next sort (scheduleJobs)
    LightLock_Lock(&g_lock);
    const bool idle = g_requests.empty();
    if (idle) g_requests.push_back(j);
    LightLock_Unlock(&g_lock);
    if (!idle) { g_incoming.push_back(j); return; }
    if (g_thread) LightEvent_Signal(&g_wake);
}

void deferFree(FreeKind k, void* p, int slot = -1) { g_deferred.push_back({k, p, slot, g_frame}); }

// perf log: an allocation that failed - what was asked and how much linear memory was free then (much free and still a
// failure = the heap is fragmented)
void noteOom(uint32_t& counter, uint32_t size) {
    ++counter;
    g_stats.oomLastSize = size;
    g_stats.oomLastFree = linearSpaceFree();
}

// ------------------------------------------------------------------ assets
GpuTex* acquireTex(uint32_t id) {
    auto it = g_tex.find(id);
    if (it != g_tex.end()) {
        GpuTex* t = it->second;
        t->refs++;
        if (readKey(g_anchor.p, g_anchor.cls) < readKey(t->anchor, t->cls)) {   // a nearer user: uploaded sooner
            memcpy(t->anchor, g_anchor.p, sizeof t->anchor);
            t->cls = g_anchor.cls;
        }
        return t;
    }
    const W3dEntry* e = g_index->find(W3D_ENTRY_TEXTURE, id);
    if (!e) { ++g_stats.texNotInPack; return &g_white; }
    GpuTex* t = new GpuTex;
    t->id = id;
    t->refs = 1;
    memcpy(t->anchor, g_anchor.p, sizeof t->anchor);
    t->cls = g_anchor.cls;
    t->stagingSize = e->size;
    g_tex[id] = t;
    t->staging = (uint8_t*)linearAlloc(e->size);
    if (!t->staging) {   // no linear memory now: white until a retry finds some (never white for good)
        noteOom(g_stats.oomStaging, e->size);
        t->tex = g_white.tex;
        t->bytes = 0;
        t->state = AssetState::Ready;
        t->retry = true;
        g_retry.push_back(t);
        ++g_stats.texOomTotal;
        return t;
    }
    submit(JobKind::Texture, e, t->staging, t);
    return t;
}

void destroyTexNow(GpuTex* t) {
    if (t->retry) {
        g_retry.erase(std::remove(g_retry.begin(), g_retry.end(), t), g_retry.end());
        t->retry = false;
    }
    if (t->staging) linearFree(t->staging);
    t->staging = nullptr;
    if (t->state == AssetState::Ready) {
        (t->vram ? g_stats.texVram : g_stats.texLinear) -= t->bytes;
        deferFree(FreeKind::Texture, t);   // GPU may still sample it this frame
    } else {
        delete t;
    }
}

void releaseTex(GpuTex* t) {
    if (t == &g_white) return;
    if (--t->refs > 0) return;
    g_tex.erase(t->id);
    if (t->state == AssetState::Loading) return;   // freed when its job completes
    if (t->state == AssetState::Uploading) {
        g_uploads.erase(std::remove(g_uploads.begin(), g_uploads.end(), t), g_uploads.end());
    }
    destroyTexNow(t);
}

Model* acquireModel(uint32_t id) {
    auto it = g_models.find(id);
    if (it != g_models.end()) {
        Model* m = it->second;
        m->refs++;
        if (readKey(g_anchor.p, g_anchor.cls) < readKey(m->anchor, m->cls)) {   // a nearer placement
            memcpy(m->anchor, g_anchor.p, sizeof m->anchor);
            m->cls = g_anchor.cls;
        }
        return m;
    }
    const W3dEntry* e = g_index->find(W3D_ENTRY_MODEL, id);
    if (!e) return nullptr;
    Model* m = new Model;
    m->id = id;
    m->refs = 1;
    memcpy(m->anchor, g_anchor.p, sizeof m->anchor);
    m->cls = g_anchor.cls;
    m->size = e->size;
    m->blob = (uint8_t*)linearAlloc(e->size);
    if (!m->blob) { noteOom(g_stats.oomModel, e->size); delete m; return nullptr; }   // (not retried: missing until the tile reloads)
    g_models[id] = m;
    submit(JobKind::Model, e, m->blob, m);
    return m;
}

std::vector<Model*> g_waitingModels;   // read, textures not all in yet (stage 5.30: drawn once they are)

void destroyModelNow(Model* m) {
    if (m->fx) fxForgetModel(m);   // its particles point into the blob and at its textures
    if (!m->drawable) g_waitingModels.erase(std::remove(g_waitingModels.begin(), g_waitingModels.end(), m), g_waitingModels.end());
    for (GpuTex* t : m->tex) releaseTex(t);
    m->tex.clear();
    g_stats.geomBytes -= m->size;
    deferFree(FreeKind::Linear, m->blob);
    deferFree(FreeKind::ModelStruct, m);
}

void releaseModel(Model* m) {
    if (--m->refs > 0) return;
    g_models.erase(m->id);
    if (m->state == AssetState::Loading) return;   // freed when its job completes
    destroyModelNow(m);
}

// ------------------------------------------------------------------ creatures
CreatureModel* acquireCreatureModel(uint32_t id) {
    auto it = g_cmodels.find(id);
    if (it != g_cmodels.end()) { it->second->refs++; return it->second; }
    const W3dEntry* e = g_index->find(W3D_ENTRY_CREATURE, id);
    if (!e) return nullptr;
    CreatureModel* m = new CreatureModel;
    m->id = id;
    m->refs = 1;
    m->size = e->size;
    m->blob = (uint8_t*)linearAlloc(e->size);
    if (!m->blob) { noteOom(g_stats.oomCreature, e->size); delete m; return nullptr; }
    g_cmodels[id] = m;
    submit(JobKind::Creature, e, m->blob, m);
    return m;
}

void destroyCreatureModelNow(CreatureModel* m) {
    if (m->fx) fxForgetModel(m);   // (6.13) its particles point into the blob and at its textures
    for (GpuTex* t : m->fxTex) if (t) releaseTex(t);
    m->fxTex.clear();
    for (GpuTex* t : m->slotTex) if (t) releaseTex(t);
    m->slotTex.clear();
    g_stats.geomBytes -= m->size;
    deferFree(FreeKind::Linear, m->blob);
    deferFree(FreeKind::CreatureStruct, m);
}

void releaseCreatureModel(CreatureModel* m) {
    if (--m->refs > 0) return;
    g_cmodels.erase(m->id);
    if (m->state == AssetState::Loading) return;   // freed when its job completes
    destroyCreatureModelNow(m);
}

void destroyDisplayNow(CreatureDisplay* d) {
    for (int k = 0; k < 6; ++k) {
        GpuTex*& t = d->tex[k];
        if (!t) continue;
        if (d->look && k == 3 && t != &g_white) destroyTexNow(t);   // composited body: owned by this display
        else releaseTex(t);
        t = nullptr;
    }
    if (d->model) releaseCreatureModel(d->model);
    d->model = nullptr;
    deferFree(FreeKind::DisplayStruct, d);   // the renderer may still hold it this frame
}

// ------------------------------------------------------------------ players
}  // namespace
struct PlayerBuild {
    W3dLook look;
    CharInfo* chr = nullptr;
    bool resolved = false;
    CharImg* layers[7] = {};
    uint16_t scalp = 1, f100 = 101, f200 = 201, f300 = 301;   // geoset choices (applied once the model is in)
    // worn armour (stage 5.19): the ITEM entries of the body slots, then their texture layers and geosets
    ItemDisplay* items[W3D_BODY_EQ] = {};
    bool itemsResolved = false;
    W3dEquipGeo eqGeo{};
    std::vector<CharImg*> eqLayers;
};
namespace {

CharInfo* acquireCharInfo(uint32_t id) {
    auto it = g_chars.find(id);
    if (it != g_chars.end()) return it->second;
    const W3dEntry* e = g_index->find(W3D_ENTRY_CHARACTER, id);
    if (!e) return nullptr;
    CharInfo* c = new CharInfo;
    c->id = id;
    c->size = e->size;
    c->blob = (uint8_t*)malloc(e->size);
    if (!c->blob) { delete c; return nullptr; }
    g_chars[id] = c;
    submit(JobKind::CharInfo, e, c->blob, c);
    return c;
}

CharImg* acquireCharImg(uint32_t id) {
    auto it = g_charImgs.find(id);
    if (it != g_charImgs.end()) { it->second->refs++; return it->second; }
    const W3dEntry* e = g_index->find(W3D_ENTRY_CHARIMG, id);
    if (!e) return nullptr;
    CharImg* c = new CharImg;
    c->id = id;
    c->refs = 1;
    c->size = e->size;
    c->data = (uint8_t*)malloc(e->size);
    if (!c->data) { delete c; return nullptr; }
    g_charImgs[id] = c;
    submit(JobKind::CharImg, e, c->data, c);
    return c;
}

void releaseCharImg(CharImg* c) {
    if (--c->refs > 0) return;
    g_charImgs.erase(c->id);
    if (c->state == AssetState::Loading) return;   // freed when its job completes
    free(c->data);
    delete c;
}

ItemDisplay* acquireItem(uint32_t id) {
    if (!id) return nullptr;
    auto it = g_items.find(id);
    if (it != g_items.end()) { it->second->refs++; return it->second; }
    const W3dEntry* e = g_index->find(W3D_ENTRY_ITEM, id);
    if (!e || !w3d_item_valid(nullptr, e->size)) return nullptr;
    ItemDisplay* d = new ItemDisplay;
    d->id = id;
    d->refs = 1;
    memset(&d->d, 0, sizeof d->d);
    g_items[id] = d;
    // (a copy of the entry would lose its file: the pool parts are other files than the map pack)
    submit(JobKind::Item, e, (uint8_t*)&d->d, d, sizeof(W3dItemDisplay));   // later versions may append fields
    return d;
}

void releaseItem(ItemDisplay* d) {
    if (!d || --d->refs > 0) return;
    g_items.erase(d->id);
    if (d->state == AssetState::Loading) return;   // freed when its job completes
    delete d;
}

void dropBuild(CreatureDisplay* d) {
    if (!d->build) return;
    for (CharImg*& l : d->build->layers) if (l) { releaseCharImg(l); l = nullptr; }
    for (CharImg*& l : d->build->eqLayers) if (l) { releaseCharImg(l); l = nullptr; }
    for (ItemDisplay*& it : d->build->items) if (it) { releaseItem(it); it = nullptr; }
    delete d->build;
    d->build = nullptr;
    g_pendingPlayers.erase(std::remove(g_pendingPlayers.begin(), g_pendingPlayers.end(), d), g_pendingPlayers.end());
}

void failPlayer(CreatureDisplay* d) {
    d->failed = true;
    d->state = AssetState::Ready;
    dropBuild(d);
}

// Body texture of one player: composite the layers, upload as a 128x128 RGB565 texture.
GpuTex* compositeBody(const PlayerBuild* b) {
    const int A = W3D_CHAR_ATLAS;
    const uint8_t* L[7 + W3D_MAX_EQ_LAYERS];
    int n = 0;
    for (int k = 0; k < 7; ++k) L[n++] = (b->layers[k] && b->layers[k]->ok) ? b->layers[k]->data : nullptr;
    for (const CharImg* c : b->eqLayers)
        if (c && c->ok && n < 7 + W3D_MAX_EQ_LAYERS) L[n++] = c->data;
    uint16_t* staging = (uint16_t*)linearAlloc(A * A * 2);
    if (!staging) { noteOom(g_stats.oomStaging, A * A * 2); return &g_white; }
    w3d_char_composite(L, n, g_charRgb, staging);
    GSPGPU_FlushDataCache(staging, A * A * 2);
    GpuTex* t = new GpuTex;
    t->refs = 1;
    C3D_TexInitParams p{};
    p.width = A;
    p.height = A;
    p.maxLevel = 0;
    p.format = GPU_RGB565;
    p.type = GPU_TEX_2D;
    p.onVram = vramRoomFor(A * A * 2);   // 6.43: never the last bytes of VRAM (vram_guard.hpp)
    t->vram = p.onVram && C3D_TexInitWithParams(&t->tex, NULL, p);
    bool ok = t->vram;
    if (!ok) { p.onVram = false; ok = C3D_TexInitWithParams(&t->tex, NULL, p); }
    if (!ok) noteOom(g_stats.oomUpload, A * A * 2);
    else if (!t->vram) { ++g_stats.texVramMiss; g_stats.texVramMissBytes += A * A * 2; }
    if (ok) {
        C3D_TexLoadImage(&t->tex, staging, GPU_TEXFACE_2D, 0);
        C3D_TexFlush(&t->tex);   // in linear memory the copy is a memcpy: still in the CPU cache (see tryUpload)
        C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_NEAREST);
        C3D_TexSetWrap(&t->tex, GPU_REPEAT, GPU_REPEAT);
        t->bytes = A * A * 2;
        (t->vram ? g_stats.texVram : g_stats.texLinear) += t->bytes;
    } else {
        t->tex = g_white.tex;
        t->bytes = 0;
    }
    deferFree(FreeKind::Linear, staging);   // (inside a frame the GX copy runs later)
    t->state = AssetState::Ready;
    return t;
}

// Advances pending player displays: CHRS -> resolve the look -> model, hair, layers ->
// geosets + composite (at most one composite per frame: ~16k texels of blending on the CPU).
void pumpPlayers() {
    bool composed = false;
    for (CreatureDisplay* d : std::vector<CreatureDisplay*>(g_pendingPlayers)) {
        PlayerBuild* b = d->build;
        if (!b->chr || b->chr->state != AssetState::Ready) {
            if (!b->chr) failPlayer(d);
            continue;
        }
        if (!b->chr->ok) { failPlayer(d); continue; }
        if (!b->resolved) {
            const W3dCharHeader* h = w3d_char(b->chr->blob);
            W3dCharResolved r;
            w3d_char_resolve(b->chr->blob, &b->look, &r);
            d->d = W3dDisplay{};
            d->d.modelId = h->modelId;
            d->d.scale = h->scale > 0 ? h->scale : 1.0f;
            d->model = acquireCreatureModel(h->modelId);
            if (!d->model) { failPlayer(d); continue; }
            if (r.hairTex) { d->d.tex[4] = r.hairTex; d->tex[4] = acquireTex(r.hairTex); }
            for (int k = 0; k < 7; ++k)
                if (r.layers[k]) b->layers[k] = acquireCharImg(r.layers[k]);
            // geosets need the model's submesh list: chosen once it is resident
            b->scalp = r.scalp; b->f100 = r.f100; b->f200 = r.f200; b->f300 = r.f300;
            for (int k = 0; k < W3D_BODY_EQ; ++k) b->items[k] = acquireItem(d->eq[k]);
            b->resolved = true;
        }
        if (!b->itemsResolved) {   // worn armour: wait for the ITEM entries, then take their layers, geosets, cloak
            bool itemWait = false;
            for (ItemDisplay* it : b->items) itemWait |= it && it->state != AssetState::Ready;
            if (itemWait) continue;
            const W3dItemDisplay* worn[W3D_EQ_SLOTS] = {};
            for (int k = 0; k < W3D_BODY_EQ; ++k)
                if (b->items[k] && b->items[k]->ok) worn[W3D_BODY_EQ_SLOTS[k]] = &b->items[k]->d;
            W3dEquipResolved er;
            w3d_equip_resolve(worn, b->look.sex, &er);
            b->eqGeo = er.geo;
            if (er.hideHair) b->scalp = 1;
            if (er.capeTex) d->tex[5] = acquireTex(er.capeTex);
            for (int k = 0; k < er.layerCount; ++k)
                if (CharImg* c = acquireCharImg(er.layers[k])) b->eqLayers.push_back(c);
            b->itemsResolved = true;
        }
        if (d->model->state != AssetState::Ready) continue;
        bool wait = false;
        for (CharImg* l : b->layers) wait |= l && l->state != AssetState::Ready;
        for (CharImg* l : b->eqLayers) wait |= l->state != AssetState::Ready;
        if (wait || composed) continue;
        if (!d->model->h) { failPlayer(d); continue; }
        const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(d->model->blob + d->model->h->offSubmeshes);
        std::vector<uint16_t> all;
        for (u32 i = 0; i < d->model->h->submeshCount; ++i) all.push_back(sm[i].geoset);
        d->d.geosetCount = (uint16_t)w3d_humanoid_geosets(all.data(), (int)all.size(), b->look.race, b->scalp, b->f100,
                                                          b->f200, b->f300, &b->eqGeo, d->d.geosets, W3D_DISPLAY_GEOSETS);
        d->tex[3] = compositeBody(b);
        composed = true;
        g_stats.composites++;
        d->state = AssetState::Ready;
        dropBuild(d);
    }
}

// ------------------------------------------------------------------ tiles
TileExt* findTile(int tx, int ty) {
    for (TileExt* t : g_tiles)
        if (t->tx == tx && t->ty == ty && !t->doomed) return t;
    return nullptr;
}

void finishTileRelease(TileExt* t) {
    g_stats.bldgClosed -= t->bldgsClosed;   // (stage 6.11)
    for (GpuTex* x : t->ttex) releaseTex(x);
    for (GpuTex* x : t->wtex) releaseTex(x);
    for (Model* m : t->models) releaseModel(m);
    if (t->slot >= 0) {
        g_stats.geomBytes -= g_slotSize;
        deferFree(FreeKind::Slot, nullptr, t->slot);
    }
    if (t->water) {
        g_stats.geomBytes -= t->waterSize;
        deferFree(FreeKind::Linear, t->water);
    }
    if (t->plcm) free(t->plcm);
    if (t->liq) { free(t->liq); t->liq = nullptr; }
    if (t->grnd) { free(t->grnd); t->grnd = nullptr; }
    if (t->coll) { freeColl(t->coll, t->collLinear); g_stats.collBytes -= t->collSize; t->coll = nullptr; }
    if (t->collLoad) t->collLoad->tile = nullptr;   // freed when the read completes
    g_tiles.erase(std::remove(g_tiles.begin(), g_tiles.end(), t), g_tiles.end());
    delete t;
}

void releaseTile(TileExt* t) {
    t->doomed = true;
    t->state = TileState::Loading;   // drop out of the render list right away
    if (t->pendingJobs == 0) finishTileRelease(t);
}

bool requestTile(int tx, int ty) {
    if (g_freeSlots.empty()) return false;
    const uint32_t id = w3d_tile_id(tx, ty);
    const W3dEntry* te = g_index->find(W3D_ENTRY_TERRAIN, id);
    if (!te) return false;
    TileExt* t = new TileExt;
    t->tx = tx;
    t->ty = ty;
    t->requestTick = svcGetSystemTick();
    t->slot = g_freeSlots.back();
    g_freeSlots.pop_back();
    t->terr = g_pool + (size_t)t->slot * g_slotSize;
    g_stats.geomBytes += g_slotSize;
    const float centre[3] = {(32.0f - ty - 0.5f) * kTileSize, (32.0f - tx - 0.5f) * kTileSize, g_cam[2]};
    AnchorScope as(centre, kClsTile);
    submit(JobKind::Terrain, te, t->terr, t);
    t->pendingJobs++;
    if (const W3dEntry* we = g_index->find(W3D_ENTRY_WATER, id)) {
        t->water = (uint8_t*)linearAlloc(we->size);
        if (!t->water) noteOom(g_stats.oomWater, we->size);
        if (t->water) {
            t->waterSize = we->size;
            g_stats.geomBytes += we->size;
            submit(JobKind::Water, we, t->water, t);
            t->pendingJobs++;
        }
    }
    if (const W3dEntry* le = g_index->find(W3D_ENTRY_LIQUID, id)) {   // stage 5.26: small (a few KB, ~20 KB of open sea)
        t->liq = (uint8_t*)malloc(le->size);
        if (t->liq) {
            t->liqSize = le->size;
            submit(JobKind::Liquid, le, t->liq, t);
            t->pendingJobs++;
        }
    }
    if (const W3dEntry* ge = g_index->find(W3D_ENTRY_GROUND, id)) {   // 6.18a: the exact ground (74 KB), read with the terrain
        t->grnd = (uint8_t*)malloc(ge->size);
        if (t->grnd) {
            t->grndSize = ge->size;
            submit(JobKind::Ground, ge, t->grnd, t);
            t->pendingJobs++;
        }
    }
    if (const W3dEntry* pe = g_index->find(W3D_ENTRY_PLACEMENT, id)) {
        t->plcm = (uint8_t*)malloc(pe->size);
        if (t->plcm) {
            submit(JobKind::Placement, pe, t->plcm, t);
            t->pendingJobs++;
        }
    }
    g_tiles.push_back(t);
    return true;
}

// Sun direction in model space (inverse of Rz * Ry * Rx).
void lightToModel(const float rot[3], float out[3]) {
    static const float kSun[3] = {0.30f, 0.40f, 0.866f};
    float v[3] = {kSun[0], kSun[1], kSun[2]};
    float c, s, tmp;
    c = cosf(-rot[2]); s = sinf(-rot[2]);
    tmp = c * v[0] - s * v[1]; v[1] = s * v[0] + c * v[1]; v[0] = tmp;
    c = cosf(-rot[1]); s = sinf(-rot[1]);
    tmp = c * v[0] + s * v[2]; v[2] = -s * v[0] + c * v[2]; v[0] = tmp;
    c = cosf(-rot[0]); s = sinf(-rot[0]);
    tmp = c * v[1] - s * v[2]; v[2] = s * v[1] + c * v[2]; v[1] = tmp;
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
}

// perf log: what the IO thread did, counted on the main thread when the read is collected (dropped reads never ran)
double g_ioMaxReadMs = 0;
void countIo(const Job* j) {
    if (!j->readUs && !j->cpuUs && !j->ok) return;   // dropped unread (scheduleJobs) or a read handed back at shutdown
    const int k = std::min((int)j->kind, kStreamJobKinds - 1);
    g_stats.ioReads++;
    g_stats.ioReadsKind[k]++;
    if (j->ok) { g_stats.ioBytes += j->size; g_stats.ioBytesKind[k] += j->size; } else g_stats.ioFails++;
    const double ms = j->readUs / 1000.0;
    g_stats.ioReadMs += ms;
    g_stats.ioReadMsKind[k] += ms;
    g_stats.ioCpuMs += j->cpuUs / 1000.0;
    g_ioMaxReadMs = std::max(g_ioMaxReadMs, ms);
}

// Stage 6.10: the tile's instances into a 4x4 grid by position (the grid spans their bounding box), each cell a contiguous
// range of t->instances with a bounding sphere of the positions; the radii are added by the renderer (Tile::Chunk).
void buildChunks(Tile* t) {
    t->chunks.clear();
    std::vector<Instance>& v = t->instances;
    if (v.empty()) return;
    constexpr int kN = 4;
    float lo[2] = {v[0].pos[0], v[0].pos[1]}, hi[2] = {lo[0], lo[1]};
    for (const Instance& in : v)
        for (int k = 0; k < 2; ++k) { lo[k] = std::min(lo[k], in.pos[k]); hi[k] = std::max(hi[k], in.pos[k]); }
    auto cell = [&](const Instance& in) {
        int c[2];
        for (int k = 0; k < 2; ++k) {
            const float w = hi[k] - lo[k];
            c[k] = w > 0 ? std::min(kN - 1, std::max(0, (int)((in.pos[k] - lo[k]) / w * kN))) : 0;
        }
        return c[1] * kN + c[0];
    };
    std::stable_sort(v.begin(), v.end(), [&](const Instance& a, const Instance& b) { return cell(a) < cell(b); });
    for (uint32_t i = 0; i < v.size();) {
        const int c = cell(v[i]);
        Tile::Chunk ch;
        ch.begin = i;
        float a[3] = {v[i].pos[0], v[i].pos[1], v[i].pos[2]}, b[3] = {a[0], a[1], a[2]};
        for (; i < v.size() && cell(v[i]) == c; ++i)
            for (int k = 0; k < 3; ++k) { a[k] = std::min(a[k], v[i].pos[k]); b[k] = std::max(b[k], v[i].pos[k]); }
        ch.end = i;
        float d2 = 0;
        for (int k = 0; k < 3; ++k) { ch.center[k] = (a[k] + b[k]) * 0.5f; d2 += (b[k] - a[k]) * (b[k] - a[k]); }
        ch.half = sqrtf(d2) * 0.5f;
        t->chunks.push_back(ch);
    }
}

// Stage 6.11: the groups of one WMO are written one after another (the converter: placeWmo), each with the WMO's turn and scale
// but its own position (the group's centre in the WMO's frame, bit-identical turns). A run of neighbouring placements with
// bit-equal rot / scale, at most kBldgSpan yards from the run's first, and at least two different models in it is taken for
// a building: probably (the doodads of the ADT might line up that way too; once a model is read, only its WMO groups count -
// updateBuildings). bldgOf[i] = the run's number (1-based), 0 = a lone model.
constexpr float kBldgSpan = 250.0f;
// 6.33: ownerOf[i] = the building whose doodad placement i is (0: none). The converter writes a tile's ADT doodads first, then
// every WMO: its groups, then its doodads (placeWmo) - so a lone placement after a building run is that WMO's doodad. It is
// given to the building run (of those since the first one) whose group centres' box, widened by kOwnerMargin, holds it,
// the latest such run first (a WMO spread over more than kBldgSpan is several runs). Shown only once that building is: before
// 6.33 Stormwind's trees and lamps stood in the air while the city's own floor (WMO groups) was still being read.
constexpr float kOwnerMargin = 40.0f;
void findBuildings(const W3dPlacement* p, uint32_t n, std::vector<uint16_t>& bldgOf, std::vector<uint16_t>& ownerOf, int firstId) {
    bldgOf.assign(n, 0);
    ownerOf.assign(n, 0);
    struct Run { uint16_t id; float lo[3], hi[3]; };
    std::vector<Run> runs;
    int id = firstId;
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i + 1;
        bool distinct = false;
        while (j < n && memcmp(p[i].rot, p[j].rot, 3 * sizeof(float)) == 0 && p[i].scale == p[j].scale) {
            const float dx = p[j].pos[0] - p[i].pos[0], dy = p[j].pos[1] - p[i].pos[1], dz = p[j].pos[2] - p[i].pos[2];
            if (dx * dx + dy * dy + dz * dz > kBldgSpan * kBldgSpan) break;
            if (p[j].modelId != p[i].modelId) distinct = true;
            ++j;
        }
        if (distinct && id < 65535) {
            ++id;
            Run r;
            r.id = (uint16_t)id;
            for (int c = 0; c < 3; ++c) { r.lo[c] = p[i].pos[c]; r.hi[c] = p[i].pos[c]; }
            for (uint32_t k = i; k < j; ++k) {
                bldgOf[k] = (uint16_t)id;
                for (int c = 0; c < 3; ++c) { r.lo[c] = std::min(r.lo[c], p[k].pos[c]); r.hi[c] = std::max(r.hi[c], p[k].pos[c]); }
            }
            runs.push_back(r);
        } else if (!runs.empty()) {   // lone placements after a building: its doodads, probably
            for (uint32_t k = i; k < j; ++k)
                for (size_t q = runs.size(); q-- > 0;) {
                    const Run& r = runs[q];
                    bool in = true;
                    for (int c = 0; c < 3 && in; ++c) in = p[k].pos[c] >= r.lo[c] - kOwnerMargin && p[k].pos[c] <= r.hi[c] + kOwnerMargin;
                    if (in) { ownerOf[k] = r.id; break; }
                }
        }
        i = j;
    }
}

// After buildChunks has put the instances in their final order: the buildings' member lists.
void buildBuildings(Tile* t) {
    t->bldgs.clear();
    t->bldgsClosed = 0;
    uint16_t top = 0;
    for (const Instance& in : t->instances) top = std::max(top, in.bldg);
    t->bldgs.resize(top);
    for (uint32_t i = 0; i < t->instances.size(); ++i)
        if (t->instances[i].bldg) t->bldgs[t->instances[i].bldg - 1].members.push_back(i);
    // ids were handed out in order but a building whose models failed to load may be gone: close the gaps by dropping
    // empty ones (they open at once below)
    for (Tile::Bldg& b : t->bldgs) {
        if (b.members.size() < 2) { b.open = true; continue; }   // one group left (the rest failed): nothing to wait for
        ++t->bldgsClosed;
        ++g_stats.bldgFound;
    }
    g_stats.bldgClosed += t->bldgsClosed;
}

// Opens a building when every group in it that is not an interior is drawable, or kBldgWaitMs after the first one was.
void updateBuildings(Tile* t, u64 nowTick, uint32_t nowMs) {
    for (Tile::Bldg& b : t->bldgs) {
        if (b.open) continue;
        int pending = 0, have = 0;
        for (uint32_t i : b.members) {
            const Model* m = t->instances[i].model;
            if (m->state == AssetState::Ready && (!m->h || !m->building || m->interior)) continue;   // failed / a doodad / an interior: not waited for
            if (m->drawable) { ++have; continue; }
            ++pending;   // (still being read: it may turn out to be a doodad; then it stops counting)
        }
        if (pending && have && !b.firstTick) b.firstTick = nowTick;
        const double waited = b.firstTick ? ticksToMs(nowTick - b.firstTick) : 0.0;
        if (pending && waited < kBldgWaitMs) continue;
        b.open = true;
        b.openMs = nowMs;
        --t->bldgsClosed;
        --g_stats.bldgClosed;
        if (pending) ++g_stats.bldgOpenedLate; else ++g_stats.bldgOpenedFull;
        g_stats.bldgWaitMsSum += waited;
        g_stats.bldgWaitMsMax = std::max(g_stats.bldgWaitMsMax, waited);
    }
}

void onJobDone(Job* j) {
    countIo(j);
    switch (j->kind) {
        case JobKind::Texture: {
            GpuTex* t = (GpuTex*)j->owner;
            if (t->refs <= 0) {                     // orphaned while loading
                destroyTexNow(t);
                break;
            }
            if (!j->ok) {                           // read failed: white stand-in, tile can still finish
                ++g_stats.texReadFailed;
                linearFree(t->staging);
                t->staging = nullptr;
                t->tex = g_white.tex;
                t->bytes = 0;
                t->state = AssetState::Ready;
                break;
            }
            t->state = AssetState::Uploading;
            g_uploads.push_back(t);
            break;
        }
        case JobKind::Model: {
            Model* m = (Model*)j->owner;
            if (m->refs <= 0) {                     // orphaned while loading
                linearFree(m->blob);
                delete m;
                break;
            }
            m->state = AssetState::Ready;
            g_stats.geomBytes += m->size;
            if (!j->ok) { m->h = nullptr; break; }
            GSPGPU_FlushDataCache(m->blob, m->size);
            m->h = (const W3dModelHeader*)m->blob;
            m->subs = (const W3dSubmesh*)(m->blob + m->h->offSubmeshes);
            m->building = m->h->submeshCount && (m->subs[0].flags & W3D_SUBF_BUILDING);
            m->interior = m->h->submeshCount && (m->subs[0].flags & W3D_SUBF_INTERIOR);
            m->lods = w3d_model_lods(m->blob, m->size);   // stage 6.12
            m->uva = w3d_model_uvanim(m->blob, m->size);  // 6.41
            m->portals = w3d_model_portals(m->blob, m->size);   // 6.42
            const uint32_t* ids = (const uint32_t*)(m->blob + m->h->offTexIds);
            {   // its textures are read where the model is needed; a building's before a doodad's
                AnchorScope as(m->anchor, m->cls == kClsUnit ? kClsUnit : m->building ? kClsBuilding : kClsDoodad);
                for (u32 i = 0; i < m->h->texCount; ++i) m->tex.push_back(acquireTex(ids[i]));
                m->fx = w3d_model_fx(m->blob, m->size);   // stage 5.32
                m->meshHidden = w3d_emitter_volume(m->fx, m->h->vertexCount, m->h->indexCount, m->h->bmin, m->h->bmax);
                m->fxAnim = w3d_fx_anim(m->blob, m->size, m->fx);   // stage 5.35
                if (m->fx) {
                    m->fxTexBase = (uint16_t)m->tex.size();
                    const uint32_t* fids = (const uint32_t*)(m->blob + m->fx->offTexIds);
                    for (u32 i = 0; i < m->fx->texCount; ++i) m->tex.push_back(acquireTex(fids[i]));
                }
            }
            g_waitingModels.push_back(m);
            break;
        }
        case JobKind::Display: {
            CreatureDisplay* d = (CreatureDisplay*)j->owner;
            if (d->refs <= 0) { delete d; break; }   // orphaned while loading
            d->state = AssetState::Ready;
            if (!j->ok) { d->failed = true; break; }
            d->model = acquireCreatureModel(d->d.modelId);
            if (!d->model) { d->failed = true; break; }
            for (int k = 0; k < 5; ++k)
                if (d->d.tex[k]) d->tex[k] = acquireTex(d->d.tex[k]);
            break;
        }
        case JobKind::CharInfo: {
            CharInfo* c = (CharInfo*)j->owner;
            c->state = AssetState::Ready;
            c->ok = j->ok && w3d_char_valid(c->blob, c->size);
            break;
        }
        case JobKind::Item: {
            ItemDisplay* d = (ItemDisplay*)j->owner;
            if (d->refs <= 0) { delete d; break; }   // orphaned while loading
            d->state = AssetState::Ready;
            d->ok = j->ok;
            break;
        }
        case JobKind::CharImg: {
            CharImg* c = (CharImg*)j->owner;
            if (c->refs <= 0) { free(c->data); delete c; break; }   // orphaned while loading
            c->state = AssetState::Ready;
            c->ok = j->ok && w3d_charimg_valid(c->data, c->size);
            break;
        }
        case JobKind::Creature: {
            CreatureModel* m = (CreatureModel*)j->owner;
            if (m->refs <= 0) {                     // orphaned while loading
                linearFree(m->blob);
                delete m;
                break;
            }
            m->state = AssetState::Ready;
            g_stats.geomBytes += m->size;
            if (!j->ok) { m->h = nullptr; break; }
            GSPGPU_FlushDataCache(m->blob, m->size);
            if (!w3d_creature_valid(m->blob, m->size)) {   // 6.35: never drawn (its palettes could write past the uniforms)
                m->h = nullptr;
                ++g_stats.badCreatureModels;
                g_stats.lastBadCreature = m->id;
                break;
            }
            m->h = (const W3dCreatureHeader*)m->blob;
            m->lods = w3d_creature_lods(m->blob, m->size);   // stage 6.12
            const W3dCreatureTex* ct = (const W3dCreatureTex*)(m->blob + m->h->offTex);
            for (u32 i = 0; i < m->h->texCount; ++i)
                m->slotTex.push_back(ct[i].kind == W3D_CTEX_FIXED && ct[i].texId ? acquireTex(ct[i].texId) : nullptr);
            m->fx = w3d_model_fx(m->blob, m->size);   // stage 6.13
            m->meshHidden = w3d_emitter_volume(m->fx, m->h->vertexCount, m->h->indexCount, m->h->bmin, m->h->bmax);
            if (m->fx) {
                const uint32_t* fids = (const uint32_t*)(m->blob + m->fx->offTexIds);
                for (u32 i = 0; i < m->fx->texCount; ++i) m->fxTex.push_back(acquireTex(fids[i]));
            }
            break;
        }
        case JobKind::Collision: {
            CollLoad* c = (CollLoad*)j->owner;
            TileExt* t = c->tile;
            if (!t || t->doomed || !j->ok) {
                freeColl(c->buf, c->linear);
            } else {
                t->coll = c->buf;
                t->collSize = c->size;
                t->collLinear = c->linear;
                g_stats.collBytes += c->size;
            }
            if (t) t->collLoad = nullptr;
            delete c;
            break;
        }
        case JobKind::Terrain:
        case JobKind::Water:
        case JobKind::Liquid:
        case JobKind::Ground:
        case JobKind::Placement: {
            TileExt* t = (TileExt*)j->owner;
            t->pendingJobs--;
            if (!t->doomed && j->ok) {
                if (j->kind == JobKind::Terrain) {
                    GSPGPU_FlushDataCache(t->terr, j->size);
                    t->th = (const W3dTerrainHeader*)t->terr;
                    t->batches = (const W3dTerrainBatch*)(t->terr + t->th->offBatches);
                    const uint32_t* ids = (const uint32_t*)(t->terr + t->th->offTexIds);
                    const float c[3] = {(32.0f - t->ty - 0.5f) * kTileSize, (32.0f - t->tx - 0.5f) * kTileSize, g_cam[2]};
                    AnchorScope as(c, kClsTerrainTex);
                    for (u32 i = 0; i < t->th->texCount; ++i) t->ttex.push_back(acquireTex(ids[i]));
                } else if (j->kind == JobKind::Ground) {
                    if (!w3d_ground_valid(t->grnd, t->grndSize)) { free(t->grnd); t->grnd = nullptr; }
                } else if (j->kind == JobKind::Liquid) {
                    if (!w3d_liquid_valid(t->liq, t->liqSize)) { free(t->liq); t->liq = nullptr; }
                } else if (j->kind == JobKind::Water) {
                    GSPGPU_FlushDataCache(t->water, j->size);
                    t->wh = (const W3dWaterHeader*)t->water;
                    t->wbatches = (const W3dWaterBatch*)(t->water + t->wh->offBatches);
                    const uint32_t* ids = (const uint32_t*)(t->water + t->wh->offTexIds);
                    const float c[3] = {(32.0f - t->ty - 0.5f) * kTileSize, (32.0f - t->tx - 0.5f) * kTileSize, g_cam[2]};
                    AnchorScope as(c, kClsTerrainTex);
                    for (u32 i = 0; i < t->wh->texCount; ++i) t->wtex.push_back(acquireTex(ids[i]));
                } else if (j->kind == JobKind::Placement) {
                    W3dPlacementHeader ph;
                    memcpy(&ph, t->plcm, sizeof ph);
                    const W3dPlacement* p = (const W3dPlacement*)(t->plcm + sizeof ph);
                    t->instances.reserve(ph.count);
                    std::unordered_map<uint32_t, Model*> local;
                    std::vector<uint16_t> bldgOf, ownerOf;   // stage 6.11: 1 + the building of each placement, 0 = a lone model; 6.33: owners
                    t->bldgs.clear();
                    findBuildings(p, ph.count, bldgOf, ownerOf, (int)t->bldgs.size());
                    for (u32 i = 0; i < ph.count; ++i) {
                        Model*& m = local[p[i].modelId];
                        const uint8_t cls = bldgOf[i] ? kClsBldModel : kClsObject;
                        AnchorScope as(p[i].pos, cls);   // read in order of distance from the camera, probable buildings first
                        if (!m) {
                            m = acquireModel(p[i].modelId);
                            if (m) t->models.push_back(m);
                        } else if (readKey(p[i].pos, cls) < readKey(m->anchor, m->cls)) {
                            memcpy(m->anchor, p[i].pos, sizeof m->anchor);   // a nearer copy of it on this tile
                            m->cls = cls;
                        }
                        if (!m) continue;
                        Instance in;
                        in.bldg = bldgOf[i];
                        in.owner = ownerOf[i];
                        in.model = m;
                        memcpy(in.pos, p[i].pos, sizeof in.pos);
                        memcpy(in.rot, p[i].rot, sizeof in.rot);
                        in.scale = p[i].scale;
                        in.radius = 0;   // filled once the model header is known
                        lightToModel(in.rot, in.lightModel);
                        t->instances.push_back(in);
                    }
                    free(t->plcm);
                    t->plcm = nullptr;
                    buildChunks(t);
                    buildBuildings(t);
                }
            }
            if (j->kind == JobKind::Terrain && !j->ok) t->th = nullptr;
            if (j->kind == JobKind::Liquid && !j->ok && t->liq) { free(t->liq); t->liq = nullptr; }
            if (j->kind == JobKind::Ground && !j->ok && t->grnd) { free(t->grnd); t->grnd = nullptr; }
            if (t->pendingJobs == 0) {
                if (t->doomed) finishTileRelease(t);
                else t->state = TileState::WaitingDeps;
            }
            break;
        }
    }
    delete j;
}

bool tileDepsReady(Tile* t) {
    for (GpuTex* x : t->ttex) if (x->state != AssetState::Ready) return false;
    for (GpuTex* x : t->wtex) if (x->state != AssetState::Ready) return false;
    for (Model* m : t->models) {
        if (m->state != AssetState::Ready || !m->texturesReady()) return false;
    }
    return true;
}

#ifdef W3D_PC_TEST
u32 texTotalSize(const C3D_Tex& t) { return t.size; }   // (the stub keeps the whole chain's size)
#else
u32 texTotalSize(const C3D_Tex& t) { return C3D_TexCalcTotalSize(t.size, t.maxLevel); }
#endif
int g_gxCopies = 0;   // GX copies queued by the current upload pass (citro3d's queue holds 32 entries a frame)

// Creates the GPU texture from t->staging. false: no memory anywhere (t is untouched).
bool tryUpload(GpuTex* t) {
    W3dTexHeader th;
    memcpy(&th, t->staging, sizeof th);
    GSPGPU_FlushDataCache(t->staging, t->stagingSize);
    C3D_TexInitParams p{};
    p.width = th.width;
    p.height = th.height;
    p.maxLevel = (u8)(th.mipCount - 1);
    switch (th.format) {
        case W3D_TEX_ETC1: p.format = GPU_ETC1; break;
        case W3D_TEX_ETC1A4: p.format = GPU_ETC1A4; break;
        case W3D_TEX_RGBA4: p.format = GPU_RGBA4; break;
        case W3D_TEX_RGBA5551: p.format = GPU_RGBA5551; break;
        default: p.format = GPU_RGB565;
    }
    p.type = GPU_TEX_2D;
    p.onVram = vramRoomFor(th.dataSize);   // 6.43: never the last bytes of VRAM (vram_guard.hpp)
    C3D_Tex tex;
    bool vram = p.onVram && C3D_TexInitWithParams(&tex, NULL, p);
    bool ok = vram;
    if (!ok) { p.onVram = false; ok = C3D_TexInitWithParams(&tex, NULL, p); }
    if (!ok) { noteOom(g_stats.oomUpload, th.dataSize); return false; }
    if (!vram) { ++g_stats.texVramMiss; g_stats.texVramMissBytes += th.dataSize; }   // VRAM full: slower sampling from FCRAM
    t->tex = tex;
    t->vram = vram;
    const uint8_t* data = t->staging + W3D_TEX_DATA_OFFSET;
    u32 total = 0;
    for (u32 l = 0, w = th.width, h = th.height; l < th.mipCount; ++l, w >>= 1, h >>= 1) total += w3d_tex_level_size(w, h, th.format);
    if (vram && total == texTotalSize(t->tex)) {
        // Stage 6: one GX copy for the whole mip chain (the levels follow each other in both). Called inside a frame
        // (streamerUploadTextures after C3D_FrameBegin) citro3d queues it ahead of the frame's drawing and returns at once;
        // outside a frame it first waits for the GPU to finish the previous frame - perf log, tile 31_49: ~15 ms per
        // texture, one texture a frame, 20 FPS for 12 s while 441 textures came in. The staging buffer is read by the copy
        // later: it is freed with the usual two-frame delay (deferFree), never at once.
        C3D_SyncTextureCopy((u32*)data, 0, (u32*)t->tex.data, 0, total, 8);
        ++g_gxCopies;
    } else {   // linear memory: a memcpy per level (or an unexpected layout)
        u32 w = th.width, h = th.height, off = 0;
        for (int l = 0; l < th.mipCount; ++l) {
            C3D_TexLoadImage(&t->tex, data + off, GPU_TEXFACE_2D, l);
            off += w3d_tex_level_size(w, h, th.format);
            if (vram) ++g_gxCopies;
            w >>= 1;
            h >>= 1;
        }
    }
    // A texture that did not fit in VRAM lives in linear memory, where C3D_TexLoadImage is a plain memcpy: without a
    // flush the GPU read whatever was in RAM before - noise on objects and players once VRAM filled up (stage 5.31,
    // the photo after the free camera's longer draw distance). VRAM textures are a DMA copy and need nothing.
    C3D_TexFlush(&t->tex);
    C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexSetFilterMipmap(&t->tex, th.mipCount > 1 ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(&t->tex, GPU_REPEAT, GPU_REPEAT);
    t->bytes = th.dataSize;
    (t->vram ? g_stats.texVram : g_stats.texLinear) += t->bytes;
    return true;
}

// Textures that found no memory are drawn white meanwhile; every ~half second a couple of them try again
// (no staging buffer yet: allocate it and read the entry; filled staging: upload).
void retryTextures() {
    g_stats.texOom = (int)g_retry.size();
    if (g_retry.empty() || (g_frame & 31) != 0) return;
    int budget = 2;
    for (size_t i = 0; i < g_retry.size() && budget > 0;) {
        GpuTex* t = g_retry[i];
        --budget;
        if (!t->staging) {
            const W3dEntry* e = g_index->find(W3D_ENTRY_TEXTURE, t->id);
            t->staging = e ? (uint8_t*)linearAlloc(e->size) : nullptr;
            if (!e) { t->retry = false; g_retry.erase(g_retry.begin() + i); continue; }   // not in this pack: white for good
            if (t->staging) {
                t->retry = false;
                g_retry.erase(g_retry.begin() + i);
                t->state = AssetState::Loading;   // the normal path: read job -> upload
                submit(JobKind::Texture, e, t->staging, t);
                continue;
            }
        } else if (tryUpload(t)) {
            deferFree(FreeKind::Linear, t->staging);   // (the GX copy may not have run yet)
            t->staging = nullptr;
            t->retry = false;
            g_retry.erase(g_retry.begin() + i);
            continue;
        }
        ++i;
    }
}

// maxCopies: GX copies this pass may queue (inside a frame citro3d's queue takes 32 entries in all, the frame's own
// clears / splits / display transfers included)
void uploadTextures(double budgetMs, int maxCopies) {
    u64 t0 = svcGetSystemTick();
    g_gxCopies = 0;
    retryTextures();
    if (g_uploads.size() > 1)   // the nearest first (stage 5.30)
        std::stable_sort(g_uploads.begin(), g_uploads.end(), [](const GpuTex* a, const GpuTex* b) {
            return readKey(a->anchor, a->cls) < readKey(b->anchor, b->cls);
        });
    while (!g_uploads.empty() && ticksToMs(svcGetSystemTick() - t0) < budgetMs && g_gxCopies < maxCopies) {
        GpuTex* t = g_uploads.front();
        g_uploads.pop_front();
        if (tryUpload(t)) {
            deferFree(FreeKind::Linear, t->staging);   // (the GX copy may not have run yet)
            t->staging = nullptr;
            ++g_stats.uploadsLastFrame;
            g_stats.uploadBytesLastFrame += t->bytes;
        } else {   // out of memory: white for now, the staging copy stays for the retry
            t->tex = g_white.tex;
            t->bytes = 0;
            t->retry = true;
            g_retry.push_back(t);
            ++g_stats.texOomTotal;
        }
        t->state = AssetState::Ready;
    }
}

// A queued read nobody needs any more (its tile was let go, its model / texture released): dropped unread.
bool orphaned(const Job* j) {
    switch (j->kind) {
        case JobKind::Texture: return ((const GpuTex*)j->owner)->refs <= 0;
        case JobKind::Model: return ((const Model*)j->owner)->refs <= 0;
        case JobKind::Creature: return ((const CreatureModel*)j->owner)->refs <= 0;
        case JobKind::Display: return ((const CreatureDisplay*)j->owner)->refs <= 0;
        case JobKind::Item: return ((const ItemDisplay*)j->owner)->refs <= 0;
        case JobKind::CharImg: return ((const CharImg*)j->owner)->refs <= 0;
        case JobKind::Terrain: case JobKind::Water: case JobKind::Liquid: case JobKind::Ground: case JobKind::Placement:
            return ((const TileExt*)j->owner)->doomed;
        case JobKind::Collision: {
            const CollLoad* c = (const CollLoad*)j->owner;
            return !c->tile || c->tile->doomed;
        }
        default: return false;
    }
}

// The read queue in the order of readKey (stage 5.30): new reads join, dropped ones complete unread (onJobDone frees
// them like any orphan), the most wanted ends at the back where the worker takes it. Reads are ranked in bands that
// widen with the distance (96 yd near the camera, then x2.5 each): near the camera the nearest first, farther away mostly
// in file order, which the SD card reads much faster (tests/run_streamer.sh with SD_KBPS, a synthetic city: the first
// ground in about a third of the time, what stands near in about 60%, every tile's ground a third sooner, the last
// far barrel about 10% later than in plain file order).
void scheduleJobs() {
    static u32 lastSort = 0;
    LightLock_Lock(&g_lock);
    const bool fresh = !g_incoming.empty();
    g_requests.insert(g_requests.end(), g_incoming.begin(), g_incoming.end());
    g_incoming.clear();
    if (fresh || g_frame - lastSort >= 8) {
        lastSort = g_frame;
        size_t w = 0;
        for (size_t i = 0; i < g_requests.size(); ++i) {
            Job* j = g_requests[i];
            if (orphaned(j)) {
                j->ok = false;
                g_done.push_back(j);
                ++g_stats.jobsCancelled;
                continue;
            }
            j->key = readKey(j->anchor, j->cls);
            g_requests[w++] = j;
        }
        g_requests.resize(w);
        static const float kBand = tunable("W3D_BAND", 96.0f), kGrow = tunable("W3D_GROW", 1.5f);
        // bands widen with the distance: what is near is read nearest first, what is far mostly in file order
        auto band = [](float key) { return (int)(logf(1.0f + key / kBand) * (1.0f / logf(1.0f + kGrow))); };
        // the ground (terrain, its textures, collision) before the models of the same band: a district shows its streets first
        static const int kTier = (int)tunable("W3D_GTIER", 1.0f);
        for (Job* j : g_requests) j->key = (float)(kTier ? band(j->key) * 2 + (groundClass(j->cls) ? 0 : 1) : band(j->key));
        std::sort(g_requests.begin(), g_requests.end(), [](const Job* a, const Job* b) {
            const int ba = (int)a->key, bb = (int)b->key;
            if (ba != bb) return ba > bb;                 // the most wanted last
            if (a->file != b->file) return a->file > b->file;
            return a->offset > b->offset;                 // forwards through the file, from the back
        });
    }
    g_stats.maxQueued = std::max(g_stats.maxQueued, (int)g_requests.size());
    const bool any = !g_requests.empty();
    LightLock_Unlock(&g_lock);
    if (any && g_thread) LightEvent_Signal(&g_wake);
}

void processDeferred() {
    size_t w = 0;
    for (size_t i = 0; i < g_deferred.size(); ++i) {
        Deferred& d = g_deferred[i];
        if (g_frame - d.frame < 2) { g_deferred[w++] = d; continue; }
        switch (d.kind) {
            case FreeKind::Linear: linearFree(d.p); break;
            case FreeKind::Slot: g_freeSlots.push_back(d.slot); break;
            case FreeKind::Texture: {
                GpuTex* t = (GpuTex*)d.p;
                if (t->bytes) C3D_TexDelete(&t->tex);
                delete t;
                break;
            }
            case FreeKind::ModelStruct: delete (Model*)d.p; break;
            case FreeKind::CreatureStruct: delete (CreatureModel*)d.p; break;
            case FreeKind::DisplayStruct: delete (CreatureDisplay*)d.p; break;
        }
    }
    g_deferred.resize(w);
}

}  // namespace

// ================================================================== API
// ------------------------------------------------------------------ 6.24: the far ground
// The map's FARL entry stays in memory for the whole visit; vertex buffers (linear memory, 4.6 KB) are made for the tiles
// actually drawn and the least recently used are dropped beyond kFarCache.
namespace {
// 6.31: the map-wide blobs live in linear memory (CPU-only data, but the normal heap is the scarce one)
struct LinBlob {
    uint8_t* p = nullptr;
    uint32_t n = 0;
    uint8_t* data() const { return p; }
    bool resize(uint32_t size) { clear(); p = (uint8_t*)linearAlloc(size); n = p ? size : 0; return p != nullptr; }
    void clear() { if (p) linearFree(p); p = nullptr; n = 0; }
};
LinBlob g_farBlob;
const W3dFarHeader* g_farHd = nullptr;
GpuTex* g_farTex = nullptr;
uint16_t* g_farIdx = nullptr;          // linear: the shared index list
struct FarVB { W3dTerrainVertex* v = nullptr; uint32_t used = 0; float zmin = 0, zmax = 0; };
std::unordered_map<int, FarVB> g_farVB;   // [tileY * 64 + tileX]
uint32_t g_farFrame = 0;
constexpr size_t kFarCache = 128;   // (6.34: the horizon at 1200 yd is up to 9 x 9 tiles)

void farLoad() {
    const uint32_t size = streamerEntrySize(W3D_ENTRY_FARLOD, 0);
    if (!size) return;
    if (!g_farBlob.resize(size)) return;
    if (!streamerReadEntry(W3D_ENTRY_FARLOD, 0, g_farBlob.data(), size) || !w3d_far_valid(g_farBlob.data(), size)) {
        g_farBlob.clear();
        return;
    }
    g_farIdx = (uint16_t*)linearAlloc(W3D_FAR_INDICES * sizeof(uint16_t));
    if (!g_farIdx) { g_farBlob.clear(); return; }
    w3d_far_build_indices(g_farIdx);
    GSPGPU_FlushDataCache(g_farIdx, W3D_FAR_INDICES * sizeof(uint16_t));
    g_farHd = (const W3dFarHeader*)g_farBlob.data();
    const float here[3] = {0, 0, 0};
    AnchorScope as(here, kClsTile);   // small, wanted at once (it hides the holes while the rest loads)
    g_farTex = acquireTex(g_farHd->texId);
    g_stats.farTiles = (int)g_farHd->tileCount;
}

// ------------------------------------------------------------------ 6.29: the far buildings
LinBlob g_foBlob;
const W3dFarObjHeader* g_foHd = nullptr;
GpuTex* g_foTex = nullptr;
struct FarObjVB {
    W3dTerrainVertex* v = nullptr;
    uint16_t* i = nullptr;
    uint32_t idxCount = 0;
    std::vector<uint32_t> objStart;   // first index of each object of the tile (+ the end)
    uint32_t used = 0;
    float center[3] = {0, 0, 0}, radius = 0;
};
std::unordered_map<int, FarObjVB> g_foVB;   // [tileY * 64 + tileX]
constexpr size_t kFarObjCache = 160;   // (6.34: to the horizon)

void farObjLoad() {
    const uint32_t size = streamerEntrySize(W3D_ENTRY_FAROBJ, 0);
    if (!size) return;
    if (!g_foBlob.resize(size)) return;
    if (!streamerReadEntry(W3D_ENTRY_FAROBJ, 0, g_foBlob.data(), size) || !w3d_farobj_valid(g_foBlob.data(), size)) {
        g_foBlob.clear();
        return;
    }
    g_foHd = (const W3dFarObjHeader*)g_foBlob.data();
    const float here[3] = {0, 0, 0};
    AnchorScope as(here, kClsTile);
    g_foTex = acquireTex(g_foHd->paletteTexId);
    g_stats.farObjects = (int)g_foHd->objectCount;
}

void farObjFree(FarObjVB& vb, bool now) {
    if (now) { if (vb.v) linearFree(vb.v); if (vb.i) linearFree(vb.i); }
    else { if (vb.v) deferFree(FreeKind::Linear, vb.v); if (vb.i) deferFree(FreeKind::Linear, vb.i); }
    vb.v = nullptr;
    vb.i = nullptr;
}

void farObjUnload() {
    for (auto& [k, vb] : g_foVB) farObjFree(vb, true);
    g_foVB.clear();
    if (g_foTex) releaseTex(g_foTex);
    g_foTex = nullptr;
    g_foBlob.clear();
    g_foHd = nullptr;
    g_stats.farObjects = g_stats.farObjDrawn = g_stats.farObjTiles = 0;
}

void farUnload() {
    for (auto& [k, vb] : g_farVB) if (vb.v) linearFree(vb.v);
    g_farVB.clear();
    if (g_farTex) releaseTex(g_farTex);
    g_farTex = nullptr;
    if (g_farIdx) linearFree(g_farIdx);
    g_farIdx = nullptr;
    g_farBlob.clear();
    g_farHd = nullptr;
    g_stats.farTiles = g_stats.farDrawn = g_stats.farCached = 0;
}
}  // namespace

bool streamerFarDraws(const float cam[3], float maxDist, float detailDist, std::vector<FarDraw>& out, GpuTex** atlas, const uint16_t** indices,
                      float* zScale) {
    out.clear();
    g_stats.farDrawn = 0;
    if (!g_farHd || !g_farTex || g_farTex->state != AssetState::Ready || g_farTex == &g_white) return false;
    ++g_farFrame;
    static bool detailed[64 * 64];
    memset(detailed, 0, sizeof detailed);
    for (const Tile* t : g_ready)
        if (t->tx >= 0 && t->tx < 64 && t->ty >= 0 && t->ty < 64 && !tileBeyond(t->tx, t->ty, cam, detailDist)) detailed[t->ty * 64 + t->tx] = true;
    const float TS = W3D_FAR_TILE_SIZE;
    const int ctx = (int)floorf(32.0f - cam[1] / TS), cty = (int)floorf(32.0f - cam[0] / TS);
    const int r = (int)ceilf(maxDist / TS) + 1;
    for (int ty = std::max(0, cty - r); ty <= std::min(63, cty + r); ++ty)
        for (int tx = std::max(0, ctx - r); tx <= std::min(63, ctx + r); ++tx) {
            if (detailed[ty * 64 + tx]) continue;
            const W3dFarTile* ft = w3d_far_tile(g_farBlob.data(), tx, ty);
            if (!ft) continue;
            float minX, minY;
            w3d_far_tile_origin(tx, ty, &minX, &minY);
            // nearest point of the tile's square to the camera (x / y only: a coarse cut, the frustum test follows)
            const float nx = std::clamp(cam[0], minX, minX + TS) - cam[0], ny = std::clamp(cam[1], minY, minY + TS) - cam[1];
            if (nx * nx + ny * ny > maxDist * maxDist) continue;
            FarVB& vb = g_farVB[ty * 64 + tx];
            if (!vb.v) {
                vb.v = (W3dTerrainVertex*)linearAlloc(W3D_FAR_VERTS * sizeof(W3dTerrainVertex));
                if (!vb.v) { g_farVB.erase(ty * 64 + tx); continue; }
                w3d_far_build_vertices(g_farHd, ft, vb.v);
                GSPGPU_FlushDataCache(vb.v, W3D_FAR_VERTS * sizeof(W3dTerrainVertex));
                int16_t lo = 32767, hi = -32767;
                for (int k = 0; k < W3D_FAR_VERTS; ++k) { lo = std::min(lo, ft->h[k]); hi = std::max(hi, ft->h[k]); }
                vb.zmin = lo * g_farHd->zScale;
                vb.zmax = hi * g_farHd->zScale;
            }
            vb.used = g_farFrame;
            FarDraw d;
            d.verts = vb.v;
            d.origin[0] = minX; d.origin[1] = minY; d.origin[2] = 0;
            d.center[0] = minX + TS * 0.5f; d.center[1] = minY + TS * 0.5f; d.center[2] = 0.5f * (vb.zmin + vb.zmax);
            const float hz = 0.5f * (vb.zmax - vb.zmin);
            d.radius = sqrtf(2.0f * (TS * 0.5f) * (TS * 0.5f) + hz * hz);
            out.push_back(d);
        }
    // drop the least recently used buffers beyond the cap (never one drawn this frame: the GPU reads it)
    while (g_farVB.size() > kFarCache) {
        auto old = g_farVB.end();
        for (auto it = g_farVB.begin(); it != g_farVB.end(); ++it)
            if (it->second.used != g_farFrame && (old == g_farVB.end() || it->second.used < old->second.used)) old = it;
        if (old == g_farVB.end()) break;
        deferFree(FreeKind::Linear, old->second.v);
        g_farVB.erase(old);
    }
    g_stats.farDrawn = (int)out.size();
    g_stats.farCached = (int)g_farVB.size();
    *atlas = g_farTex;
    *indices = g_farIdx;
    *zScale = g_farHd->zScale;
    return !out.empty();
}

bool streamerFarObjectDraws(const float cam[3], float maxDist, float detailDist, std::vector<FarObjDraw>& out, GpuTex** palette, float* zScale) {
    out.clear();
    g_stats.farObjDrawn = 0;
    if (!g_foHd || !g_foTex || g_foTex->state != AssetState::Ready || g_foTex == &g_white) return false;
    const float zs = g_farHd ? g_farHd->zScale : 0.25f;
    static uint32_t frame = 0;
    ++frame;
    static const Tile* loaded[64 * 64];
    memset(loaded, 0, sizeof loaded);
    for (const Tile* t : g_ready)
        if (t->tx >= 0 && t->tx < 64 && t->ty >= 0 && t->ty < 64) loaded[t->ty * 64 + t->tx] = t;
    const W3dFarObject* objs = w3d_farobj_objects(g_foBlob.data());
    const W3dFarShell* shells = w3d_farobj_shells(g_foBlob.data());
    const float TS = W3D_FAR_TILE_SIZE;
    const int ctx = (int)floorf(32.0f - cam[1] / TS), cty = (int)floorf(32.0f - cam[0] / TS);
    const int r = (int)ceilf(maxDist / TS) + 2;   // (a building may stand well beyond its tile)
    int built = 0;
    static std::vector<std::pair<uint32_t, uint32_t>> shown;   // building groups a loaded tile shows: (model id, instance index)
    for (int ty = std::max(0, cty - r); ty <= std::min(63, cty + r); ++ty)
        for (int tx = std::max(0, ctx - r); tx <= std::min(63, ctx + r); ++tx) {
            const int k = ty * 64 + tx;
            const uint32_t first = g_foHd->tileStart[k], count = g_foHd->tileStart[k + 1] - first;
            if (!count) continue;
            auto it = g_foVB.find(k);
            if (it == g_foVB.end() || !it->second.v) {
                if (built >= 2) continue;   // at most two new buffers a frame (a few thousand vertices each)
                uint32_t nv = 0, ni = 0;
                for (uint32_t i = 0; i < count; ++i) { nv += shells[objs[first + i].shell].vertCount; ni += shells[objs[first + i].shell].indexCount; }
                if (!nv || nv > 65535) continue;
                FarObjVB vb;
                vb.v = (W3dTerrainVertex*)linearAlloc(nv * sizeof(W3dTerrainVertex));
                vb.i = (uint16_t*)linearAlloc(ni * sizeof(uint16_t));
                if (!vb.v || !vb.i) { farObjFree(vb, true); continue; }
                vb.objStart.resize(count + 1);
                w3d_farobj_build_tile(g_foBlob.data(), tx, ty, zs, vb.v, nv, vb.i, ni, &vb.idxCount, vb.objStart.data());
                GSPGPU_FlushDataCache(vb.v, nv * sizeof(W3dTerrainVertex));
                GSPGPU_FlushDataCache(vb.i, ni * sizeof(uint16_t));
                // the sphere around the tile's buildings
                float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                for (uint32_t i = 0; i < count; ++i) {
                    const W3dFarObject& o = objs[first + i];
                    const float rad = shells[o.shell].radius;
                    for (int q = 0; q < 3; ++q) { lo[q] = std::min(lo[q], o.pos[q] - rad); hi[q] = std::max(hi[q], o.pos[q] + rad); }
                }
                float r2 = 0;
                for (int q = 0; q < 3; ++q) { vb.center[q] = 0.5f * (lo[q] + hi[q]); r2 += 0.25f * (hi[q] - lo[q]) * (hi[q] - lo[q]); }
                vb.radius = sqrtf(r2);
                it = g_foVB.insert_or_assign(k, std::move(vb)).first;
                ++built;
            }
            FarObjVB& vb = it->second;
            // the coarse cut: the buildings' sphere within the draw distance
            const float dx = vb.center[0] - cam[0], dy = vb.center[1] - cam[1];
            if (sqrtf(dx * dx + dy * dy) - vb.radius > maxDist) continue;
            vb.used = frame;
            float minX, minY;
            w3d_far_tile_origin(tx, ty, &minX, &minY);
            FarObjDraw d;
            d.verts = vb.v;
            d.origin[0] = minX; d.origin[1] = minY; d.origin[2] = 0;
            memcpy(d.center, vb.center, sizeof d.center);
            d.radius = vb.radius;
            const Tile* t = loaded[k];
            // 6.34: the whole tile beyond the real buildings' distance (its nearest building centre farther than detailDist)
            bool allBeyond = t != nullptr;
            for (uint32_t i = 0; i < count && allBeyond; ++i) {
                const W3dFarObject& o = objs[first + i];
                const float ox = o.pos[0] - cam[0], oy = o.pos[1] - cam[1], oz = o.pos[2] - cam[2];
                allBeyond = ox * ox + oy * oy + oz * oz > detailDist * detailDist;
            }
            if (!t || allBeyond) {   // the tile is not there (or all of it is beyond the real buildings): all of its buildings
                d.idx = vb.i;
                d.count = vb.idxCount;
                if (d.count) out.push_back(d);
                g_stats.farObjDrawn += (int)count;
                continue;
            }
            // a loaded tile: the buildings it does not show yet, each found by its first group's placement - shown when that group
            // is: its Tile::Bldg is open, or (a one-group building, no Bldg) its model is drawable
            shown.clear();
            for (uint32_t m = 0; m < t->instances.size(); ++m) {
                const Instance& in = t->instances[m];
                if (!in.model || !in.model->building) continue;
                const bool vis = in.bldg ? (in.bldg <= t->bldgs.size() && t->bldgs[in.bldg - 1].open) : in.model->drawable;
                if (vis) shown.push_back({in.model->id, m});
            }
            uint32_t runStart = 0, runEnd = 0;
            auto flush = [&]() {
                if (runEnd > runStart) { FarObjDraw e = d; e.idx = vb.i + runStart; e.count = runEnd - runStart; out.push_back(e); }
                runStart = runEnd = 0;
            };
            for (uint32_t i = 0; i < count; ++i) {
                const W3dFarObject& o = objs[first + i];
                bool open = false;
                // 6.34: a building whose centre is beyond the real buildings' distance is a shell (overlap rather than a gap)
                const float ox = o.pos[0] - cam[0], oy = o.pos[1] - cam[1], oz = o.pos[2] - cam[2];
                const bool beyond = ox * ox + oy * oy + oz * oz > detailDist * detailDist;
                for (const auto& [mid, m] : shown) {
                    if (beyond) break;
                    if (mid != o.groupModel) continue;
                    const Instance& in = t->instances[m];
                    if (fabsf(in.pos[0] - o.groupPos[0]) < 0.05f && fabsf(in.pos[1] - o.groupPos[1]) < 0.05f &&
                        fabsf(in.pos[2] - o.groupPos[2]) < 0.05f) { open = true; break; }
                }
                const uint32_t a = vb.objStart[i], b = vb.objStart[i + 1];
                if (open || a == b) { flush(); continue; }
                if (runEnd == a && runEnd > runStart) runEnd = b;
                else { flush(); runStart = a; runEnd = b; }
                ++g_stats.farObjDrawn;
            }
            flush();
        }
    // drop the least recently used buffers beyond the cap (never one drawn this frame)
    while (g_foVB.size() > kFarObjCache) {
        auto old = g_foVB.end();
        for (auto it = g_foVB.begin(); it != g_foVB.end(); ++it)
            if (it->second.used != frame && (old == g_foVB.end() || it->second.used < old->second.used)) old = it;
        if (old == g_foVB.end()) break;
        farObjFree(old->second, false);
        g_foVB.erase(old);
    }
    g_stats.farObjTiles = (int)g_foVB.size();
    *palette = g_foTex;
    *zScale = zs;
    return !out.empty();
}

bool streamerInit(PackIndex* index, char* err, size_t errLen) {
    g_index = index;
    g_quit = false;   // a map change shuts the streamer down and starts it again on the next pack
    g_freeSlots.clear();
    g_pool = nullptr;
    for (int& f : g_ioFiles) f = -1;
    for (int k = 0; k < index->fileCount; ++k) {
        g_ioFiles[k] = open(index->files[k], O_RDONLY);
        if (g_ioFiles[k] < 0) { snprintf(err, errLen, "cannot reopen %s", index->files[k]); return false; }
    }

    // White 8x8 stand-in texture.
    C3D_TexInit(&g_white.tex, 8, 8, GPU_RGB565);
    u16* px = (u16*)linearAlloc(128);
    for (int i = 0; i < 64; ++i) px[i] = 0xFFFF;
    GSPGPU_FlushDataCache(px, 128);
    C3D_TexUpload(&g_white.tex, px);
    linearFree(px);
    C3D_TexSetWrap(&g_white.tex, GPU_REPEAT, GPU_REPEAT);
    g_white.state = AssetState::Ready;
    g_white.refs = 1 << 30;

    // Terrain slot pool: up to half of the free linear heap, 9..36 slots
    // (36 = a 5x5 load radius plus a ring that is still being released).
    g_slotSize = (index->maxTerrainSize + 127) & ~127u;
    u32 freeLin = linearSpaceFree();
    g_slotCount = std::clamp((int)(freeLin / 2 / g_slotSize), 9, 36);
    while (g_slotCount >= 9 && !(g_pool = (uint8_t*)linearAlloc((size_t)g_slotSize * g_slotCount))) g_slotCount -= 4;
    if (!g_pool) { snprintf(err, errLen, "no linear memory for the terrain pool"); return false; }
    for (int i = g_slotCount - 1; i >= 0; --i) g_freeSlots.push_back(i);
    g_stats.slotsTotal = g_slotCount;

    // IO thread: core 2 on New 3DS, else core 1 with a CPU-time grant, else synchronous.
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (n3ds) {
        g_thread = threadCreate(workerMain, NULL, 32 * 1024, prio - 1, 2, false);
        if (g_thread) g_stats.workerCore = 2;
    }
    if (!g_thread && R_SUCCEEDED(APT_SetAppCpuTimeLimit(30))) {
        g_thread = threadCreate(workerMain, NULL, 32 * 1024, prio - 1, 1, false);
        if (g_thread) g_stats.workerCore = 1;
    }
    if (!g_thread) {
        g_thread = threadCreate(workerMain, NULL, 32 * 1024, prio + 1, 0, false);
        if (g_thread) g_stats.workerCore = 0;
    }
    farLoad();   // 6.24 (after the worker: the atlas is read like any texture)
    farObjLoad();   // 6.29
    return true;
}

void streamerShutdown() {
#ifndef W3D_PC_TEST
    // Stage 6: texture copies queued in the last frame read staging buffers and write textures that are freed below at
    // once; they ran at the start of that frame's GPU work, two vertical blanks later they are long done.
    gspWaitForVBlank();
    gspWaitForVBlank();
#endif
    if (g_thread) {
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    // Complete every outstanding job (unrun ones as failed) through the normal path,
    // so assets orphaned while loading are freed exactly as during play.
    farObjUnload();   // 6.29
    farUnload();   // 6.24: (the atlas is released before the jobs are drained: a read still running frees it on completion)
    // Completions may submit follow-up jobs (display -> model -> textures): loop until drained.
    for (;;) {
        for (Job* j : g_requests) j->ok = false;
        for (Job* j : g_incoming) j->ok = false;
        std::deque<Job*> left;
        left.swap(g_done);
        left.insert(left.end(), g_requests.begin(), g_requests.end());
        left.insert(left.end(), g_incoming.begin(), g_incoming.end());
        g_requests.clear();
        g_incoming.clear();
        if (left.empty()) break;
        for (Job* j : left) onJobDone(j);
    }
    // Displays still held by the caller (normally none): force-release.
    for (auto& [id, d] : std::unordered_map<uint32_t, CreatureDisplay*>(g_displays)) {
        d->refs = 1;
        streamerReleaseDisplay(d);
    }
    for (auto& [key, d] : std::unordered_map<uint64_t, CreatureDisplay*>(g_playerDisplays)) {
        d->refs = 1;
        streamerReleaseDisplay(d);
    }
    for (auto& [id, c] : g_chars) { free(c->blob); delete c; }
    g_chars.clear();
    for (auto& [id, it] : g_items) delete it;   // none left normally
    g_items.clear();
    for (auto& [id, c] : g_charImgs) { free(c->data); delete c; }   // none left normally
    g_charImgs.clear();
    // Drop everything; the GPU is idle at shutdown.
    while (!g_tiles.empty()) {
        TileExt* t = g_tiles.back();
        t->pendingJobs = 0;
        finishTileRelease(t);
    }
    for (auto& d : g_deferred) d.frame = g_frame - 10;
    processDeferred();
    if (g_pool) linearFree(g_pool);
    g_pool = nullptr;
    g_freeSlots.clear();
    g_deferred.clear();
    g_retry.clear();
    for (int& f : g_ioFiles) {
        if (f >= 0) close(f);
        f = -1;
    }
    g_index = nullptr;
    C3D_TexDelete(&g_white.tex);
}

uint32_t streamerNowMs() { return (uint32_t)(uint64_t)ticksToMs(svcGetSystemTick()); }

bool streamerHasDisplay(uint32_t displayId) { return g_index && g_index->find(W3D_ENTRY_DISPLAY, displayId); }

void streamerSetView(float yaw) {
    g_fwd[0] = cosf(yaw);
    g_fwd[1] = sinf(yaw);
    g_haveFwd = true;
}

void streamerSetBoost(bool on) { g_boost = on; }
void streamerSetWorldWanted(bool on) { g_worldWanted = on; }

// The texture side of the frame: player bodies and texture uploads. Inside a frame the GX copies are queued with the
// frame's GPU work (no wait); outside one each waits for the GPU (loading loops, where it is idle anyway).
static void uploadPass(bool inFrame) {
    const u64 a = svcGetSystemTick();
    pumpPlayers();
    const u64 b = svcGetSystemTick();
    g_stats.playersMsLastFrame = ticksToMs(b - a);
    uploadTextures(g_boost ? 14.0 : g_uploads.size() > 24 ? 5.0 : 2.5, !inFrame ? 1 << 30 : g_boost ? 12 : 8);   // (stage 6.3: room for the GPU kicks' entries too)
    g_stats.uploadMsLastFrame = ticksToMs(svcGetSystemTick() - b);
}

void streamerUploadTextures() {
    g_stats.uploadsLastFrame = 0;
    g_stats.uploadBytesLastFrame = 0;
    uploadPass(true);
    g_stats.uploadsPending = (int)g_uploads.size();
}

void streamerUpdate(const float camPos[3], float drawDist, bool uploadsLater) {
    memcpy(g_cam, camPos, sizeof g_cam);
    const u64 st0 = svcGetSystemTick();
    g_stats.jobsDoneLastFrame = 0;
    g_stats.uploadsLastFrame = 0;
    g_stats.uploadBytesLastFrame = 0;
    // 1. Collect finished IO.
    if (!g_thread) {   // synchronous fallback: a few jobs per frame
        u64 t0 = svcGetSystemTick();
        while (ticksToMs(svcGetSystemTick() - t0) < 4.0) {
            Job* j = nullptr;
            if (!g_requests.empty()) { j = g_requests.back(); g_requests.pop_back(); }
            if (!j) break;
            runJob(j);
            g_done.push_back(j);
        }
    }
    for (;;) {
        LightLock_Lock(&g_lock);
        Job* j = nullptr;
        if (!g_done.empty()) { j = g_done.front(); g_done.pop_front(); }
        LightLock_Unlock(&g_lock);
        if (!j) break;
        onJobDone(j);
        ++g_stats.jobsDoneLastFrame;
    }
    const u64 st1 = svcGetSystemTick();
    g_stats.doneMsLastFrame = ticksToMs(st1 - st0);

    // 2. Player bodies and texture uploads under a frame budget (more while many wait, most of the frame under a loading
    // screen) - here, or in streamerUploadTextures() inside the frame (stage 6: the main loop).
    if (!uploadsLater) uploadPass(false);
    const u64 st3 = svcGetSystemTick();

    // 3. Progressive tiles (stage 5.30): the ground is drawn as soon as it and its textures are in, the water with its
    // textures, each model as soon as it and its textures are (Model::drawable): a city's tile no longer waits for its
    // last barrel before anything of it shows.
    const u64 nowTick = svcGetSystemTick();
    const uint32_t nowMs = (uint32_t)(uint64_t)ticksToMs(nowTick);
    for (size_t i = 0; i < g_waitingModels.size();) {
        Model* m = g_waitingModels[i];
        if (m->texturesReady()) {
            m->drawable = true;
            m->drawableMs = nowMs;
            g_waitingModels[i] = g_waitingModels.back();
            g_waitingModels.pop_back();
        } else {
            ++i;
        }
    }
    g_stats.modelsWaiting = (int)g_waitingModels.size();
    for (TileExt* t : g_tiles) {
        if (t->doomed || !t->th) continue;   // terrain read failed: stays invisible
        if (t->state == TileState::WaitingDeps) {
            bool ground = true;
            for (GpuTex* x : t->ttex) if (x->state != AssetState::Ready) { ground = false; break; }
            if (!ground) continue;
            t->state = TileState::Ready;
            t->readyMs = nowMs;
            double ms = ticksToMs(svcGetSystemTick() - t->requestTick);
            g_stats.tileLoads++;
            g_stats.lastTileMs = ms;
            g_stats.maxTileMs = std::max(g_stats.maxTileMs, ms);
            g_stats.sumTileMs += ms;
        }
        if (t->state == TileState::Ready && t->bldgsClosed > 0) updateBuildings(t, nowTick, nowMs);   // stage 6.11
        if (t->state != TileState::Ready || t->complete) continue;
        if (!t->waterReady && t->wh) {
            bool w = true;
            for (GpuTex* x : t->wtex) if (x->state != AssetState::Ready) { w = false; break; }
            t->waterReady = w;
        }
        if (tileDepsReady(t)) {
            t->complete = true;
            double ms = ticksToMs(svcGetSystemTick() - t->requestTick);
            g_stats.tileFullLoads++;
            g_stats.lastTileFullMs = ms;
            g_stats.maxTileFullMs = std::max(g_stats.maxTileFullMs, ms);
            g_stats.sumTileFullMs += ms;
        }
    }

    // 3b. Collision near the camera only (load within kCollLoadDist of a tile, drop beyond kCollDropDist).
    for (TileExt* t : g_tiles) {
        if (!g_worldWanted) break;   // 6.37: login screens (the tiles are released below)
        if (t->doomed || !t->th) continue;
        float x0 = (32.0f - t->ty - 1.0f) * kTileSize, y0 = (32.0f - t->tx - 1.0f) * kTileSize;
        float dx = std::max({x0 - camPos[0], 0.0f, camPos[0] - (x0 + kTileSize)});
        float dy = std::max({y0 - camPos[1], 0.0f, camPos[1] - (y0 + kTileSize)});
        float d = sqrtf(dx * dx + dy * dy);
        if (d <= kCollLoadDist && !t->coll && !t->collLoad) {
            const W3dEntry* ce = g_index->find(W3D_ENTRY_COLLISION, w3d_tile_id(t->tx, t->ty));
            if (!ce) continue;
            bool linear = false;
            uint8_t* buf = nullptr;
            if (heapFreeNow() > ce->size + kCollHeapReserve) buf = (uint8_t*)malloc(ce->size);   // 6.31: never the heap's last MBs
            if (!buf) { buf = (uint8_t*)linearAlloc(ce->size); linear = true; ++g_stats.collHeapMiss; }
            if (!buf) { ++g_stats.collFailed; g_stats.oomLastSize = ce->size; g_stats.oomLastFree = linearSpaceFree(); continue; }
            t->collLoad = new CollLoad{t, buf, ce->size, linear};
            const float near[3] = {std::clamp(camPos[0], x0, x0 + kTileSize), std::clamp(camPos[1], y0, y0 + kTileSize), camPos[2]};
            AnchorScope as(near, kClsColl);
            submit(JobKind::Collision, ce, buf, t->collLoad);
        } else if (d > kCollDropDist && t->coll) {
            freeColl(t->coll, t->collLinear);
            g_stats.collBytes -= t->collSize;
            t->coll = nullptr;
        }
    }

    // 4. Decide the wanted set around the camera.
    int ctx = (int)floorf(32.0f - camPos[1] / kTileSize);
    int cty = (int)floorf(32.0f - camPos[0] / kTileSize);
    int r = drawDist > 450.0f ? 2 : 1;
    if ((2 * r + 1) * (2 * r + 1) > g_slotCount) r = 1;
    // 6.31: the normal heap running dry (tiles' heights, instances, models' tables grow with the ring): back to 3 x 3 and the
    // tiles beyond it are released, until there is room again
    g_stats.heapFree = heapFreeNow();
    if (!g_stats.heapShrunk && g_stats.heapFree < kHeapLow) { g_stats.heapShrunk = true; ++g_stats.heapShrinks; }
    else if (g_stats.heapShrunk && g_stats.heapFree > kHeapOk) g_stats.heapShrunk = false;
    if (g_stats.heapShrunk) r = 1;
    const int keep = g_stats.heapShrunk ? 1 : r + 1;

    for (TileExt* t : std::vector<TileExt*>(g_tiles)) {
        if (t->doomed) continue;
        if (!g_worldWanted || abs(t->tx - ctx) > keep || abs(t->ty - cty) > keep) releaseTile(t);
    }
    if (!g_worldWanted) r = -1;   // 6.37: nothing wanted on the login screens

    struct Want { int tx, ty; float d2; bool ahead; };
    std::vector<Want> want;
    for (int dx = -r; dx <= r; ++dx)
        for (int dy = -r; dy <= r; ++dy) {
            int tx = ctx + dx, ty = cty + dy;
            if (!g_index->hasTile(tx, ty) || findTile(tx, ty)) continue;
            float cxw = (32.0f - ty - 0.5f) * kTileSize, cyw = (32.0f - tx - 0.5f) * kTileSize;
            float ddx = cxw - camPos[0], ddy = cyw - camPos[1];
            want.push_back({tx, ty, ddx * ddx + ddy * ddy, false});
        }
    // 6.24: read ahead - the tiles around the point 300 yards in front of the camera that lie beyond the wanted ring (still
    // within 'keep', so they are not released again): by the time the walker gets there their ground is in. Only into free
    // terrain slots (never evicting for it), after everything of the ring.
    if (g_haveFwd && !g_stats.heapShrunk && g_worldWanted) {
        const float px = camPos[0] + g_fwd[0] * 300.0f, py = camPos[1] + g_fwd[1] * 300.0f;
        const int ptx = (int)floorf(32.0f - py / kTileSize), pty = (int)floorf(32.0f - px / kTileSize);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const int tx = ptx + dx, ty = pty + dy;
                if (std::max(abs(tx - ctx), abs(ty - cty)) != r + 1) continue;   // only the next ring
                if (!g_index->hasTile(tx, ty) || findTile(tx, ty)) continue;
                const float cxw = (32.0f - ty - 0.5f) * kTileSize, cyw = (32.0f - tx - 0.5f) * kTileSize;
                const float ddx = cxw - camPos[0], ddy = cyw - camPos[1];
                want.push_back({tx, ty, ddx * ddx + ddy * ddy, true});
            }
    }
    std::sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.ahead != b.ahead ? !a.ahead : a.d2 < b.d2; });

    int loading = 0;
    for (TileExt* t : g_tiles) if (!t->doomed && t->state != TileState::Ready) ++loading;
    for (const Want& w : want) {
        static const int kMaxLoading = (int)tunable("W3D_TILES", 4.0f);
        if (loading >= kMaxLoading) break;   // the read queue puts the nearest tile's contents first (stage 5.30)
        if (w.ahead && (g_freeSlots.empty() || loading > 0)) break;   // read-ahead: only when the ring is in and a slot is free
        if (g_freeSlots.empty()) {
            // Evict the farthest tile outside the wanted radius.
            TileExt* victim = nullptr;
            int worst = r;
            for (TileExt* t : g_tiles) {
                if (t->doomed) continue;
                int d = std::max(abs(t->tx - ctx), abs(t->ty - cty));
                if (d > worst) { worst = d; victim = t; }
            }
            if (victim) releaseTile(victim);
            if (g_freeSlots.empty()) break;   // slot frees after the deferred delay
        }
        if (requestTile(w.tx, w.ty)) ++loading;
    }

    // 5. This frame's reads into the queue, in order.
    const u64 st4 = svcGetSystemTick();
    g_stats.tilesMsLastFrame = ticksToMs(st4 - st3);
    scheduleJobs();
    const u64 st5 = svcGetSystemTick();
    g_stats.scheduleMsLastFrame = ticksToMs(st5 - st4);

    // 6. Ready list + stats.
    g_ready.clear();
    g_stats.tilesLoading = 0;
    for (TileExt* t : g_tiles) {
        if (t->doomed) continue;
        if (t->state == TileState::Ready) g_ready.push_back(t);
        else g_stats.tilesLoading++;
    }
    g_stats.tilesReady = (int)g_ready.size();
    g_stats.slotsUsed = g_slotCount - (int)g_freeSlots.size();
    LightLock_Lock(&g_lock);
    g_stats.jobsQueued = (int)g_requests.size();
    for (int& q : g_stats.queuedByClass) q = 0;
    g_stats.queuedBytes = 0;
    for (const Job* j : g_requests) {
        g_stats.queuedByClass[std::min<int>(j->cls, kStreamClasses - 1)]++;
        g_stats.queuedBytes += j->size;
    }
    g_stats.ioDoneWaiting = (int)g_done.size();
    LightLock_Unlock(&g_lock);
    g_stats.models = (int)g_models.size();
    g_stats.textures = (int)g_tex.size();
    g_stats.uploadsPending = (int)g_uploads.size();
    g_stats.displays = (int)g_displays.size();
    g_stats.players = (int)g_playerDisplays.size();
    g_stats.creatureModels = (int)g_cmodels.size();
    g_stats.retryTextures = (int)g_retry.size();
    g_stats.deferredFrees = (int)g_deferred.size();
    g_stats.updateMsLastFrame = ticksToMs(svcGetSystemTick() - st0);
}

CreatureDisplay* streamerAcquireDisplay(uint32_t displayId) {
    auto it = g_displays.find(displayId);
    if (it != g_displays.end()) { it->second->refs++; return it->second; }
    const W3dEntry* e = g_index->find(W3D_ENTRY_DISPLAY, displayId);
    if (!e || e->size < sizeof(W3dDisplay)) return nullptr;
    CreatureDisplay* d = new CreatureDisplay;
    d->id = displayId;
    d->refs = 1;
    g_displays[displayId] = d;
    submit(JobKind::Display, e, (uint8_t*)&d->d, d, sizeof(W3dDisplay));
    return d;
}

ItemDisplay* streamerAcquireItem(uint32_t displayId) { return acquireItem(displayId); }
void streamerReleaseItem(ItemDisplay* it) { releaseItem(it); }
Model* streamerAcquireItemModel(uint32_t modelId) { return modelId ? acquireModel(modelId) : nullptr; }
void streamerReleaseItemModel(Model* m) { if (m) releaseModel(m); }

int streamerCharInfo(uint32_t race, uint32_t sex, const uint8_t** blob, uint32_t* size) {
    CharInfo* c = acquireCharInfo(w3d_char_id(race, sex));
    if (!c) return -1;
    if (c->state != AssetState::Ready) return 0;
    if (!c->ok) return -1;
    *blob = c->blob;
    *size = c->size;
    return 1;
}

bool streamerReadEntry(uint32_t type, uint32_t id, void* dst, uint32_t size) {
    const W3dEntry* e = g_index ? g_index->find(type, id) : nullptr;
    if (!e || e->size < size) return false;
    FILE* f = fopen(g_index->files[g_index->fileIndex(e)][0] ? g_index->files[g_index->fileIndex(e)] : g_index->path, "rb");
    if (!f) return false;
    const bool ok = fseek(f, (long)e->offset, SEEK_SET) == 0 && fread(dst, 1, size, f) == size;
    fclose(f);
    return ok;
}

uint32_t streamerEntrySize(uint32_t type, uint32_t id) {
    const W3dEntry* e = g_index ? g_index->find(type, id) : nullptr;
    return e ? e->size : 0;
}

CreatureDisplay* streamerAcquirePlayerDisplay(const W3dLook& look, const uint32_t* equip) {
    uint32_t eq[W3D_BODY_EQ] = {};
    uint64_t key = w3d_look_key(&look);
    if (equip) {
        for (int k = 0; k < W3D_BODY_EQ; ++k) eq[k] = equip[W3D_BODY_EQ_SLOTS[k]];
        key = (key ^ w3d_equip_key(equip)) | (1ull << 63);
    }
    auto it = g_playerDisplays.find(key);
    if (it != g_playerDisplays.end()) { it->second->refs++; return it->second; }
    uint32_t cid = w3d_char_id(look.race, look.sex);
    if (!g_index->find(W3D_ENTRY_CHARACTER, cid)) return nullptr;
    CreatureDisplay* d = new CreatureDisplay;
    d->look = key;
    memcpy(d->eq, eq, sizeof eq);
    d->refs = 1;
    d->build = new PlayerBuild;
    d->build->look = look;
    d->build->chr = acquireCharInfo(cid);
    g_playerDisplays[key] = d;
    g_pendingPlayers.push_back(d);
    return d;
}

void streamerReleaseDisplay(CreatureDisplay* d) {
    if (!d || --d->refs > 0) return;
    if (d->look) {   // player: nothing of its own is in flight, its pieces are refcounted
        g_playerDisplays.erase(d->look);
        dropBuild(d);
        destroyDisplayNow(d);
        return;
    }
    g_displays.erase(d->id);
    if (d->state == AssetState::Loading) return;   // freed when its job completes
    destroyDisplayNow(d);
}

void streamerEndFrame() {
    ++g_frame;
    const u64 t0 = svcGetSystemTick();
    processDeferred();
    g_stats.deferredMsLastFrame = ticksToMs(svcGetSystemTick() - t0);
}

const std::vector<Tile*>& streamerReadyTiles() { return g_ready; }
const StreamStats& streamerStats() { return g_stats; }

// The blocks fit now: tried with real allocations (VRAM is only allocated on the main thread).
static bool vramBlocksFit(const uint32_t* blocks, int n) {
    void* got[4] = {};
    uint32_t sum = 0;
    for (int i = 0; i < n && i < 4; ++i) sum += blocks[i];
    if (!vramRoomFor(sum)) return false;   // 6.43: probing them must not take the last bytes of VRAM (vram_guard.hpp)
    bool ok = true;
    for (int i = 0; i < n && i < 4 && ok; ++i) ok = (got[i] = vramAlloc(blocks[i])) != nullptr;
    for (int i = 0; i < n && i < 4; ++i) if (got[i]) vramFree(got[i]);
    return ok;
}

uint32_t streamerEvictVram(const uint32_t* blocks, int n) {
#ifdef W3D_PC_TEST
    (void)blocks; (void)n;
    return 0;
#else
    std::vector<GpuTex*> cand;
    for (const auto& [id, t] : g_tex)
        if (t->vram && t->state == AssetState::Ready && t->bytes && !t->retry) cand.push_back(t);
    std::sort(cand.begin(), cand.end(), [](const GpuTex* a, const GpuTex* b) {   // the least wanted (farthest) first
        return readKey(a->anchor, a->cls) > readKey(b->anchor, b->cls);
    });
    // Stage 6.9: until the blocks themselves fit (a render target is two blocks - colour and depth - each in one piece;
    // 6.5 stopped at enough free bytes in all, and three tries in a row still found no 768 KB piece)
    uint32_t moved = 0;
    for (GpuTex* t : cand) {
        if (vramBlocksFit(blocks, n)) break;
        C3D_TexInitParams p{};
        p.width = t->tex.width;
        p.height = t->tex.height;
        p.maxLevel = t->tex.maxLevel;
        p.format = t->tex.fmt;
        p.type = GPU_TEX_2D;
        p.onVram = false;
        C3D_Tex lin;
        if (!C3D_TexInitWithParams(&lin, NULL, p)) break;   // no linear memory either
        const u32 total = texTotalSize(t->tex);
        // 6.28: the new block is cacheable linear memory that may still have dirty lines in the CPU's data cache (whatever
        // used it before). The GPU writes the copy straight into RAM; a dirty line written back later puts the old bytes
        // over it - textures turned to noise after switching to 3D / 800x240 (the photo on the character screen: the
        // world's textures shared with the scene). Write the lines back and drop them before the copy, drop again after.
        GSPGPU_FlushDataCache(lin.data, total);
        // outside a frame this waits for the GPU first (C3Di_SafeTextureCopy): the old copy is no longer read after it
        C3D_SyncTextureCopy((u32*)t->tex.data, 0, (u32*)lin.data, 0, total, 8);
        GSPGPU_InvalidateDataCache(lin.data, total);
        ++g_stats.texEvicted;
        lin.param = t->tex.param;   // filters, wrap and mip settings as they were
        lin.border = t->tex.border;
        lin.lodParam = t->tex.lodParam;
        C3D_TexDelete(&t->tex);
        t->tex = lin;
        t->vram = false;
        g_stats.texVram -= t->bytes;
        g_stats.texLinear += t->bytes;
        moved += t->bytes;
    }
    return moved;
#endif
}

double streamerTakeMaxReadMs() {
    const double m = g_ioMaxReadMs;
    g_ioMaxReadMs = 0;
    return m;
}

const char* streamerJobKindName(int k) {
    static const char* kN[kStreamJobKinds] = {"terr", "water", "plcm", "coll", "model", "tex", "disp", "crt", "chri", "chimg", "item", "liqd", "grnd"};
    return k >= 0 && k < kStreamJobKinds ? kN[k] : "?";
}
const char* streamerClassName(int c) {
    static const char* kN[kStreamClasses] = {"unit", "tile", "coll", "terrtex", "object", "bldtex", "doodtex", "bldmodel"};
    return c >= 0 && c < kStreamClasses ? kN[c] : "?";
}

// Perf log (stage 6): where the streamer's memory is, walked once a second on the main thread.
void streamerMemory(StreamMemory* o) {
    *o = StreamMemory();
    o->poolBytes = g_slotSize * (uint32_t)g_slotCount;
    o->slotSize = g_slotSize;
    o->poolUsedBytes = g_slotSize * (uint32_t)(g_slotCount - (int)g_freeSlots.size());
    for (const TileExt* t : g_tiles) {
        ++o->tiles;
        o->waterBytes += t->water ? t->waterSize : 0;
        if (t->th) o->heightBytes += (uint32_t)(t->height.capacity() * sizeof(float));   // (built by the IO thread before th is set)
        o->instances += (uint32_t)t->instances.size();
        o->instanceBytes += (uint32_t)(t->instances.capacity() * sizeof(Instance) + t->models.capacity() * sizeof(Model*) +
                                       t->chunks.capacity() * sizeof(Tile::Chunk));
        o->liquidBytes += t->liq ? t->liqSize : 0;
        o->groundBytes += t->grnd ? t->grndSize : 0;
        if (t->coll) (t->collLinear ? o->collLinearBytes : o->collHeapBytes) += t->collSize;
        if (t->collLoad) o->collLoadingBytes += t->collLoad->size;
        if (t->doomed) ++o->tilesDoomed;
        if (t->complete) ++o->tilesComplete;
    }
    for (const auto& [id, m] : g_models) {
        ++o->models;
        if (m->state != AssetState::Ready) { ++o->modelsLoading; o->modelLoadingBytes += m->size; continue; }
        if (!m->h) { ++o->modelsFailed; continue; }
        if (m->building) { ++o->buildings; o->buildingBytes += m->size; if (m->interior) ++o->interiors; }
        else if (m->cls == kClsUnit) { ++o->unitModels; o->unitModelBytes += m->size; }   // worn items, spell effects, login scenes
        else { ++o->doodads; o->doodadBytes += m->size; }
        if (m->fx) ++o->fxModels;
        if (m->drawable) ++o->modelsDrawable;
    }
    for (const auto& [id, m] : g_cmodels) {
        ++o->creatureModels;
        o->creatureBytes += m->size;
    }
    for (const auto& [id, t] : g_tex) {
        ++o->textures;
        const int c = std::min<int>(t->cls, kStreamClasses - 1);
        if (t->state == AssetState::Ready && t->bytes) {
            if (t->vram) { o->texVramByClass[c] += t->bytes; ++o->texVramCount; }
            else { o->texLinearByClass[c] += t->bytes; ++o->texLinearCount; }
            o->texCountByClass[c]++;
        }
        if (t->staging) {
            if (t->state == AssetState::Loading) o->stagingReadingBytes += t->stagingSize;
            else if (t->retry) o->stagingRetryBytes += t->stagingSize;
            else o->stagingUploadBytes += t->stagingSize;
        }
        if (t->retry) ++o->texWhiteNow;
    }
    for (const auto& [k, d] : g_playerDisplays)   // composited bodies are owned by their display (not in g_tex)
        if (d->tex[3] && d->tex[3] != &g_white && d->tex[3]->bytes) {
            ++o->bodyTextures;
            (d->tex[3]->vram ? o->bodyVramBytes : o->bodyLinearBytes) += d->tex[3]->bytes;
        }
    o->displays = (uint32_t)g_displays.size();
    o->playerDisplays = (uint32_t)g_playerDisplays.size();
    o->pendingPlayers = (uint32_t)g_pendingPlayers.size();
    for (const auto& [id, c] : g_charImgs) { ++o->charImgs; o->charImgBytes += c->size; }
    for (const auto& [id, c] : g_chars) o->charInfoBytes += c->size;
    o->items = (uint32_t)g_items.size();
    o->deferred = (uint32_t)g_deferred.size();
    o->indexBytes = g_index ? (uint32_t)(g_index->entries.capacity() * sizeof(W3dEntry) + g_index->fileOf.capacity()) : 0;
    o->indexEntries = g_index ? (uint32_t)g_index->entries.size() : 0;
}
GpuTex* streamerWhiteTexture() { return &g_white; }

static Tile* tileAt(float wx, float wy) {
    int tx = (int)floorf(32.0f - wy / kTileSize), ty = (int)floorf(32.0f - wx / kTileSize);
    for (TileExt* t : g_tiles)
        if (!t->doomed && t->tx == tx && t->ty == ty && t->th && t->pendingJobs == 0) return t;
    return nullptr;
}

bool streamerCollisionReadyAt(float wx, float wy) {
    Tile* t = tileAt(wx, wy);
    if (!t || t->state != TileState::Ready) return false;
    return t->coll || !g_index->find(W3D_ENTRY_COLLISION, w3d_tile_id(t->tx, t->ty));
}

bool streamerTileReadyAt(float wx, float wy) {
    Tile* t = tileAt(wx, wy);
    return t && t->state == TileState::Ready;
}

int streamerFloorsAt(float wx, float wy, int max, float* zs, uint16_t* flags) {   // 6.19b diagnostics
    Tile* t = tileAt(wx, wy);
    return t && t->coll ? w3d_coll_floors_at(t->coll, wx, wy, max, zs, flags) : 0;
}

bool streamerFloorHeight(float wx, float wy, float zFeet, float* out, bool* onFloor, bool* held) {
    float terrain = 0;
    bool hole = false;
    bool haveT = streamerGroundHeightEx(wx, wy, &terrain, &hole);
    // 6.17: in an ADT hole the "terrain" is a membrane filled in over the building's opening (Ironforge's gate): never a floor
    // above the feet - walking in through the gate climbed it step by step and carried the character up the mountain.
    Tile* t = tileAt(wx, wy);
    const uint8_t* coll = t ? t->coll : nullptr;
    // 6.21: nor where a building's floor lies under the membrane: the opening is open, the walker goes down onto that floor
    if (haveT && hole && !w3d_hole_terrain_usable(coll, wx, wy, zFeet, kStepHeight, terrain)) haveT = false;
    if (held) *held = false;
    if (!haveT && !coll) return false;
    int f = 0;
    *out = w3d_walk_height(coll, wx, wy, zFeet, kStepHeight, haveT ? 1 : 0, terrain, &f);
    if (!haveT && f == 0) return false;   // (no terrain and no floor or ceiling of a building here: nothing at all)
    // 6.16: below every surface we know (w3d_walk_height then gives the lowest one ABOVE the feet): only a little sinking is
    // corrected that way. Deeper than that we are in a place whose floor is not there (a city inside a mountain - Ironforge,
    // the tram exits - while its collision is still loading, or missing): lifting the character onto the mountain's surface
    // put it high up in the sky. It stays at its height instead (where the server put it) until a floor turns up: while the
    // tile's collision is not loaded yet, or when the surface above is a building's (a ceiling) more than 6 yards up.
    // (Only for those cases: under the open world's ground with nothing of a building around - fallen through - the old lift
    // onto the terrain is the right way back; so is a short lift onto a floor just above, sunk through it.)
    const bool collPending = !coll && t && g_index->find(W3D_ENTRY_COLLISION, w3d_tile_id(t->tx, t->ty));
    if (*out > zFeet + kStepHeight + 1.0f && (collPending || (f != 0 && *out > zFeet + 6.0f))) {
        *out = zFeet;
        f = 1;
        if (held) *held = true;
        ++g_stats.floorHolds;
    }
    if (onFloor) *onFloor = f != 0;
    return true;
}

bool streamerCollideMove(float fromX, float fromY, float zFeet, float* toX, float* toY) {
    // Walls are clipped per tile: take every ready tile under the move's box (at most 4
    // unless the move is longer than a tile, which only a teleport does).
    const float r = kWalkerRadius;
    float x0 = std::min(fromX, *toX) - r, x1 = std::max(fromX, *toX) + r;
    float y0 = std::min(fromY, *toY) - r, y1 = std::max(fromY, *toY) + r;
    const uint8_t* colls[4];
    int n = 0;
    const float xs[2] = {x0, x1}, ys[2] = {y0, y1};
    for (float px : xs)
        for (float py : ys) {
            Tile* t = tileAt(px, py);
            if (!t || !t->coll) continue;
            bool dup = false;
            for (int i = 0; i < n; ++i) dup |= colls[i] == t->coll;
            if (!dup) colls[n++] = t->coll;
        }
    if (!n) return false;
    return w3d_collide_move(colls, n, fromX, fromY, zFeet, toX, toY, r) != 0;
}

bool streamerCeilingAbove(float wx, float wy, float zFrom, float* ceil) {
    Tile* t = tileAt(wx, wy);
    return t && t->coll && w3d_ceiling_above(t->coll, wx, wy, zFrom, ceil) != 0;
}

bool streamerLiquidAt(float wx, float wy, float zRef, LiquidHere* out) {
    Tile* t = tileAt(wx, wy);
    if (!t || !t->liq) return false;
    W3dLiquidHit hit;
    if (!w3d_liquid_at(t->liq, wx, wy, &hit)) return false;
    if (!(hit.flags & W3D_LIQ_WMO)) {   // the terrain's lake does not reach into a cave under it
        float ground;
        if (streamerGroundHeight(wx, wy, &ground) && zRef < ground - 2.0f) return false;
    }
    out->level = hit.level;
    out->flags = hit.flags;
    const W3dLiquidHeader* h = (const W3dLiquidHeader*)t->liq;
    out->look = h->look[hit.flags & W3D_LIQ_KIND];
    return true;
}

// 6.14: the terrain of an instance map is a flat stand-in quad 300 yards below the building (the converter's emitDummyTerrain):
// landing on it means the character fell out of the building.
bool streamerGroundIsStandIn(float wx, float wy) {
    Tile* t = tileAt(wx, wy);
    return t && t->th && t->th->vertexCount <= 4;
}

bool streamerGroundHeight(float wx, float wy, float* out) { return streamerGroundHeightEx(wx, wy, out, nullptr); }

// 6.18c: a move over the terrain that goes uphill onto ground steeper than the walkable slope loses its uphill part.
bool streamerSlopeMove(float fromX, float fromY, float zFeet, float* toX, float* toY) {
    Tile* tb = tileAt(*toX, *toY);
    if (!tb || !tb->grnd) return false;
    Tile* ta = tileAt(fromX, fromY);
    return w3d_slope_move(ta ? ta->grnd : nullptr, tb->grnd, fromX, fromY, zFeet, kStepHeight, toX, toY) != 0;
}

bool streamerGroundHeightEx(float wx, float wy, float* out, bool* inHole) {
    Tile* t = tileAt(wx, wy);
    if (inHole) *inHole = false;
    if (t && t->grnd) {   // 6.18a: the ADT's own triangles
        W3dGroundHit hit;
        if (w3d_ground_at(t->grnd, wx, wy, &hit)) {
            *out = hit.z;
            if (inHole) *inHole = hit.hole != 0;
            return true;
        }
    }
    if (!t || t->height.empty()) return false;
    float fx = (wx - t->th->origin[0]) / kTileSize * (kHeightN - 1);
    float fy = (wy - t->th->origin[1]) / kTileSize * (kHeightN - 1);
    fx = std::clamp(fx, 0.0f, (float)(kHeightN - 1));
    fy = std::clamp(fy, 0.0f, (float)(kHeightN - 1));
    int x0 = std::min((int)fx, kHeightN - 2), y0 = std::min((int)fy, kHeightN - 2);
    float ax = fx - x0, ay = fy - y0;
    const float* H = t->height.data();
    float h00 = H[y0 * kHeightN + x0], h10 = H[y0 * kHeightN + x0 + 1];
    float h01 = H[(y0 + 1) * kHeightN + x0], h11 = H[(y0 + 1) * kHeightN + x0 + 1];
    float h = (h00 * (1 - ax) + h10 * ax) * (1 - ay) + (h01 * (1 - ax) + h11 * ax) * ay;
    *out = h + t->th->origin[2];
    if (inHole && t->covered.size() == (size_t)kHeightN * kHeightN) {
        const uint8_t* C = t->covered.data();
        *inHole = !(C[y0 * kHeightN + x0] && C[y0 * kHeightN + x0 + 1] && C[(y0 + 1) * kHeightN + x0] && C[(y0 + 1) * kHeightN + x0 + 1]);
    }
    return true;
}
