// w3d_format.h — compact asset pack shared by the PC converter and the 3DS client.
//
// Everything is little-endian, 4-byte aligned POD that the 3DS can use straight
// from the file buffer (no parsing, no pointer fix-ups beyond adding offsets).
// Layout of a pack:
//
//   W3dPackHeader
//   ...entry blobs, each aligned to W3D_BLOB_ALIGN...
//   W3dEntry[entryCount]           at header.indexOffset
//
// Coordinates are WoWee "render space": X/Y horizontal, Z up, yards.
// Positions inside blobs are int16, decoded in the vertex shader as
//   local = s16 * posScale + posOffset
// and the CPU adds a camera-relative translation, because PICA200 floats are
// 24-bit (16-bit mantissa) and absolute world coords (~17000 yd) would jitter.
#pragma once
#include <stdint.h>

#define W3D_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define W3DP_MAGIC    W3D_FOURCC('W', '3', 'D', 'P')
#define W3DP_VERSION  5u   // v5: item displays (armour on players) + attachment points of creature models; v4: creature
                           // animations of every kind + mount seats; v3: terrain LODs + skirts; v2: WATR
#define W3D_BLOB_ALIGN 16u

enum W3dEntryType {
    W3D_ENTRY_TERRAIN   = W3D_FOURCC('T', 'E', 'R', 'R'),
    W3D_ENTRY_TEXTURE   = W3D_FOURCC('T', 'E', 'X', ' '),
    W3D_ENTRY_MODEL     = W3D_FOURCC('M', 'O', 'D', 'L'),
    W3D_ENTRY_PLACEMENT = W3D_FOURCC('P', 'L', 'C', 'M'),
    W3D_ENTRY_WATER     = W3D_FOURCC('W', 'A', 'T', 'R'),
    W3D_ENTRY_COLLISION = W3D_FOURCC('C', 'O', 'L', 'L'),   // optional (stage 5.0b): walkable building floors
    W3D_ENTRY_CREATURE  = W3D_FOURCC('C', 'M', 'D', 'L'),   // stage 5.3: skinned, animated model (id = path hash)
    W3D_ENTRY_DISPLAY   = W3D_FOURCC('C', 'D', 'S', 'P'),   // stage 5.3: creature display (id = display id)
    W3D_ENTRY_CHARACTER = W3D_FOURCC('C', 'H', 'R', 'S'),   // stage 5.5: player race/sex (id = w3d_char_id)
    W3D_ENTRY_CHARIMG   = W3D_FOURCC('C', 'I', 'M', 'G'),   // stage 5.5: body texture layer (id = path hash)
    W3D_ENTRY_ITEM      = W3D_FOURCC('I', 'T', 'E', 'M'),   // stage 5.19: worn item look (id = ItemDisplayInfo id)
    W3D_ENTRY_LIQUID    = W3D_FOURCC('L', 'I', 'Q', 'D'),   // stage 5.26: where the liquids of a tile are (swimming, id = tile)
    W3D_ENTRY_PACKINFO  = W3D_FOURCC('P', 'I', 'N', 'F'),   // stage 5.27: what this file is in a set of packs (id 0)
    W3D_ENTRY_GLUE      = W3D_FOURCC('G', 'L', 'U', 'E'),   // stage 5.29: a login / character screen scene (W3dGlueScene)
    W3D_ENTRY_SPELLFX   = W3D_FOURCC('S', 'V', 'F', 'X'),   // stage 5.33: spell visuals -> effect models (w3d_fx.h, id 0)
};

// ---------------------------------------------------------------- pack sets (stage 5.27)
// One conversion of everything (w3dconv --all) writes a shared asset pool and a small pack per map:
//   common.w3dp, common1.w3dp, ...  every entry that is not a tile (textures, models, buildings, creatures, players,
//                                   items); several parts because FAT32 holds no file of 4 GB or more
//   map<ID>.w3dp                    the map's tiles only: TERR, PLCM, WATR, LIQD, COLL
// Each file carries a PINF entry; the client opening a map pack opens the pool parts next to it and checks that every
// file comes from the same conversion (buildId). A pack without PINF (older converters) stands alone.
enum W3dPackKind { W3D_PACK_STANDALONE = 0, W3D_PACK_POOL = 1, W3D_PACK_MAP = 2 };
typedef struct {
    uint32_t buildId;             // the same in every file of one conversion
    uint16_t kind;                // W3dPackKind
    uint16_t part;                // pool: this part's number (0 = common.w3dp, k = common<k>.w3dp)
    uint16_t partCount;           // pool: parts in the set (a map pack: 0, the pool says)
    uint16_t pad;
    uint32_t mapId;               // map: Map.dbc id
    uint32_t reserved[3];
} W3dPackInfo;                    // 28 bytes
// Entry types that belong to a map (by tile id); everything else goes to the pool.
static inline int w3d_is_tile_entry(uint32_t type) {
    return type == W3D_ENTRY_TERRAIN || type == W3D_ENTRY_PLACEMENT || type == W3D_ENTRY_WATER || type == W3D_ENTRY_LIQUID ||
           type == W3D_ENTRY_COLLISION;
}

typedef struct {
    uint32_t magic;        // W3DP_MAGIC
    uint32_t version;      // W3DP_VERSION
    uint32_t entryCount;
    uint32_t indexOffset;  // byte offset of W3dEntry[entryCount]
} W3dPackHeader;

typedef struct {
    uint32_t type;    // W3dEntryType
    uint32_t id;      // FNV-1a of the lower-case, '/'-separated game path (or tile id)
    uint32_t offset;  // from start of file
    uint32_t size;    // bytes
} W3dEntry;

// ---------------------------------------------------------------- textures
enum W3dTexFormat {
    W3D_TEX_RGB565   = 0,  // 16 bpp, Morton-swizzled 8x8 tiles
    W3D_TEX_ETC1     = 1,  // 4 bpp, PICA tile order, blocks byte-swapped
    W3D_TEX_ETC1A4   = 2,  // 8 bpp (4-bit alpha block + ETC1 block)
    W3D_TEX_RGBA4    = 3,  // 16 bpp
    W3D_TEX_RGBA5551 = 4,  // 16 bpp, 1-bit alpha
};

#define W3D_TEXF_HAS_ALPHA 0x01u

typedef struct {
    uint16_t width, height;  // power of two, 8..1024
    uint8_t  format;         // W3dTexFormat
    uint8_t  mipCount;       // levels stored, >= 1 (citro3d maxLevel = mipCount-1)
    uint8_t  flags;          // W3D_TEXF_*
    uint8_t  pad;
    uint32_t dataSize;       // all levels, level 0 first, in GPU layout
    // uint8_t data[dataSize] follows (offset 12, padded to 16 by the writer)
} W3dTexHeader;
#define W3D_TEX_DATA_OFFSET 16u

// ---------------------------------------------------------------- terrain
// One ADT tile. One vertex buffer for the whole tile, batches grouped by
// 4x4-chunk region and by the (tex0, tex1) pair so they can be frustum culled
// and drawn with few texture switches.
typedef struct {
    int16_t x, y, z, w;   // w unused (keeps 8-byte alignment)
    uint8_t r, g, b, a;   // r = baked lighting, g = tex2 weight (5.36), a = tex1 blend weight
    int16_t u, v;         // tiling UV * W3D_TERRAIN_UV_SCALE
} W3dTerrainVertex;       // 16 bytes
#define W3D_TERRAIN_UV_SCALE (1.0f / 256.0f)

// Each batch carries three index ranges over the same vertices: LOD 0 (every
// vertex), LOD 1 (every 2nd), LOD 2 (every 4th). Every range also contains
// vertical "skirts" along 4x4-chunk region borders so neighbouring batches at
// different LODs never show cracks.
#define W3D_TERRAIN_LODS 3
typedef struct {
    uint16_t tex0, tex1;          // indices into the tile's texture id table
    uint32_t firstIndex[W3D_TERRAIN_LODS];
    uint32_t indexCount[W3D_TERRAIN_LODS];
    float    bmin[3], bmax[3];    // tile-local bounds (after posScale/posOffset)
} W3dTerrainBatch;                // 52 bytes

typedef struct {
    uint32_t tileX, tileY;
    float    origin[3];           // render-space position of tile-local (0,0,0)
    float    posScale[3];
    float    posOffset[3];        // tile-local offset (normally 0)
    uint32_t gridSize;            // vertices per chunk edge: 9, 5 or 3
    uint32_t vertexCount, indexCount, batchCount, texCount;
    uint32_t offTexIds;           // uint32_t[texCount] texture entry ids
    uint32_t offBatches;          // W3dTerrainBatch[batchCount]
    uint32_t offVertices;         // W3dTerrainVertex[vertexCount]
    uint32_t offIndices;          // uint16_t[indexCount] (triangle list)
    // 5.36: a third ground texture per batch (base + two overlays, as WoW splats up to four layers). The vertex's
    // green channel is the second overlay's weight (a = the first one's). Older packs end the header at offIndices:
    // there offTexIds < sizeof(W3dTerrainHeader), and w3d_terrain_tex2() answers "none".
    uint32_t offTex2;             // uint16_t[batchCount]: index into the texture table; 0 = no third layer
} W3dTerrainHeader;

static inline const uint16_t* w3d_terrain_tex2(const W3dTerrainHeader* h, const uint8_t* base) {
    return (h->offTexIds >= sizeof(W3dTerrainHeader) && h->offTex2) ? (const uint16_t*)(base + h->offTex2) : 0;
}

// ---------------------------------------------------------------- models
typedef struct {
    int16_t x, y, z, w;
    int8_t  nx, ny, nz, pad;
    int16_t u, v;                 // * W3D_MODEL_UV_SCALE, already v-flipped for PICA
} W3dModelVertex;                 // 16 bytes
#define W3D_MODEL_UV_SCALE (1.0f / 4096.0f)

enum W3dBlend { W3D_BLEND_OPAQUE = 0, W3D_BLEND_ALPHAKEY = 1, W3D_BLEND_ALPHA = 2, W3D_BLEND_ADD = 3 };
#define W3D_SUBF_TWO_SIDED 0x01u
#define W3D_SUBF_UNLIT     0x02u
#define W3D_SUBF_BUILDING  0x04u   // WMO group: drawn with doodads off, no doodad fade
#define W3D_SUBF_INTERIOR  0x08u   // WMO interior-only group: drawn only near it (no portals yet)
#define W3D_NO_TEXTURE     0xFFFFu

typedef struct {
    uint16_t tex;                 // index into model texture id table or W3D_NO_TEXTURE
    uint8_t  blend;               // W3dBlend
    uint8_t  flags;               // W3D_SUBF_*
    uint32_t firstIndex;
    uint32_t indexCount;
} W3dSubmesh;                     // 12 bytes

typedef struct {
    float    posScale[3];
    float    posOffset[3];
    float    bmin[3], bmax[3];    // model space
    float    radius;              // bounding sphere radius around origin
    uint32_t vertexCount, indexCount, submeshCount, texCount;
    uint32_t offTexIds, offSubmeshes, offVertices, offIndices;
    float    uvScale;             // vertex u/v * uvScale (long WMO streets tile far past +-8); absent in older packs
} W3dModelHeader;
// Older blobs end their header 4 bytes earlier (the texture ids start at offTexIds < sizeof the header).
static inline float w3d_model_uv_scale(const W3dModelHeader* h) {
    return (h->offTexIds >= sizeof(W3dModelHeader) && h->uvScale > 0) ? h->uvScale : W3D_MODEL_UV_SCALE;
}

// ---------------------------------------------------------------- placements
typedef struct {
    uint32_t modelId;             // MODL entry id
    float    pos[3];              // render space
    float    rot[3];              // radians; matrix = T * Rz(rot[2]) * Ry(rot[1]) * Rx(rot[0]) * S
    float    scale;
} W3dPlacement;                   // 32 bytes

typedef struct {
    uint32_t count;
    uint32_t pad[3];
    // W3dPlacement[count]
} W3dPlacementHeader;

// ---------------------------------------------------------------- water
// Liquid surfaces of one tile: the terrain's (MCLQ) and the buildings' (WMO MLIQ) that the tile owns.
// Vertices use the terrain vertex layout: rgb = the liquid's colour (Light.dbc river / ocean colour,
// shallow -> deep), a = opacity (by depth), uv = world-aligned tiling. Same id as the tile's TERR entry.
// Since stage 5.26 the textures are the game's own animated ones (XTextures\river\lake_a.N.blp, ocean_h, lava,
// slime): the batch's frames are 'frames' consecutive entries of the texture id table starting at 'tex',
// shown W3D_WATER_FRAME_MS each (frames == 0 in older packs: one procedural texture).
enum W3dLiquid { W3D_LIQUID_WATER = 0, W3D_LIQUID_OCEAN = 1, W3D_LIQUID_MAGMA = 2, W3D_LIQUID_SLIME = 3 };
#define W3D_WATER_FRAME_MS 60u        // 30 frames: the 1.8 s cycle of the original

typedef struct {
    uint8_t  liquid;              // W3dLiquid
    uint8_t  frames;              // animation frames (0 or 1 = a still texture)
    uint16_t tex;                 // index into the water texture id table (first frame)
    uint32_t firstIndex;
    uint32_t indexCount;
    float    bmin[3], bmax[3];    // tile-local
} W3dWaterBatch;                  // 36 bytes

typedef struct {
    uint32_t tileX, tileY;
    float    origin[3];           // same convention as W3dTerrainHeader
    float    posScale[3];
    float    posOffset[3];
    uint32_t vertexCount, indexCount, batchCount, texCount;
    uint32_t offTexIds, offBatches, offVertices, offIndices;
} W3dWaterHeader;

// ---------------------------------------------------------------- liquid query (stage 5.26)
// Where the liquids of one tile are and how high, for movement (swimming, water walking, falling into water) and the
// underwater view; CPU data (heap). The tile is the 128 x 128 grid of liquid cells (4.17 yd, the MCLQ / MLIQ cell):
//   gx = (maxX - x) / W3D_LIQ_CELL (rows, render -x), gy = (maxY - y) / W3D_LIQ_CELL (columns, render -y),
//   chunk = (gx / 8) * 16 + gy / 8 (the ADT's MCNK order), cell = (gx % 8) * 8 + gy % 8,
// corner heights [row 0..8][col 0..8] of a chunk, bilinear inside a cell; a chunk where two surfaces meet (a river
// beside lava, a pool of a building over a lake) keeps four corners per cell instead (W3D_LIQ_PERCELL). Building
// liquids are rasterised into the same grid (a cell is wet when its centre is over a wet MLIQ cell, flat at the
// height there); a cell covered twice takes the higher surface.
#define W3D_LIQ_CELL  (533.33333f / 128.0f)
#define W3D_LIQ_DRY   0xFFu
#define W3D_LIQ_KIND  0x03u       // W3dLiquid
#define W3D_LIQ_DEEP  0x04u       // deep water: fatigue (MCLQ "dark water" 0x80)
#define W3D_LIQ_WMO   0x08u       // from a building: no cave test against the terrain
#define W3D_LIQ_FLAT  0xFFFFFFFFu // W3dLiquidChunk.offHeights: every corner at 'base'
#define W3D_LIQ_PERCELL 0x80000000u   // offHeights bit: int16_t[64][4] (corners (r,k) (r,k+1) (r+1,k) (r+1,k+1)) instead of [81]
typedef struct {
    uint8_t  cell[64];            // W3D_LIQ_* per cell, W3D_LIQ_DRY = no liquid
    float    base;                // corner height = base + h / 32
    uint32_t offHeights;          // int16_t[81] (| W3D_LIQ_PERCELL: [64][4]) from the start of the blob, or W3D_LIQ_FLAT
} W3dLiquidChunk;                 // 72 bytes

typedef struct {
    uint8_t  close[4], far_[4];   // surface colour, shallow / deep (rgb, a unused)
    uint8_t  fog[4];              // under the surface
    float    fogEnd;              // yards of sight under the surface
} W3dLiquidLook;                  // 16 bytes

typedef struct {
    uint32_t tileX, tileY;
    float    maxX, maxY;          // render-space corner of the tile where gx = gy = 0
    uint32_t chunkCount;
    uint32_t offChunkIndex;       // uint16_t[256]: chunk record or 0xFFFF (dry)
    uint32_t offChunks;           // W3dLiquidChunk[chunkCount]
    uint32_t pad;
    W3dLiquidLook look[4];        // by W3dLiquid (Light.dbc of the tile at noon; magma / slime: fixed)
} W3dLiquidHeader;

// ---------------------------------------------------------------- collision (floors + walls)
// Collision triangles of buildings (WMO collision faces) and doodads (M2 bounding
// triangles) that touch one tile, clipped to it. |normal.z| >= 0.5 = floor (ground
// height), steeper = wall (flags & W3D_COLL_WALL, horizontal push-out). Packs from
// before walls hold floors only (flags 0). CPU data: the 3DS reads it into ordinary
// heap memory. Same id as the tile's TERR entry.
//   x = origin[0] + qx * W3D_COLL_XY_SCALE, y likewise, z = origin[2] + qz * zScale
// A uniform grid of gridN x gridN cells over the tile lists the triangles overlapping
// each cell: cellStart[c] .. cellStart[c + 1] index into triIndex[].
#define W3D_COLL_XY_SCALE (533.33333f / 65535.0f)

typedef struct {
    uint16_t x[3], y[3];          // tile-local, * W3D_COLL_XY_SCALE
    int16_t  z[3];                // * zScale, relative to origin[2]
    uint16_t flags;               // W3D_COLL_*
} W3dCollTri;                     // 20 bytes
#define W3D_COLL_WALL 1u          // steep face: not a floor, blocks horizontal movement
#define W3D_COLL_M2   2u          // from a doodad (M2) rather than a building (statistics only)

typedef struct {
    uint32_t tileX, tileY;
    float    origin[3];           // tile min corner (x, y) and the z the int16 heights are relative to
    float    zScale;
    uint32_t gridN;               // cells per tile edge (32 or 64)
    uint32_t triCount, indexCount;
    uint32_t offCells;            // uint32_t[gridN * gridN + 1]
    uint32_t offIndex;            // uint32_t[indexCount]
    uint32_t offTris;             // W3dCollTri[triCount]
} W3dCollHeader;

// ---------------------------------------------------------------- creatures (stage 5.3/5.4)
// Skinned model: bind-pose vertices with up to 2 bone influences, drawn in
// submeshes whose bones fit one palette (<= W3D_PALETTE_BONES) so the vertex
// shader holds the palette as 3 vec4 rows per bone. Bone indices in vertices
// are palette-local and pre-multiplied by 3 (the uniform row offset).
//
// Animation is baked at W3D_ANIM_FPS: for every sequence and bone three
// channels (rotation, translation, scale) each point into the int16 key pool:
//   W3D_CHAN_NONE        -> identity rotation / zero translation / unit scale
//   off                  -> one constant key
//   off | W3D_CHAN_ANIM  -> 'frames' keys (one per sample)
// rotation keys: 4 x int16 quaternion (x, y, z, w) * 1/32767
// translation keys: 3 x int16 * transScale; scale keys: 3 x int16 * 1/4096
// Bone matrix (WoWee computeBoneMatrices): parent * T(pivot) * T(t) * R(q) * S(s) * T(-pivot).
#define W3D_PALETTE_BONES 24
#define W3D_ANIM_FPS 30.0f
#define W3D_CHAN_NONE 0xFFFFFFFFu
#define W3D_CHAN_ANIM 0x80000000u
#define W3D_SCALE_KEY (1.0f / 4096.0f)

// Animation ids (AnimationData.dbc); the converter keeps the list in creature_model.cpp (creatureAnimIds).
enum W3dAnimId {
    W3D_ANIM_STAND = 0, W3D_ANIM_DEATH = 1, W3D_ANIM_SPELL = 2, W3D_ANIM_WALK = 4, W3D_ANIM_RUN = 5, W3D_ANIM_DEAD = 6,
    W3D_ANIM_RISE = 7, W3D_ANIM_STAND_WOUND = 8, W3D_ANIM_COMBAT_WOUND = 9, W3D_ANIM_COMBAT_CRITICAL = 10,
    W3D_ANIM_SHUFFLE_LEFT = 11, W3D_ANIM_SHUFFLE_RIGHT = 12, W3D_ANIM_WALK_BACKWARDS = 13, W3D_ANIM_STUN = 14,
    W3D_ANIM_ATTACK_UNARMED = 16, W3D_ANIM_ATTACK_1H = 17, W3D_ANIM_ATTACK_2H = 18, W3D_ANIM_ATTACK_2HL = 19,
    W3D_ANIM_PARRY_UNARMED = 20, W3D_ANIM_PARRY_1H = 21, W3D_ANIM_PARRY_2H = 22, W3D_ANIM_PARRY_2HL = 23,
    W3D_ANIM_SHIELD_BLOCK = 24, W3D_ANIM_READY_UNARMED = 25, W3D_ANIM_READY_1H = 26, W3D_ANIM_READY_2H = 27,
    W3D_ANIM_READY_2HL = 28, W3D_ANIM_READY_BOW = 29, W3D_ANIM_DODGE = 30, W3D_ANIM_SPELL_PRECAST = 31,
    W3D_ANIM_SPELL_CAST = 32, W3D_ANIM_NPC_WELCOME = 34, W3D_ANIM_NPC_GOODBYE = 35, W3D_ANIM_BLOCK = 36,
    W3D_ANIM_JUMP_START = 37, W3D_ANIM_JUMP = 38, W3D_ANIM_JUMP_END = 39, W3D_ANIM_FALL = 40,
    W3D_ANIM_SWIM_IDLE = 41, W3D_ANIM_SWIM = 42, W3D_ANIM_SWIM_LEFT = 43, W3D_ANIM_SWIM_RIGHT = 44,
    W3D_ANIM_SWIM_BACKWARDS = 45, W3D_ANIM_ATTACK_BOW = 46, W3D_ANIM_FIRE_BOW = 47, W3D_ANIM_READY_RIFLE = 48,
    W3D_ANIM_ATTACK_RIFLE = 49, W3D_ANIM_LOOT = 50, W3D_ANIM_READY_SPELL_DIRECTED = 51, W3D_ANIM_READY_SPELL_OMNI = 52,
    W3D_ANIM_SPELL_CAST_DIRECTED = 53, W3D_ANIM_SPELL_CAST_OMNI = 54, W3D_ANIM_BATTLE_ROAR = 55,
    W3D_ANIM_EMOTE_TALK = 60, W3D_ANIM_ATTACK_OFF = 87, W3D_ANIM_MOUNT = 91, W3D_ANIM_FLY = 135,
    W3D_ANIM_SIT_GROUND_DOWN = 96, W3D_ANIM_SIT_GROUND = 97, W3D_ANIM_SIT_GROUND_UP = 98,
    W3D_ANIM_SLEEP_DOWN = 99, W3D_ANIM_SLEEP = 100, W3D_ANIM_SLEEP_UP = 101,
    W3D_ANIM_SIT_CHAIR_LOW = 102, W3D_ANIM_SIT_CHAIR_MED = 103, W3D_ANIM_SIT_CHAIR_HIGH = 104,
    W3D_ANIM_ATTACK_THROWN = 107, W3D_ANIM_READY_THROWN = 108,
    W3D_ANIM_KNEEL_START = 114, W3D_ANIM_KNEEL_LOOP = 115, W3D_ANIM_KNEEL_END = 116,
    W3D_ANIM_STEALTH_WALK = 119, W3D_ANIM_STEALTH_STAND = 120,
    W3D_ANIM_CHANNEL_CAST_DIRECTED = 124, W3D_ANIM_CHANNEL_CAST_OMNI = 125,
};

typedef struct {
    int16_t x, y, z, w;           // * posScale + posOffset (bind pose, model space)
    int8_t  nx, ny, nz, pad;
    int16_t u, v;                 // * W3D_MODEL_UV_SCALE, v-flipped
    uint8_t bone[2];              // palette-local bone index * 3
    uint8_t weight[2];            // 0..255, sum 255
} W3dSkinVertex;                  // 20 bytes

typedef struct {
    uint16_t texSlot;             // index into W3dCreatureTex[] (W3D_NO_TEXTURE = untextured)
    uint8_t  blend, flags;        // W3dBlend, W3D_SUBF_*
    uint16_t geoset;              // M2 submesh id (character models select geosets per display)
    uint16_t palette;             // index into W3dBonePalette[]
    uint32_t firstIndex, indexCount;
} W3dSkinSubmesh;                 // 16 bytes

typedef struct {
    uint16_t count;
    uint16_t bones[W3D_PALETTE_BONES];   // global bone indices
    uint16_t pad;
} W3dBonePalette;                 // 52 bytes

typedef struct {
    int16_t  parent;              // -1 = root; always < own index
    uint16_t flags;               // M2 bone flags
    float    pivot[3];
} W3dBone;                        // 16 bytes

typedef struct {
    uint16_t animId;              // W3dAnimId
    uint16_t frames;              // samples (>= 1), evenly spaced over [0, durationMs] (~W3D_ANIM_FPS)
    uint32_t durationMs;
    float    moveSpeed;           // yd/s the animation was authored for (walk/run playback rate)
    uint32_t offChannels;         // uint32_t[boneCount * 3]: rotation, translation, scale per bone
} W3dAnimSeq;                     // 16 bytes

enum W3dCreatureTexKind { W3D_CTEX_FIXED = 0, W3D_CTEX_SKIN1 = 1, W3D_CTEX_SKIN2 = 2, W3D_CTEX_SKIN3 = 3,
                          W3D_CTEX_BODY = 4, W3D_CTEX_HAIR = 5, W3D_CTEX_CAPE = 6, W3D_CTEX_NONE = 255 };
#define W3D_CTEX_KINDS 7             // SKIN1..3, BODY, HAIR, CAPE use CreatureDisplay::tex[kind - 1]
typedef struct {
    uint8_t  kind;                // W3dCreatureTexKind (per-display kinds are filled from W3dDisplay)
    uint8_t  blendAlpha;          // texture needs alpha (alpha key / blend)
    uint16_t pad;
    uint32_t texId;               // TEX entry id for W3D_CTEX_FIXED
} W3dCreatureTex;                 // 8 bytes

typedef struct {
    float    posScale[3], posOffset[3];
    float    bmin[3], bmax[3];    // bind pose
    float    radius;              // around the origin, generous (animation)
    float    transScale;
    uint32_t vertexCount, indexCount, submeshCount, texCount;
    uint32_t boneCount, seqCount, paletteCount, keyCount;
    uint32_t offTex, offSubmeshes, offVertices, offIndices;
    uint32_t offPalettes, offBones, offSeqs, offKeys;
    int16_t  seatBone;            // mounts: the rider's seat (attachment 0/5/6/7/8), -1 = none
    uint16_t pad2;
    float    seat[3];             // offset from that bone's pivot, model space (bind pose)
    uint32_t attachCount;         // v5: attachment points worn items hang on (W3dAttach)
    uint32_t offAttach;
} W3dCreatureHeader;

// Attachment points of a character / creature model (M2 attachments the client uses): where an item model is
// hung. Position is in bind-pose model space; the item rides the bone: bone matrix * T(pos).
// Ids (M2): 0 shield, 1 right hand, 2 left hand, 5 right shoulder, 6 left shoulder, 11 helm, 12 back,
// 26 main-hand sheath, 27 off-hand sheath, 28 shield sheath, 30/31 large weapon left/right, 32/33 hip weapon left/right.
typedef struct {
    uint16_t id;
    int16_t  bone;
    float    pos[3];
} W3dAttach;                      // 16 bytes
enum W3dAttachId { W3D_ATT_SHIELD = 0, W3D_ATT_RHAND = 1, W3D_ATT_LHAND = 2, W3D_ATT_RSHOULDER = 5, W3D_ATT_LSHOULDER = 6,
                   W3D_ATT_HELM = 11, W3D_ATT_BACK = 12, W3D_ATT_SHEATH_MAIN = 26, W3D_ATT_SHEATH_OFF = 27,
                   W3D_ATT_SHEATH_SHIELD = 28, W3D_ATT_LARGE_LEFT = 30, W3D_ATT_LARGE_RIGHT = 31, W3D_ATT_HIP_LEFT = 32,
                   W3D_ATT_HIP_RIGHT = 33 };

#define W3D_DISPLAY_GEOSETS 32
typedef struct {
    uint32_t modelId;             // CMDL id
    float    scale;               // CreatureDisplayInfo scale
    uint32_t tex[5];              // TEX ids for W3D_CTEX_SKIN1..3, BODY, HAIR (0 = none)
    uint16_t geosetCount;         // 0 = every submesh
    uint16_t geosets[W3D_DISPLAY_GEOSETS];
    uint16_t pad;
} W3dDisplay;                     // 96 bytes

// ---------------------------------------------------------------- glue screens (stage 5.29)
// The 3D scenes behind the login screen and the character screens: Interface\Glues\Models\UI_*.m2 converted as one static
// MODL, with the camera and the character's mark the artist put into the model (M2 camera 0, attachment 0). The entry id
// says which screen: W3D_GLUE_MAINMENU, else the race id (1..8) of the character screens (Gnome and Troll share the Dwarf and
// Orc scenes, as in the game). Coordinates are the model's own (drawn at the origin, unrotated).
enum { W3D_GLUE_MAINMENU = 0 };
enum { W3D_GLUE_HAS_STAND = 1, W3D_GLUE_HAS_CAMERA = 2 };
typedef struct {
    uint32_t modelId;             // MODL id, 0 = none
    float    camPos[3], camTarget[3];
    float    fov;                 // the M2 camera's diagonal field of view (radians), 0 = unknown
    float    nearClip, farClip;
    float    stand[3];            // where the character stands
    uint32_t flags;               // W3D_GLUE_HAS_*
    float    radius;              // of the scene's vertices around the origin
    uint32_t reserved[3];
} W3dGlueScene;                   // 76 bytes

// ---------------------------------------------------------------- player characters (stage 5.5)
// A player is drawn with its race model (CMDL) and a body texture composited on the 3DS
// from CharSections layers chosen by PLAYER_BYTES (skin, face, hair style/colour) and
// PLAYER_BYTES_2 (facial hair): base skin, underwear, face lower/upper, facial hair
// lower/upper. The atlas is the game's 256x256 layout at half size (W3D_CHAR_ATLAS);
// each layer is a CIMG entry placed at its region. Hair texture = ordinary TEX entry.
#define W3D_CHAR_ATLAS 128

enum W3dCharSectionKind { W3D_CSEC_SKIN = 0, W3D_CSEC_FACE = 1, W3D_CSEC_FACIAL_HAIR = 2, W3D_CSEC_HAIR = 3,
                          W3D_CSEC_UNDERWEAR = 4 };

typedef struct {
    uint16_t w, h;                // pixels
    uint16_t x, y;                // top-left in the W3D_CHAR_ATLAS atlas
    // followed by w*h uint16 RGBA4 (0xRGBA), rows top to bottom, no tiling
} W3dCharImage;

typedef struct {
    uint8_t  kind;                // W3dCharSectionKind
    uint8_t  variation;           // face / facial hair / hair style (0 for skin, underwear)
    uint8_t  color;               // skin colour (skin, face, underwear) or hair colour
    uint8_t  pad;
    uint32_t img[2];              // CIMG ids, 0 = none. skin: base; face: lower, upper; facial hair: lower,
                                  // upper; underwear: pelvis, torso; hair: img[0] = TEX id
} W3dCharSection;                 // 12 bytes

typedef struct {
    uint8_t  kind;                // 0 = hair style (CharHairGeosets), 1 = facial hair (CharacterFacialHairStyles)
    uint8_t  variation;
    uint16_t g[3];                // hair: g[0] = scalp geoset (1 = bald); facial: 1xx, 2xx, 3xx variants (0..99)
} W3dCharStyle;                   // 8 bytes

typedef struct {
    uint32_t race, sex;           // ChrRaces id, 0 male / 1 female
    uint32_t modelId;             // CMDL id of the race model
    float    scale;               // CreatureDisplayInfo scale of the race's native display (1 if unknown)
    uint32_t sectionCount, styleCount;
    uint32_t offSections, offStyles;
} W3dCharHeader;

static inline uint32_t w3d_char_id(uint32_t race, uint32_t sex) { return race * 2u + (sex & 1u); }


// ---------------------------------------------------------------- worn items (stage 5.19)
// ItemDisplayInfo rows converted for the 3DS. A player's body texture gets the armour painted into the same
// 128x128 atlas as the skin (CIMG layers, placed by region, composited in slot order); the model parts are
// ordinary static MODL entries (baked with the display's texture) hung on the character model's attachments.
//   region   atlas rect at 256x256 (the game's)      halved here
//   0 arm upper   (  0,   0, 128, 64)
//   1 arm lower   (  0,  64, 128, 64)
//   2 hand        (  0, 128, 128, 32)
//   3 torso upper (128,   0, 128, 64)
//   4 torso lower (128,  64, 128, 32)
//   5 leg upper   (128,  96, 128, 64)
//   6 leg lower   (128, 160, 128, 64)
//   7 foot        (128, 224, 128, 32)
#define W3D_ITEM_REGIONS 8
#define W3D_ITEM_HELMS 16            // helmet models per race / sex: index (race - 1) * 2 + sex, races 1..8

// Equipment slots of the player (PLAYER_VISIBLE_ITEM / inventory slot numbers).
enum W3dEquipSlot { W3D_EQ_HEAD = 0, W3D_EQ_NECK = 1, W3D_EQ_SHOULDERS = 2, W3D_EQ_SHIRT = 3, W3D_EQ_CHEST = 4,
                    W3D_EQ_WAIST = 5, W3D_EQ_LEGS = 6, W3D_EQ_FEET = 7, W3D_EQ_WRISTS = 8, W3D_EQ_HANDS = 9,
                    W3D_EQ_FINGER1 = 10, W3D_EQ_FINGER2 = 11, W3D_EQ_TRINKET1 = 12, W3D_EQ_TRINKET2 = 13,
                    W3D_EQ_BACK = 14, W3D_EQ_MAINHAND = 15, W3D_EQ_OFFHAND = 16, W3D_EQ_RANGED = 17,
                    W3D_EQ_TABARD = 18, W3D_EQ_SLOTS = 19 };

typedef struct {
    uint8_t  gg[3];               // ItemDisplayInfo GeosetGroup1..3 (0 = leaves the group alone), clamped to 0..20
    uint8_t  hideHair;            // helmet: bit 0 hides a man's hair, bit 1 a woman's
    uint32_t img[2][W3D_ITEM_REGIONS];   // [sex][region] CIMG ids (0 = none): <name>_M / _F, else _U, else the bare name
    uint32_t cape[2];             // TEX ids of the cloak texture [sex] (0 = none)
    uint32_t model[2];            // MODL ids: [0] the weapon / shield / left shoulder (the left model, else the right),
                                  // [1] the right shoulder
    uint32_t helm[W3D_ITEM_HELMS];   // MODL ids of the helmet per race / sex (0 = no model for that body)
} W3dItemDisplay;                 // 148 bytes

static inline int w3d_item_valid(const uint8_t* blob, uint32_t size) { (void)blob; return size >= sizeof(W3dItemDisplay); }

// Entry id of a tile's TERR / PLCM / WATR / COLL / LIQD entries.
static inline uint32_t w3d_tile_id(uint32_t tileX, uint32_t tileY) { return tileX * 64u + tileY; }

// ---------------------------------------------------------------- helpers
static inline uint32_t w3d_fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '\\') c = '/';
        h ^= (uint8_t)c;
        h *= 16777619u;
    }
    return h;
}

// Bytes taken by one mip level in GPU layout.
static inline uint32_t w3d_tex_level_size(uint32_t w, uint32_t h, uint8_t fmt) {
    switch (fmt) {
        case W3D_TEX_ETC1:   return w * h / 2;
        case W3D_TEX_ETC1A4: return w * h;
        case 5:              return w * h;   // W3D_TEX_A8 (w3d_ui.h)
        default:             return w * h * 2;
    }
}
