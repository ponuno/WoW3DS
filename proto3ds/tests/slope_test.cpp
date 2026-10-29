// 6.18c: the walking rules on synthetic data (no pack): the slope limit of the terrain (w3d_slope_move over a GRND of tilted
// ground), the step height on building floors (w3d_walk_height over a COLL with floors 1 and 2 yards up) and the wall/floor
// boundary that the converter and the client share (W3D_WALK_MIN_NZ).
#include "w3d_collision.h"
#include "w3d_ground.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// A tile whose ground rises along render x: z = z0 + tan(deg) * (x - 260) for x within 10 yd of 260, flat beyond.
static std::vector<uint8_t> tiltedGround(float deg, float z0) {
    std::vector<uint8_t> b(sizeof(W3dGroundHeader) + 256 * sizeof(W3dGroundChunk));
    W3dGroundHeader* h = (W3dGroundHeader*)b.data();
    std::memset(h, 0, sizeof *h);
    h->origin[0] = 0; h->origin[1] = 0; h->origin[2] = 0;
    h->zScale = 0.002f;   // 65535 * 0.002 = 131 yd of range
    h->chunkCount = 256;
    W3dGroundChunk* c = (W3dGroundChunk*)(b.data() + sizeof *h);
    const float q = 533.33333f / 128.0f, t = tanf(deg * 3.14159265f / 180.0f);
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            W3dGroundChunk& ch = c[cy * 16 + cx];
            std::memset(&ch, 0, sizeof ch);
            ch.flags = W3D_GROUND_CHUNK_PRESENT;
            for (int i = 0; i < 145; ++i) {
                int r = i / 17, k = i % 17;
                float row = cy * 8 + r + (k >= 9 ? 0.5f : 0.0f);
                float x = (1.0f - row / 128.0f) * 533.33333f;
                float dx = x - 260.0f;
                if (dx < -10.0f) dx = -10.0f;
                if (dx > 10.0f) dx = 10.0f;   // (the slope is 20 yd wide: 80 degrees must fit the 16-bit range)
                float z = z0 + t * dx;
                ch.h[i] = (uint16_t)lroundf(z / h->zScale);
            }
        }
    return b;
}

int main() {
    std::puts("-- slope limit of the terrain");
    for (float deg : {20.0f, 40.0f, 49.0f, 51.0f, 60.0f, 80.0f}) {
        std::vector<uint8_t> g = tiltedGround(deg, 70.0f);
        const uint8_t* gp = g.data();
        W3dGroundHit hit;
        CHECK(w3d_ground_at(gp, 260.0f, 260.0f, &hit), "no ground");
        const float want = cosf(deg * 3.14159265f / 180.0f);
        CHECK(fabsf(hit.nz - want) < 0.003f, "%.0f deg: normal z %.4f want %.4f", deg, hit.nz, want);
        const bool walkable = deg < W3D_WALK_MAX_SLOPE_DEG;
        float zf;
        w3d_ground_height(gp, 260.0f, 260.0f, &zf, nullptr);
        // uphill = towards +x (z grows with x)
        float tx = 261.0f, ty = 260.0f;
        int cut = w3d_slope_move(gp, gp, 260.0f, 260.0f, zf, 1.5f, &tx, &ty);
        CHECK(cut == (walkable ? 0 : 1), "%.0f deg uphill: %s", deg, cut ? "blocked" : "free");
        if (!walkable) CHECK(fabsf(tx - 260.0f) < 1e-3f && fabsf(ty - 260.0f) < 1e-3f, "%.0f deg: uphill part not removed (%.3f %.3f)", deg, tx, ty);
        // diagonal uphill: the contour part (y) stays
        tx = 261.0f; ty = 261.0f;
        w3d_slope_move(gp, gp, 260.0f, 260.0f, zf, 1.5f, &tx, &ty);
        if (!walkable) CHECK(fabsf(tx - 260.0f) < 1e-3f && fabsf(ty - 261.0f) < 1e-3f, "%.0f deg diagonal: got %.3f %.3f", deg, tx, ty);
        // downhill and along the contour are free
        tx = 259.0f; ty = 260.0f;
        CHECK(!w3d_slope_move(gp, gp, 260.0f, 260.0f, zf, 1.5f, &tx, &ty), "%.0f deg downhill blocked", deg);
        tx = 260.0f; ty = 261.0f;
        CHECK(!w3d_slope_move(gp, gp, 260.0f, 260.0f, zf, 1.5f, &tx, &ty), "%.0f deg along the contour blocked", deg);
        // on a building floor (feet 12 yd above this ground) the terrain below does not count
        tx = 261.0f; ty = 260.0f;
        CHECK(!w3d_slope_move(gp, gp, 260.0f, 260.0f, zf + 12.0f, 1.5f, &tx, &ty), "%.0f deg: blocked a walker standing 12 yd above it", deg);
        std::printf("   %2.0f deg: normal z %.3f -> %s\n", deg, hit.nz, walkable ? "walkable" : "too steep");
    }

    std::puts("-- step height on building floors");
    // one COLL tile (origin 0,0,0), two floor squares: A at z = 1.0 (x 10..20), B at z = 2.0 (x 30..40)
    struct Quad { float x0, x1, z; };
    const Quad quads[2] = {{10, 20, 1.0f}, {30, 40, 2.0f}};
    std::vector<W3dCollTri> tris;
    const float S = W3D_COLL_XY_SCALE, ZS = 0.01f;
    auto q = [&](float v) { return (uint16_t)lroundf(v / S); };
    for (const Quad& s : quads) {
        W3dCollTri a{}, b{};
        const float xs[4] = {s.x0, s.x1, s.x1, s.x0}, ys[4] = {0, 0, 10, 10};
        const int ia[3] = {0, 1, 2}, ib[3] = {0, 2, 3};
        for (int i = 0; i < 3; ++i) {
            a.x[i] = q(xs[ia[i]]); a.y[i] = q(ys[ia[i]]); a.z[i] = (int16_t)lroundf(s.z / ZS);
            b.x[i] = q(xs[ib[i]]); b.y[i] = q(ys[ib[i]]); b.z[i] = (int16_t)lroundf(s.z / ZS);
        }
        tris.push_back(a); tris.push_back(b);
    }
    std::vector<uint32_t> cells = {0, (uint32_t)tris.size()}, index;
    for (uint32_t i = 0; i < tris.size(); ++i) index.push_back(i);
    std::vector<uint8_t> coll(sizeof(W3dCollHeader));
    W3dCollHeader ch{};
    ch.gridN = 1; ch.triCount = (uint32_t)tris.size(); ch.indexCount = (uint32_t)index.size();
    ch.zScale = ZS;
    ch.offCells = (uint32_t)coll.size();
    coll.insert(coll.end(), (uint8_t*)cells.data(), (uint8_t*)(cells.data() + cells.size()));
    ch.offIndex = (uint32_t)coll.size();
    coll.insert(coll.end(), (uint8_t*)index.data(), (uint8_t*)(index.data() + index.size()));
    ch.offTris = (uint32_t)coll.size();
    coll.insert(coll.end(), (uint8_t*)tris.data(), (uint8_t*)(tris.data() + tris.size()));
    std::memcpy(coll.data(), &ch, sizeof ch);
    int onFloor = 0;
    float z = w3d_walk_height(coll.data(), 15.0f, 5.0f, 0.0f, 1.5f, 1, 0.0f, &onFloor);
    CHECK(onFloor && fabsf(z - 1.0f) < 0.02f, "1 yd step: z %.3f onFloor %d", z, onFloor);
    z = w3d_walk_height(coll.data(), 35.0f, 5.0f, 0.0f, 1.5f, 1, 0.0f, &onFloor);
    CHECK(!onFloor && fabsf(z - 0.0f) < 0.02f, "2 yd step: z %.3f onFloor %d (must stay on the ground)", z, onFloor);
    z = w3d_walk_height(coll.data(), 35.0f, 5.0f, 0.6f, 1.5f, 1, 0.0f, &onFloor);   // standing 0.6 up (on the first step): 2.0 is 1.4 above
    CHECK(onFloor && fabsf(z - 2.0f) < 0.02f, "second step from 0.6 up: z %.3f onFloor %d", z, onFloor);
    std::printf("   1 yd step: climbed, 2 yd step: not, 1.4 yd from the feet: climbed\n");

    std::puts("-- wall / floor boundary");
    CHECK(fabsf(W3D_WALK_MIN_NZ - cosf(W3D_WALK_MAX_SLOPE_DEG * 3.14159265f / 180.0f)) < 1e-4f, "constants disagree");
    std::printf("   walkable up to %.0f degrees (normal z >= %.4f), the same number in the converter (wall flag) and the client\n",
                W3D_WALK_MAX_SLOPE_DEG, W3D_WALK_MIN_NZ);
    std::puts(g_fail ? "SLOPE TESTS: FAIL" : "SLOPE TESTS: PASS");
    return g_fail ? 1 : 0;
}
