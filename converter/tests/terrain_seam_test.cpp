// terrain_seam_test — cracks in the ground between terrain batches drawn at different LODs (stage 6.30).
//   terrain_seam_test <pack.w3dp> region|batch [expect-cracks]
// For many cameras over the tile it picks every batch's LOD as the client does (region: w3d_terrain_lod.h, 6.30; batch: by the
// distance to the batch's own box, as before 6.30), gathers the drawn triangles and looks along every chunk border for places
// where the ground on the two sides has different heights (a T-junction between a fine and a coarse level). Each such place must
// be closed by a skirt (a vertical triangle on that line) spanning both heights; one that is not is a crack: the sky shows
// through it. "expect-cracks" turns it into the negative control (the old per-batch choice on a patchwork of textures).
#include "w3d_terrain_lod.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <vector>

struct Seg { float t0, t1, z0, z1; };
struct Tri2 { float t[3], z[3]; };
struct Line { std::vector<Seg> open; std::vector<Tri2> skirts; };

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const bool region = std::strcmp(argv[2], "region") == 0;
    const bool expectCracks = argc > 3 && std::strcmp(argv[3], "expect-cracks") == 0;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    W3dPackHeader ph;
    std::memcpy(&ph, b.data(), sizeof ph);
    const W3dEntry* e = (const W3dEntry*)(b.data() + ph.indexOffset);
    long cracks = 0, checked = 0, cams = 0, mixedLod = 0;
    float worst = 0;
    int tiles = 0;
    for (uint32_t ei = 0; ei < ph.entryCount; ++ei) {
        if (e[ei].type != W3D_ENTRY_TERRAIN) continue;
        ++tiles;
        const uint8_t* terr = b.data() + e[ei].offset;
        const W3dTerrainHeader* th = (const W3dTerrainHeader*)terr;
        const W3dTerrainBatch* bt = (const W3dTerrainBatch*)(terr + th->offBatches);
        const W3dTerrainVertex* v = (const W3dTerrainVertex*)(terr + th->offVertices);
        const uint16_t* idx = (const uint16_t*)(terr + th->offIndices);
        std::vector<uint8_t> maxLod(th->batchCount), breg(th->batchCount);
        W3dTerrainRegion regs[W3D_TERRAIN_REGIONS];
        w3d_terrain_batch_max_lod(terr, maxLod.data());
        w3d_terrain_regions(terr, maxLod.data(), breg.data(), regs);
        auto world = [&](uint16_t i, float out[3]) {
            out[0] = th->origin[0] + th->posOffset[0] + v[i].x * th->posScale[0];
            out[1] = th->origin[1] + th->posOffset[1] + v[i].y * th->posScale[1];
            out[2] = th->origin[2] + th->posOffset[2] + v[i].z * th->posScale[2];
        };
        const float ts = 533.33333f;
        // cameras: a grid over the tile and around it, 2 yd over the middle height
        for (int cy = -1; cy <= 9; ++cy)
            for (int cx = -1; cx <= 9; ++cx) {
                const float cam[3] = {th->origin[0] + (cx + 0.37f) * ts / 8, th->origin[1] + (cy + 0.61f) * ts / 8,
                                      th->origin[2] + th->posOffset[2] + 2.0f};
                ++cams;
                std::vector<int> lod(th->batchCount);
                std::vector<int> regLod(W3D_TERRAIN_REGIONS, -1);
                for (uint32_t i = 0; i < th->batchCount; ++i) {
                    if (region) {
                        lod[i] = w3d_terrain_region_lod(&regs[breg[i]], th->origin, cam);
                    } else {   // before 6.30 (render.cpp): the batch's own sphere, capped by its own max LOD
                        float r2 = 0, d2 = 0;
                        for (int k = 0; k < 3; ++k) {
                            const float c = th->origin[k] + 0.5f * (bt[i].bmin[k] + bt[i].bmax[k]), h = 0.5f * (bt[i].bmax[k] - bt[i].bmin[k]);
                            r2 += h * h;
                            d2 += (c - cam[k]) * (c - cam[k]);
                        }
                        const float d = std::sqrt(d2) - std::sqrt(r2);
                        lod[i] = d < W3D_TERRAIN_LOD1_DIST ? 0 : d < W3D_TERRAIN_LOD2_DIST ? 1 : 2;
                        if (lod[i] > maxLod[i]) lod[i] = maxLod[i];
                    }
                    int& rl = regLod[breg[i]];
                    if (rl < 0) rl = lod[i];
                    else if (rl != lod[i]) ++mixedLod;
                }
                // the drawn triangles
                std::map<std::tuple<int, int, int, int, int, int>, int> edges;   // surface edges by their (quantised) end points
                struct T { float p[3][3]; int16_t q[3][3]; };
                std::vector<T> surf;
                std::map<std::pair<int, int>, Line> lines;   // (axis, int16 coordinate): axis 0 = x constant (runs along y)
                for (uint32_t i = 0; i < th->batchCount; ++i)
                    for (uint32_t k = 0; k + 2 < bt[i].indexCount[lod[i]]; k += 3) {
                        const uint16_t* tri = idx + bt[i].firstIndex[lod[i]] + k;
                        T t;
                        for (int c = 0; c < 3; ++c) {
                            world(tri[c], t.p[c]);
                            t.q[c][0] = v[tri[c]].x; t.q[c][1] = v[tri[c]].y; t.q[c][2] = v[tri[c]].z;
                        }
                        const float ax = t.p[1][0] - t.p[0][0], ay = t.p[1][1] - t.p[0][1], bx = t.p[2][0] - t.p[0][0], by = t.p[2][1] - t.p[0][1];
                        if (std::fabs(ax * by - ay * bx) > 1e-4f) { surf.push_back(t); continue; }
                        // vertical: a skirt (on an axis-aligned line)
                        for (int axis = 0; axis < 2; ++axis)
                            if (t.q[0][axis] == t.q[1][axis] && t.q[1][axis] == t.q[2][axis]) {
                                Tri2 s;
                                for (int c = 0; c < 3; ++c) { s.t[c] = t.p[c][1 - axis]; s.z[c] = t.p[c][2]; }
                                lines[{axis, t.q[0][axis]}].skirts.push_back(s);
                            }
                    }
                auto ekey = [](const int16_t* a, const int16_t* c) {
                    std::tuple<int, int, int> x{a[0], a[1], a[2]}, y{c[0], c[1], c[2]};
                    if (y < x) std::swap(x, y);
                    return std::tuple<int, int, int, int, int, int>{std::get<0>(x), std::get<1>(x), std::get<2>(x), std::get<0>(y), std::get<1>(y), std::get<2>(y)};
                };
                for (const T& t : surf)
                    for (int c = 0; c < 3; ++c) ++edges[ekey(t.q[c], t.q[(c + 1) % 3])];
                for (const T& t : surf)
                    for (int c = 0; c < 3; ++c) {
                        const int d = (c + 1) % 3;
                        if (edges[ekey(t.q[c], t.q[d])] != 1) continue;
                        for (int axis = 0; axis < 2; ++axis)
                            if (t.q[c][axis] == t.q[d][axis]) {
                                Seg s{t.p[c][1 - axis], t.p[d][1 - axis], t.p[c][2], t.p[d][2]};
                                if (s.t0 > s.t1) { std::swap(s.t0, s.t1); std::swap(s.z0, s.z1); }
                                lines[{axis, t.q[c][axis]}].open.push_back(s);
                            }
                    }
                // along every line: where open edges of both sides meet, the heights must agree or a skirt must span them
                for (auto& [key, L] : lines) {
                    std::set<float> ts;
                    for (const Seg& s : L.open) { ts.insert(s.t0); ts.insert(s.t1); }
                    for (float t : ts) {
                        float lo = 1e30f, hi = -1e30f;
                        int n = 0;
                        for (const Seg& s : L.open)
                            if (t >= s.t0 - 1e-3f && t <= s.t1 + 1e-3f) {
                                const float z = s.t1 > s.t0 ? s.z0 + (s.z1 - s.z0) * (t - s.t0) / (s.t1 - s.t0) : s.z0;
                                lo = std::min(lo, z); hi = std::max(hi, z); ++n;
                            }
                        if (n < 2) continue;
                        ++checked;
                        if (hi - lo < 0.02f) continue;
                        // the skirts' vertical extents at t
                        std::vector<std::pair<float, float>> iv;
                        for (const Tri2& s : L.skirts) {
                            float a = 1e30f, c = -1e30f;
                            for (int q = 0; q < 3; ++q) {
                                const int r = (q + 1) % 3;
                                const float t0 = s.t[q], t1 = s.t[r];
                                if (std::fabs(t1 - t0) < 1e-5f) {
                                    if (std::fabs(t - t0) < 1e-3f) { a = std::min({a, s.z[q], s.z[r]}); c = std::max({c, s.z[q], s.z[r]}); }
                                    continue;
                                }
                                const float u = (t - t0) / (t1 - t0);
                                if (u < -1e-4f || u > 1 + 1e-4f) continue;
                                const float z = s.z[q] + (s.z[r] - s.z[q]) * std::clamp(u, 0.0f, 1.0f);
                                a = std::min(a, z); c = std::max(c, z);
                            }
                            if (a <= c) iv.push_back({a, c});
                        }
                        std::sort(iv.begin(), iv.end());
                        float reach = lo;   // covered from lo up to reach
                        for (auto& [a, c] : iv)
                            if (a <= reach + 0.01f) reach = std::max(reach, c);
                        // (skirts hang below their edge: the interval must start at or below lo)
                        bool startOk = false;
                        for (auto& [a, c] : iv) startOk |= a <= lo + 0.01f && c >= lo - 0.01f;
                        if (!startOk || reach < hi - 0.01f) {
                            ++cracks;
                            worst = std::max(worst, hi - lo);
                            if (std::getenv("SEAM_DEBUG") && cracks <= 40)
                                std::printf("  crack: axis %d at %d t %.2f gap %.2f..%.2f, skirt reach %.2f (%s), %zu skirts\n", key.first,
                                            key.second, t, lo, hi, reach, startOk ? "starts" : "no start", L.skirts.size());
                        }
                    }
                }
            }
    }
    std::printf("terrain seams (%s): %d tiles, %ld cameras, %ld seam points with both sides drawn, %ld cracks (worst gap %.2f yd), "
                "%ld batches at another LOD than their region\n",
                region ? "region" : "batch", tiles, cams, checked, cracks, worst, mixedLod);
    const bool ok = tiles > 0 && checked > 0 && (expectCracks ? cracks > 0 : cracks == 0);
    std::printf("TERRAIN SEAMS (%s%s): %s\n", region ? "region" : "batch", expectCracks ? ", negative control" : "", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
