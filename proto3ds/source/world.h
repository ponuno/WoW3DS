// world.h — shared types of the stage-3 client: pack index, streamed tiles,
// refcounted GPU assets, renderer entry points.
#pragma once
#include <3ds.h>
#include <citro3d.h>

#include <stdint.h>
#include <stdio.h>

#include <vector>

#include "w3d_format.h"
#include "w3d_character.h"
#include "w3d_fx.h"
#include "hud.hpp"

static constexpr float kTileSize = 533.33333f;
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
    uint16_t fxTexBase = 0;
    // per-frame visibility bucket (filled by the renderer)
    std::vector<const struct Instance*> visible;
    bool texturesReady() const;
};

struct Instance {
    Model* model;
    float pos[3];
    float rot[3];
    float scale;
    float radius;
    float lightModel[3];
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
    // Worn item models (stage 5.19): static models hung on bones of the unit (helmet, shoulders, weapons).
    struct Worn {
        Model* model;
        int16_t bone;                // bone of the unit's model carrying the attachment
        float pos[3];                // attachment point, bind-pose model space
    };
    static constexpr int kMaxWorn = 7;
    Worn worn[kMaxWorn];
    int wornCount = 0;
};

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
    std::vector<Instance> instances;
    std::vector<Model*> models;      // one reference per distinct model
    // Stage 5.30: a tile is drawn as soon as its terrain and the terrain's textures are in (state Ready); its water when the
    // water's textures are, every model when the model is (Model::drawable). complete = everything of it is in.
    bool waterReady = false;
    bool complete = false;
};

// ------------------------------------------------------------------ streamer
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
    int players = 0;                 // player displays (distinct looks) alive
    u32 composites = 0;              // body textures composited so far
    // stage 5.30: progressive tiles and the read queue
    u32 tileFullLoads = 0;           // tiles that became complete (every model and texture in)
    double lastTileFullMs = 0, maxTileFullMs = 0, sumTileFullMs = 0;   // request -> complete
    u32 jobsCancelled = 0;           // reads dropped before they ran (their tile / model / texture was let go)
    int maxQueued = 0;               // longest read queue seen
    int modelsWaiting = 0;           // models read whose textures are not all in yet (not drawn)
};

bool streamerInit(PackIndex* index, char* err, size_t errLen);
void streamerShutdown();
// Load/unload around the camera; call once per frame before rendering.
void streamerUpdate(const float camPos[3], float drawDist);
// Stage 5.30: the direction the camera looks (reads in front come first) and a loading screen being up (texture uploads may
// take most of the frame then).
void streamerSetView(float yaw);
void streamerSetBoost(bool on);
// Ready tiles for rendering.
const std::vector<Tile*>& streamerReadyTiles();
const StreamStats& streamerStats();
bool streamerTileReadyAt(float wx, float wy);
// The tile there is ready and so is its COLL (or it has none). COLL is read only for tiles
// within ~200 yd of the camera, after the tile itself.
bool streamerCollisionReadyAt(float wx, float wy);
// Ground height (bilinear) at a render-space position; false if no ready tile there.
bool streamerGroundHeight(float wx, float wy, float* out);
// Where a walker with feet at zFeet stands at (wx, wy): terrain or a building floor it can
// step onto (kStepHeight up), see w3d_walk_height(). onFloor = standing on a building.
static constexpr float kStepHeight = 1.5f;
bool streamerFloorHeight(float wx, float wy, float zFeet, float* out, bool* onFloor = nullptr);
// Walls (stage 5, COLL triangles flagged W3D_COLL_WALL): moves a walker (cylinder radius
// kWalkerRadius, feet at zFeet) from (fromX, fromY) towards (*toX, *toY), stopping at walls
// and sliding along them (w3d_collide_move over every ready tile the move touches).
// Returns true if a wall changed the target.
static constexpr float kWalkerRadius = 0.35f;
bool streamerCollideMove(float fromX, float fromY, float zFeet, float* toX, float* toY);
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
};

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
};

// Stage 5: world objects drawn as coloured direction markers until creature models exist.
struct Marker {
    float pos[3];                // feet position (render space)
    float yaw;                   // facing, radians (0 = +x)
    float height, width;         // yards
    u32 color;                   // 0xAABBGGRR
    int shape = 0;               // 0 diamond, 1 flat ring with an arrow on the ground (width = radius, yaw = arrow)
};

void rendererInit();
void fxForgetModel(const void* model);   // fx.cpp: a model with effects is being freed
void rendererShutdown();
void rendererSetProjection(float drawDist);   // also rebuilds the fog LUT
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
};
// Stage 5.33: where a point of a unit drawn in the last frames is (W3dSpellSlot: hands, chest, head, feet), from its
// animated skeleton; false when the unit was not drawn lately or its model has no such attachment.
bool rendererUnitPoint(uint64_t guid, int slot, float out[3]);
void rendererDrawScene(const Camera& cam, float vfovDeg, float nearZ, float farZ, const std::vector<SceneModel>& models,
                       const std::vector<CreatureInstance>* creatures, FrameCounters& fc);   // the sky's colour behind everything (RGBA8), water when under it
void rendererDraw(C3D_RenderTarget* target, const Camera& cam, const RenderSettings& rs, FrameCounters& fc,
                  const std::vector<Marker>* markers = nullptr,
                  const std::vector<CreatureInstance>* creatures = nullptr,
                  const std::vector<SceneModel>* effects = nullptr);   // stage 5.33: spell effects (spell_view.cpp)
