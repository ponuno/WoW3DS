// 6.15: a street lamp (a thin post under a finely tessellated lantern) must keep its post in the lighter LODs: the far lamps of
// Stormwind were lanterns hanging in the air. Any LOD that is produced must reach as far as the full mesh.
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "conv.h"

using namespace w3d;

int main() {
    int fails = 0;
    for (int variant = 0; variant < 4; ++variant) {
        std::vector<float> pos;
        std::vector<uint32_t> idx;
        std::map<std::tuple<int, int, int>, uint32_t> weld;
        auto vert = [&](float x, float y, float z) {   // welded: shells share their vertices like real meshes
            auto k = std::make_tuple((int)lroundf(x * 1000), (int)lroundf(y * 1000), (int)lroundf(z * 1000));
            auto it = weld.find(k);
            if (it != weld.end()) return it->second;
            pos.insert(pos.end(), {x, y, z});
            return weld[k] = (uint32_t)(pos.size() / 3 - 1);
        };
        const float radius = variant == 0 ? 0.05f : variant == 1 ? 0.1f : 0.03f;
        const int N = variant >= 2 ? 6 : 16;
        const uint32_t cb = vert(0, 0, 0), ct = vert(0, 0, 3.4f);
        for (int i = 0; i < N; ++i) {
            float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
            uint32_t b0 = vert(radius * cosf(a0), radius * sinf(a0), 0), b1 = vert(radius * cosf(a1), radius * sinf(a1), 0);
            uint32_t t0 = vert(radius * cosf(a0), radius * sinf(a0), 3.4f), t1 = vert(radius * cosf(a1), radius * sinf(a1), 3.4f);
            idx.insert(idx.end(), {b0, b1, t1, b0, t1, t0, cb, b1, b0, ct, t0, t1});
        }
        // lantern: a 0.5 x 0.5 x 0.6 box, every face 6 x 6 quads (flat: the simplifier's first victims)
        const int G = variant == 3 ? 1 : 6;   // variant 3: a bare box lantern - the budget can only be met by eating the post
        for (int f = 0; f < 6; ++f)
            for (int a = 0; a < G; ++a)
                for (int b = 0; b < G; ++b) {
                    auto P = [&](int i, int j) {
                        float u = -0.25f + 0.5f * i / G, v = -0.25f + 0.5f * j / G, w = 3.4f + 0.6f * j / G;
                        float h = (f & 1) ? 0.25f : -0.25f;
                        switch (f >> 1) {
                            case 0: return vert(h, u, 3.4f + 0.6f * (j / (float)G));
                            case 1: return vert(u, h, 3.4f + 0.6f * (j / (float)G));
                            default: return vert(u, v, (f & 1) ? 4.0f : 3.4f);
                        }
                    };
                    uint32_t q0 = P(a, b), q1 = P(a + 1, b), q2 = P(a + 1, b + 1), q3 = P(a, b + 1);
                    idx.insert(idx.end(), {q0, q1, q2, q0, q2, q3});
                }
        std::vector<float> uv(pos.size() / 3 * 2, 0.0f);
        auto lods = simplifyLods(idx.data(), idx.size(), pos.data(), pos.size() / 3, 12, uv.data(), 8, nullptr, false);
        for (int k = 0; k < 2; ++k) {
            if (lods[k].empty()) { std::printf("variant %d LOD %d: none\n", variant, k + 1); continue; }
            float zmin = 1e30f;
            for (uint32_t i : lods[k]) zmin = std::min(zmin, pos[i * 3 + 2]);
            std::printf("variant %d LOD %d: %zu of %zu tris, lowest z %.2f\n", variant, k + 1, lods[k].size() / 3, idx.size() / 3, zmin);
            if (zmin > 0.1f) { ++fails; std::printf("FAIL: the post is gone\n"); }
        }
    }
    std::printf("LOD THIN: %s (levels refused: %zu)\n", fails ? "FAIL" : "PASS", (size_t)g_lodStats.rejected);
    return fails != 0;
}
