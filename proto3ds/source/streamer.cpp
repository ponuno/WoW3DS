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
#include "w3d_collision.h"
#include "w3d_liquid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <unordered_map>

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

bool Model::texturesReady() const {
    for (GpuTex* t : tex)
        if (t->state != AssetState::Ready) return false;
    return true;
}

// ================================================================== internals
namespace {

enum class JobKind : uint8_t { Terrain, Water, Placement, Collision, Model, Texture, Display, Creature, CharInfo, CharImg, Item, Liquid };

struct Job {
    JobKind kind;
    uint8_t file;                    // PackIndex::files (0: the map pack, 1..: the pool parts)
    uint32_t offset, size;
    uint8_t* dst;
    void* owner;
    bool ok = false;
};

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

void freeColl(uint8_t* p, bool linear) {
    if (linear) linearFree(p);
    else free(p);
}

PackIndex* g_index = nullptr;
FILE* g_ioFiles[PackIndex::kMaxFiles] = {};   // one handle per pack file, used by the IO thread only

Thread g_thread = nullptr;
std::atomic<bool> g_quit{false};
LightLock g_lock;
LightEvent g_wake;
std::deque<Job*> g_requests, g_done;   // guarded by g_lock

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
}

void runJob(Job* j) {
    FILE* f = j->file < PackIndex::kMaxFiles ? g_ioFiles[j->file] : nullptr;
    j->ok = f && fseek(f, j->offset, SEEK_SET) == 0 && fread(j->dst, 1, j->size, f) == j->size;
    if (j->ok && j->kind != JobKind::Placement && j->kind != JobKind::Collision && j->kind != JobKind::Display &&
        j->kind != JobKind::CharInfo && j->kind != JobKind::CharImg && j->kind != JobKind::Item &&
        j->kind != JobKind::Liquid) {   // CPU-only data needs no flush
        // Write the data back from THIS core's L1 cache: ARM11 MPCore cache
        // maintenance is per-core, and the GPU/GX engine read memory directly,
        // so a flush issued later on the main core may not see these lines.
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)j->dst, j->size);
    }
    if (j->ok && j->kind == JobKind::Terrain) buildHeightField((Tile*)j->owner);
}

void workerMain(void*) {
    while (!g_quit) {
        LightEvent_Wait(&g_wake);
        for (;;) {
            LightLock_Lock(&g_lock);
            Job* j = nullptr;
            if (!g_requests.empty()) { j = g_requests.front(); g_requests.pop_front(); }
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
    LightLock_Lock(&g_lock);
    g_requests.push_back(j);
    LightLock_Unlock(&g_lock);
    if (g_thread) LightEvent_Signal(&g_wake);
}

void deferFree(FreeKind k, void* p, int slot = -1) { g_deferred.push_back({k, p, slot, g_frame}); }

// ------------------------------------------------------------------ assets
GpuTex* acquireTex(uint32_t id) {
    auto it = g_tex.find(id);
    if (it != g_tex.end()) { it->second->refs++; return it->second; }
    const W3dEntry* e = g_index->find(W3D_ENTRY_TEXTURE, id);
    if (!e) { ++g_stats.texNotInPack; return &g_white; }
    GpuTex* t = new GpuTex;
    t->id = id;
    t->refs = 1;
    t->stagingSize = e->size;
    g_tex[id] = t;
    t->staging = (uint8_t*)linearAlloc(e->size);
    if (!t->staging) {   // no linear memory now: white until a retry finds some (never white for good)
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
    if (it != g_models.end()) { it->second->refs++; return it->second; }
    const W3dEntry* e = g_index->find(W3D_ENTRY_MODEL, id);
    if (!e) return nullptr;
    Model* m = new Model;
    m->id = id;
    m->refs = 1;
    m->size = e->size;
    m->blob = (uint8_t*)linearAlloc(e->size);
    if (!m->blob) { delete m; return nullptr; }
    g_models[id] = m;
    submit(JobKind::Model, e, m->blob, m);
    return m;
}

void destroyModelNow(Model* m) {
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
    if (!m->blob) { delete m; return nullptr; }
    g_cmodels[id] = m;
    submit(JobKind::Creature, e, m->blob, m);
    return m;
}

void destroyCreatureModelNow(CreatureModel* m) {
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
    if (!staging) return &g_white;
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
    p.onVram = true;
    t->vram = C3D_TexInitWithParams(&t->tex, NULL, p);
    bool ok = t->vram;
    if (!ok) { p.onVram = false; ok = C3D_TexInitWithParams(&t->tex, NULL, p); }
    if (ok) {
        C3D_TexLoadImage(&t->tex, staging, GPU_TEXFACE_2D, 0);
        C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_NEAREST);
        C3D_TexSetWrap(&t->tex, GPU_REPEAT, GPU_REPEAT);
        t->bytes = A * A * 2;
        (t->vram ? g_stats.texVram : g_stats.texLinear) += t->bytes;
    } else {
        t->tex = g_white.tex;
        t->bytes = 0;
    }
    linearFree(staging);
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
    submit(JobKind::Terrain, te, t->terr, t);
    t->pendingJobs++;
    if (const W3dEntry* we = g_index->find(W3D_ENTRY_WATER, id)) {
        t->water = (uint8_t*)linearAlloc(we->size);
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

void onJobDone(Job* j) {
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
            const uint32_t* ids = (const uint32_t*)(m->blob + m->h->offTexIds);
            for (u32 i = 0; i < m->h->texCount; ++i) m->tex.push_back(acquireTex(ids[i]));
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
            m->h = (const W3dCreatureHeader*)m->blob;
            const W3dCreatureTex* ct = (const W3dCreatureTex*)(m->blob + m->h->offTex);
            for (u32 i = 0; i < m->h->texCount; ++i)
                m->slotTex.push_back(ct[i].kind == W3D_CTEX_FIXED && ct[i].texId ? acquireTex(ct[i].texId) : nullptr);
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
        case JobKind::Placement: {
            TileExt* t = (TileExt*)j->owner;
            t->pendingJobs--;
            if (!t->doomed && j->ok) {
                if (j->kind == JobKind::Terrain) {
                    GSPGPU_FlushDataCache(t->terr, j->size);
                    t->th = (const W3dTerrainHeader*)t->terr;
                    t->batches = (const W3dTerrainBatch*)(t->terr + t->th->offBatches);
                    const uint32_t* ids = (const uint32_t*)(t->terr + t->th->offTexIds);
                    for (u32 i = 0; i < t->th->texCount; ++i) t->ttex.push_back(acquireTex(ids[i]));
                } else if (j->kind == JobKind::Liquid) {
                    if (!w3d_liquid_valid(t->liq, t->liqSize)) { free(t->liq); t->liq = nullptr; }
                } else if (j->kind == JobKind::Water) {
                    GSPGPU_FlushDataCache(t->water, j->size);
                    t->wh = (const W3dWaterHeader*)t->water;
                    t->wbatches = (const W3dWaterBatch*)(t->water + t->wh->offBatches);
                    const uint32_t* ids = (const uint32_t*)(t->water + t->wh->offTexIds);
                    for (u32 i = 0; i < t->wh->texCount; ++i) t->wtex.push_back(acquireTex(ids[i]));
                } else if (j->kind == JobKind::Placement) {
                    W3dPlacementHeader ph;
                    memcpy(&ph, t->plcm, sizeof ph);
                    const W3dPlacement* p = (const W3dPlacement*)(t->plcm + sizeof ph);
                    t->instances.reserve(ph.count);
                    std::unordered_map<uint32_t, Model*> local;
                    for (u32 i = 0; i < ph.count; ++i) {
                        Model*& m = local[p[i].modelId];
                        if (!m) {
                            m = acquireModel(p[i].modelId);
                            if (m) t->models.push_back(m);
                        }
                        if (!m) continue;
                        Instance in;
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
                }
            }
            if (j->kind == JobKind::Terrain && !j->ok) t->th = nullptr;
            if (j->kind == JobKind::Liquid && !j->ok && t->liq) { free(t->liq); t->liq = nullptr; }
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
    p.onVram = true;
    C3D_Tex tex;
    bool vram = C3D_TexInitWithParams(&tex, NULL, p);
    bool ok = vram;
    if (!ok) { p.onVram = false; ok = C3D_TexInitWithParams(&tex, NULL, p); }
    if (!ok) return false;
    t->tex = tex;
    t->vram = vram;
    const uint8_t* data = t->staging + W3D_TEX_DATA_OFFSET;
    u32 w = th.width, h = th.height, off = 0;
    for (int l = 0; l < th.mipCount; ++l) {
        C3D_TexLoadImage(&t->tex, data + off, GPU_TEXFACE_2D, l);
        off += w3d_tex_level_size(w, h, th.format);
        w >>= 1;
        h >>= 1;
    }
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
            linearFree(t->staging);
            t->staging = nullptr;
            t->retry = false;
            g_retry.erase(g_retry.begin() + i);
            continue;
        }
        ++i;
    }
}

void uploadTextures(double budgetMs) {
    u64 t0 = svcGetSystemTick();
    retryTextures();
    while (!g_uploads.empty() && ticksToMs(svcGetSystemTick() - t0) < budgetMs) {
        GpuTex* t = g_uploads.front();
        g_uploads.pop_front();
        if (tryUpload(t)) {
            linearFree(t->staging);
            t->staging = nullptr;
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
bool streamerInit(PackIndex* index, char* err, size_t errLen) {
    g_index = index;
    g_quit = false;   // a map change shuts the streamer down and starts it again on the next pack
    g_freeSlots.clear();
    g_pool = nullptr;
    for (int k = 0; k < index->fileCount; ++k) {
        g_ioFiles[k] = fopen(index->files[k], "rb");
        if (!g_ioFiles[k]) { snprintf(err, errLen, "cannot reopen %s", index->files[k]); return false; }
        setvbuf(g_ioFiles[k], NULL, _IOFBF, k ? 32 * 1024 : 64 * 1024);
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
    return true;
}

void streamerShutdown() {
    if (g_thread) {
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    // Complete every outstanding job (unrun ones as failed) through the normal path,
    // so assets orphaned while loading are freed exactly as during play.
    // Completions may submit follow-up jobs (display -> model -> textures): loop until drained.
    for (;;) {
        for (Job* j : g_requests) j->ok = false;
        std::deque<Job*> left;
        left.swap(g_done);
        left.insert(left.end(), g_requests.begin(), g_requests.end());
        g_requests.clear();
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
    for (FILE*& f : g_ioFiles) {
        if (f) fclose(f);
        f = nullptr;
    }
    g_index = nullptr;
    C3D_TexDelete(&g_white.tex);
}

bool streamerHasDisplay(uint32_t displayId) { return g_index && g_index->find(W3D_ENTRY_DISPLAY, displayId); }

void streamerUpdate(const float camPos[3], float drawDist) {
    // 1. Collect finished IO.
    if (!g_thread) {   // synchronous fallback: a few jobs per frame
        u64 t0 = svcGetSystemTick();
        while (ticksToMs(svcGetSystemTick() - t0) < 4.0) {
            Job* j = nullptr;
            if (!g_requests.empty()) { j = g_requests.front(); g_requests.pop_front(); }
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
    }

    pumpPlayers();

    // 2. Texture uploads (GX copies) under a frame budget.
    u64 u0 = svcGetSystemTick();
    uploadTextures(2.5);
    g_stats.uploadMsLastFrame = ticksToMs(svcGetSystemTick() - u0);

    // 3. Promote tiles whose dependencies are resident.
    for (TileExt* t : g_tiles) {
        if (t->doomed || t->state != TileState::WaitingDeps) continue;
        if (!t->th) continue;   // terrain read failed: stays invisible
        if (!tileDepsReady(t)) continue;
        for (Instance& in : t->instances)
            in.radius = in.model->h ? in.model->h->radius * in.scale : 0;
        t->state = TileState::Ready;
        double ms = ticksToMs(svcGetSystemTick() - t->requestTick);
        g_stats.tileLoads++;
        g_stats.lastTileMs = ms;
        g_stats.maxTileMs = std::max(g_stats.maxTileMs, ms);
        g_stats.sumTileMs += ms;
    }

    // 3b. Collision near the camera only (load within kCollLoadDist of a tile, drop beyond kCollDropDist).
    for (TileExt* t : g_tiles) {
        if (t->doomed || !t->th) continue;
        float x0 = (32.0f - t->ty - 1.0f) * kTileSize, y0 = (32.0f - t->tx - 1.0f) * kTileSize;
        float dx = std::max({x0 - camPos[0], 0.0f, camPos[0] - (x0 + kTileSize)});
        float dy = std::max({y0 - camPos[1], 0.0f, camPos[1] - (y0 + kTileSize)});
        float d = sqrtf(dx * dx + dy * dy);
        if (d <= kCollLoadDist && !t->coll && !t->collLoad) {
            const W3dEntry* ce = g_index->find(W3D_ENTRY_COLLISION, w3d_tile_id(t->tx, t->ty));
            if (!ce) continue;
            bool linear = false;
            uint8_t* buf = (uint8_t*)malloc(ce->size);
            if (!buf) { buf = (uint8_t*)linearAlloc(ce->size); linear = true; }
            if (!buf) { ++g_stats.collFailed; continue; }
            t->collLoad = new CollLoad{t, buf, ce->size, linear};
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
    const int keep = r + 1;

    for (TileExt* t : std::vector<TileExt*>(g_tiles)) {
        if (t->doomed) continue;
        if (abs(t->tx - ctx) > keep || abs(t->ty - cty) > keep) releaseTile(t);
    }

    struct Want { int tx, ty; float d2; };
    std::vector<Want> want;
    for (int dx = -r; dx <= r; ++dx)
        for (int dy = -r; dy <= r; ++dy) {
            int tx = ctx + dx, ty = cty + dy;
            if (!g_index->hasTile(tx, ty) || findTile(tx, ty)) continue;
            float cxw = (32.0f - ty - 0.5f) * kTileSize, cyw = (32.0f - tx - 0.5f) * kTileSize;
            float ddx = cxw - camPos[0], ddy = cyw - camPos[1];
            want.push_back({tx, ty, ddx * ddx + ddy * ddy});
        }
    std::sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.d2 < b.d2; });

    int loading = 0;
    for (TileExt* t : g_tiles) if (!t->doomed && t->state != TileState::Ready) ++loading;
    for (const Want& w : want) {
        if (loading >= 2) break;   // keep latency low for the nearest tiles
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

    // 5. Ready list + stats.
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
    LightLock_Unlock(&g_lock);
    g_stats.models = (int)g_models.size();
    g_stats.textures = (int)g_tex.size();
    g_stats.uploadsPending = (int)g_uploads.size();
    g_stats.displays = (int)g_displays.size();
    g_stats.players = (int)g_playerDisplays.size();
    g_stats.creatureModels = (int)g_cmodels.size();
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
    processDeferred();
}

const std::vector<Tile*>& streamerReadyTiles() { return g_ready; }
const StreamStats& streamerStats() { return g_stats; }
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

bool streamerFloorHeight(float wx, float wy, float zFeet, float* out, bool* onFloor) {
    float terrain = 0;
    bool haveT = streamerGroundHeight(wx, wy, &terrain);
    Tile* t = tileAt(wx, wy);
    const uint8_t* coll = t ? t->coll : nullptr;
    if (!haveT && !coll) return false;
    int f = 0;
    *out = w3d_walk_height(coll, wx, wy, zFeet, kStepHeight, haveT ? 1 : 0, terrain, &f);
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

bool streamerGroundHeight(float wx, float wy, float* out) {
    Tile* t = tileAt(wx, wy);
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
    return true;
}
