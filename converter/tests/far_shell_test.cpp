// far_shell_test (6.29): buildFarShell on dense meshes like real buildings - a 24 x 16 x 30 yd tower of 40 x 40 quads per side
// (19200 triangles) with a 3 yd spire, and a thin wall (two faces 0.3 yd apart) - stays within its triangle budget, inside the
// mesh's box, covers it, keeps colours per side, and drops buildings below the size threshold.
#include "conv.h"

#include <cmath>
#include <cstdio>

using namespace w3d;
static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void quadGrid(std::vector<float>& t, std::vector<float>& rgb, const float o[3], const float u[3], const float v[3], int n,
                     const float c[3]) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            float p[4][3];
            const int di[4] = {0, 1, 1, 0}, dj[4] = {0, 0, 1, 1};
            for (int k = 0; k < 4; ++k)
                for (int q = 0; q < 3; ++q) p[k][q] = o[q] + (float)(i + di[k]) / n * u[q] + (float)(j + dj[k]) / n * v[q];
            for (int k : {0, 1, 2, 0, 2, 3}) t.insert(t.end(), p[k], p[k] + 3);
            rgb.insert(rgb.end(), c, c + 3);
            rgb.insert(rgb.end(), c, c + 3);
        }
}

int main() {
    std::vector<float> t, rgb;
    const float red[3] = {0.8f, 0.1f, 0.1f}, grey[3] = {0.5f, 0.5f, 0.5f};
    const float X = 24, Y = 16, Z = 30;
    const float O[3] = {-12, -8, 0};
    // four walls and a roof, 40 x 40 quads each
    const float o1[3] = {O[0], O[1], 0}, u1[3] = {X, 0, 0}, v1[3] = {0, 0, Z};
    const float o2[3] = {O[0], O[1] + Y, 0}, u2[3] = {X, 0, 0}, v2[3] = {0, 0, Z};
    const float o3[3] = {O[0], O[1], 0}, u3[3] = {0, Y, 0}, v3[3] = {0, 0, Z};
    const float o4[3] = {O[0] + X, O[1], 0}, u4[3] = {0, Y, 0}, v4[3] = {0, 0, Z};
    const float o5[3] = {O[0], O[1], Z}, u5[3] = {X, 0, 0}, v5[3] = {0, Y, 0};
    quadGrid(t, rgb, o1, u1, v1, 40, red);
    quadGrid(t, rgb, o2, u2, v2, 40, red);
    quadGrid(t, rgb, o3, u3, v3, 40, grey);
    quadGrid(t, rgb, o4, u4, v4, 40, grey);
    quadGrid(t, rgb, o5, u5, v5, 40, grey);
    ConvStats::FarShellSrc s;
    const bool ok = buildFarShell(t, rgb, 20.0f, 300, s);
    CHECK(ok, "a shell for a 30 yd building");
    const size_t tris = s.idx.size() / 3, verts = s.pos.size() / 3;
    CHECK(tris > 20 && tris <= 300, "%zu triangles from %zu (budget 300)", tris, t.size() / 9);
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (size_t v = 0; v < verts; ++v)
        for (int q = 0; q < 3; ++q) { lo[q] = std::min(lo[q], s.pos[v * 3 + q]); hi[q] = std::max(hi[q], s.pos[v * 3 + q]); }
    CHECK(lo[0] >= -12.01f && hi[0] <= 12.01f && lo[1] >= -8.01f && hi[1] <= 8.01f && lo[2] >= -0.01f && hi[2] <= 30.01f,
          "inside the box: %.1f..%.1f %.1f..%.1f %.1f..%.1f", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    CHECK(hi[0] - lo[0] > 19 && hi[1] - lo[1] > 12 && hi[2] - lo[2] > 25, "covers it: %.1f x %.1f x %.1f", hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    // colours: vertices on the red walls (y at the edge, below the roof) are reddish
    int redOk = 0, redN = 0;
    for (size_t v = 0; v < verts; ++v)
        if ((s.pos[v * 3 + 1] < -7 || s.pos[v * 3 + 1] > 7) && std::fabs(s.pos[v * 3]) < 8 && s.pos[v * 3 + 2] > 3 && s.pos[v * 3 + 2] < 25) {
            ++redN;
            redOk += s.rgb[v * 3] > s.rgb[v * 3 + 2] + 0.15f;
        }
    CHECK(redN > 0 && redOk == redN, "the red walls stay red (%d of %d)", redOk, redN);
    bool nOk = true, iOk = true;
    for (size_t v = 0; v < verts; ++v) {
        const float l = std::sqrt(s.nrm[v * 3] * s.nrm[v * 3] + s.nrm[v * 3 + 1] * s.nrm[v * 3 + 1] + s.nrm[v * 3 + 2] * s.nrm[v * 3 + 2]);
        nOk &= std::fabs(l - 1.0f) < 0.01f;
    }
    for (uint32_t i : s.idx) iOk &= i < verts;
    CHECK(nOk && iOk, "unit normals, indices in range");
    CHECK(s.radius > 30 && s.radius < 33, "radius %.1f", s.radius);
    // too small: no shell
    std::vector<float> st, sr;
    const float so[3] = {0, 0, 0}, su[3] = {6, 0, 0}, sv[3] = {0, 0, 4};
    quadGrid(st, sr, so, su, sv, 4, grey);
    ConvStats::FarShellSrc small;
    CHECK(!buildFarShell(st, sr, 20.0f, 300, small), "a 6 yd shed gets none");
    // a smaller budget is kept too
    ConvStats::FarShellSrc tight;
    CHECK(buildFarShell(t, rgb, 20.0f, 80, tight) && tight.idx.size() / 3 <= 80, "budget 80: %zu triangles", tight.idx.size() / 3);
    std::printf("FAR SHELL: %zu -> %zu triangles, %zu vertices; %d checks, %d failed -> %s\n", t.size() / 9, tris, verts, g_ok + g_fail,
                g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
