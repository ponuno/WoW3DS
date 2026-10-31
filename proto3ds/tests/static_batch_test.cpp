// 6.44: the merging of small static doodads (static_batch.cpp): batches per texture, positions / normals / uvs carried into the group's
// frame, levels of detail, the 65535-vertex split, eligibility, size classes.
#include <math.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "static_batch.hpp"

static int bad = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++bad; } }

// A model: a grid of n x n quads in the xy plane (z = 0.5 * x), part 0 the left half (texture slot 0), part 1 the right half (slot 1);
// optionally a lighter level of part 0 (its first two triangles) and a blended part.
static std::vector<uint8_t> model(int n, bool lod, uint8_t blend1 = W3D_BLEND_OPAQUE) {
    std::vector<W3dModelVertex> v;
    const float sc = 1.0f / 1024.0f;   // posScale
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            W3dModelVertex q{};
            q.x = (int16_t)(x * 1024); q.y = (int16_t)(y * 1024); q.z = (int16_t)(x * 512);
            q.nx = 0; q.ny = 0; q.nz = 127;
            q.u = (int16_t)(x * 4096 / n); q.v = (int16_t)(y * 4096 / n);
            v.push_back(q);
        }
    std::vector<uint16_t> i0, i1;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const uint16_t a = (uint16_t)(y * (n + 1) + x), b = a + 1, c = a + n + 1, d = c + 1;
            auto& dst = x < n / 2 ? i0 : i1;
            dst.insert(dst.end(), {a, b, d, a, d, c});
        }
    std::vector<uint16_t> idx = i0;
    idx.insert(idx.end(), i1.begin(), i1.end());
    const uint32_t lodFirst = (uint32_t)idx.size();
    if (lod) idx.insert(idx.end(), i0.begin(), i0.begin() + 6);
    W3dModelHeader h{};
    h.posScale[0] = h.posScale[1] = h.posScale[2] = sc;
    h.uvScale = 1.0f / 4096.0f;
    h.vertexCount = (uint32_t)v.size();
    h.indexCount = (uint32_t)idx.size();
    h.submeshCount = 2;
    h.texCount = 2;
    W3dSubmesh s[2] = {};
    s[0].tex = 0; s[0].blend = W3D_BLEND_OPAQUE; s[0].firstIndex = 0; s[0].indexCount = (uint32_t)i0.size();
    s[1].tex = 1; s[1].blend = blend1; s[1].firstIndex = (uint32_t)i0.size(); s[1].indexCount = (uint32_t)i1.size();
    W3dSubLod l[2] = {};
    if (lod) { l[0].firstIndex[0] = lodFirst; l[0].indexCount[0] = 6; }
    std::vector<uint8_t> b(sizeof h);
    auto put = [&](const void* p, size_t sz) { while (b.size() % 4) b.push_back(0); size_t at = b.size(); b.resize(at + sz); memcpy(&b[at], p, sz); return (uint32_t)at; };
    uint32_t tex[2] = {11, 22};
    h.offTexIds = put(tex, sizeof tex);
    h.offSubmeshes = put(s, sizeof s);
    h.offVertices = put(v.data(), v.size() * sizeof(W3dModelVertex));
    h.offIndices = put(idx.data(), idx.size() * 2);
    if (lod) { h.lodCount = W3D_MODEL_LODS; h.offLods = put(l, sizeof l); }
    memcpy(b.data(), &h, sizeof h);
    return b;
}

int main() {
    check(sbatch::bucketOf(0.5f) == 0 && sbatch::bucketOf(2.0f) == 1 && sbatch::bucketOf(8.0f) == 2 && sbatch::bucketOf(20.0f) == -1, "size classes");
    std::vector<uint8_t> m = model(4, true);
    check(sbatch::eligibleModel(m.data(), (uint32_t)m.size()), "an opaque model takes part");
    std::vector<uint8_t> blended = model(4, false, W3D_BLEND_ALPHA);
    check(!sbatch::eligibleModel(blended.data(), (uint32_t)blended.size()), "a model with a blended part does not");
    std::vector<uint8_t> cut = model(4, false, W3D_BLEND_ALPHAKEY);
    check(sbatch::eligibleModel(cut.data(), (uint32_t)cut.size()), "a cut-out part is fine");

    // three placements: turned 90 degrees about Z and scaled 2, plain, turned about X; textures A / B (the same for all)
    const uintptr_t keys[2] = {0xA, 0xB};
    std::vector<sbatch::MemberIn> mem = {
        {m.data(), {100, 200, 10}, {0, 0, 1.5707963f}, 2.0f, keys},
        {m.data(), {110, 205, 11}, {0, 0, 0}, 1.0f, keys},
        {m.data(), {120, 190, 12}, {0.3f, 0, 0}, 1.0f, keys},
    };
    const float origin[3] = {112, 198, 10};
    std::vector<sbatch::BatchOut> out = sbatch::build(mem, origin);
    check(out.size() == 2, "two batches: one per texture");
    if (out.size() != 2) { std::printf("STATIC BATCH: FAIL\n"); return 1; }
    const sbatch::BatchOut& a = out[0].tex == 0xA ? out[0] : out[1];
    check(a.count[0] == 3 * 8 * 6, "texture A: the left half of each of the three (8 quads each)");
    check(a.count[1] == 3 * 6 && a.count[2] == 3 * 6, "levels 1 and 2: the lighter part (2 triangles each)");
    bool inRange = true;
    for (uint16_t i : a.idx) inRange &= i < a.v.size();
    check(inRange, "every index inside the batch's vertices");
    // the first vertex of member 0 (grid 0,0 -> model (0,0,0)) is at its placement: (100,200,10) - origin
    auto pos = [&](const W3dModelVertex& q, float o[3]) {
        o[0] = q.x * a.scale[0] + a.offset[0]; o[1] = q.y * a.scale[1] + a.offset[1]; o[2] = q.z * a.scale[2] + a.offset[2];
    };
    float p[3];
    pos(a.v[0], p);
    check(fabsf(p[0] + 12) < 0.01f && fabsf(p[1] - 2) < 0.01f && fabsf(p[2] - 0) < 0.01f, "member 0's corner at its placement");
    // vertex (1,0) of member 0: model (1, 0, 0.5) * 2 turned 90 degrees -> (0, 2, 1) + (100,200,10) - origin = (-12, 4, 1)
    pos(a.v[1], p);
    check(fabsf(p[0] + 12) < 0.01f && fabsf(p[1] - 4) < 0.01f && fabsf(p[2] - 1) < 0.01f, "turn and scale baked in");
    // member 2 is turned about X: its normals lean (0, -sin, cos)
    const W3dModelVertex& last = a.v.back();
    const float nl = sqrtf((float)(last.nx * last.nx + last.ny * last.ny + last.nz * last.nz));
    check(fabsf(last.ny / nl + sinf(0.3f)) < 0.02f && fabsf(last.nz / nl - cosf(0.3f)) < 0.02f, "normals turned with the placement");
    // uvs: member 1's vertex (2,0) has u = 0.5
    {
        const size_t per = a.v.size() / 3;   // (each member contributes the same vertex range)
        const W3dModelVertex& q = a.v[per + 2];
        check(fabsf(q.u * a.uvScale - 0.5f) < 0.002f && fabsf(q.v * a.uvScale) < 0.002f, "texture coordinates kept");
    }
    // many members: more than 65535 vertices split into several batches of the same texture
    {
        std::vector<uint8_t> big = model(60, false);   // 61 x 61 = 3721 vertices
        check(sbatch::eligibleModel(big.data(), (uint32_t)big.size()), "a 3721-vertex model takes part");
        std::vector<sbatch::MemberIn> many;
        for (int i = 0; i < 40; ++i) many.push_back({big.data(), {(float)i, 0, 0}, {0, 0, 0}, 1.0f, keys});
        std::vector<sbatch::BatchOut> o2 = sbatch::build(many, origin);
        size_t aBatches = 0, verts = 0, tris = 0;
        bool ok = true;
        for (const auto& b : o2) {
            ok &= b.v.size() <= 65535;
            if (b.tex == 0xA) { ++aBatches; verts += b.v.size(); tris += b.count[0] / 3; }
        }
        check(ok && aBatches >= 2, "over 65535 vertices: split, each batch within 16-bit indices");
        check(tris == 40u * 30u * 60u * 2u, "no triangle lost in the split");
        (void)verts;
    }
    std::printf("STATIC BATCH: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
