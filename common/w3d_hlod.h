// w3d_hlod.h — 6.57: the horizon of a map (HLOD): map<ID>.hlod next to the map pack, made by `w3docc --hlod` from the packs.
//
// Each tile is cut into 4 x 4 blocks (133 yd). A block's buildings (WMO groups that are drawn from afar: outside groups and interior
// groups with portals) and its large doodads are merged into one coarse mesh (vertex clustering, two levels), coloured from their
// textures (a palette, as the far buildings' shells) - one draw instead of tens or hundreds. The client draws a block's mesh when the
// block is farther than the switch distance (and its members - the placements it stands for - are then skipped), and also past the
// draw distance up to the horizon, where the tile itself is not loaded at all.
//
// Layout:
//   W3dHlodHeader
//   palette: palBytes of W3D_TEX_RGB565 texels in the GPU's tiled layout, palW x palH (colour c: the 4 x 4 cell at
//            ((c % (palW / 4)) * 4, (c / (palW / 4)) * 4), as W3D_FAROBJ_CELL)
//   W3dHlodTile[tileCount] at tileTableOff, sorted by (tx, ty)
//   per tile, at dataOff (16-aligned), dataSize bytes - read in one piece, kept as is in linear memory by the client:
//     uint32_t blockCount, pad[3]; W3dHlodBlock[blockCount]; then the blocks' vertices (16-aligned), indices and members at offsets
//     from the start of the tile's data.
// Vertices: W3dTerrainVertex (x, y, z = (position - block origin) / W3D_HLOD_POS_UNIT, u / v into the palette in W3D_FAR_UV_SCALE
// units, r = b = light (w3d_far_light), g = a = 0) - the far ground's shader draws them, both sides (culling off).
#pragma once
#include <stdint.h>

#define W3D_HLOD_MAGIC 0x31444C48u   // "HLD1"
#define W3D_HLOD_VERSION 2u          // v2 (6.57d): three levels, much coarser (v1 had 2100 triangles a block: more than the real ones)
#define W3D_HLOD_BLOCKS 4            // blocks per tile side
#define W3D_HLOD_POS_UNIT (1.0f / 16.0f)   // yards per vertex unit (+-2048 yd around the block's origin)
#define W3D_HLOD_LEVELS 3

typedef struct {
    uint32_t magic, version, buildId;
    float switchDist;             // the builder's: level 0 is made for a camera this far from the block (the client's default switch)
    float levelDist[W3D_HLOD_LEVELS - 1];   // level k + 1 from levelDist[k] on
    float cell[W3D_HLOD_LEVELS];  // the clustering grid of each level (yards), for the report
    uint32_t tileCount, tileTableOff;
    uint16_t palW, palH;
    uint32_t palOff, palBytes;
    uint32_t pad[2];
} W3dHlodHeader;                  // 64 bytes

typedef struct {
    int32_t tx, ty;               // tile indices (as the pack's: tile id = w3d_tile_id(tx, ty))
    uint32_t dataOff, dataSize;
} W3dHlodTile;                    // 16 bytes

typedef struct {
    uint8_t bx, by;               // block in the tile (0..3)
    uint16_t flags;
    float origin[3];              // render space: vertices are relative to it
    float bmin[3], bmax[3];       // render-space bounds of the members (the switch looks at the nearest point of this box)
    uint32_t vertOff[W3D_HLOD_LEVELS], vertCount[W3D_HLOD_LEVELS];   // offsets from the tile's data
    uint32_t idxOff[W3D_HLOD_LEVELS], idxCount[W3D_HLOD_LEVELS];     // uint16 indices into that level's vertices
    uint32_t memberOff, memberCount;   // uint32 PLCM indices of the tile: the placements this block stands for
} W3dHlodBlock;                   // 96 bytes

// The blocks of a tile's data checked against its size; 0: damaged.
static inline int w3d_hlod_tile_valid(const uint8_t* d, uint32_t size) {
    if (!d || size < 16) return 0;
    const uint32_t n = *(const uint32_t*)d;
    if (n > W3D_HLOD_BLOCKS * W3D_HLOD_BLOCKS || 16ull + n * sizeof(W3dHlodBlock) > size) return 0;
    const W3dHlodBlock* b = (const W3dHlodBlock*)(d + 16);
    for (uint32_t i = 0; i < n; ++i) {
        for (int l = 0; l < W3D_HLOD_LEVELS; ++l) {
            if ((b[i].vertOff[l] & 15) || (uint64_t)b[i].vertOff[l] + b[i].vertCount[l] * 16ull > size || b[i].vertCount[l] > 65535) return 0;
            if ((b[i].idxOff[l] & 1) || (uint64_t)b[i].idxOff[l] + b[i].idxCount[l] * 2ull > size) return 0;
            const uint16_t* ix = (const uint16_t*)(d + b[i].idxOff[l]);
            for (uint32_t k = 0; k < b[i].idxCount[l]; ++k) if (ix[k] >= b[i].vertCount[l]) return 0;
        }
        if ((b[i].memberOff & 3) || (uint64_t)b[i].memberOff + b[i].memberCount * 4ull > size) return 0;
    }
    return 1;
}
static inline const W3dHlodBlock* w3d_hlod_blocks(const uint8_t* d) { return (const W3dHlodBlock*)(d + 16); }
static inline uint32_t w3d_hlod_block_count(const uint8_t* d) { return *(const uint32_t*)d; }
