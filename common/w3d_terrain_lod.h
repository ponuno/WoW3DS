// w3d_terrain_lod.h — which terrain LOD each batch of a tile is drawn at (stage 6.30), shared by the 3DS client and the PC
// seam test.
//
// A tile's batches are keyed by (4x4-chunk region, texture triple): one region of mixed ground (Durotar, the Barrens) is split
// into several batches. The converter puts skirts only along region borders, so every batch of a region must be drawn at the
// same LOD: picked per batch (by the distance to its own box, as before 6.30), two chunks side by side in one region got LOD 1
// and LOD 2 and the sky showed through the T-junctions between them - short white dashes on the ground at 40-200 yd.
// Here the LOD comes from the region (the union of its batches' boxes), capped by the coarsest level every batch of the region
// can take (6.13: a coarse level must not lose ground around holes).
#pragma once
#include <math.h>
#include <stdint.h>

#include "w3d_format.h"

#define W3D_TERRAIN_REGIONS 16
#define W3D_TERRAIN_LOD1_DIST 40.0f    // yards from the camera to the region's bounding sphere
#define W3D_TERRAIN_LOD2_DIST 140.0f

typedef struct {
    float bmin[3], bmax[3];   // tile-local, the union of its batches' boxes
    uint8_t maxLod;           // the coarsest LOD all its batches keep their ground at
    uint8_t used;
} W3dTerrainRegion;

// The region (0..15) of a batch: its box lies in one 4x4-chunk square of the tile (tile-local x / y in 0..tile size).
static inline int w3d_terrain_batch_region(const W3dTerrainBatch* b) {
    const float q = 533.33333f / 4.0f;
    int rx = (int)floorf(0.5f * (b->bmin[0] + b->bmax[0]) / q), ry = (int)floorf(0.5f * (b->bmin[1] + b->bmax[1]) / q);
    rx = rx < 0 ? 0 : rx > 3 ? 3 : rx;
    ry = ry < 0 ? 0 : ry > 3 ? 3 : ry;
    return ry * 4 + rx;
}

// The x / y area of a range of triangles (the skirts are vertical and add nothing).
static inline double w3d_terrain_area(const W3dTerrainVertex* v, uint32_t vcount, const uint16_t* idx, uint32_t first, uint32_t count) {
    double a = 0;
    for (uint32_t k = first; k + 2 < first + count; k += 3) {
        const uint16_t i0 = idx[k], i1 = idx[k + 1], i2 = idx[k + 2];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
        const double ax = v[i1].x - v[i0].x, ay = v[i1].y - v[i0].y, bx = v[i2].x - v[i0].x, by = v[i2].y - v[i0].y;
        a += fabs(ax * by - ay * bx);
    }
    return a;
}

// Stage 6.13: per batch, the coarsest LOD that still covers the ground of the finer one (packs before 6.13 dropped every LOD-2
// block touching a hole). batchMaxLod[batchCount].
static inline void w3d_terrain_batch_max_lod(const uint8_t* terr, uint8_t* batchMaxLod) {
    const W3dTerrainHeader* th = (const W3dTerrainHeader*)terr;
    const W3dTerrainBatch* b = (const W3dTerrainBatch*)(terr + th->offBatches);
    const W3dTerrainVertex* v = (const W3dTerrainVertex*)(terr + th->offVertices);
    const uint16_t* idx = (const uint16_t*)(terr + th->offIndices);
    for (uint32_t i = 0; i < th->batchCount; ++i) {
        batchMaxLod[i] = W3D_TERRAIN_LODS - 1;
        double prev = w3d_terrain_area(v, th->vertexCount, idx, b[i].firstIndex[0], b[i].indexCount[0]);
        for (int l = 1; l < W3D_TERRAIN_LODS; ++l) {
            const double a = w3d_terrain_area(v, th->vertexCount, idx, b[i].firstIndex[l], b[i].indexCount[l]);
            if (a < prev * 0.99) { batchMaxLod[i] = (uint8_t)(l - 1); break; }
            prev = a;
        }
    }
}

// The regions of a tile: batchRegion[batchCount], regions[16].
static inline void w3d_terrain_regions(const uint8_t* terr, const uint8_t* batchMaxLod, uint8_t* batchRegion, W3dTerrainRegion* regions) {
    const W3dTerrainHeader* th = (const W3dTerrainHeader*)terr;
    const W3dTerrainBatch* b = (const W3dTerrainBatch*)(terr + th->offBatches);
    for (int r = 0; r < W3D_TERRAIN_REGIONS; ++r) {
        for (int k = 0; k < 3; ++k) { regions[r].bmin[k] = 1e30f; regions[r].bmax[k] = -1e30f; }
        regions[r].maxLod = W3D_TERRAIN_LODS - 1;
        regions[r].used = 0;
    }
    for (uint32_t i = 0; i < th->batchCount; ++i) {
        const int r = w3d_terrain_batch_region(&b[i]);
        batchRegion[i] = (uint8_t)r;
        W3dTerrainRegion* g = &regions[r];
        for (int k = 0; k < 3; ++k) {
            if (b[i].bmin[k] < g->bmin[k]) g->bmin[k] = b[i].bmin[k];
            if (b[i].bmax[k] > g->bmax[k]) g->bmax[k] = b[i].bmax[k];
        }
        if (batchMaxLod[i] < g->maxLod) g->maxLod = batchMaxLod[i];
        g->used = 1;
    }
}

// The LOD of a region for a camera at cam (render space); origin = the tile's origin.
static inline int w3d_terrain_region_lod(const W3dTerrainRegion* g, const float origin[3], const float cam[3]) {
    float r2 = 0, d2 = 0;
    for (int k = 0; k < 3; ++k) {
        const float c = origin[k] + 0.5f * (g->bmin[k] + g->bmax[k]), e = 0.5f * (g->bmax[k] - g->bmin[k]);
        r2 += e * e;
        d2 += (c - cam[k]) * (c - cam[k]);
    }
    const float d = sqrtf(d2) - sqrtf(r2);
    int lod = d < W3D_TERRAIN_LOD1_DIST ? 0 : d < W3D_TERRAIN_LOD2_DIST ? 1 : 2;
    if (lod > g->maxLod) lod = g->maxLod;
    return lod;
}
