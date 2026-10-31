// world.h — shared types of the stage-3 client: pack index, streamed tiles,
// refcounted GPU assets, renderer entry points.
#pragma once
#include <3ds.h>
#include <citro3d.h>

#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <vector>

#include "w3d_format.h"
#include "wmo_portals.hpp"
#include "w3d_terrain_lod.h"
#include "w3d_character.h"
#include "w3d_fx.h"
#include "hud.hpp"

static constexpr float kTileSize = 533.33333f;
// 6.34c: a whole tile farther than d from the camera (x / y, its square's nearest point): with the horizon on, such a loaded
// tile is drawn with its far ground mesh (one draw) instead of its detailed batches, and its water is left out
inline bool tileBeyond(int tx, int ty, const float cam[3], float d) {
    const float x0 = (32.0f - ty - 1.0f) * kTileSize, y0 = (32.0f - tx - 1.0f) * kTileSize;
    const float dx = cam[0] < x0 ? x0 - cam[0] : cam[0] > x0 + kTileSize ? cam[0] - x0 - kTileSize : 0.0f;
    const float dy = cam[1] < y0 ? y0 - cam[1] : cam[1] > y0 + kTileSize ? cam[1] - y0 - kTileSize : 0.0f;
    return dx * dx + dy * dy > d * d;
}
static constexpr int kHeightN = 129;  // height samples per tile edge (4.17 yd spacing)

static inline double ticksToMs(u64 t) { return (double)t * 1000.0 / (double)SYSCLOCK_ARM11; }

// ------------------------------------------------------------------ pack index
struct PackIndex {
    FILE* file = nullptr;            // main-thread handle (startup only)
    char path[128] = {0};            // the pack opened (a map pack, or a standalone pack)
    std::vector<W3dEntry> entries;   // sorted by (type, id)
    uint32_t maxTerrainSize = 0;
    int minTileX = 99, minTileY = 99, maxTileX = -1, maxTileY = -1;
    // Stage 5.27: a map pack of 'w3dconv --all' takes its assets from the pool next to it (common.w3dp, common1.w3dp
    // ...): the entries of every file in one index, fileOf[i] = the file of entries[i] (files[0] = the map pack).
    static constexpr int kMaxFiles = 9;
    char files[kMaxFiles][128] = {};
    int fileCount = 0;
    std::vector<uint8_t> fileOf;
    uint32_t buildId = 0;            // 0: a standalone pack
    int poolParts = 0;

    bool open(const char* path, char* err, size_t errLen);
    const W3dEntry* find(uint32_t type, uint32_t id) const;
    int fileIndex(const W3dEntry* e) const {
        return (e >= entries.data() && e < entries.data() + entries.size()) ? fileOf[(size_t)(e - entries.data())] : 0;
    }
    bool hasTile(int tx, int ty) const { return find(W3D_ENTRY_TERRAIN, w3d_tile_id(tx, ty)) != nullptr; }
};

// ------------------------------------------------------------------ assets
enum class AssetState : uint8_t { Loading, Uploading, Ready };

struct GpuTex {
    uint32_t id = 0;
    int refs = 0;
    AssetState state = AssetState::Loading;
    C3D_Tex tex;
    bool vram = false;
    uint32_t bytes = 0;
    uint8_t* staging = nullptr;      // linear buffer filled by the worker
    uint32_t stagingSize = 0;
    bool retry = false;              // out of memory: drawn white meanwhile, streamer tries again (g_retry)
    // stage 5.30: where it is needed (the nearest user known) and what for: the order of reads and uploads
    float anchor[3] = {0, 0, 0};
    uint8_t cls = 0;
};

struct Model {
    uint32_t id = 0;
    int refs = 0;
    AssetState state = AssetState::Loading;
    uint8_t* blob = nullptr;         // linear memory, GPU reads it directly
    uint32_t size = 0;
    const W3dModelHeader* h = nullptr;
    const W3dSubmesh* subs = nullptr;
    std::vector<GpuTex*> tex;
    bool building = false;           // WMO group (W3D_SUBF_BUILDING)
    bool drawable = false;           // stage 5.30: the blob and every texture are in: drawn on its own, not with its tile
    float anchor[3] = {0, 0, 0};     // the nearest placement known (read order of the model and its textures)
    uint8_t cls = 0;
    bool interior = false;           // interior-only WMO group (W3D_SUBF_INTERIOR)
    // stage 5.32: particle emitters and glow sprites (common/w3d_fx.h) at the end of the blob, or null; their textures
    // are tex[fxTexBase ...] (acquired with the model's own, so drawable covers them too)
    const W3dFxHeader* fx = nullptr;
    bool meshHidden = false;         // 6.17: the mesh is an emitter volume (w3d_emitter_volume): only its particles are shown
    uint16_t fxTexBase = 0;
    W3dFxAnimView fxAnim = {0, 0, 0, 0, 0, 0};   // stage 5.35: baked bone animation of an effect model (a == null: none)
    // per-frame visibility bucket (filled by the renderer)
    std::vector<const struct Instance*> visible;
    uint32_t mvBase = 0;             // stage 6.10: where its visible instances' matrices start (renderer, per frame)
    const W3dPortalTable* portals = nullptr;   // 6.42: the portals of a WMO group (its doors to the neighbouring groups), null: none in this pack
    const W3dUvAnimTable* uva = nullptr;   // 6.41: moving textures of its submeshes (M2 texture transforms), null: none
    const W3dSubLod* lods = nullptr; // stage 6.12: lighter versions of the submeshes (null: none in this pack)
    uint32_t drawableMs = 0;         // stage 6.11: when it became drawable (kFadeMs of fading in start there), streamerNowMs() clock
    uint32_t fadeCount = 0;          // stage 6.11: its visible instances that are fading this frame (renderer, per frame)
    bool texturesReady() const;
};

struct Instance {
    Model* model;
    float pos[3];
    float rot[3];
    float scale;
    float radius;
    float lightModel[3];
    uint32_t animMs = 0;             // stage 5.35: age of an animated effect (Model::fxAnim)
    uint8_t lod = 0;                 // stage 6.12: the level drawn this frame (renderer; 0 = full)
    int32_t sgroup = -1;             // 6.44: index in Tile::sgroups (a small static doodad merged with its neighbours), -1: drawn alone
    int32_t pnode = -1;              // 6.42: index in Tile::pnodes (a building group of a tile with portals), -1: none
    uint16_t bldg = 0;               // stage 6.11: 1 + index in Tile::bldgs of the building (WMO groups sharing one transform), 0 = none
    uint16_t owner = 0;              // 6.33: 1 + the building whose doodad (tree, lamp, bench of the WMO) it is: shown only with it
    float fade = 1.0f;               // stage 6.11: opacity this frame (the renderer sets it while the instance appears / at the edge)
};

// Creature (stage 5.3): a CDSP display references a CMDL skinned model plus per-display
// textures. Both are refcounted; the main loop acquires a display per visible unit.
struct CreatureModel {
    uint32_t id = 0;
    int refs = 0;
    AssetState state = AssetState::Loading;
    uint8_t* blob = nullptr;         // linear memory (vertices/indices read by the GPU)
    uint32_t size = 0;
    const W3dCreatureHeader* h = nullptr;   // null = load failed
    std::vector<GpuTex*> slotTex;    // per W3dCreatureTex slot: FIXED textures, else nullptr
    const W3dSubLod* lods = nullptr; // stage 6.12: lighter versions of the submeshes (null: none in this pack)
    // stage 6.13: particle emitters behind the blob (game objects' braziers, forges ...; common/w3d_fx.h), their textures
    const W3dFxHeader* fx = nullptr;
    bool meshHidden = false;         // 6.17: the mesh is an emitter volume (w3d_emitter_volume): only its particles are shown
    std::vector<GpuTex*> fxTex;
    bool fxReady() const;            // every fx texture is in
};

struct PlayerBuild;
struct CreatureDisplay {
    uint32_t id = 0;
    uint64_t look = 0;               // player (stage 5.5): key of look + body equipment, 0 = a CDSP display
    uint32_t eq[W3D_BODY_EQ] = {};   // player: ItemDisplayInfo ids of the body-relevant slots (W3D_BODY_EQ_SLOTS)
    PlayerBuild* build = nullptr;    // player: layers still loading / body texture not composited yet
    int refs = 0;
    AssetState state = AssetState::Loading;
    W3dDisplay d;
    CreatureModel* model = nullptr;
    GpuTex* tex[6] = {};             // SKIN1..3, BODY, HAIR, CAPE (W3dCreatureTexKind - 1)
    bool failed = false;
    bool ready() const;              // model + every texture resident
};

// What the renderer needs for one animated creature this frame.
struct CreatureInstance {
    const CreatureDisplay* disp;
    uint64_t guid = 0;               // stage 5.33: the unit (0 for a mount): the renderer records its hands / chest / head
    float pos[3];                    // feet, render space
    float yaw;
    float scale;                     // object scale (display scale is applied by the renderer)
    int seq;                         // sequence index (-1 = bind pose)
    float timeMs;
    bool loop;
    bool tilted = false;             // stage 5.41: an object standing at an angle: 'rot' replaces the turn by yaw
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // model -> render rotation, row-major
    // Worn item models (stage 5.19): static models hung on bones of the unit (helmet, shoulders, weapons).
    struct Worn {
        Model* model;
        int16_t bone;                // bone of the unit's model carrying the attachment
        float pos[3];                // attachment point, bind-pose model space
    };
    static constexpr int kMaxWorn = 7;
    Worn worn[kMaxWorn];
    int wornCount = 0;
    float fade = 1.0f;               // stage 6.12: opacity (appearing, or at the far edge); < 1 is drawn alpha-blended
};
// Stage 6.12: units farther than this are posed in step with the others of their model and animation (one pose per model and
// sequence a frame, 10 poses a second) and drawn with the lighter meshes; nearer ones are animated each on its own.
constexpr float kCreatureFullAnimRange = 110.0f;
// The level of a model's mesh at distance d for a bounding radius r: by the size on the screen.
static inline uint8_t w3d_lod_for(float d2, float r) {
    const float r1 = r * 12.0f, r2 = r * 30.0f;   // (smaller on the screen than at first planned: the switch shows less)
    return d2 > r2 * r2 ? 2 : d2 > r1 * r1 ? 1 : 0;
}

enum class TileState : uint8_t { Loading, WaitingDeps, Ready };

struct Tile {
    int tx = 0, ty = 0;
    TileState state = TileState::Loading;
    int pendingJobs = 0;
    u64 requestTick = 0;
    // terrain (lives in a fixed-size pool slot)
    int slot = -1;
    uint8_t* terr = nullptr;
    const W3dTerrainHeader* th = nullptr;
    const W3dTerrainBatch* batches = nullptr;
    std::vector<GpuTex*> ttex;
    std::vector<float> height;       // kHeightN^2, tile-local z (+ th->origin[2])
    // 6.17: 1 where a sample point lies on drawn ground, 0 inside an ADT hole (a building's door into the mountain: Ironforge's
    // gate, cellars, cave and dungeon entrances). The heights there are filled in from the neighbours - a membrane over the hole
    // that is not ground: the walker never steps up onto it.
    std::vector<uint8_t> covered;
    // Stage 6.13: per batch, the coarsest terrain LOD that still covers the ground of the finer one. Packs before 6.13 dropped
    // every 16-yd LOD-2 block that touched a hole (a building's footprint) - beyond 140 yd the ground around Stormwind's houses
    // vanished and lamps / trees hung in the air over the sky. Filled by the IO thread with the height field.
    std::vector<uint8_t> batchMaxLod;
    // Stage 6.30: every batch of a 4x4-chunk region is drawn at the region's LOD (w3d_terrain_lod.h; skirts are only on region
    // borders - per batch, mixed ground left cracks between chunks).
    std::vector<uint8_t> batchRegion;
    W3dTerrainRegion regions[W3D_TERRAIN_REGIONS];
    // water (optional, linear memory)
    uint8_t* water = nullptr;
    const W3dWaterHeader* wh = nullptr;
    const W3dWaterBatch* wbatches = nullptr;
    std::vector<GpuTex*> wtex;
    // doodads
    uint8_t* plcm = nullptr;         // malloc'd until parsed
    uint8_t* coll = nullptr;         // COLL entry (floors + walls), malloc'd while resident
    u32 collSize = 0;
    uint8_t* liq = nullptr;          // LIQD entry (where the liquids are, stage 5.26), malloc'd with the tile
    u32 liqSize = 0;
    uint8_t* grnd = nullptr;         // 6.18a: GRND entry (the ADT's 145 heights + holes per chunk), malloc'd with the tile; null in older packs
    u32 grndSize = 0;
    std::vector<Instance> instances;
    std::vector<Model*> models;      // one reference per distinct model
    // Stage 6.10: the instances sorted into a 4x4 grid of chunks (contiguous ranges of instances); the renderer rejects a
    // whole chunk by its bounding sphere before it looks at the instances. The radii come with the models' headers: until
    // every model of a chunk is in (ready), its instances are tested one by one.
    struct Chunk {
        uint32_t begin = 0, end = 0;
        float center[3] = {0, 0, 0};
        float half = 0;              // the instances' positions lie within this of the center
        float maxRb = 0, maxRd = 0, maxRi = 0;   // the largest radius of a building / doodad / interior group in it
        bool hasB = false, hasD = false, hasI = false;
        bool ready = false;
    };
    std::vector<Chunk> chunks;
    // Stage 6.11: a building is a run of neighbouring placements with one turn and scale (a WMO's groups); it is shown whole - when every
    // group that is not an interior is drawable - or, if one lags, kBldgWaitMs after the first one was.
    struct Bldg {
        std::vector<uint32_t> members;   // indices into 'instances'
        u64 firstTick = 0;               // the first non-interior group became drawable (0: none yet)
        bool open = false;               // shown
        uint32_t openMs = 0;             // when it opened (streamerNowMs), the fade-in starts there
    };
    std::vector<Bldg> bldgs;
    int bldgsClosed = 0;
    // 6.44: small static doodads merged per 32-yd cell, size class and owning building (render.cpp, static_batch.hpp). The plan is made
    // once every model of the tile is in; a group's buffers are built when it is first near enough to be drawn merged.
    struct StaticBatch {
        GpuTex* tex = nullptr;
        uint8_t blend = 0, flags = 0;
        W3dModelVertex* v = nullptr;     // linear memory (freed with the tile, deferred)
        uint16_t* idx = nullptr;         // linear memory
        uint32_t first[3] = {0, 0, 0}, count[3] = {0, 0, 0};   // full / lighter levels
        float scale[3] = {1, 1, 1}, offset[3] = {0, 0, 0}, uvScale = 1;
    };
    struct StaticGroup {
        std::vector<uint32_t> members;   // indices into 'instances'
        float center[3] = {0, 0, 0}, radius = 0;   // a sphere around every member's sphere; the batches' origin
        float minR = 0, maxR = 0;
        uint16_t owner = 0;              // the building that must be shown (Instance::owner), 0: none
        uint8_t state = 0;               // 0 not built, 1 built, 2 cannot be (out of memory)
        uint32_t bornMs = 0;             // the latest member became drawable
        uint32_t firstBatch = 0, batchCount = 0;
        uint32_t frame = 0;              // the frame of the decision below
        bool merged = false;             // drawn merged this frame (its members are not drawn alone)
    };
    std::vector<StaticGroup> sgroups;
    std::vector<StaticBatch> sbatches;
    uint32_t sbytes = 0;                 // linear memory of the batches
    uint32_t sinstances = 0;             // instances.size() when planned
    uint8_t sstate = 0;                  // 0 not planned, 1 planned
    // 6.42: the portal graph of the tile's building groups (render.cpp portalsReady): built once every group is in, walked once a frame
    std::vector<wmoportal::Node> pnodes;
    uint32_t pinstances = 0;         // instances.size() when built
    uint32_t pframe = 0;             // the frame the walk was made in
    uint32_t pcomp = 0xFFFFFFFFu;    // the building culled this frame (wmoportal::reach), none: nothing culled
    uint8_t pstate = 0;              // 0: not built, 1: built, 2: no group of the tile has portals
    uint32_t readyMs = 0;            // when the ground became drawable (streamerNowMs): objects fade in from there
    // Stage 5.30: a tile is drawn as soon as its terrain and the terrain's textures are in (state Ready); its water when the
    // water's textures are, every model when the model is (Model::drawable). complete = everything of it is in.
    bool waterReady = false;
    bool complete = false;
};

// ------------------------------------------------------------------ streamer
static constexpr int kStreamJobKinds = 13;   // streamer job kinds (streamer.cpp JobKind; 6.18a added GRND)
struct StreamStats {
    int tilesReady = 0, tilesLoading = 0, slotsUsed = 0, slotsTotal = 0;
    int jobsQueued = 0, models = 0, textures = 0, uploadsPending = 0, displays = 0, creatureModels = 0;
    u32 tileLoads = 0;
    double lastTileMs = 0, maxTileMs = 0, sumTileMs = 0;   // request -> drawable (stage 5.30: the ground is drawn)
    u32 texVram = 0, texLinear = 0, geomBytes = 0;
    int texOom = 0;                  // textures waiting for memory (drawn white until it frees)
    u32 texNotInPack = 0;            // acquireTex: id not in the pack (drawn white)
    u32 texReadFailed = 0;           // texture read from the SD card failed (drawn white)
    u32 texOomTotal = 0;             // times a texture had to wait
    int workerCore = -9;             // core the IO thread runs on (-9 = synchronous fallback)
    double uploadMsLastFrame = 0;
    u32 collBytes = 0;               // COLL (floors + walls) resident in the heap
    u32 collFailed = 0;              // COLL entries dropped because malloc failed (no walls/floors there)
    int farTiles = 0;                // 6.24: tiles of the far ground (0: the pack has none)
    int farDrawn = 0;                // ... drawn last frame (where the detailed ground was not there)
    int farCached = 0;               // ... with a vertex buffer
    int farObjects = 0;              // 6.29: far buildings of the map (0: the pack has none)
    int farObjDrawn = 0;             // ... drawn last frame (their tile not loaded, or the real building not shown yet)
    int farObjTiles = 0;             // ... tiles with a vertex buffer
    u32 floorHolds = 0;              // 6.16: floor queries with nothing under the feet and a surface far above (kept the height)
    int players = 0;                 // player displays (distinct looks) alive
    u32 composites = 0;              // body textures composited so far
    // stage 5.30: progressive tiles and the read queue
    u32 tileFullLoads = 0;           // tiles that became complete (every model and texture in)
    double lastTileFullMs = 0, maxTileFullMs = 0, sumTileFullMs = 0;   // request -> complete
    u32 jobsCancelled = 0;           // reads dropped before they ran (their tile / model / texture was let go)
    int maxQueued = 0;               // longest read queue seen
    int modelsWaiting = 0;           // models read whose textures are not all in yet (not drawn)
    // Stage 6 (perf log): the SD card and the IO thread, counted when a read is collected (dropped reads never ran).
    uint64_t ioBytes = 0;
    u32 ioReads = 0, ioFails = 0;
    double ioReadMs = 0, ioCpuMs = 0;   // the IO thread's time in fread / in the cache flush + height fields
    uint64_t ioBytesKind[kStreamJobKinds] = {};      // by kind (streamerJobKindName)
    u32 ioReadsKind[kStreamJobKinds] = {};
    double ioReadMsKind[kStreamJobKinds] = {};
    int queuedByClass[8] = {};          // the read queue now, by class (streamerClassName)
    uint64_t queuedBytes = 0;
    int ioDoneWaiting = 0;              // reads finished, not collected yet
    // the main thread's cost of streamerUpdate / streamerEndFrame in the last frame, by stage
    double updateMsLastFrame = 0, doneMsLastFrame = 0, playersMsLastFrame = 0, tilesMsLastFrame = 0, scheduleMsLastFrame = 0,
           deferredMsLastFrame = 0;
    int jobsDoneLastFrame = 0, uploadsLastFrame = 0;
    u32 uploadBytesLastFrame = 0;
    int retryTextures = 0, deferredFrees = 0;
    // what did not fit: allocations that failed (cumulative), the last one's size and the linear memory free then
    u32 oomStaging = 0, oomModel = 0, oomCreature = 0, oomWater = 0, oomUpload = 0;
    u32 oomLastSize = 0, oomLastFree = 0;
    u32 texVramMiss = 0;                // textures that went to linear memory because VRAM was full
    uint64_t texVramMissBytes = 0;
    uint32_t texEvicted = 0;          // 6.28: textures moved from VRAM to linear memory for a render target (3D / wide)
    u32 collHeapMiss = 0;               // COLL that did not fit in the normal heap (went to linear memory)
    u32 badCreatureModels = 0;          // 6.35: CMDL blobs that failed w3d_creature_valid (not drawn)
    u32 lastBadCreature = 0;            // ... the last one's id
    // 6.31: the normal heap (24 MB) is kept from running dry: COLL takes it only above a reserve, and under kHeapLow the wanted
    // ring shrinks to 3 x 3 tiles (7o crashed in Stormwind at 533 yd with 944 KB left: libctru / citro3d do not survive a
    // failed malloc)
    u32 heapFree = 0;                   // measured once a frame (mallinfo)
    bool heapShrunk = false;            // the ring is 3 x 3 for want of heap now
    u32 heapShrinks = 0;                // times it happened
    // stage 6.11: buildings (placements sharing a transform) shown whole
    u32 bldgFound = 0, bldgOpenedFull = 0, bldgOpenedLate = 0;   // groups parsed / opened with every group in / opened by the timeout
    int bldgClosed = 0;                 // closed now (their groups hidden until the rest arrive)
    double bldgWaitMsSum = 0, bldgWaitMsMax = 0;   // first group drawable -> the building opened
};
static constexpr int kStreamClasses = 8;
const char* streamerJobKindName(int k);
const char* streamerClassName(int c);
double streamerTakeMaxReadMs();          // the longest single read since the last call (resets)

// Stage 6 (perf log): the streamer's memory by owner, walked on demand (a few hundred map entries: once a second).
struct StreamMemory {
    u32 poolBytes = 0, poolUsedBytes = 0, slotSize = 0;            // terrain slot pool (linear)
    u32 tiles = 0, tilesDoomed = 0, tilesComplete = 0;
    u32 waterBytes = 0;                                            // linear
    u32 heightBytes = 0, instanceBytes = 0, instances = 0, liquidBytes = 0, groundBytes = 0;   // heap
    u32 collHeapBytes = 0, collLinearBytes = 0, collLoadingBytes = 0;
    u32 models = 0, modelsLoading = 0, modelsFailed = 0, modelsDrawable = 0, modelLoadingBytes = 0;
    u32 buildings = 0, interiors = 0, buildingBytes = 0, doodads = 0, doodadBytes = 0, unitModels = 0, unitModelBytes = 0, fxModels = 0;
    u32 creatureModels = 0, creatureBytes = 0;                     // linear
    u32 textures = 0, texVramCount = 0, texLinearCount = 0, texWhiteNow = 0;
    u32 texVramByClass[8] = {}, texLinearByClass[8] = {}, texCountByClass[8] = {};
    u32 stagingReadingBytes = 0, stagingUploadBytes = 0, stagingRetryBytes = 0;   // linear, texture data not on the GPU yet
    u32 bodyTextures = 0, bodyVramBytes = 0, bodyLinearBytes = 0;  // players' composited bodies
    u32 displays = 0, playerDisplays = 0, pendingPlayers = 0, charImgs = 0, charImgBytes = 0, charInfoBytes = 0, items = 0;
    u32 deferred = 0, indexBytes = 0, indexEntries = 0;
};
void streamerMemory(StreamMemory* out);
// Stage 6.5: moves textures out of VRAM into linear memory (farthest first, a GX copy each) until 'bytes' of VRAM are
// free; returns how much moved. Outside a frame only (it waits for the GPU): for a render target that did not fit.
uint32_t streamerEvictVram(const uint32_t* blocks, int n);   // (stage 6.9: until these blocks fit, each in one piece)

// Stage 6.11: the clock of Model::drawableMs / Tile::readyMs / Tile::Bldg::openMs (milliseconds, wraps after 49 days: only
// differences are used).
uint32_t streamerNowMs();
constexpr uint32_t kFadeMs = 500;        // an object appears over this long (6.12: 0.3 s read as a pop on the console)
constexpr uint32_t kBldgWaitMs = 2500;   // a building waits for its lagging groups this long

bool streamerInit(PackIndex* index, char* err, size_t errLen);
void streamerShutdown();
// Load/unload around the camera; call once per frame before rendering. uploadsLater: the texture uploads (and the
// players' bodies) wait for streamerUploadTextures(), called inside the frame after C3D_FrameBegin (stage 6: there the
// GX copies queue with the frame's GPU work instead of each waiting for the GPU to go idle).
void streamerUpdate(const float camPos[3], float drawDist, bool uploadsLater = false);
void streamerUploadTextures();
// Stage 5.30: the direction the camera looks (reads in front come first) and a loading screen being up (texture uploads may
// take most of the frame then).
void streamerSetView(float yaw);
void streamerSetBoost(bool on);
// 6.37: false on the login screens - no world tiles are read (the ones loaded are released): the glue scene gets the card and
// the VRAM to itself. ensureMap and the world switch it back on.
void streamerSetWorldWanted(bool on);
// Ready tiles for rendering.
const std::vector<Tile*>& streamerReadyTiles();
// 6.24: the far ground (w3d_farlod.h): the tiles to draw this frame - within maxDist of the camera, without detailed ground.
// False when the pack has none or its atlas is not uploaded yet. The vertex buffers stay valid until the next call.
struct FarDraw { const W3dTerrainVertex* verts; float origin[3]; float center[3]; float radius; };
// 6.29: the far buildings (w3d_farobj.h): per tile, one buffer of every shell; a draw is a range of its indices (the whole tile
// while it is not loaded, else the buildings it does not show yet). x / y scale W3D_FAROBJ_XY_SCALE, z zScale.
struct FarObjDraw { const W3dTerrainVertex* verts; const uint16_t* idx; uint32_t count; float origin[3]; float center[3]; float radius; };
// 6.34: maxDist = the horizon (far shells are drawn that far); detailDist = how far the real buildings are drawn - beyond it a
// loaded tile's buildings are shells too
bool streamerFarObjectDraws(const float cam[3], float maxDist, float detailDist, std::vector<FarObjDraw>& out, GpuTex** palette, float* zScale);
// 6.34c: detailDist - a loaded tile wholly beyond it gets its far ground too (the renderer leaves out its detailed batches)
bool streamerFarDraws(const float cam[3], float maxDist, float detailDist, std::vector<FarDraw>& out, GpuTex** atlas, const uint16_t** indices,
                      float* zScale);
const StreamStats& streamerStats();
bool streamerTileReadyAt(float wx, float wy);
// The tile there is ready and so is its COLL (or it has none). COLL is read only for tiles
// within ~200 yd of the camera, after the tile itself.
bool streamerCollisionReadyAt(float wx, float wy);
// Ground height (bilinear) at a render-space position; false if no ready tile there.
bool streamerGroundHeight(float wx, float wy, float* out);
// The same; inHole (6.17): the point is in an ADT hole - the height there is filled in from the edges, not ground.
bool streamerGroundHeightEx(float wx, float wy, float* out, bool* inHole);
// True where the ground is the stand-in quad of an instance map (far below the building), not a real terrain.
bool streamerGroundIsStandIn(float wx, float wy);
// Where a walker with feet at zFeet stands at (wx, wy): terrain or a building floor it can
// step onto (kStepHeight up), see w3d_walk_height(). onFloor = standing on a building.
// 6.17: a particle emitter's volume: a box mesh (<= 24 vertices, <= 36 indices) with emitters and bounds over 5 yards. It exists only
// to spawn particles and the game never draws it (WoWee m2_renderer: "emitter volume hidden"); drawn, it was Orgrimmar's big grey
// cubes around the smoke of the forges and braziers.
static inline bool w3d_emitter_volume(const W3dFxHeader* fx, uint32_t vertexCount, uint32_t indexCount, const float bmin[3],
                                      const float bmax[3]) {
    if (!fx || !fx->emitterCount || vertexCount > 24 || indexCount > 36) return false;
    float m = 0;
    for (int c = 0; c < 3; ++c) m = std::max(m, bmax[c] - bmin[c]);
    return m > 5.0f;
}
static constexpr float kStepHeight = 1.5f;
// held (6.16): no floor under the feet and the nearest surface far above them (a cave whose floor is not loaded): *out is
// zFeet itself - the walker keeps its height instead of being lifted onto the surface above.
int streamerFloorsAt(float wx, float wy, int max, float* zs, uint16_t* flags);   // 6.19b: every floor triangle over a point (log)
bool streamerFloorHeight(float wx, float wy, float zFeet, float* out, bool* onFloor = nullptr, bool* held = nullptr);
// Walls (stage 5, COLL triangles flagged W3D_COLL_WALL): moves a walker (cylinder radius
// kWalkerRadius, feet at zFeet) from (fromX, fromY) towards (*toX, *toY), stopping at walls
// and sliding along them (w3d_collide_move over every ready tile the move touches).
// Returns true if a wall changed the target.
static constexpr float kWalkerRadius = 0.35f;
bool streamerCollideMove(float fromX, float fromY, float zFeet, float* toX, float* toY);
// 6.42: the walker's head is kHeadHeight over the feet; streamerCeilingAbove() gives the lowest collision face over (wx, wy) higher
// than zFrom (feet + kCeilingFrom: what is lower belongs to the floor under the feet), so a jump stops under a low roof.
static constexpr float kHeadHeight = 2.0f;
static constexpr float kCeilingFrom = 0.5f;
bool streamerCeilingAbove(float wx, float wy, float zFrom, float* ceil);
// 6.18c: the slope limit of the terrain (50 degrees, w3d_slope_move): a move uphill onto steeper ground keeps only its part along
// the contour. Needs the GRND of the tile (older packs: no limit). Returns true if the target changed.
bool streamerSlopeMove(float fromX, float fromY, float zFeet, float* toX, float* toY);
// Liquids (stage 5.26, LIQD): the surface over render (wx, wy) for something whose feet / eyes are at zRef. A liquid of
// the terrain does not count below the terrain (a cave under a lake): zRef more than 2 yd under the ground there.
struct LiquidHere {
    float level;                     // surface height
    uint8_t flags;                   // W3D_LIQ_* (kind, deep, from a building)
    W3dLiquidLook look;              // colours and the underwater fog of the tile
    int kind() const { return flags & W3D_LIQ_KIND; }
};
bool streamerLiquidAt(float wx, float wy, float zRef, LiquidHere* out);
// Creature displays (nullptr if the pack has no such display). Release every acquired display.
bool streamerHasDisplay(uint32_t displayId);   // the pack has a CDSP entry with this id (ids 0x80000000|n: game objects)
CreatureDisplay* streamerAcquireDisplay(uint32_t displayId);
// Players: race model + body texture composited from CharSections layers for this look
// (nullptr if the pack has no CHRS for the race/sex). Release with streamerReleaseDisplay.
// equip (optional) = ItemDisplayInfo ids by W3dEquipSlot (W3D_EQ_SLOTS entries): their armour is painted on the body.
CreatureDisplay* streamerAcquirePlayerDisplay(const W3dLook& look, const uint32_t* equip = nullptr);
// Stage 5.29 (character creation): the CHRS entry of a race / sex: 1 = ready (blob, size), 0 = loading, -1 = not in the pack.
int streamerCharInfo(uint32_t race, uint32_t sex, const uint8_t** blob, uint32_t* size);
// Reads the first 'size' bytes of an entry right away (small entries only: the GLUE scenes).
bool streamerReadEntry(uint32_t type, uint32_t id, void* dst, uint32_t size);
uint32_t streamerEntrySize(uint32_t type, uint32_t id);   // 0 = not in the pack (stage 5.33)
// Worn items (stage 5.19): an ITEM entry (nullptr if the pack has none; check state / ok) and the item models.
struct ItemDisplay {
    uint32_t id = 0;
    int refs = 0;
    AssetState state = AssetState::Loading;
    bool ok = false;
    W3dItemDisplay d;
};
ItemDisplay* streamerAcquireItem(uint32_t displayId);
void streamerReleaseItem(ItemDisplay* it);
Model* streamerAcquireItemModel(uint32_t modelId);       // nullptr if the pack has none
void streamerReleaseItemModel(Model* m);
void streamerReleaseDisplay(CreatureDisplay* d);
void streamerEndFrame();             // advances the deferred-free clock
GpuTex* streamerWhiteTexture();

// ------------------------------------------------------------------ renderer
struct Camera {
    float pos[3] = {0, 0, 0};
    float yaw = 0, pitch = -0.2f;
    float drawDist = 250.0f;
    void forward(float out[3]) const;
};

struct FrameCounters {
    u32 drawCalls = 0, tris = 0, visibleInstances = 0, visibleBatches = 0, texBinds = 0, waterBatches = 0, markers = 0, buildings = 0;
    u32 creatures = 0, boneUploads = 0;
    double poseMs = 0;
    u32 fxParticles = 0, fxGlows = 0, fxQuads = 0, fxSources = 0;   // stage 5.32
    double fxMs = 0;
    u32 lodBatches[W3D_TERRAIN_LODS] = {0, 0, 0};
    // Stage 6 (perf log): rendererDraw's CPU time by pass (both eyes added up), and the draws / triangles of each pass.
    enum Pass { kSky, kTerrain, kCollect, kOpaque, kMarkers, kCreatures, kWater, kAlpha, kFx, kPasses };
    double passMs[kPasses] = {};
    u32 passDraws[kPasses] = {}, passTris[kPasses] = {};
    u32 eyes = 0;                    // rendererDraw calls this frame (2 in stereoscopic 3D)
    // stage 6.3: GPU kicks this frame and when the first one went (svcGetSystemTick; 0 = none)
    u32 kicks = 0;
    u64 firstKickTick = 0;
    double kickLeadMs = 0;           // main.cpp: first kick -> C3D_FrameEnd (the GPU's head start; C3D_GetDrawingTime counts from FrameEnd)
    double kickMs = 0;               // CPU time of the kicks (cache flush + split)
    double prepareMs = 0;            // stage 6.4: rendererPrepare's poses + particles (its visibility is passMs[kCollect])
    double matrixMs = 0;             // stage 6.10: the visible instances' matrices (inside prepareMs)
    u32 sbGroups = 0, sbMembers = 0, sbDraws = 0, sbBuilt = 0;   // 6.44: merged groups drawn / their doodads / draw calls / groups built this frame
    u32 chunksCulled = 0, chunksTested = 0;   // stage 6.10: tile chunks rejected whole / whose instances were tested
    u32 lodDraws[3] = {0, 0, 0};     // stage 6.12: model + creature draws by mesh level
    u32 sharedPoses = 0, farUnits = 0;   // stage 6.12: far units posed together / their number
    u32 fading = 0;                  // stage 6.11: instances drawn alpha-blended this frame (appearing / dissolving at the edge)
};

// 6.46: the GPU test's switches (RenderSettings::gpuTestOff): one thing at a time is left out to see what it costs the GPU
constexpr uint32_t kGtTextures = 1, kGtTerrainLayers = 2, kGtTerrain = 4, kGtBuildings = 8, kGtAlpha = 16;
struct RenderSettings {
    bool doodads = true;
    bool fog = true;
    bool modelsNoCull = false;
    float doodadRange = 45.0f;   // doodad view distance = 40 yd + doodadRange * radius
    int forceLod = -1;           // -1 = by distance
    float timeSec = 0;
    bool creatures = true;
    // stage 5.26: the camera is under a liquid's surface - fog in its colour, sight ends at waterFogEnd
    bool underwater = false;
    uint8_t waterFog[3] = {28, 62, 78};
    float waterFogEnd = 45.0f;
    int effects = 2;             // stage 5.32: particles and lamp glows: 0 off, 1 low, 2 high
    // Stage 6.3: hand the GPU the commands built so far after the terrain and after the opaque models, so it draws them
    // while the CPU builds the rest (not with two eyes: their passes share buffers). pad.txt 'gpukick'.
    bool gpuKick = false;
    // Stage 6.11: new objects fade in over kFadeMs and small doodads dissolve at the edge of their distance (alpha-blended,
    // so the faded instances cost the transparent pass). pad.txt 'fadein'.
    bool fadeIn = true;
    uint32_t nowMs = 0;          // streamerNowMs() of this frame (main.cpp sets it)
    uint32_t gpuTestOff = 0;     // 6.46: what the GPU test (/gputest) switches off this phase (kGt* bits)
    bool staticBatch = true;     // 6.44: small static doodads drawn merged per cell (pad.txt 'staticbatch')
};

// Stage 5: world objects drawn as coloured direction markers until creature models exist.
struct Marker {
    float pos[3];                // feet position (render space)
    float yaw;                   // facing, radians (0 = +x)
    float height, width;         // yards
    u32 color;                   // 0xAABBGGRR
    int shape = 0;               // 0 diamond, 1 flat ring with an arrow on the ground (width = radius, yaw = arrow),
                                 // 2 the ring alone (UI stage 10: where a ground spell will land),
                                 // 3 / 4 the selection circle: a thin ring / the faint disc inside it (stage 5.44)
};

void rendererInit();
void fxForgetModel(const void* model);   // fx.cpp: a model with effects is being freed
void rendererShutdown();
void rendererSetProjection(float drawDist);   // also rebuilds the fog LUT
void rendererSetHorizon(float yards);         // 6.34b: 0 = off; else the ground / far shells to this distance (then call rendererSetProjection)
void rendererSetViewer(const float* pos);    // 6.42: the character's position (seeds the portal walk too), null: none
void rendererSetEye(float iod);               // stereoscopic 3D: the projection of one eye (-iod left, +iod right, 0 = mono)
u32 rendererClearColor(const RenderSettings& rs);
// Stage 5.29: a login / character screen scene: static models and creatures only (no terrain, sky, water or fog), seen
// through a camera with its own vertical field of view and clip planes.
struct SceneModel {
    Model* model = nullptr; float pos[3] = {0, 0, 0}; float rot[3] = {0, 0, 0}; float scale = 1;
    // stage 5.33, spell effects in the world (rendererDraw's effects): fxKey identifies the effect between frames for its
    // particle emitters; emit = false while it lingers (no mesh, no new particles: the ones thrown fade out)
    uint32_t fxKey = 0;
    bool emit = true;
    uint32_t fxAgeMs = 0;       // stage 5.35: since the effect started (the bone animation of the model)
};
// Stage 5.33: where a point of a unit drawn in the last frames is (W3dSpellSlot: hands, chest, head, feet), from its
// animated skeleton; false when the unit was not drawn lately or its model has no such attachment.
bool rendererUnitPoint(uint64_t guid, int slot, float out[3]);
void rendererDrawScene(const Camera& cam, float vfovDeg, float nearZ, float farZ, const std::vector<SceneModel>& models,
                       const std::vector<CreatureInstance>* creatures, FrameCounters& fc);
// UI stage 8: a unit frame's portrait - the unit's head and shoulders, seen from the front, drawn into the render target
// that is current (C3D_FrameDrawOn; a square texture). ci: the unit as the world draws it (display, worn items); its
// position, facing and animation are replaced (standing, at the origin). corr: 2x2 signed permutation (row-major) applied
// to clip x / y so that the picture lands upright in the texture (portrait.cpp finds it once by drawing a test pattern).
bool rendererDrawPortrait(const CreatureInstance& ci, const float corr[4], FrameCounters& fc, bool body = false, float turn = 0);   // the sky's colour behind everything (RGBA8), water when under it
// Stage 6.3: call right before C3D_FrameEnd. After an early GPU kick citro3d's queue is running, so FrameEnd's last command
// list goes to the GPU at once - before FrameEnd's own cache flush: flush here first, and end the frame with
// C3D_FrameEnd(GX_CMDLIST_FLUSH) so that the list's last words (written by the split) are flushed too (stage 6.8).
void rendererBeforeFrameEnd(FrameCounters& fc);
struct tag_gxCmdQueue_s* rendererGxQueue();   // stage 6.7: citro3d's GX queue (nullptr when its layout does not look right)
// Stage 6.4: the frame's visibility (which doodads / buildings / effect meshes are drawn) before C3D_FrameBegin, while
// the GPU still draws the previous frame; rendererDraw (any number of eyes) then uses it, rendererEndFrame drops it.
void rendererPrepare(const Camera& cam, const RenderSettings& rs, FrameCounters& fc, const std::vector<SceneModel>* effects,
                     const std::vector<CreatureInstance>* creatures);
void rendererEndFrame();
void rendererDraw(C3D_RenderTarget* target, const Camera& cam, const RenderSettings& rs, FrameCounters& fc,
                  const std::vector<Marker>* markers = nullptr,
                  const std::vector<CreatureInstance>* creatures = nullptr,
                  const std::vector<SceneModel>* effects = nullptr);   // stage 5.33: spell effects (spell_view.cpp)
