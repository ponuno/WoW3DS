// w3d_ground.h — the exact ground of a tile (GRND entry, stage 6.18a), shared by the 3DS client, the converter and PC tests.
//
// An ADT chunk (MCNK) holds 145 heights: a 9x9 grid of corners with a centre vertex in every one of the 8x8 squares
// (heightMap.heights: corner (row, col) = row*17 + col, centre of square (row, col) = row*17 + 9 + col). The game draws
// every square as four triangles around the centre, so the ground a walker stands on is those triangles. The old client
// field (a 129x129 bilinear grid built from the drawn corners) dropped the centres: up to a yard off on rough ground.
//
// Layout (render space, as the TERR entry of the same tile):
//   gridX = cy*8 + row (continuous, 0..128)  ->  x = origin.x + (1 - gridX/128) * 533.333
//   gridY = cx*8 + col                       ->  y = origin.y + (1 - gridY/128) * 533.333
// Square (r, c) of a chunk: corners A=(r,c) B=(r,c+1) C=(r+1,c) D=(r+1,c+1), centre M; triangles (M,A,B) (M,B,D) (M,D,C) (M,C,A).
// Hole bit of a square: (r/2)*4 + (c/2) of the chunk's `holes` (WoWee ADTChunk::isHole(r, c)).
#pragma once
#include <math.h>
#include <stdint.h>

#include "w3d_collision.h"   // W3D_WALK_MIN_NZ
#include "w3d_format.h"

#define W3D_GROUND_CHUNK_PRESENT 1u
#define W3D_GROUND_TILE_SIZE 533.33333f

typedef struct {
    uint16_t h[145];              // z = origin[2] + h * zScale
    uint16_t holes;               // MCNK hole mask
    uint16_t flags;               // W3D_GROUND_CHUNK_PRESENT (a chunk without a height map has none)
} W3dGroundChunk;                 // 294 bytes

typedef struct {
    uint32_t tileX, tileY;
    float    origin[3];           // tile min corner (x, y) and the z the heights are relative to
    float    zScale;
    uint32_t chunkCount;          // 256, chunk (cx, cy) at cy*16 + cx, W3dGroundChunk[] right after this header
    uint32_t pad;
} W3dGroundHeader;                // 32 bytes

typedef struct {
    float z;                      // height of the ground at the point
    float nz;                     // z of the unit normal of the triangle (1 = flat)
    float gx, gy;                 // dz/dx, dz/dy of the triangle (render space): uphill is (gx, gy)
    int   hole;                   // the square is in an ADT hole
} W3dGroundHit;

static inline int w3d_ground_valid(const uint8_t* blob, uint32_t size) {
    if (!blob || size < sizeof(W3dGroundHeader)) return 0;
    const W3dGroundHeader* h = (const W3dGroundHeader*)blob;
    if (h->chunkCount != 256 || !(h->zScale > 0.0f)) return 0;
    return size >= sizeof(W3dGroundHeader) + 256ull * sizeof(W3dGroundChunk);
}

// The ground at render (x, y). Returns 0 where there is none (outside the tile, or in a chunk without a height map).
static inline int w3d_ground_at(const uint8_t* blob, float x, float y, W3dGroundHit* out) {
    const W3dGroundHeader* hd = (const W3dGroundHeader*)blob;
    const float TS = W3D_GROUND_TILE_SIZE, q = TS / 128.0f;
    float lx = x - hd->origin[0], ly = y - hd->origin[1];
    if (lx < -0.01f || ly < -0.01f || lx > TS + 0.01f || ly > TS + 0.01f) return 0;
    float gX = (1.0f - lx / TS) * 128.0f, gY = (1.0f - ly / TS) * 128.0f;   // row / column in squares, tile-wide
    if (gX < 0) gX = 0;
    if (gY < 0) gY = 0;
    if (gX > 128.0f) gX = 128.0f;
    if (gY > 128.0f) gY = 128.0f;
    int cy = (int)(gX / 8.0f), cx = (int)(gY / 8.0f);
    if (cy > 15) cy = 15;
    if (cx > 15) cx = 15;
    const W3dGroundChunk* ch = (const W3dGroundChunk*)(blob + sizeof(W3dGroundHeader)) + cy * 16 + cx;
    if (!(ch->flags & W3D_GROUND_CHUNK_PRESENT)) return 0;
    float row = gX - cy * 8.0f, col = gY - cx * 8.0f;   // 0..8
    int r = (int)row, c = (int)col;
    if (r > 7) r = 7;
    if (c > 7) c = 7;
    const float fr = row - r - 0.5f, fc = col - c - 0.5f;   // from the square's centre, -0.5..0.5 (rows, columns)
    const float zs = hd->zScale, z0 = hd->origin[2];
    const float zM = z0 + ch->h[r * 17 + 9 + c] * zs;
    const float zA = z0 + ch->h[r * 17 + c] * zs, zB = z0 + ch->h[r * 17 + c + 1] * zs;
    const float zC = z0 + ch->h[(r + 1) * 17 + c] * zs, zD = z0 + ch->h[(r + 1) * 17 + c + 1] * zs;
    // The triangle (M, P, Q): P and Q as offsets from the centre in (row, column) units.
    float pr, pc, qr, qc, zP, zQ;
    if (fabsf(fc) > fabsf(fr)) {
        if (fc > 0) { pr = -0.5f; pc = 0.5f; zP = zB; qr = 0.5f; qc = 0.5f; zQ = zD; }     // right (M, B, D)
        else        { pr = 0.5f; pc = -0.5f; zP = zC; qr = -0.5f; qc = -0.5f; zQ = zA; }   // left (M, C, A)
    } else {
        if (fr < 0) { pr = -0.5f; pc = -0.5f; zP = zA; qr = -0.5f; qc = 0.5f; zQ = zB; }   // top (M, A, B)
        else        { pr = 0.5f; pc = 0.5f; zP = zD; qr = 0.5f; qc = -0.5f; zQ = zC; }     // bottom (M, D, C)
    }
    const float det = pr * qc - qr * pc;   // = +-0.5 for every one of the four
    const float a = (fr * qc - qr * fc) / det, b = (pr * fc - fr * pc) / det;
    const float dP = zP - zM, dQ = zQ - zM;
    out->z = zM + a * dP + b * dQ;
    const float dzdr = (dP * qc - dQ * pc) / det, dzdc = (-dP * qr + dQ * pr) / det;   // per square
    out->gx = -dzdr / q;   // render x falls as the row grows
    out->gy = -dzdc / q;
    out->nz = 1.0f / sqrtf(1.0f + out->gx * out->gx + out->gy * out->gy);
    out->hole = (ch->holes >> ((r / 2) * 4 + (c / 2))) & 1;
    return 1;
}

static inline int w3d_ground_height(const uint8_t* blob, float x, float y, float* z, int* inHole) {
    W3dGroundHit hit;
    if (!w3d_ground_at(blob, x, y, &hit)) return 0;
    *z = hit.z;
    if (inHole) *inHole = hit.hole;
    return 1;
}

// ---------------------------------------------------------------- slope limit (stage 6.18c)
// The retail client does not let a walker climb ground steeper than 50 degrees (W3D_WALK_MIN_NZ, w3d_collision.h). A move that
// ends on a steeper triangle of the terrain and goes uphill in it loses its uphill part: the walker slides along the slope
// (the contour), never up it. Downhill and sideways are free (nobody gets stuck on a slope they stand on). Only the terrain
// counts: feet more than 'step' above the ground there are on a floor or in the air, and the move is left alone.
// gFrom / gTo = the GRND blobs of the tiles at the start and the end of the move (either may be null: no limit then).
// Returns 1 if the target was changed.
static inline int w3d_slope_move(const uint8_t* gFrom, const uint8_t* gTo, float fromX, float fromY, float zFeet, float step,
                                 float* toX, float* toY) {
    if (!gTo) return 0;
    W3dGroundHit b;
    if (!w3d_ground_at(gTo, *toX, *toY, &b) || b.hole) return 0;
    if (b.nz >= W3D_WALK_MIN_NZ) return 0;   // walkable
    if (zFeet > b.z + step) return 0;          // standing on a floor or in the air above it
    float dx = *toX - fromX, dy = *toY - fromY;
    float gl = sqrtf(b.gx * b.gx + b.gy * b.gy);
    if (gl < 1e-6f) return 0;
    const float ux = b.gx / gl, uy = b.gy / gl;          // uphill, horizontal
    const float up = dx * ux + dy * uy;
    if (up <= 1e-5f) return 0;                           // downhill or along the contour
    W3dGroundHit a;
    if (gFrom && w3d_ground_at(gFrom, fromX, fromY, &a) && !a.hole && a.z >= b.z - 0.001f) return 0;   // (not actually rising)
    *toX = fromX + dx - up * ux;
    *toY = fromY + dy - up * uy;
    return 1;
}
