// Stage 6.47: buildings' triangles. /gputest (6.46) in Stormwind: the buildings were 46% of the frame's triangles at a draw distance
// of 120 and "lightest LOD everywhere" saved only 12%. Two causes in buildModelBlob:
//   1. a group under its budget was not welded, so its LOD levels saw every panel edge (WMO batches carry their own vertices,
//      positions off by float noise) as a locked border - the levels hardly simplified;
//   2. the 0.15-yd floor bound (6.19b) held walls and roofs too.
// Here: a street block - a bumpy floor and four walls of separate 2 x 2 yd panels (own vertices, noise 1e-4), each panel bent by
// up to 0.25 yd. Checked:
//   A. under the budget: LOD 1 / 2 exist, are much lighter, and still cover the surface (no cracks): every point of the original
//      surface lies within the level's error (+ a margin) of the level's mesh; the header carries the errors;
//   B. over the budget: floors stay within 0.15 yd, walls simplify further than floors;
//   C. a model that is not a building keeps share-made levels and no errors in the header.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <array>
#include <set>
#include <algorithm>

#include "conv.h"
#include "w3d_format.h"

using namespace w3d;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static float pointTri(const float p[3], const float a[3], const float b[3], const float c[3]) {
    float ab[3], ac[3], ap[3];
    for (int k = 0; k < 3; ++k) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; ap[k] = p[k] - a[k]; }
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    float q[3];
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    float bp[3], cp[3];
    for (int k = 0; k < 3; ++k) { bp[k] = p[k] - b[k]; cp[k] = p[k] - c[k]; }
    const float d3 = dot(ab, bp), d4 = dot(ac, bp), d5 = dot(ab, cp), d6 = dot(ac, cp);
    const float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
    if (d1 <= 0 && d2 <= 0) { for (int k = 0; k < 3; ++k) q[k] = a[k]; }
    else if (d3 >= 0 && d4 <= d3) { for (int k = 0; k < 3; ++k) q[k] = b[k]; }
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) { const float v = d1 / (d1 - d3); for (int k = 0; k < 3; ++k) q[k] = a[k] + v * ab[k]; }
    else if (d6 >= 0 && d5 <= d6) { for (int k = 0; k < 3; ++k) q[k] = c[k]; }
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) { const float w = d2 / (d2 - d6); for (int k = 0; k < 3; ++k) q[k] = a[k] + w * ac[k]; }
    else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; ++k) q[k] = b[k] + w * (c[k] - b[k]);
    } else {
        const float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
        for (int k = 0; k < 3; ++k) q[k] = a[k] + ab[k] * v + ac[k] * w;
    }
    const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static float noise(int i) { return (float)((i * 2654435761u) % 1000) * 1e-7f; }   // 0 .. 1e-4
static float bump(int a, int b) { return (float)((((uint32_t)a * 73856093u) ^ ((uint32_t)b * 19349663u)) % 1000u) / 1000.0f; }   // 0 .. 1

// The block: floor 40 x 40 yd (a quad per yard, cobbles up to 0.3 yd), walls 40 x 10 yd round it of 2 x 2 yd panels, each
// subdivided 4 x 4 and bumped out by up to 0.3 yd (a panel's own vertices). Floor points listed in floorPts, all in allPts.
struct Block { ModelSource m; std::vector<float> floorPts, wallPts; };

static Block makeBlock(uint8_t flags) {
    Block b;
    ModelSource::Sub floor, wall;
    floor.texId = 0; wall.texId = 1;
    floor.blend = wall.blend = W3D_BLEND_OPAQUE;
    floor.flags = wall.flags = flags;
    auto addV = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        ModelSource::V w{};
        w.p[0] = x; w.p[1] = y; w.p[2] = z; w.n[0] = nx; w.n[1] = ny; w.n[2] = nz; w.uv[0] = u; w.uv[1] = v;
        b.m.verts.push_back(w);
        return (uint32_t)(b.m.verts.size() - 1);
    };
    auto centre = [&](std::vector<float>& pts, uint32_t i0, uint32_t i1, uint32_t i2) {
        for (int k = 0; k < 3; ++k) pts.push_back((b.m.verts[i0].p[k] + b.m.verts[i1].p[k] + b.m.verts[i2].p[k]) / 3.0f);
    };
    const int F = 40;
    const uint32_t f0 = (uint32_t)b.m.verts.size();
    for (int y = 0; y <= F; ++y)
        for (int x = 0; x <= F; ++x) {
            const float z = 0.3f * bump(x, y);   // cobbles: more than the floor bound, less than the wall bound
            addV((float)x - 20, (float)y - 20, z, 0, 0, 1, x * 0.25f, y * 0.25f);
        }
    for (int y = 0; y < F; ++y)
        for (int x = 0; x < F; ++x) {
            const uint32_t a = f0 + y * (F + 1) + x, c = a + 1, d = a + F + 1, e = d + 1;
            floor.idx.insert(floor.idx.end(), {a, c, e, a, e, d});
            centre(b.floorPts, a, c, e); centre(b.floorPts, a, e, d);
        }
    // walls: side s (0..3) runs along the block's edge, outward normal
    const int S = 4;
    int seed = 1;
    for (int side = 0; side < 4; ++side)
        for (int pu = 0; pu < 20; ++pu)
            for (int pv = 0; pv < 5; ++pv) {
                const uint32_t base = (uint32_t)b.m.verts.size();
                for (int j = 0; j <= S; ++j)
                    for (int i = 0; i <= S; ++i) {
                        const float u = pu * 2.0f + i * (2.0f / S), v = pv * 2.0f + j * (2.0f / S);
                        // bow: a smooth bulge over the whole wall (0.25 yd), so every panel's interior is curved
                        const float bow = 0.3f * bump(side * 1000 + pu * S + i, pv * S + j);   // (the same at a panel edge's copies)
                        const float t = u - 20.0f, out = 20.0f + bow;
                        float x, y, nx, ny;
                        switch (side) {
                            case 0: x = t; y = -out; nx = 0; ny = -1; break;
                            case 1: x = out; y = t; nx = 1; ny = 0; break;
                            case 2: x = -t; y = out; nx = 0; ny = 1; break;
                            default: x = -out; y = -t; nx = -1; ny = 0; break;
                        }
                        // float noise between panels' copies of a shared edge
                        addV(x + noise(seed++), y + noise(seed++), v + noise(seed++), nx, ny, 0, u * 0.5f, v * 0.5f);
                    }
                for (int j = 0; j < S; ++j)
                    for (int i = 0; i < S; ++i) {
                        const uint32_t a = base + j * (S + 1) + i, c = a + 1, d = a + S + 1, e = d + 1;
                        // CCW seen from outside (normal n): (a, c, e) for side 0 faces -y... let orientByNormals fix any side
                        wall.idx.insert(wall.idx.end(), {a, c, e, a, e, d});
                        centre(b.wallPts, a, c, e); centre(b.wallPts, a, e, d);
                    }
            }
    b.m.texIds = {1, 2};
    b.m.subs.push_back(floor);
    b.m.subs.push_back(wall);
    return b;
}

struct Decoded {
    const W3dModelHeader* h = nullptr;
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    std::vector<W3dSubmesh> subs;
    const W3dSubLod* lods = nullptr;
};

static Decoded decode(const std::vector<uint8_t>& blob) {
    Decoded d;
    d.h = (const W3dModelHeader*)blob.data();
    const W3dModelVertex* v = (const W3dModelVertex*)(blob.data() + d.h->offVertices);
    for (uint32_t i = 0; i < d.h->vertexCount; ++i) {
        d.pos.push_back(v[i].x * d.h->posScale[0] + d.h->posOffset[0]);
        d.pos.push_back(v[i].y * d.h->posScale[1] + d.h->posOffset[1]);
        d.pos.push_back(v[i].z * d.h->posScale[2] + d.h->posOffset[2]);
    }
    const uint16_t* ix = (const uint16_t*)(blob.data() + d.h->offIndices);
    for (uint32_t i = 0; i < d.h->indexCount; ++i) d.idx.push_back(ix[i]);
    const W3dSubmesh* s = (const W3dSubmesh*)(blob.data() + d.h->offSubmeshes);
    d.subs.assign(s, s + d.h->submeshCount);
    d.lods = w3d_model_lods(blob.data(), (uint32_t)blob.size());
    return d;
}

// Triangles drawn at level k (0 = full; else the lightest there is up to k, as the client picks)
static std::vector<uint32_t> level(const Decoded& d, int k) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < d.subs.size(); ++i) {
        uint32_t first = d.subs[i].firstIndex, count = d.subs[i].indexCount;
        if (k && d.lods)
            for (int j = k; j >= 1; --j)
                if (d.lods[i].indexCount[j - 1]) { first = d.lods[i].firstIndex[j - 1]; count = d.lods[i].indexCount[j - 1]; break; }
        out.insert(out.end(), d.idx.begin() + first, d.idx.begin() + first + count);
    }
    return out;
}

// The worst distance from the points to the mesh, and how many are farther than lim
static float worst(const std::vector<float>& pts, const Decoded& d, const std::vector<uint32_t>& tri, float lim, size_t* over) {
    float w = 0;
    *over = 0;
    for (size_t p = 0; p < pts.size(); p += 3) {
        float best = 1e30f;
        for (size_t t = 0; t + 2 < tri.size(); t += 3) {
            const float dd = pointTri(&pts[p], &d.pos[tri[t] * 3], &d.pos[tri[t + 1] * 3], &d.pos[tri[t + 2] * 3]);
            if (dd < best) best = dd;
            if (best < 0.01f) break;
        }
        if (best > lim) ++*over;
        if (best > w) w = best;
    }
    return w;
}

int main() {
    const float lodErr[2] = {0.2f, 0.6f};
    // ---- A. under the budget: the levels (borders locked as in 6.47, and free as the 6.47b default: still covered within the error)
    for (int lockB = 1; lockB >= 0; --lockB) {
        std::printf("A: borders %s\n", lockB ? "locked" : "free");
        Block b = makeBlock(W3D_SUBF_BUILDING);
        const size_t n0 = (b.floorPts.size() + b.wallPts.size()) / 3;
        size_t tris = 0;
        auto blob = buildModelBlob(b.m, 100000, &tris, nullptr, 0.15f, 0.5f, lodErr, lockB != 0);
        CHECK(!blob.empty(), "A: no blob");
        if (!blob.empty()) {
            Decoded d = decode(blob);
            CHECK(tris == n0, "A: under the budget the full mesh is kept (%zu of %zu)", tris, n0);
            CHECK(d.lods, "A: no LOD table");
            CHECK(std::fabs(w3d_model_lod_err(d.h, 1) - 0.2f) < 1e-6f && std::fabs(w3d_model_lod_err(d.h, 2) - 0.6f) < 1e-6f,
                  "A: header errors %.3f %.3f", w3d_model_lod_err(d.h, 1), w3d_model_lod_err(d.h, 2));
            std::vector<float> all = b.floorPts;
            all.insert(all.end(), b.wallPts.begin(), b.wallPts.end());
            size_t prev = tris;
            for (int k = 1; k <= 2; ++k) {
                auto t = level(d, k);
                size_t over = 0;
                const float lim = lodErr[k - 1] * 1.25f + 0.02f;
                const float w = worst(all, d, t, lim, &over);
                std::printf("A: level %d: %zu of %zu tris (%.0f%%), worst surface distance %.3f yd, %zu of %zu points past %.2f\n", k,
                            t.size() / 3, tris, 100.0 * (t.size() / 3) / tris, w, over, all.size() / 3, lim);
                CHECK(over == 0, "A: level %d leaves %zu points uncovered (a crack or a fold)", k, over);
                CHECK(t.size() / 3 < prev, "A: level %d not lighter than the one below", k);
                prev = t.size() / 3;
            }
            CHECK(level(d, 1).size() / 3 * 2 < tris, "A: level 1 should drop at least half (%zu of %zu)", level(d, 1).size() / 3, tris);
        }
    }
    // ---- B. over the budget: floors held, walls lighter
    {
        Block b = makeBlock(W3D_SUBF_BUILDING);
        const size_t nf = b.floorPts.size() / 3, nw = b.wallPts.size() / 3;
        g_simpStats[1].floorOut = 0; g_simpStats[1].wallOut = 0;
        size_t tris = 0;
        auto blob = buildModelBlob(b.m, 1500, &tris, nullptr, 0.15f, 0.5f, lodErr);
        CHECK(!blob.empty(), "B: no blob");
        if (!blob.empty()) {
            Decoded d = decode(blob);
            const size_t fo = g_simpStats[1].floorOut, wo = g_simpStats[1].wallOut;
            auto t = level(d, 0);
            size_t over = 0;
            const float wf = worst(b.floorPts, d, t, 0.15f + 0.02f, &over);
            std::printf("B: %zu -> %zu tris; floors %zu -> %zu, walls %zu -> %zu; floor worst %.3f yd (%zu past 0.17)\n", nf + nw, tris,
                        nf, fo, nw, wo, wf, over);
            CHECK(over == 0, "B: %zu floor points moved more than 0.15 yd", over);
            CHECK((double)wo / nw < (double)fo / nf, "B: walls should simplify further than floors (%zu/%zu vs %zu/%zu)", wo, nw, fo, nf);
            size_t wover = 0;
            const float ww = worst(b.wallPts, d, t, 0.5f * 1.25f + 0.02f, &wover);
            std::printf("B: wall worst %.3f yd (%zu past 0.645)\n", ww, wover);
            CHECK(wover == 0, "B: %zu wall points uncovered", wover);
        }
    }
    // ---- C. not a building: share-made levels, no errors in the header
    {
        Block b = makeBlock(0);
        size_t tris = 0;
        auto blob = buildModelBlob(b.m, 100000, &tris, nullptr, 0.15f, 0.5f, lodErr);
        CHECK(!blob.empty(), "C: no blob");
        if (!blob.empty()) {
            Decoded d = decode(blob);
            CHECK(w3d_model_lod_err(d.h, 1) == 0 && w3d_model_lod_err(d.h, 2) == 0, "C: a model carries building errors");
        }
    }
    // ---- D. the cause (control): the same walls given to the LOD maker unwelded, as a pre-6.47 group under its budget was (every
    // panel edge an open border, locked), against welded: the unwelded level stays much heavier
    {
        auto level1 = [&](bool weld) {
            Block b = makeBlock(W3D_SUBF_BUILDING);
            if (weld)
                for (auto& v : b.m.verts)
                    for (float& c : v.p) c = std::round(c * 500.0f) / 500.0f;
            const auto& w = b.m.subs[1].idx;
            auto lv = simplifyLods(w.data(), w.size(), &b.m.verts[0].p[0], b.m.verts.size(), sizeof(ModelSource::V), &b.m.verts[0].uv[0],
                                   sizeof(ModelSource::V), nullptr, true, lodErr);
            return lv[0].empty() ? w.size() / 3 : lv[0].size() / 3;
        };
        const size_t raw = level1(false), welded = level1(true);
        std::printf("D: walls' level 1 of 12800 tris: unwelded %zu, welded %zu\n", raw, welded);
        CHECK(raw * 2 > welded * 3, "D: welding should make the level much lighter (%zu vs %zu) - the test no longer shows the cause", raw,
              welded);
    }
    // ---- E. 6.47b: open pieces (beams / trims, each its own mesh, nearly every vertex on a border - what WMO detail is built of):
    // with the borders locked the levels keep almost everything; free, they simplify and stay within the error
    {
        size_t lv1[2] = {0, 0};
        for (int lockB = 1; lockB >= 0; --lockB) {
            ModelSource m;
            ModelSource::Sub sub;
            sub.texId = 0; sub.blend = W3D_BLEND_OPAQUE; sub.flags = W3D_SUBF_BUILDING;
            std::vector<float> pts;
            const int S = 6;
            for (int p = 0; p < 120; ++p) {
                const uint32_t base = (uint32_t)m.verts.size();
                const float ox = (p % 12) * 3.3f - 20, oz = (p / 12) * 1.7f;
                for (int j = 0; j <= S; ++j)
                    for (int i = 0; i <= S; ++i) {
                        ModelSource::V v{};
                        const float u = i * 3.0f / S, w = j * 1.5f / S;
                        v.p[0] = ox + u; v.p[1] = -20 + 0.15f * std::sin(u * 2.1f) * std::sin(w * 2.0f + p); v.p[2] = oz + w;
                        v.n[1] = -1; v.uv[0] = u * 0.3f; v.uv[1] = w * 0.3f;
                        m.verts.push_back(v);
                    }
                for (int j = 0; j < S; ++j)
                    for (int i = 0; i < S; ++i) {
                        const uint32_t a = base + j * (S + 1) + i, c = a + 1, d = a + S + 1, e = d + 1;
                        sub.idx.insert(sub.idx.end(), {a, c, e, a, e, d});
                        for (int k = 0; k < 3; ++k) pts.push_back((m.verts[a].p[k] + m.verts[c].p[k] + m.verts[e].p[k]) / 3.0f);
                        for (int k = 0; k < 3; ++k) pts.push_back((m.verts[a].p[k] + m.verts[e].p[k] + m.verts[d].p[k]) / 3.0f);
                    }
            }
            m.texIds = {1};
            m.subs.push_back(sub);
            size_t tris = 0;
            auto blob = buildModelBlob(m, 100000, &tris, nullptr, 0.15f, 0.5f, lodErr, lockB != 0);
            CHECK(!blob.empty(), "E: no blob");
            if (blob.empty()) continue;
            Decoded d = decode(blob);
            for (int k = 1; k <= 2; ++k) {
                auto t = level(d, k);
                size_t over = 0;
                const float lim = lodErr[k - 1] * 1.25f + 0.02f;
                const float w = worst(pts, d, t, lim, &over);
                std::printf("E: pieces, borders %s, level %d: %zu of %zu tris (%.0f%%), worst %.3f yd, %zu points past %.2f\n",
                            lockB ? "locked" : "free", k, t.size() / 3, tris, 100.0 * (t.size() / 3) / tris, w, over, lim);
                CHECK(over == 0, "E: level %d leaves %zu points farther than its error", k, over);
                if (k == 1) lv1[lockB] = t.size() / 3;
            }
        }
        CHECK(lv1[0] * 2 < lv1[1], "E: free borders should make level 1 much lighter (%zu vs locked %zu)", lv1[0], lv1[1]);
    }
    // ---- F. 6.47c: every triangle its own three vertices (one place, one uv, normals a few degrees apart): before 6.47c every edge
    // was a seam of distinct vertices, locked - nothing simplified (7v log: Stromgarde, Fenris Keep 100% at both levels)
    {
        Block src = makeBlock(W3D_SUBF_BUILDING);
        ModelSource m;
        m.texIds = src.m.texIds;
        int jit = 0;
        for (const auto& sub : src.m.subs) {
            ModelSource::Sub d = sub;
            d.idx.clear();
            for (uint32_t i : sub.idx) {
                ModelSource::V v = src.m.verts[i];
                const float j = 0.05f * (float)((jit++ % 5) - 2) / 2.0f;   // up to ~3 degrees apart
                v.n[0] += j; v.n[1] -= j;
                m.verts.push_back(v);
                d.idx.push_back((uint32_t)m.verts.size() - 1);
            }
            m.subs.push_back(d);
        }
        std::vector<float> all = src.floorPts;
        all.insert(all.end(), src.wallPts.begin(), src.wallPts.end());
        size_t tris = 0;
        auto blob = buildModelBlob(m, 100000, &tris, nullptr, 0.15f, 0.5f, lodErr, false);
        CHECK(!blob.empty(), "F: no blob");
        if (!blob.empty()) {
            Decoded d = decode(blob);
            for (int k = 1; k <= 2; ++k) {
                auto t = level(d, k);
                size_t over = 0;
                const float lim = lodErr[k - 1] * 1.25f + 0.02f;
                const float w = worst(all, d, t, lim, &over);
                std::printf("F: own vertices per triangle, level %d: %zu of %zu tris (%.0f%%), worst %.3f yd, %zu past %.2f\n", k,
                            t.size() / 3, tris, 100.0 * (t.size() / 3) / tris, w, over, lim);
                CHECK(over == 0, "F: level %d leaves %zu points farther than its error", k, over);
                CHECK(t.size() / 3 * 2 < tris, "F: level %d should be at most half (%zu of %zu)", k, t.size() / 3, tris);
            }
        }
    }
    // ---- G. 6.48: a big group cut into pieces - simplified whole first, then cut; every triangle in exactly one piece, in place
    {
        Block b = makeBlock(W3D_SUBF_BUILDING);
        for (auto& v : b.m.verts) { v.p[0] *= 6.0f; v.p[1] *= 6.0f; }   // 240 x 240 yd
        simplifyModelSource(b.m, 1500, 0.15f, 0.5f);
        // every triangle as its 9 coordinates; matched within 1 mm through a grid of its first corner
        std::vector<std::array<float, 9>> want, got;
        for (const auto& sb : b.m.subs)
            for (size_t t = 0; t + 2 < sb.idx.size(); t += 3) {
                std::array<float, 9> k;
                for (int i = 0; i < 3; ++i) for (int q = 0; q < 3; ++q) k[i * 3 + q] = b.m.verts[sb.idx[t + i]].p[q];
                want.push_back(k);
            }
        const size_t n = want.size();
        auto chunks = splitModelSource(b.m, 64.0f, 256);
        float worstR = 0;
        for (const auto& c : chunks) {
            float r = 0;
            for (const auto& v : c.src.verts) r = std::max(r, std::sqrt(v.p[0] * v.p[0] + v.p[1] * v.p[1]));
            worstR = std::max(worstR, r);
            for (const auto& sb : c.src.subs)
                for (size_t t = 0; t + 2 < sb.idx.size(); t += 3) {
                    std::array<float, 9> k;
                    for (int i = 0; i < 3; ++i) for (int q = 0; q < 3; ++q) k[i * 3 + q] = c.src.verts[sb.idx[t + i]].p[q] + c.offset[q];
                    got.push_back(k);
                }
        }
        std::map<std::array<int64_t, 3>, std::vector<size_t>> grid;
        for (size_t i = 0; i < want.size(); ++i) grid[{(int64_t)std::floor(want[i][0]), (int64_t)std::floor(want[i][1]), (int64_t)std::floor(want[i][2])}].push_back(i);
        std::vector<char> used(want.size(), 0);
        size_t unmatched = 0;
        for (const auto& k : got) {
            bool hit = false;
            const int64_t gx = (int64_t)std::floor(k[0]), gy = (int64_t)std::floor(k[1]), gz = (int64_t)std::floor(k[2]);
            for (int dx = -1; dx <= 1 && !hit; ++dx) for (int dy = -1; dy <= 1 && !hit; ++dy) for (int dz = -1; dz <= 1 && !hit; ++dz) {
                auto it = grid.find({gx + dx, gy + dy, gz + dz});
                if (it == grid.end()) continue;
                for (size_t i : it->second) {
                    if (used[i]) continue;
                    bool same = true;
                    for (int q = 0; q < 9 && same; ++q) same = std::fabs(want[i][q] - k[q]) < 1e-3f;
                    if (same) { used[i] = 1; hit = true; break; }
                }
            }
            if (!hit) ++unmatched;
        }
        std::printf("G: %zu tris cut into %zu pieces, largest horizontal radius %.0f yd\n", n, chunks.size(), worstR);
        CHECK(chunks.size() >= 4, "G: a 240-yd group should be cut (%zu pieces)", chunks.size());
        CHECK(got.size() == want.size() && unmatched == 0, "G: the pieces do not hold exactly the group's triangles (%zu vs %zu, %zu unmatched)",
              got.size(), want.size(), unmatched);
        CHECK(worstR < 110.0f, "G: a piece is too wide (%.0f yd)", worstR);
    }
    printSimplifyReport();
    std::printf("WMO LOD: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
