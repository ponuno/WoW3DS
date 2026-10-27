// w3d_liquid.h — liquid queries on LIQD entries (stage 5.26; shared by the 3DS client, the converter and PC tests).
#pragma once
#include <stdint.h>
#include <string.h>

#include "w3d_format.h"

typedef struct {
    float   level;                // surface height at the point
    uint8_t flags;                // W3D_LIQ_* of the cell (kind, deep, building)
} W3dLiquidHit;

static inline int w3d_liquid_kind(uint8_t flags) { return flags & W3D_LIQ_KIND; }

// The blob is sound enough to query (bounds of every table checked once, after reading it).
static inline int w3d_liquid_valid(const uint8_t* blob, uint32_t size) {
    if (!blob || size < sizeof(W3dLiquidHeader)) return 0;
    W3dLiquidHeader h;
    memcpy(&h, blob, sizeof h);
    if (h.chunkCount > 256) return 0;
    if (h.offChunkIndex > size || size - h.offChunkIndex < 256 * 2) return 0;
    if (h.offChunks > size || (size - h.offChunks) / sizeof(W3dLiquidChunk) < h.chunkCount) return 0;
    const uint16_t* index = (const uint16_t*)(blob + h.offChunkIndex);
    for (int c = 0; c < 256; ++c)
        if (index[c] != 0xFFFF && index[c] >= h.chunkCount) return 0;
    const W3dLiquidChunk* ch = (const W3dLiquidChunk*)(blob + h.offChunks);
    for (uint32_t i = 0; i < h.chunkCount; ++i) {
        if (ch[i].offHeights == W3D_LIQ_FLAT) continue;
        const uint32_t off = ch[i].offHeights & ~W3D_LIQ_PERCELL, need = (ch[i].offHeights & W3D_LIQ_PERCELL) ? 64 * 4 * 2 : 81 * 2;
        if (off > size || size - off < need || (off & 1)) return 0;
    }
    return 1;
}

// Liquid at render-space (x, y): 1 and *out when a wet cell of this tile covers the point, else 0.
static inline int w3d_liquid_at(const uint8_t* blob, float x, float y, W3dLiquidHit* out) {
    const W3dLiquidHeader* h = (const W3dLiquidHeader*)blob;
    const float fx = (h->maxX - x) / W3D_LIQ_CELL, fy = (h->maxY - y) / W3D_LIQ_CELL;
    if (!(fx >= 0.0f && fy >= 0.0f && fx < 128.0f && fy < 128.0f)) return 0;
    const int gx = (int)fx, gy = (int)fy;
    const uint16_t* index = (const uint16_t*)(blob + h->offChunkIndex);
    const uint16_t ci = index[(gx >> 3) * 16 + (gy >> 3)];
    if (ci == 0xFFFF) return 0;
    const W3dLiquidChunk* c = (const W3dLiquidChunk*)(blob + h->offChunks) + ci;
    const int r = gx & 7, k = gy & 7;
    const uint8_t f = c->cell[r * 8 + k];
    if (f == W3D_LIQ_DRY) return 0;
    float level = c->base;
    if (c->offHeights != W3D_LIQ_FLAT) {
        const int16_t* hh = (const int16_t*)(blob + (c->offHeights & ~W3D_LIQ_PERCELL));
        const float u = fx - (float)gx, v = fy - (float)gy;   // within the cell: u along rows, v along columns
        float h00, h01, h10, h11;
        if (c->offHeights & W3D_LIQ_PERCELL) {
            const int16_t* q = hh + (r * 8 + k) * 4;
            h00 = q[0]; h01 = q[1]; h10 = q[2]; h11 = q[3];
        } else {
            h00 = hh[r * 9 + k]; h01 = hh[r * 9 + k + 1]; h10 = hh[(r + 1) * 9 + k]; h11 = hh[(r + 1) * 9 + k + 1];
        }
        level += ((h00 * (1 - v) + h01 * v) * (1 - u) + (h10 * (1 - v) + h11 * v) * u) * (1.0f / 32.0f);
    }
    out->level = level;
    out->flags = f;
    return 1;
}

// Standing / swimming thresholds of the client (yards, water surface minus feet):
//   deeper than W3D_SWIM_DEPTH: swimming; the swimmer floats with the feet that far under the surface;
//   a swimmer whose feet touch a bottom shallower than W3D_SWIM_EXIT stands up and wades.
#define W3D_SWIM_DEPTH 1.45f
#define W3D_SWIM_EXIT  1.25f
