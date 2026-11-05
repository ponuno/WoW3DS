// 6.84: lighter versions of a crown of flat leaf cards: whole cards go, the largest and the outermost stay.
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

#include "conv.h"

using namespace w3d;

int main() {
    int fails = 0, checks = 0;
    auto check = [&](bool ok, const char* what, double v) { ++checks; if (!ok) { ++fails; std::printf("FAIL %s (%g)\n", what, v); } };
    // 80 cards (two triangles each, four private vertices) scattered over a crown of radius 6, sizes 0.8 .. 3.2 yd
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    unsigned seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; };
    double fullArea = 0;
    for (int c = 0; c < 80; ++c) {
        const float a = rnd() * 6.2831853f, e = (rnd() - 0.3f) * 1.6f, r = 2.0f + rnd() * 4.0f, sz = 0.8f + rnd() * 2.4f;
        const float cx = r * std::cos(a), cy = r * std::sin(a), cz = 6.0f + r * std::sin(e);
        const uint32_t b = (uint32_t)(pos.size() / 3);
        pos.insert(pos.end(), {cx - sz, cy, cz - sz, cx + sz, cy, cz - sz, cx + sz, cy, cz + sz, cx - sz, cy, cz + sz});
        idx.insert(idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
        fullArea += 4.0 * sz * sz;
    }
    auto lv = cardLods(idx.data(), idx.size(), pos.data(), 3 * sizeof(float));
    const double t0 = idx.size() / 3.0;
    check(!lv[0].empty() && !lv[1].empty(), "both levels made", 0);
    const double r1 = lv[0].size() / 3.0 / t0, r2 = lv[1].size() / 3.0 / t0;
    check(r1 > 0.5 && r1 < 0.72, "level 1 about 60% of the triangles", r1);
    check(r2 > 0.25 && r2 < 0.45, "level 2 about 35% of the triangles", r2);
    check(lv[0].size() % 6 == 0 && lv[1].size() % 6 == 0, "whole cards (two triangles each)", 0);
    // nested, valid
    std::set<uint32_t> s1(lv[0].begin(), lv[0].end());
    bool nested = true, valid = true;
    for (uint32_t i : lv[1]) nested = nested && s1.count(i);
    for (uint32_t i : lv[0]) valid = valid && i < pos.size() / 3;
    check(nested, "level 2 inside level 1", 0);
    check(valid, "valid indices", 0);
    // the crown keeps its extent and most of its area (the largest cards stay)
    auto stats = [&](const std::vector<uint32_t>& v, float lo[3], float hi[3], double* area) {
        for (int k = 0; k < 3; ++k) { lo[k] = 1e30f; hi[k] = -1e30f; }
        *area = 0;
        for (size_t t = 0; t + 2 < v.size(); t += 3) {
            const float *a = &pos[v[t] * 3], *b = &pos[v[t + 1] * 3], *c = &pos[v[t + 2] * 3];
            for (int k = 0; k < 3; ++k) { lo[k] = std::min({lo[k], a[k], b[k], c[k]}); hi[k] = std::max({hi[k], a[k], b[k], c[k]}); }
            float e1[3], e2[3];
            for (int k = 0; k < 3; ++k) { e1[k] = b[k] - a[k]; e2[k] = c[k] - a[k]; }
            const double gx = (double)e1[1] * e2[2] - (double)e1[2] * e2[1], gy = (double)e1[2] * e2[0] - (double)e1[0] * e2[2],
                         gz = (double)e1[0] * e2[1] - (double)e1[1] * e2[0];
            *area += 0.5 * std::sqrt(gx * gx + gy * gy + gz * gz);
        }
    };
    float flo[3], fhi[3], l2lo[3], l2hi[3];
    double fa, a2;
    stats(idx, flo, fhi, &fa);
    stats(lv[1], l2lo, l2hi, &a2);
    for (int k = 0; k < 3; ++k) {
        check(l2lo[k] <= flo[k] + 1e-4f && l2hi[k] >= fhi[k] - 1e-4f, "the extent is the same on every axis", k);
    }
    check(a2 > 0.55 * fa, "level 2 keeps over half of the area (the large cards stay)", a2 / fa);
    // the same through simplifyLods(cards = true)
    auto lv2 = simplifyLods(idx.data(), idx.size(), pos.data(), pos.size() / 3, 3 * sizeof(float), pos.data(), 3 * sizeof(float), nullptr, false,
                            nullptr, true);
    check(lv2[0] == lv[0] && lv2[1] == lv[1], "simplifyLods(cards) = cardLods", 0);
    // too few cards / triangles: nothing to gain
    auto few = cardLods(idx.data(), 3 * 12, pos.data(), 3 * sizeof(float));
    check(few[0].empty() && few[1].empty(), "a handful of triangles stays as it is", 0);
    std::printf("CARD LOD: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
