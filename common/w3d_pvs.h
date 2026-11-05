// w3d_pvs.h — 6.54: the precomputed visibility of a map (map<ID>.pvs next to map<ID>.w3dp), made by `w3docc --pvs` from the
// finished packs and read by the 3DS client. Shared by both.
//
// Each tile of the computed area is cut into cellsPerSide x cellsPerSide cells (cellSize yards; a tile is 533.33 yd). A cell holds
// one or more height levels (the floors found in it, clustered: a street, a bridge over it, a cellar); each level lists what can be
// seen from anywhere in the cell at that height (eyes 2 and 8 yd over every floor at the cell's corners and centre, all directions):
// the doodads (non-building placements, by their index in the tile's PLCM) and the ground batches (index in the tile's TERR), per
// tile. A tile listed with nothing = nothing of it can be seen; a tile not listed was too far to matter. (v3: WMO groups too.)
// Occluders were only what the client surely draws from there: the ground, outdoor building groups, indoor ones near (radius + 25,
// or radius + 70 inside the same building), opaque parts of doodads within 40 + occRange x radius. Cut-out / blended parts hide nothing.
//
// v2: a cell's floors at one height make up to three levels, by how high the camera is over them (bands): up to 4.75 yd (eyes
// 2 and 4.5 yd), 7.25 (+ 7) and 10.25 (+ 10): an eye high over a street sees over the roofs, a camera near the floor does not.
// The client uses every level whose [zlo, zhi] holds the camera's height; doodads within nearAlways yards are drawn anyway. Off when its doodad range is outside [occRange, maxRange] or its draw
// distance over maxDist (the candidates were picked with those).
#pragma once
#include <stdint.h>
#include <string.h>

#define W3D_PVS_MAGIC 0x56503357u   // "W3PV"
#define W3D_PVS_VERSION 4u   // v4 (6.56): interior WMO groups seen from outside to the draw distance (the same layout as v3);
                             // v3 (6.54d): WMO groups listed, a level's flags (far hidden); v2 (6.54b): camera-height bands
#define W3D_PVS_FAR_HIDDEN 1u   // a level's flag: nothing past the draw distance can be seen from it (6.54d)

typedef struct {
    uint32_t magic, version;
    uint32_t buildId;        // the map pack's PINF buildId (0: a standalone pack)
    float cellSize;          // yards
    uint32_t cellsPerSide;   // cells along a tile edge
    float occRange;          // doodads hid others only within 40 + occRange x radius (the client's range must not be lower)
    float maxRange;          // candidates: doodads within 40 + maxRange x radius (the client's range must not be higher)
    float maxDist;           // the draw distance it was made for (the client's must not be higher)
    float nearAlways;        // yards: doodads this near the camera are drawn whatever the set says
    float eyeAbove;          // the highest eye sampled over a floor (yards)
    uint32_t tileCount;
    uint32_t tileTableOff;   // W3dPvsTile[tileCount], sorted by (tx, ty)
} W3dPvsHeader;

typedef struct {
    uint16_t tx, ty;
    float origin[2];         // the tile's ground origin (render space x, y): cell (cx, cy) covers origin + [cx, cx + 1) x cellSize
    uint32_t cellTableOff;   // uint32_t[cellsPerSide^2] (cx + cy * cellsPerSide): the cell's offset in the file, 0 = no data
} W3dPvsTile;

// A cell: uint8_t levelCount, then per level: float zlo, float zhi (the camera's height range), uint32_t bytes, then the level's bytes:
//   varint flags (W3D_PVS_FAR_HIDDEN); varint groups; per group: int8_t dtx, int8_t dty (tile relative to the cell's), then three
//   lists - doodads (PLCM index), ground batches (TERR batch index), WMO groups (PLCM index; v3) - each a varint count and that many
//   varints (the first index, then each the gap to the previous minus 1).
// v3 (6.54d): WMO groups are listed too (outdoor and indoor; nearer than nearAlways + their radius they are drawn anyway); a level
// flagged far-hidden: no eye of it saw anything past maxDist - 2 cells below the far ground's tops - the client then skips the far
// ground, the far building shells and the loaded tiles' ground past its distance.

typedef struct {
    float zlo, zhi;          // the camera's height range
    uint32_t bytes;
    const uint8_t* data;
} W3dPvsLevel;

static inline uint32_t w3d_pvs_varint(const uint8_t** p, const uint8_t* end) {
    uint32_t v = 0;
    for (int s = 0; s < 35 && *p < end; s += 7) {
        const uint8_t b = *(*p)++;
        v |= (uint32_t)(b & 0x7F) << s;
        if (!(b & 0x80)) return v;
    }
    return v;
}

// The levels of a cell (at most 'max'); returns how many, 0 = none / damaged.
static inline int w3d_pvs_levels(const uint8_t* file, uint32_t size, uint32_t cellOff, W3dPvsLevel* out, int max) {
    if (cellOff >= size) return 0;   // (cellOff 0: a cell read on its own, at the buffer's start)
    const uint8_t* p = file + cellOff;
    const uint8_t* end = file + size;
    const int n = *p++;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (end - p < 12) return 0;
        W3dPvsLevel l;
        memcpy(&l.zlo, p, 4);
        memcpy(&l.zhi, p + 4, 4);
        memcpy(&l.bytes, p + 8, 4);
        p += 12;
        if ((uint32_t)(end - p) < l.bytes) return 0;
        l.data = p;
        p += l.bytes;
        if (k < max) out[k++] = l;
    }
    return k;
}
