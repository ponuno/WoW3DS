// w3d_worldobj.h — geometry of world objects the client moves or blocks (stage 5.41), shared by the 3DS client and PC tests:
//  - a tilted object's rotation matrix from GAMEOBJECT_ROTATION (logs, planks, camp parts stand at an angle),
//  - closed doors and gates as oriented boxes that push the walker out (the world's own collision has no doors).
#pragma once
#include <math.h>
#include <stdint.h>

// Row-major 3x3 rotation of the unit quaternion (x y z w), model space -> render space (column vectors).
static inline void w3d_obj_rot_from_quat(const float q[4], float r[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - z * w);     r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w);     r[7] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}

// Rotation about Z by yaw (the usual upright object), same layout.
static inline void w3d_obj_rot_from_yaw(float yaw, float r[9]) {
    const float c = cosf(yaw), s = sinf(yaw);
    r[0] = c; r[1] = -s; r[2] = 0; r[3] = s; r[4] = c; r[5] = 0; r[6] = 0; r[7] = 0; r[8] = 1;
}

// A box that blocks walking: centre (render space), half extents along its own x / y, z range, yaw about Z.
typedef struct {
    float cx, cy, z0, z1;
    float hx, hy;
    float yaw;
} W3dBlockBox;

// Box of a door model: the model's bind-pose bounds (bmin / bmax, model space) scaled and turned like the object. A door is
// thin: the short side is widened to at least minHalf so a walker cannot slip through the leaf (the radius is added later).
static inline W3dBlockBox w3d_block_from_bounds(const float bmin[3], const float bmax[3], const float pos[3], float yaw, float scale,
                                                float minHalf) {
    W3dBlockBox b;
    const float mx = 0.5f * (bmin[0] + bmax[0]) * scale, my = 0.5f * (bmin[1] + bmax[1]) * scale;
    const float c = cosf(yaw), s = sinf(yaw);
    b.cx = pos[0] + c * mx - s * my;
    b.cy = pos[1] + s * mx + c * my;
    b.hx = 0.5f * (bmax[0] - bmin[0]) * scale;
    b.hy = 0.5f * (bmax[1] - bmin[1]) * scale;
    if (b.hx < minHalf) b.hx = minHalf;
    if (b.hy < minHalf) b.hy = minHalf;
    b.z0 = pos[2] + bmin[2] * scale;
    b.z1 = pos[2] + bmax[2] * scale;
    b.yaw = yaw;
    return b;
}

// Pushes the walker's destination (*tx, *ty) out of every box it would end inside (circle of radius r, feet at zFeet; boxes
// below the knees - the top lower than zFeet + 0.75 - or above the head do not block, like walls). A walker already inside a
// box because the door just closed on it is let out the shortest way only when it moves outward; otherwise it is held.
// Returns 1 when the destination was changed.
static inline int w3d_block_boxes_move(const W3dBlockBox* boxes, int n, float fromX, float fromY, float zFeet, float* tx, float* ty,
                                       float r) {
    int hit = 0;
    for (int i = 0; i < n; ++i) {
        const W3dBlockBox& b = boxes[i];
        if (zFeet + 0.75f > b.z1 || zFeet + 1.8f < b.z0) continue;
        const float c = cosf(b.yaw), s = sinf(b.yaw);
        // destination in the box's frame
        const float dx = *tx - b.cx, dy = *ty - b.cy;
        float lx = c * dx + s * dy, ly = -s * dx + c * dy;
        const float ex = b.hx + r, ey = b.hy + r;
        if (fabsf(lx) >= ex || fabsf(ly) >= ey) continue;   // (the grown box is a little generous at its corners)
        // where did it come from, in the same frame
        const float fx0 = fromX - b.cx, fy0 = fromY - b.cy;
        const float flx = c * fx0 + s * fy0, fly = -s * fx0 + c * fy0;
        float nx, ny;   // corrected destination, box frame
        const bool fromInside = fabsf(flx) < ex && fabsf(fly) < ey;
        if (fromInside) {
            // shut in: allow only movement that does not go deeper along the shorter way out
            const float px = ex - fabsf(flx), py = ey - fabsf(fly);
            if (px < py) { nx = (flx >= 0 ? 1.f : -1.f) * ex; ny = ly; } else { nx = lx; ny = (fly >= 0 ? 1.f : -1.f) * ey; }
            // do not teleport the walker through the door: stay at the old spot unless the push is short (< 1.5 yd)
            if (fabsf(nx - flx) + fabsf(ny - fly) > 1.5f) { nx = flx; ny = fly; }
        } else {
            // slide: remove the penetration along the axis it entered from (the smaller overlap), keep the other
            const float ox = ex - fabsf(lx), oy = ey - fabsf(ly);
            const bool enteredX = fabsf(flx) >= ex, enteredY = fabsf(fly) >= ey;
            if (enteredX && !enteredY) { nx = (lx >= 0 ? 1.f : -1.f) * ex; ny = ly; }
            else if (enteredY && !enteredX) { nx = lx; ny = (ly >= 0 ? 1.f : -1.f) * ey; }
            else if (ox < oy) { nx = (lx >= 0 ? 1.f : -1.f) * ex; ny = ly; }
            else { nx = lx; ny = (ly >= 0 ? 1.f : -1.f) * ey; }
        }
        lx = nx; ly = ny;
        *tx = b.cx + c * lx - s * ly;
        *ty = b.cy + s * lx + c * ly;
        hit = 1;
    }
    return hit;
}
