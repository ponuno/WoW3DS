// 6.42: ceilings over a jumping walker: w3d_ceiling_above / w3d_rise_under_ceiling on a hand-built COLL tile
// (a floor at z 10, a roof 2.6 yd above it over x < 20, a bridge deck at 14 over y > 30, a steep roof flagged as a wall).
#include "w3d_collision.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int bad = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++bad; } }

int main() {
    const float ox = 0, oy = 0, oz = 10.0f, zs = 1.0f / 256.0f;
    const uint32_t N = 1;
    std::vector<W3dCollTri> tris;
    auto quad = [&](float x0, float y0, float x1, float y1, float z, uint16_t flags) {
        auto q = [&](float v) { return (uint16_t)(v / W3D_COLL_XY_SCALE); };
        auto qz = [&](float v) { return (int16_t)((v - oz) / zs); };
        W3dCollTri a{}, b{};
        a.x[0] = q(x0); a.y[0] = q(y0); a.x[1] = q(x1); a.y[1] = q(y0); a.x[2] = q(x1); a.y[2] = q(y1);
        b.x[0] = q(x0); b.y[0] = q(y0); b.x[1] = q(x1); b.y[1] = q(y1); b.x[2] = q(x0); b.y[2] = q(y1);
        for (int k = 0; k < 3; ++k) a.z[k] = b.z[k] = qz(z);
        a.flags = b.flags = flags;
        tris.push_back(a); tris.push_back(b);
    };
    quad(0, 0, 100, 100, 10.0f, 0);       // the floor
    quad(0, 0, 20, 100, 12.6f, 0);        // a low roof, x < 20
    quad(0, 30, 100, 100, 14.0f, 0);      // a bridge deck, y > 30
    quad(60, 0, 80, 20, 12.0f, W3D_COLL_WALL);   // a face flagged as a wall: never a ceiling
    W3dCollHeader h{};
    h.origin[0] = ox; h.origin[1] = oy; h.origin[2] = oz; h.zScale = zs; h.gridN = N; h.triCount = (uint32_t)tris.size();
    h.indexCount = (uint32_t)tris.size();
    std::vector<uint32_t> cells = {0, h.indexCount}, index;
    for (uint32_t i = 0; i < tris.size(); ++i) index.push_back(i);
    std::vector<uint8_t> blob(sizeof h);
    auto put = [&](const void* p, size_t n) { size_t at = blob.size(); blob.resize(at + n); std::memcpy(&blob[at], p, n); return (uint32_t)at; };
    h.offCells = put(cells.data(), cells.size() * 4);
    h.offIndex = put(index.data(), index.size() * 4);
    h.offTris = put(tris.data(), tris.size() * sizeof(W3dCollTri));
    std::memcpy(blob.data(), &h, sizeof h);
    // (the vector may move while growing: pointers are taken only now)
    const uint8_t* c = blob.data();

    float ceil = 0;
    check(!w3d_ceiling_above(c, 50, 10, 10.5f, &ceil), "open air over the floor: no ceiling");
    check(w3d_ceiling_above(c, 10, 10, 10.5f, &ceil) && ceil > 12.55f && ceil < 12.65f, "low roof found at 12.6");
    check(w3d_ceiling_above(c, 50, 50, 10.5f, &ceil) && ceil > 13.95f && ceil < 14.05f, "bridge deck found at 14");
    check(!w3d_ceiling_above(c, 70, 10, 10.5f, &ceil), "a wall-flagged face is no ceiling");
    check(!w3d_ceiling_above(nullptr, 10, 10, 10.5f, &ceil), "no collision: no ceiling");
    check(!w3d_ceiling_above(c, 10, 10, 12.7f, &ceil), "a ceiling already below the starting height is not above it");

    // a jump under the roof (head 2.0): the feet stop at 12.6 - 2.0 = 10.6 and the rise ends
    const float head = 2.0f;
    w3d_ceiling_above(c, 10, 10, 10.5f, &ceil);
    int hit = 0;
    float z = w3d_rise_under_ceiling(10.0f, 0.3f, head, 1, ceil, &hit);
    check(!hit && z > 10.29f && z < 10.31f, "a rise that stays under the roof is free");
    z = w3d_rise_under_ceiling(10.5f, 0.4f, head, 1, ceil, &hit);
    check(hit && z > 10.59f && z < 10.61f, "the head stops at the roof (feet 10.6)");
    z = w3d_rise_under_ceiling(10.0f, 0.6f, head, 1, ceil, &hit);
    check(hit && z > 10.59f && z < 10.61f, "the stop is the same when the step is large");
    // full jump arc (v 7.9555, g 19.29, 60 fps) under the roof: peak at most 10.6, no frame above
    float peak = 10.0f, vz = 7.9555473f;
    z = 10.0f;
    for (int i = 0; i < 120; ++i) {
        vz -= 19.291105f / 60.0f;
        w3d_ceiling_above(c, 10, 10, z + 0.5f, &ceil);
        float nz = w3d_rise_under_ceiling(z, vz / 60.0f, head, vz > 0 && w3d_ceiling_above(c, 10, 10, z + 0.5f, &ceil), ceil, &hit);
        if (hit) vz = 0;
        z = nz > 10.0f ? nz : 10.0f;
        if (z > peak) peak = z;
    }
    check(peak < 10.62f, "a whole jump under the roof never gets the head above it");
    // out in the open the same jump reaches its full height
    peak = 10.0f; vz = 7.9555473f; z = 10.0f;
    for (int i = 0; i < 120; ++i) {
        vz -= 19.291105f / 60.0f;
        float cz = 0;
        float nz = w3d_rise_under_ceiling(z, vz / 60.0f, head, vz > 0 && w3d_ceiling_above(c, 50, 10, z + 0.5f, &cz), cz, &hit);
        z = nz > 10.0f ? nz : 10.0f;
        if (z > peak) peak = z;
    }
    check(peak > 11.55f, "an open jump is not cut short (1.6 yd)");
    // a roof lower than the head: no rise at all
    z = w3d_rise_under_ceiling(10.0f, 0.2f, head, 1, 11.5f, &hit);
    check(hit && z == 10.0f, "under a roof lower than the head the feet stay down");
    // falling: never touched
    z = w3d_rise_under_ceiling(12.0f, -0.3f, head, 1, 12.6f, &hit);
    check(!hit && z > 11.69f && z < 11.71f, "a fall ignores the ceiling");

    std::printf("CEILING: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
