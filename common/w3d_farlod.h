// w3d_farlod.h — the far ground of a map (FARL entry, stage 6.24), shared by the converter, the 3DS client and the PC tests.
//
// Like the original client's WDL ground: a coarse height grid for EVERY tile of the map (17 x 17 heights per tile, at the
// corners of its 16 x 16 chunks, 33.3 yards apart) and a colour per chunk (a 16 x 16 cell of one atlas texture). It is small
// enough to stay in memory for the whole visit (~600 bytes + 128 texels per tile) and is drawn where the detailed ground of a
// tile is not there: a tile still being read (the holes into the sky in front of the walker), a tile not loaded at all.
//
// Heights come from the tile's ADT when the tile was converted (exact at the chunk corners: no seam against the detailed ground
// there), else from the map's WDL. Colours: the average colours of the chunk's ground textures, blended by the average of their
// alpha maps (a WDL-only tile gets the map's mean ground colour).
//
// Layout of the entry (id 0 of a map pack; a tile entry, so it stays in the map file of a shared pool):
//   W3dFarHeader, then W3dFarTile[tileCount].
// Grid: row i (0..16) = ADT chunk-corner row (render x = tile min x + (1 - i/16) * 533.33), column j = chunk-corner column
// (render y = tile min y + (1 - j/16) * 533.33); z = h * zScale (yards). Chunk (cx, cy) colour = atlas texel (cellX + cx,
// cellY + cy), image rows top-down (row 0 = the top of the image, as every pack texture).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "w3d_format.h"

#define W3D_FAR_MAGIC 0x31524146u   // "FAR1"
#define W3D_FAR_N 17                // heights per tile edge
#define W3D_FAR_VERTS (W3D_FAR_N * W3D_FAR_N)
#define W3D_FAR_INDICES (16 * 16 * 6)
#define W3D_FAR_CELL 16             // atlas texels per tile edge
#define W3D_FAR_TILE_SIZE 533.33333f

#define W3D_FAR_FROM_ADT 1u
#define W3D_FAR_FROM_WDL 2u

typedef struct {
    uint32_t magic;               // W3D_FAR_MAGIC
    uint32_t tileCount;
    uint32_t texId;               // the colour atlas (W3D_ENTRY_TEXTURE)
    uint16_t atlasW, atlasH;      // its size in texels
    float    zScale;              // yards per height unit
    uint16_t index[64 * 64];      // [tileY * 64 + tileX]: 0 none, else 1 + the record number
} W3dFarHeader;                   // 8212 bytes

typedef struct {
    uint8_t  tileX, tileY;
    uint8_t  source;              // W3D_FAR_FROM_ADT / W3D_FAR_FROM_WDL
    uint8_t  pad;
    uint16_t cellX, cellY;        // atlas texel of chunk (0, 0)
    int16_t  h[W3D_FAR_VERTS];    // [i * 17 + j]
    uint16_t pad2;
} W3dFarTile;                     // 588 bytes

static inline int w3d_far_valid(const uint8_t* blob, uint32_t size) {
    if (!blob || size < sizeof(W3dFarHeader)) return 0;
    const W3dFarHeader* h = (const W3dFarHeader*)blob;
    if (h->magic != W3D_FAR_MAGIC || !(h->zScale > 0.0f) || h->tileCount > 4096) return 0;
    if (size < sizeof(W3dFarHeader) + (uint64_t)h->tileCount * sizeof(W3dFarTile)) return 0;
    for (int k = 0; k < 64 * 64; ++k)
        if (h->index[k] > h->tileCount) return 0;
    return 1;
}

static inline const W3dFarTile* w3d_far_tile(const uint8_t* blob, int tx, int ty) {
    if (tx < 0 || ty < 0 || tx > 63 || ty > 63) return 0;
    const W3dFarHeader* h = (const W3dFarHeader*)blob;
    const uint16_t k = h->index[ty * 64 + tx];
    if (!k) return 0;
    return (const W3dFarTile*)(blob + sizeof(W3dFarHeader)) + (k - 1);
}

// Tile min corner in render space (as the TERR / GRND entries of the same tile).
static inline void w3d_far_tile_origin(int tx, int ty, float* minX, float* minY) {
    *minX = (32.0f - (float)ty - 1.0f) * W3D_FAR_TILE_SIZE;
    *minY = (32.0f - (float)tx - 1.0f) * W3D_FAR_TILE_SIZE;
}

// The ground light the converter bakes into terrain vertices (terrain.cpp bakeLight): towards the sun (0.30, 0.40, 0.866),
// ambient 0.5, diffuse 0.6.
static inline uint8_t w3d_far_light(float nx, float ny, float nz) {
    float d = nx * 0.30f + ny * 0.40f + nz * 0.866f;
    if (d < 0) d = 0;
    float v = 0.50f + 0.60f * d;
    if (v > 1.0f) v = 1.0f;
    return (uint8_t)(v * 255.0f + 0.5f);
}

// Draw constants of a tile for the terrain shader: position = s16 * scale + offset (tile-local, the model-view translates to
// the tile origin), uv = s16 * uvScale.
#define W3D_FAR_XY_SCALE (W3D_FAR_TILE_SIZE / 32767.0f)
#define W3D_FAR_UV_SCALE (1.0f / 32767.0f)
#define W3D_FAR_SINK 0.6f           // yards below the true heights: where it meets the detailed ground, the detailed one wins

// The 289 terrain vertices of a far tile (W3dTerrainVertex: r = baked light, a = 0 so both texture units show the atlas).
static inline void w3d_far_build_vertices(const W3dFarHeader* hd, const W3dFarTile* t, W3dTerrainVertex* out) {
    const float q = W3D_FAR_TILE_SIZE / 16.0f;
    for (int i = 0; i < W3D_FAR_N; ++i)
        for (int j = 0; j < W3D_FAR_N; ++j) {
            W3dTerrainVertex* vp = &out[i * W3D_FAR_N + j];
            const float lx = (1.0f - (float)i / 16.0f) * W3D_FAR_TILE_SIZE, ly = (1.0f - (float)j / 16.0f) * W3D_FAR_TILE_SIZE;
            vp->x = (int16_t)lroundf(lx / W3D_FAR_XY_SCALE);
            vp->y = (int16_t)lroundf(ly / W3D_FAR_XY_SCALE);
            vp->z = t->h[i * W3D_FAR_N + j];
            vp->w = 0;
            // normal from the neighbours (one-sided at the edges); x falls with i, y with j
            const int i0 = i > 0 ? i - 1 : i, i1 = i < 16 ? i + 1 : i, j0 = j > 0 ? j - 1 : j, j1 = j < 16 ? j + 1 : j;
            const float dzdi = (float)(t->h[i1 * W3D_FAR_N + j] - t->h[i0 * W3D_FAR_N + j]) * hd->zScale / ((float)(i1 - i0) * q);
            const float dzdj = (float)(t->h[i * W3D_FAR_N + j1] - t->h[i * W3D_FAR_N + j0]) * hd->zScale / ((float)(j1 - j0) * q);
            const float gx = -dzdi, gy = -dzdj;   // dz/dx, dz/dy
            const float inv = 1.0f / sqrtf(1.0f + gx * gx + gy * gy);
            const uint8_t light = w3d_far_light(-gx * inv, -gy * inv, inv);
            vp->r = light; vp->g = 0; vp->b = light; vp->a = 0;
            // texel centres of the tile's cell: vertex 0 -> centre of texel 0, vertex 16 -> centre of texel 15 (never the
            // neighbouring cell of the atlas); PICA t = 1 is the top row of the image
            const float s = (float)t->cellX + 0.5f + (float)j * (15.0f / 16.0f);
            const float r = (float)t->cellY + 0.5f + (float)i * (15.0f / 16.0f);
            const float u = s / (float)hd->atlasW, tt = 1.0f - r / (float)hd->atlasH;
            vp->u = (int16_t)lroundf(u / W3D_FAR_UV_SCALE);
            vp->v = (int16_t)lroundf(tt / W3D_FAR_UV_SCALE);
        }
}

// The shared index list of every far tile: two triangles per chunk, counter-clockwise seen from above (as the converter's
// terrain): (i, j) -> (x, y) negates both axes, a turn by 180 degrees, so (i, j)-CCW stays CCW.
static inline void w3d_far_build_indices(uint16_t* out) {
    int k = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) {
            const uint16_t a = (uint16_t)(i * W3D_FAR_N + j), b = (uint16_t)((i + 1) * W3D_FAR_N + j);
            const uint16_t c = (uint16_t)(i * W3D_FAR_N + j + 1), d = (uint16_t)((i + 1) * W3D_FAR_N + j + 1);
            out[k++] = a; out[k++] = b; out[k++] = c;
            out[k++] = b; out[k++] = d; out[k++] = c;
        }
}

// The height of the far ground at render (x, y) (the tests; bilinear over the 33-yard cell). 0 outside the map's tiles.
static inline int w3d_far_height(const uint8_t* blob, float x, float y, float* z) {
    const W3dFarHeader* hd = (const W3dFarHeader*)blob;
    const int ty = (int)floorf(32.0f - x / W3D_FAR_TILE_SIZE), tx = (int)floorf(32.0f - y / W3D_FAR_TILE_SIZE);
    const W3dFarTile* t = w3d_far_tile(blob, tx, ty);
    if (!t) return 0;
    float minX, minY;
    w3d_far_tile_origin(tx, ty, &minX, &minY);
    float fi = (1.0f - (x - minX) / W3D_FAR_TILE_SIZE) * 16.0f, fj = (1.0f - (y - minY) / W3D_FAR_TILE_SIZE) * 16.0f;
    if (fi < 0) fi = 0;
    if (fj < 0) fj = 0;
    if (fi > 16) fi = 16;
    if (fj > 16) fj = 16;
    int i = (int)fi, j = (int)fj;
    if (i > 15) i = 15;
    if (j > 15) j = 15;
    const float a = fi - i, b = fj - j;
    const float h00 = t->h[i * 17 + j], h01 = t->h[i * 17 + j + 1], h10 = t->h[(i + 1) * 17 + j], h11 = t->h[(i + 1) * 17 + j + 1];
    *z = ((h00 * (1 - b) + h01 * b) * (1 - a) + (h10 * (1 - b) + h11 * b) * a) * hd->zScale;
    return 1;
}
