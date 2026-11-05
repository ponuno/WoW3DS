// objcoll.cpp — 6.59: see objcoll.hpp.
#include "objcoll.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "w3d_collision.h"
#include "w3d_format.h"

namespace objcoll {

bool build(const float* tris, uint32_t triCount, std::vector<uint8_t>& out) {
    out.clear();
    if (!tris || !triCount) return false;
    const float TS = 533.33333f;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < triCount * 3; ++i)
        for (int k = 0; k < 3; ++k) {
            const float v = tris[i * 3 + k];
            if (!std::isfinite(v)) return false;
            mn[k] = std::min(mn[k], v);
            mx[k] = std::max(mx[k], v);
        }
    if (mx[0] - mn[0] > W3D_OBJWMO_MAX_EXTENT || mx[1] - mn[1] > W3D_OBJWMO_MAX_EXTENT) return false;   // (the grid is a tile's)
    W3dCollHeader h{};
    h.origin[0] = mn[0] - 1.0f;
    h.origin[1] = mn[1] - 1.0f;
    h.origin[2] = 0.5f * (mn[2] + mx[2]);
    h.zScale = std::max((mx[2] - mn[2]) / 65000.0f, 1.0f / 256.0f);
    // the format's grid spans a whole tile: 64 cells (8.3 yd) - the object fills a few of them; 128 for a big one (the cell
    // table is N * N + 1 words: 16 / 64 KB)
    const float ext = std::max(mx[0] - mn[0], mx[1] - mn[1]) + 2.0f;
    const uint32_t N = ext > 120.0f ? 128u : 64u;
    h.gridN = N;
    std::vector<W3dCollTri> q;
    std::vector<std::vector<uint32_t>> cells((size_t)N * N);
    const float cs = TS / (float)N;
    auto qxy = [](float v) { return (uint16_t)std::clamp((int)std::lround(v / W3D_COLL_XY_SCALE), 0, 65535); };
    for (uint32_t i = 0; i < triCount; ++i) {
        const float* v = tris + (size_t)i * 9;
        const float ax = v[3] - v[0], ay = v[4] - v[1], az = v[5] - v[2], bx = v[6] - v[0], by = v[7] - v[1], bz = v[8] - v[2];
        const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(len > 1e-6f)) continue;
        W3dCollTri t{};
        float lx[3], ly[3];
        for (int k = 0; k < 3; ++k) {
            lx[k] = v[k * 3] - h.origin[0];
            ly[k] = v[k * 3 + 1] - h.origin[1];
            t.x[k] = qxy(lx[k]);
            t.y[k] = qxy(ly[k]);
            t.z[k] = (int16_t)std::clamp((int)std::lround((v[k * 3 + 2] - h.origin[2]) / h.zScale), -32767, 32767);
        }
        t.flags = (uint16_t)(std::fabs(nz) / len < W3D_WALK_MIN_NZ ? W3D_COLL_WALL : 0u);
        const uint32_t ti = (uint32_t)q.size();
        q.push_back(t);
        const int cx0 = std::clamp((int)std::floor(std::min({lx[0], lx[1], lx[2]}) / cs), 0, (int)N - 1);
        const int cx1 = std::clamp((int)std::floor(std::max({lx[0], lx[1], lx[2]}) / cs), 0, (int)N - 1);
        const int cy0 = std::clamp((int)std::floor(std::min({ly[0], ly[1], ly[2]}) / cs), 0, (int)N - 1);
        const int cy1 = std::clamp((int)std::floor(std::max({ly[0], ly[1], ly[2]}) / cs), 0, (int)N - 1);
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx) cells[(size_t)cy * N + cx].push_back(ti);
    }
    if (q.empty()) return false;
    h.triCount = (uint32_t)q.size();
    std::vector<uint32_t> start((size_t)N * N + 1), index;
    for (size_t c = 0; c < (size_t)N * N; ++c) {
        start[c] = (uint32_t)index.size();
        index.insert(index.end(), cells[c].begin(), cells[c].end());
    }
    start[(size_t)N * N] = (uint32_t)index.size();
    h.indexCount = (uint32_t)index.size();
    auto align4 = [&]() { while (out.size() & 3) out.push_back(0); };
    out.resize(sizeof h);
    align4();
    h.offCells = (uint32_t)out.size();
    out.insert(out.end(), (const uint8_t*)start.data(), (const uint8_t*)(start.data() + start.size()));
    align4();
    h.offIndex = (uint32_t)out.size();
    if (!index.empty()) out.insert(out.end(), (const uint8_t*)index.data(), (const uint8_t*)(index.data() + index.size()));
    align4();
    h.offTris = (uint32_t)out.size();
    out.insert(out.end(), (const uint8_t*)q.data(), (const uint8_t*)(q.data() + q.size()));
    std::memcpy(out.data(), &h, sizeof h);
    return true;
}

// world -> object space (x, y); z: (z - pos.z) / scale
static void toLocal(const Placed& p, float x, float y, float* lx, float* ly) {
    const float dx = x - p.pos[0], dy = y - p.pos[1], c = std::cos(p.yaw), s = std::sin(p.yaw);
    *lx = (c * dx + s * dy) / p.scale;
    *ly = (-s * dx + c * dy) / p.scale;
}
static void toWorld(const Placed& p, float lx, float ly, float* x, float* y) {
    const float c = std::cos(p.yaw), s = std::sin(p.yaw);
    *x = p.pos[0] + (c * lx - s * ly) * p.scale;
    *y = p.pos[1] + (s * lx + c * ly) * p.scale;
}
static bool near(const Placed& p, float x, float y, float extra) {
    const float dx = x - p.pos[0], dy = y - p.pos[1], r = p.radius + extra;
    return p.coll && dx * dx + dy * dy <= r * r;
}

bool floorAt(const Placed& p, float x, float y, float zFeet, float step, float* z) {
    if (!near(p, x, y, 1.0f)) return false;
    float lx, ly, below = 0, above = 0;
    toLocal(p, x, y, &lx, &ly);
    if (!(w3d_coll_query(p.coll, lx, ly, (zFeet + step - p.pos[2]) / p.scale, &below, &above) & 1)) return false;
    *z = p.pos[2] + below * p.scale;
    return true;
}

bool ceilingAbove(const Placed& p, float x, float y, float zFrom, float* ceil) {
    if (!near(p, x, y, 1.0f)) return false;
    float lx, ly, c = 0;
    toLocal(p, x, y, &lx, &ly);
    if (!w3d_ceiling_above(p.coll, lx, ly, (zFrom - p.pos[2]) / p.scale, &c)) return false;
    *ceil = p.pos[2] + c * p.scale;
    return true;
}

bool collideMove(const Placed& p, float fromX, float fromY, float zFeet, float r, float* toX, float* toY) {
    if (!near(p, fromX, fromY, r + 2.0f + std::hypot(*toX - fromX, *toY - fromY))) return false;
    float fx, fy, tx, ty;
    toLocal(p, fromX, fromY, &fx, &fy);
    toLocal(p, *toX, *toY, &tx, &ty);
    const uint8_t* colls[1] = {p.coll};
    if (!w3d_collide_move(colls, 1, fx, fy, (zFeet - p.pos[2]) / p.scale, &tx, &ty, r / p.scale)) return false;
    toWorld(p, tx, ty, toX, toY);
    return true;
}

}  // namespace objcoll
