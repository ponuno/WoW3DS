// GRND entries (6.18a) against ground_ref.py: w3d_ground_at() at thousands of points.
//   ground_test <pack.w3dp> <points.txt>        points: x y z hole nz
#include "w3d_ground.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = (const W3dEntry*)(b.data() + h.indexOffset);
    const uint8_t* g = nullptr;
    uint32_t gs = 0;
    for (uint32_t i = 0; i < h.entryCount; ++i)
        if (e[i].type == W3D_ENTRY_GROUND) { g = b.data() + e[i].offset; gs = e[i].size; }
    if (!g || !w3d_ground_valid(g, gs)) { std::puts("FAIL: no valid GRND entry"); return 1; }
    const float tol = ((const W3dGroundHeader*)g)->zScale + 0.005f;   // quantisation + float
    std::ifstream pf(argv[2]);
    double x, y, z, nz;
    int hole, n = 0, bad = 0, badHole = 0, badNz = 0;
    double worst = 0;
    while (pf >> x >> y >> z >> hole >> nz) {
        W3dGroundHit hit;
        ++n;
        if (!w3d_ground_at(g, (float)x, (float)y, &hit)) { ++bad; continue; }
        const double d = std::fabs(hit.z - z);
        worst = std::fmax(worst, d);
        if (d > tol) ++bad;
        if (hit.hole != hole) ++badHole;
        if (std::fabs(hit.nz - nz) > 0.002) ++badNz;
    }
    std::printf("ground: %d points, worst height error %.4f yd (tolerance %.4f), %d wrong heights, %d wrong holes, %d wrong normals\n", n,
                worst, tol, bad, badHole, badNz);
    float zz;
    const bool outside = !w3d_ground_height(g, ((const W3dGroundHeader*)g)->origin[0] - 50.0f, 0.0f, &zz, nullptr);
    std::printf("outside the tile: %s\n", outside ? "no ground" : "FAIL (ground reported)");
    if (n < 1000 || bad || badHole || badNz || !outside) { std::puts("GROUND: FAIL"); return 1; }
    std::puts("GROUND: PASS");
    return 0;
}
