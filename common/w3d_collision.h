// w3d_collision.h — floor queries and wall push-out on COLL entries (shared by the 3DS client and PC tests).
#pragma once
#include <math.h>
#include <stdint.h>

#include "w3d_format.h"

// ---------------------------------------------------------------- walking rules (stage 6.18c)
// One set of numbers for the converter (wall or floor) and the client (steps, slopes, body). Sources:
//  * slope: the retail client refuses ground steeper than 50 degrees (WoWee include/rendering/movement_limits.hpp:
//    kMaxWalkableSlopeDegrees = 50, "kMinWalkableNormalZ = cos(50)"); a face whose normal.z is below this is not a floor.
//    (Before 6.18 the converter's boundary was 60 degrees and the terrain had no limit at all.)
//  * step: WoWee kMaxStepUp = 0.6 yd is its per-frame budget on a finely sampled floor; vMaNGOS contrib/mmap TileWorker.cpp
//    (what the server's own walking allows) climbs 1.8 yd on terrain and 1.2 yd between terrain and models. The 3DS samples the
//    floor once per frame at walking speed, so it keeps 1.5 yd (between the two vMaNGOS values; world.h kStepHeight).
//  * body: a cylinder of radius 0.35 (WoWee feetCross(0.35)); vMaNGOS agentRadius 0.2..0.3.
#define W3D_WALK_MAX_SLOPE_DEG 50.0f
#define W3D_WALK_MIN_NZ 0.64278761f   /* cos(50 degrees) */

// Heights of the floor triangles under (x, y) in render space. Reports the highest
// one at or below zMax (*below, returns bit 1) and the lowest one above zMax
// (*above, returns bit 2). 0 = no floor triangle covers the point.
static inline int w3d_coll_query(const uint8_t* blob, float x, float y, float zMax, float* below, float* above) {
    const W3dCollHeader* h = (const W3dCollHeader*)blob;
    float lx = x - h->origin[0], ly = y - h->origin[1];
    const float ts = 533.33333f;
    if (lx < 0 || ly < 0 || lx > ts || ly > ts) return 0;
    int n = (int)h->gridN;
    int cx = (int)(lx / ts * n), cy = (int)(ly / ts * n);
    if (cx >= n) cx = n - 1;
    if (cy >= n) cy = n - 1;
    const uint32_t* cells = (const uint32_t*)(blob + h->offCells);
    const uint32_t* index = (const uint32_t*)(blob + h->offIndex);
    const W3dCollTri* tris = (const W3dCollTri*)(blob + h->offTris);
    int found = 0;
    const float qx = lx / W3D_COLL_XY_SCALE, qy = ly / W3D_COLL_XY_SCALE;   // point in quantised units
    for (uint32_t k = cells[cy * n + cx]; k < cells[cy * n + cx + 1]; ++k) {
        const W3dCollTri* t = &tris[index[k]];
        if (t->flags & W3D_COLL_WALL) continue;   // walls never hold you up
        float ax = t->x[0], ay = t->y[0], bx = t->x[1], by = t->y[1], ccx = t->x[2], ccy = t->y[2];
        float den = (by - ccy) * (ax - ccx) + (ccx - bx) * (ay - ccy);
        if (fabsf(den) < 1e-3f) continue;   // degenerate in 2D (vertical piece)
        float w0 = ((by - ccy) * (qx - ccx) + (ccx - bx) * (qy - ccy)) / den;
        float w1 = ((ccy - ay) * (qx - ccx) + (ax - ccx) * (qy - ccy)) / den;
        float w2 = 1.0f - w0 - w1;
        const float eps = -1e-4f;            // include edges: no cracks between neighbours
        if (w0 < eps || w1 < eps || w2 < eps) continue;
        float z = h->origin[2] + (w0 * t->z[0] + w1 * t->z[1] + w2 * t->z[2]) * h->zScale;
        if (z <= zMax) {
            if (!(found & 1) || z > *below) *below = z;
            found |= 1;
        } else {
            if (!(found & 2) || z < *above) *above = z;
            found |= 2;
        }
    }
    return found;
}

// Diagnostics (6.19b): every floor triangle over (x, y): its height at the point and flags (W3D_COLL_M2: a doodad). Up to 'max'.
static inline int w3d_coll_floors_at(const uint8_t* blob, float x, float y, int max, float* zs, uint16_t* flags) {
    const W3dCollHeader* h = (const W3dCollHeader*)blob;
    float lx = x - h->origin[0], ly = y - h->origin[1];
    const float ts = 533.33333f;
    if (lx < 0 || ly < 0 || lx > ts || ly > ts) return 0;
    int n = (int)h->gridN;
    int cx = (int)(lx / ts * n), cy = (int)(ly / ts * n);
    if (cx >= n) cx = n - 1;
    if (cy >= n) cy = n - 1;
    const uint32_t* cells = (const uint32_t*)(blob + h->offCells);
    const uint32_t* index = (const uint32_t*)(blob + h->offIndex);
    const W3dCollTri* tris = (const W3dCollTri*)(blob + h->offTris);
    const float qx = lx / W3D_COLL_XY_SCALE, qy = ly / W3D_COLL_XY_SCALE;
    int c = 0;
    for (uint32_t k = cells[cy * n + cx]; k < cells[cy * n + cx + 1] && c < max; ++k) {
        const W3dCollTri* t = &tris[index[k]];
        if (t->flags & W3D_COLL_WALL) continue;
        float ax = t->x[0], ay = t->y[0], bx = t->x[1], by = t->y[1], ccx = t->x[2], ccy = t->y[2];
        float den = (by - ccy) * (ax - ccx) + (ccx - bx) * (ay - ccy);
        if (fabsf(den) < 1e-3f) continue;
        float w0 = ((by - ccy) * (qx - ccx) + (ccx - bx) * (qy - ccy)) / den;
        float w1 = ((ccy - ay) * (qx - ccx) + (ax - ccx) * (qy - ccy)) / den;
        float w2 = 1.0f - w0 - w1;
        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
        zs[c] = h->origin[2] + (w0 * t->z[0] + w1 * t->z[1] + w2 * t->z[2]) * h->zScale;
        flags[c++] = t->flags;
    }
    return c;
}

// Ground for a walker whose feet are at zFeet: the highest surface (terrain or
// floor) it can step onto (<= zFeet + step); if it is below everything, the
// lowest surface. haveTerrain/terrainZ = the heightfield sample.
static inline float w3d_walk_height(const uint8_t* coll, float x, float y, float zFeet, float step, int haveTerrain,
                                    float terrainZ, int* onFloor) {
    float below = 0, above = 0;
    int f = coll ? w3d_coll_query(coll, x, y, zFeet + step, &below, &above) : 0;
    int fromFloor = 0;
    float best = 0;
    int have = 0;
    if (haveTerrain && terrainZ <= zFeet + step) { best = terrainZ; have = 1; }
    if ((f & 1) && (!have || below > best)) { best = below; have = 1; fromFloor = 1; }
    if (!have) {   // under everything (e.g. spawned below a floor): take the lowest surface
        if (haveTerrain) { best = terrainZ; have = 1; }
        if ((f & 2) && (!have || above < best)) { best = above; fromFloor = 1; }
    }
    if (onFloor) *onFloor = fromFloor;
    return best;
}

// 6.21: the terrain in an ADT hole. The hole is where a building opens into the ground (a tunnel mouth, a cellar, Ironforge's
// gate); the original client has no ground there at all. Our ground there is a membrane filled in over the opening, and
// standing on it held the character in the air in front of the doorway, then the hillside it continues into stopped it
// (the Coldridge Valley tunnel). It counts only where no building has a floor under the feet (its collision is missing
// or not loaded yet) and never above the feet (6.17). Returns whether the terrain height may be used.
static inline int w3d_hole_terrain_usable(const uint8_t* coll, float x, float y, float zFeet, float step, float terrainZ) {
    if (terrainZ > zFeet + 0.05f) return 0;
    if (!coll) return 1;
    float below = 0, above = 0;
    return (w3d_coll_query(coll, x, y, zFeet + step, &below, &above) & 1) ? 0 : 1;
}

// ---------------------------------------------------------------- walls
// The walker is a vertical cylinder of radius r. A wall triangle blocks it if the part
// of the triangle inside the height slab [zLow, zHigh] (from feet + 0.75: lower steps
// and kerbs are left to the floor step) comes closer than r in XY.

#define W3D_WALL_MAX_POLY 8
#define W3D_WALL_MAX_CANDS 256

// Clips the triangle to zLow <= z <= zHigh; returns the vertex count (0 if it misses).
static inline int w3d_clip_slab(const float* tx, const float* ty, const float* tz, float zLow, float zHigh,
                                float* ox, float* oy) {
    float ax[W3D_WALL_MAX_POLY], ay[W3D_WALL_MAX_POLY], az[W3D_WALL_MAX_POLY];
    float bx[W3D_WALL_MAX_POLY], by[W3D_WALL_MAX_POLY], bz[W3D_WALL_MAX_POLY];
    int n = 3;
    for (int i = 0; i < 3; ++i) { ax[i] = tx[i]; ay[i] = ty[i]; az[i] = tz[i]; }
    for (int plane = 0; plane < 2 && n > 0; ++plane) {
        int m = 0;
        for (int i = 0; i < n; ++i) {
            int j = (i + n - 1) % n;   // previous vertex
            float dc = plane ? zHigh - az[i] : az[i] - zLow;   // >= 0 inside
            float dp = plane ? zHigh - az[j] : az[j] - zLow;
            if ((dc >= 0) != (dp >= 0)) {
                float t = dp / (dp - dc);
                bx[m] = ax[j] + (ax[i] - ax[j]) * t;
                by[m] = ay[j] + (ay[i] - ay[j]) * t;
                bz[m] = az[j] + (az[i] - az[j]) * t;
                ++m;
            }
            if (dc >= 0) { bx[m] = ax[i]; by[m] = ay[i]; bz[m] = az[i]; ++m; }
        }
        n = m;
        for (int i = 0; i < n; ++i) { ax[i] = bx[i]; ay[i] = by[i]; az[i] = bz[i]; }
    }
    for (int i = 0; i < n; ++i) { ox[i] = ax[i]; oy[i] = ay[i]; }
    return n;
}

// Closest point of a convex XY polygon (any winding, possibly flat = a segment) to (x, y).
// Returns 1 and the point itself if (x, y) is inside a polygon with area.
static inline int w3d_poly_closest(const float* px, const float* py, int n, float x, float y, float* cx, float* cy) {
    float area = 0;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        area += px[i] * py[j] - px[j] * py[i];
    }
    if (n >= 3 && fabsf(area) > 1e-6f) {
        int inside = 1;
        for (int i = 0; i < n && inside; ++i) {
            int j = (i + 1) % n;
            float c = (px[j] - px[i]) * (y - py[i]) - (py[j] - py[i]) * (x - px[i]);
            if ((area > 0 && c < 0) || (area < 0 && c > 0)) inside = 0;
        }
        if (inside) { *cx = x; *cy = y; return 1; }
    }
    float best = 3.4e38f;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        float ex = px[j] - px[i], ey = py[j] - py[i];
        float l2 = ex * ex + ey * ey;
        float t = l2 > 1e-12f ? ((x - px[i]) * ex + (y - py[i]) * ey) / l2 : 0.0f;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        float qx = px[i] + ex * t, qy = py[i] + ey * t;
        float d2 = (x - qx) * (x - qx) + (y - qy) * (y - qy);
        if (d2 < best) { best = d2; *cx = qx; *cy = qy; }
    }
    return 0;
}

// A wall triangle near a move, unpacked once per move (coordinates relative to the move's
// start, so float precision does not depend on how far the tile is from the map origin).
typedef struct {
    float x[3], y[3], z[3];
    float bx0, bx1, by0, by1;       // XY bounds
    float nx, ny;                    // horizontal unit normal
    float umin, umax;                // extent along the wall (u = -ny * x + nx * y)
} W3dWallCand;

// Appends the walls of one COLL entry whose bounds touch [x0, x1] x [y0, y1] (absolute
// render space) and the height slab, relative to (ox, oy). Skips triangles already listed
// (a triangle sits in every grid cell it overlaps). Returns the new count (<= cap).
static inline int w3d_wall_gather(const uint8_t* blob, float x0, float y0, float x1, float y1, float zLow, float zHigh,
                                  float ox, float oy, W3dWallCand* out, uint32_t* ids, int count, int cap) {
    const W3dCollHeader* h = (const W3dCollHeader*)blob;
    const float ts = 533.33333f, S = W3D_COLL_XY_SCALE;
    float lx0 = x0 - h->origin[0], lx1 = x1 - h->origin[0], ly0 = y0 - h->origin[1], ly1 = y1 - h->origin[1];
    if (lx1 < 0 || ly1 < 0 || lx0 > ts || ly0 > ts) return count;
    const int n = (int)h->gridN;
    const float cs = ts / n;
    int cx0 = (int)floorf(lx0 / cs), cx1 = (int)floorf(lx1 / cs), cy0 = (int)floorf(ly0 / cs), cy1 = (int)floorf(ly1 / cs);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > n - 1) cx1 = n - 1;
    if (cy1 > n - 1) cy1 = n - 1;
    const uint32_t* cells = (const uint32_t*)(blob + h->offCells);
    const uint32_t* index = (const uint32_t*)(blob + h->offIndex);
    const W3dCollTri* tris = (const W3dCollTri*)(blob + h->offTris);
    const float qzLow = (zLow - h->origin[2]) / h->zScale, qzHigh = (zHigh - h->origin[2]) / h->zScale;
    // bounds in quantised XY for the cheap reject
    const float qx0 = lx0 / S, qx1 = lx1 / S, qy0 = ly0 / S, qy1 = ly1 / S;
    const float dx = h->origin[0] - ox, dy = h->origin[1] - oy;
    const uint32_t tag = (uint32_t)(((uintptr_t)blob >> 4) & 0xFFu) << 24;   // tells tiles apart in ids[]
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx)
            for (uint32_t k = cells[cy * n + cx]; k < cells[cy * n + cx + 1]; ++k) {
                const W3dCollTri* t = &tris[index[k]];
                if (!(t->flags & W3D_COLL_WALL)) continue;
                int zmin = t->z[0], zmax = t->z[0], xmin = t->x[0], xmax = t->x[0], ymin = t->y[0], ymax = t->y[0];
                for (int i = 1; i < 3; ++i) {
                    if (t->z[i] < zmin) zmin = t->z[i];
                    if (t->z[i] > zmax) zmax = t->z[i];
                    if (t->x[i] < xmin) xmin = t->x[i];
                    if (t->x[i] > xmax) xmax = t->x[i];
                    if (t->y[i] < ymin) ymin = t->y[i];
                    if (t->y[i] > ymax) ymax = t->y[i];
                }
                if (zmax < qzLow || zmin > qzHigh) continue;
                if (xmin > qx1 || xmax < qx0 || ymin > qy1 || ymax < qy0) continue;
                uint32_t id = tag ^ index[k];
                int dup = 0;
                for (int i = 0; i < count && !dup; ++i) dup = ids[i] == id;
                if (dup) continue;
                if (count >= cap) return count;
                W3dWallCand* c = &out[count];
                for (int i = 0; i < 3; ++i) {
                    c->x[i] = t->x[i] * S + dx;
                    c->y[i] = t->y[i] * S + dy;
                    c->z[i] = h->origin[2] + t->z[i] * h->zScale;
                }
                float e1x = c->x[1] - c->x[0], e1y = c->y[1] - c->y[0], e1z = c->z[1] - c->z[0];
                float e2x = c->x[2] - c->x[0], e2y = c->y[2] - c->y[0], e2z = c->z[2] - c->z[0];
                float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z;
                float nl = sqrtf(nx * nx + ny * ny);
                if (nl < 1e-9f) continue;   // flat after quantisation: not a wall any more
                c->nx = nx / nl;
                c->ny = ny / nl;
                c->bx0 = xmin * S + dx; c->bx1 = xmax * S + dx;
                c->by0 = ymin * S + dy; c->by1 = ymax * S + dy;
                c->umin = 3.4e38f; c->umax = -3.4e38f;
                for (int i = 0; i < 3; ++i) {
                    float u = -c->ny * c->x[i] + c->nx * c->y[i];
                    if (u < c->umin) c->umin = u;
                    if (u > c->umax) c->umax = u;
                }
                ids[count++] = id;
            }
    return count;
}

// One pass over the gathered walls: pushes the circle (x, y, radius r; relative coordinates)
// out of each wall piece inside the slab that it overlaps, in turn, so it slides along them.
// (px, py) = where the walker came from: picks the side when it is on or inside a (sloped) wall.
// Returns the number of walls that moved it.
static inline int w3d_wall_push(const W3dWallCand* walls, int count, float* x, float* y, float prevX, float prevY,
                                float zLow, float zHigh, float r) {
    float lx = *x, ly = *y;
    int pushes = 0;
    // 6.18b: two phases. A wall made of many small coplanar triangles (an unsimplified fence, a building's panels) has seams the
    // walker can touch beyond the extent of one triangle but inside its neighbour's: pushing away from the seam's end point first
    // shoved the walker sideways (0.3 yd off the line of a straight walk into the fence). Pushes along the normal come first, they
    // put the walker clear of the whole plane; the pushes from real ends (corners) only act on what is still too close afterwards.
    int pending[W3D_WALL_MAX_CANDS];
    int npending = 0;
    for (int phase = 0; phase < 2; ++phase) {
        const int n = phase == 0 ? count : npending;
        for (int wi = 0; wi < n; ++wi) {
            const int w = phase == 0 ? wi : pending[wi];
            const W3dWallCand* c = &walls[w];
            if (c->bx0 > lx + r || c->bx1 < lx - r || c->by0 > ly + r || c->by1 < ly - r) continue;
            float px[W3D_WALL_MAX_POLY], py[W3D_WALL_MAX_POLY];
            int m = w3d_clip_slab(c->x, c->y, c->z, zLow, zHigh, px, py);
            if (m < 2) continue;
            float qx = lx, qy = ly;
            int inside = w3d_poly_closest(px, py, m, lx, ly, &qx, &qy);
            float dx = lx - qx, dy = ly - qy, d2 = dx * dx + dy * dy;
            if (d2 >= r * r) continue;
            float d = sqrtf(d2);
            float uw = -c->ny * lx + c->nx * ly;
            // The extent test uses the whole triangle, not the slab piece: the slab cuts the sloped
            // edges of neighbouring triangles of one wall at different places, and those cuts are
            // not corners to slide round.
            const bool radial = !inside && d > 1e-3f && (uw < c->umin || uw > c->umax);
            if (radial && phase == 0) { pending[npending++] = w; continue; }
            if (radial) {
                // beyond a real end of the wall: straight away from its closest point
                lx = qx + dx / d * r;
                ly = qy + dy / d * r;
            } else {
                // In front of the wall (or on/inside it): out along its horizontal normal, on the
                // side we came from, far enough to clear the whole slab piece.
                float nx = c->nx, ny = c->ny, gx = 0, gy = 0;
                for (int i = 0; i < m; ++i) { gx += px[i]; gy += py[i]; }
                gx /= m; gy /= m;
                float side = (prevX - gx) * nx + (prevY - gy) * ny;
                if (side == 0) side = (lx - gx) * nx + (ly - gy) * ny;
                if (side < 0) { nx = -nx; ny = -ny; }
                float far = -3.4e38f;
                for (int i = 0; i < m; ++i) {
                    float sk = (px[i] - lx) * nx + (py[i] - ly) * ny;
                    if (sk > far) far = sk;
                }
                float push = far + r;
                if (push <= 1e-5f) continue;
                lx += nx * push;
                ly += ny * push;
            }
            ++pushes;
        }
    }
    *x = lx;
    *y = ly;
    return pushes;
}

// Moves a walker from (fromX, fromY) towards (*toX, *toY) through the walls of the given
// COLL entries (all tiles the move can touch). Feet at zFeet; the move is cut into steps
// of at most r / 2 so a fast walker cannot jump over a thin wall. Writes the final
// position; returns 1 if a wall changed it. Not reentrant (one scratch list per file).
#define W3D_WALL_RADIUS 0.35f
#define W3D_WALL_SLAB_LOW 0.75f
#define W3D_WALL_SLAB_HIGH 2.0f
static inline int w3d_collide_move(const uint8_t* const* colls, int ncoll, float fromX, float fromY, float zFeet,
                                   float* toX, float* toY, float r) {
    static W3dWallCand cand[W3D_WALL_MAX_CANDS];
    static uint32_t ids[W3D_WALL_MAX_CANDS];
    if (ncoll <= 0) return 0;
    float dx = *toX - fromX, dy = *toY - fromY;
    float len = sqrtf(dx * dx + dy * dy);
    int steps = (int)ceilf(len / (0.5f * r));
    if (steps < 1) steps = 1;
    if (steps > 256) steps = 256;   // a teleport-sized jump: do not spend the frame on it
    const float zl = zFeet + W3D_WALL_SLAB_LOW, zh = zFeet + W3D_WALL_SLAB_HIGH;
    // Every wall the swept circle can reach (pushes move it at most ~r further).
    float m = 2.0f * r;
    float bx0 = fminf(fromX, *toX) - m, bx1 = fmaxf(fromX, *toX) + m;
    float by0 = fminf(fromY, *toY) - m, by1 = fmaxf(fromY, *toY) + m;
    int count = 0;
    for (int c = 0; c < ncoll; ++c)
        if (colls[c]) count = w3d_wall_gather(colls[c], bx0, by0, bx1, by1, zl, zh, fromX, fromY, cand, ids, count,
                                               W3D_WALL_MAX_CANDS);
    if (!count) return 0;
    float x = 0, y = 0;   // relative to (fromX, fromY)
    int hit = 0;
    for (int s = 1; s <= steps; ++s) {
        float px = x, py = y;
        x = px + dx / steps;
        y = py + dy / steps;
        for (int pass = 0; pass < 4; ++pass) {
            if (!w3d_wall_push(cand, count, &x, &y, px, py, zl, zh, r)) break;
            hit = 1;
        }
    }
    if (hit) {   // untouched moves keep the exact target
        *toX = fromX + x;
        *toY = fromY + y;
    }
    return hit;
}
