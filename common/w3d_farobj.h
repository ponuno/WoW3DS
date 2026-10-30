// w3d_farobj.h — the far buildings of a map (FARO entry, stage 6.29), shared by the converter, the 3DS client and the PC tests.
//
// WoW 1.12 has no far versions of its buildings: they appear only when their tile and their groups are read, in front of the
// walker. The converter makes one for every large building (WMO) itself: a "shell" - the outside groups' surface clustered on a
// coarse grid down to a few hundred triangles, each vertex coloured with the mean colour of the textures around it. The client
// keeps the whole entry in memory for the visit and draws the shells of a tile wherever the real buildings are not shown (the
// tile is not loaded yet, or a building of it is still being read), with the far ground's pass (terrain shader, the palette
// texture on both units, vertex a = 0, r = baked light).
//
// Layout of the entry (id 0 of a map pack; a tile entry, so it stays in the map file of a shared pool):
//   W3dFarObjHeader, W3dFarShell[shellCount], W3dFarShellVert[vertCount], uint16_t indices[indexCount] (per shell, 0-based,
//   counter-clockwise from outside), W3dFarObject[objectCount] (sorted by tile: tileStart[k] .. tileStart[k + 1] are the
//   objects of tile k = tileY * 64 + tileX - the tile whose PLCM holds the building's groups).
// Shell vertices are in the building's own space (WMO space, yards * W3D_FAROBJ_POS_UNIT); an object places it with the same
// turn as its groups' placements: world = pos + Rz(rot[2]) * Ry(rot[1]) * Rx(rot[0]) * local.
// Palette: a W3D_TEX_RGB565 texture of palW x palH texels, colour c is the 4 x 4 cell at ((c % (palW / 4)) * 4, (c / (palW / 4)) * 4).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "w3d_farlod.h"

#define W3D_FAROBJ_MAGIC 0x314F4146u   // "FAO1"
#define W3D_FAROBJ_POS_UNIT 16.0f      // shell coordinates: 1/16 yard (+-2048 yd)
#define W3D_FAROBJ_CELL 4              // palette texels per colour edge
// Drawn vertices: x / y from the tile's min corner in these units (+-2133 yd: a building of the tile may reach far beyond it),
// z in the far ground's zScale units
#define W3D_FAROBJ_XY_SCALE (4.0f * W3D_FAR_TILE_SIZE / 32767.0f)

typedef struct {
    uint32_t magic;               // W3D_FAROBJ_MAGIC
    uint32_t shellCount, vertCount, indexCount, objectCount;
    uint32_t paletteTexId;        // W3D_ENTRY_TEXTURE (RGB565)
    uint16_t palW, palH;
    uint32_t tileStart[64 * 64 + 1];
} W3dFarObjHeader;

typedef struct {
    uint32_t firstVert, firstIndex;
    uint16_t vertCount, indexCount;
    float radius;                 // around the building's origin (yards)
} W3dFarShell;                    // 16 bytes

typedef struct {
    int16_t x, y, z;              // / W3D_FAROBJ_POS_UNIT yards
    int8_t nx, ny, nz;            // normal * 127
    uint8_t pad;
    uint16_t colour;              // palette index
} W3dFarShellVert;                // 12 bytes

typedef struct {
    uint32_t shell;
    float pos[3];                 // render space
    float rot[3];                 // W3D placement turn (radians)
    uint32_t groupModel;          // the MODL id of the building's first group: the client hides the shell once that building is shown
    float groupPos[3];            // ... and that group's placed position (render space)
} W3dFarObject;                   // 44 bytes

static inline uint32_t w3d_farobj_offset_shells(void) { return (uint32_t)sizeof(W3dFarObjHeader); }
static inline uint32_t w3d_farobj_offset_verts(const W3dFarObjHeader* h) { return w3d_farobj_offset_shells() + h->shellCount * (uint32_t)sizeof(W3dFarShell); }
static inline uint32_t w3d_farobj_offset_indices(const W3dFarObjHeader* h) { return w3d_farobj_offset_verts(h) + h->vertCount * (uint32_t)sizeof(W3dFarShellVert); }
static inline uint32_t w3d_farobj_offset_objects(const W3dFarObjHeader* h) {
    return (w3d_farobj_offset_indices(h) + h->indexCount * 2u + 3u) & ~3u;
}
static inline uint32_t w3d_farobj_size(const W3dFarObjHeader* h) { return w3d_farobj_offset_objects(h) + h->objectCount * (uint32_t)sizeof(W3dFarObject); }

static inline const W3dFarShell* w3d_farobj_shells(const uint8_t* b) { return (const W3dFarShell*)(b + w3d_farobj_offset_shells()); }
static inline const W3dFarShellVert* w3d_farobj_verts(const uint8_t* b) { return (const W3dFarShellVert*)(b + w3d_farobj_offset_verts((const W3dFarObjHeader*)b)); }
static inline const uint16_t* w3d_farobj_indices(const uint8_t* b) { return (const uint16_t*)(b + w3d_farobj_offset_indices((const W3dFarObjHeader*)b)); }
static inline const W3dFarObject* w3d_farobj_objects(const uint8_t* b) { return (const W3dFarObject*)(b + w3d_farobj_offset_objects((const W3dFarObjHeader*)b)); }

static inline int w3d_farobj_valid(const uint8_t* b, uint32_t size) {
    if (!b || size < sizeof(W3dFarObjHeader)) return 0;
    const W3dFarObjHeader* h = (const W3dFarObjHeader*)b;
    if (h->magic != W3D_FAROBJ_MAGIC || h->shellCount > 65535 || h->vertCount > 16u << 20 || h->indexCount > 32u << 20 ||
        h->objectCount > 1u << 20 || !h->palW || !h->palH)
        return 0;
    if (size < w3d_farobj_size(h)) return 0;
    if (h->tileStart[64 * 64] != h->objectCount) return 0;
    for (int k = 0; k < 64 * 64; ++k)
        if (h->tileStart[k] > h->tileStart[k + 1]) return 0;
    const W3dFarShell* s = w3d_farobj_shells(b);
    for (uint32_t i = 0; i < h->shellCount; ++i)
        if ((uint64_t)s[i].firstVert + s[i].vertCount > h->vertCount || (uint64_t)s[i].firstIndex + s[i].indexCount > h->indexCount)
            return 0;
    const uint16_t* idx = w3d_farobj_indices(b);
    for (uint32_t i = 0; i < h->shellCount; ++i)
        for (uint32_t k = 0; k < s[i].indexCount; ++k)
            if (idx[s[i].firstIndex + k] >= s[i].vertCount) return 0;
    const W3dFarObject* o = w3d_farobj_objects(b);
    for (uint32_t i = 0; i < h->objectCount; ++i)
        if (o[i].shell >= h->shellCount) return 0;
    return 1;
}

// The placement turn (as the renderer's model matrix): R = Rz(c) * Ry(b) * Rx(a).
static inline void w3d_farobj_rotate(const float rot[3], const float v[3], float out[3]) {
    const float ca = cosf(rot[0]), sa = sinf(rot[0]), cb = cosf(rot[1]), sb = sinf(rot[1]), cc = cosf(rot[2]), sc = sinf(rot[2]);
    // x
    const float x1 = v[0], y1 = ca * v[1] - sa * v[2], z1 = sa * v[1] + ca * v[2];
    // y
    const float x2 = cb * x1 + sb * z1, y2 = y1, z2 = -sb * x1 + cb * z1;
    // z
    out[0] = cc * x2 - sc * y2;
    out[1] = sc * x2 + cc * y2;
    out[2] = z2;
}

// A shell vertex placed (render space).
static inline void w3d_farobj_place(const W3dFarObject* o, const W3dFarShellVert* v, float out[3], float n[3]) {
    const float l[3] = {v->x / W3D_FAROBJ_POS_UNIT, v->y / W3D_FAROBJ_POS_UNIT, v->z / W3D_FAROBJ_POS_UNIT};
    w3d_farobj_rotate(o->rot, l, out);
    for (int k = 0; k < 3; ++k) out[k] += o->pos[k];
    if (n) {
        const float ln[3] = {v->nx / 127.0f, v->ny / 127.0f, v->nz / 127.0f};
        w3d_farobj_rotate(o->rot, ln, n);
    }
}

// The terrain vertices of the objects of one tile (x / y in W3D_FAROBJ_XY_SCALE units from the tile's min corner, z in zScale units), and their indices (u16, into this tile's list).
// Returns the vertices written (0: nothing fits in maxVerts / maxIdx). objIndexStart[i] = the first index of object i (count + 1
// entries) so that single buildings can be drawn alone.
static inline uint32_t w3d_farobj_build_tile(const uint8_t* b, int tx, int ty, float zScale, W3dTerrainVertex* vout, uint32_t maxVerts,
                                             uint16_t* iout, uint32_t maxIdx, uint32_t* idxCount, uint32_t* objIndexStart) {
    const W3dFarObjHeader* h = (const W3dFarObjHeader*)b;
    const W3dFarShell* shells = w3d_farobj_shells(b);
    const W3dFarShellVert* sv = w3d_farobj_verts(b);
    const uint16_t* si = w3d_farobj_indices(b);
    const W3dFarObject* objs = w3d_farobj_objects(b);
    const uint32_t k = (uint32_t)(ty * 64 + tx), first = h->tileStart[k], count = h->tileStart[k + 1] - first;
    float minX, minY;
    w3d_far_tile_origin(tx, ty, &minX, &minY);
    const uint32_t cells = h->palW / W3D_FAROBJ_CELL;
    uint32_t nv = 0, ni = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const W3dFarObject* o = &objs[first + i];
        const W3dFarShell* s = &shells[o->shell];
        objIndexStart[i] = ni;
        if (nv + s->vertCount > maxVerts || ni + s->indexCount > maxIdx) { objIndexStart[i] = ni; continue; }
        for (uint32_t v = 0; v < s->vertCount; ++v) {
            float p[3], n[3];
            w3d_farobj_place(o, &sv[s->firstVert + v], p, n);
            W3dTerrainVertex* d = &vout[nv + v];
            long x = lroundf((p[0] - minX) / W3D_FAROBJ_XY_SCALE), y = lroundf((p[1] - minY) / W3D_FAROBJ_XY_SCALE), z = lroundf(p[2] / zScale);
            d->x = (int16_t)(x < -32767 ? -32767 : x > 32767 ? 32767 : x);
            d->y = (int16_t)(y < -32767 ? -32767 : y > 32767 ? 32767 : y);
            d->z = (int16_t)(z < -32767 ? -32767 : z > 32767 ? 32767 : z);
            d->w = 0;
            const uint8_t light = w3d_far_light(n[0], n[1], n[2]);
            d->r = light; d->g = 0; d->b = light; d->a = 0;
            const uint32_t c = sv[s->firstVert + v].colour;
            const float cx = (float)((c % cells) * W3D_FAROBJ_CELL) + W3D_FAROBJ_CELL * 0.5f;
            const float cy = (float)((c / cells) * W3D_FAROBJ_CELL) + W3D_FAROBJ_CELL * 0.5f;
            d->u = (int16_t)lroundf(cx / (float)h->palW / W3D_FAR_UV_SCALE);
            d->v = (int16_t)lroundf((1.0f - cy / (float)h->palH) / W3D_FAR_UV_SCALE);
        }
        for (uint32_t q = 0; q < s->indexCount; ++q) iout[ni + q] = (uint16_t)(nv + si[s->firstIndex + q]);
        nv += s->vertCount;
        ni += s->indexCount;
    }
    objIndexStart[count] = ni;
    *idxCount = ni;
    return nv;
}
