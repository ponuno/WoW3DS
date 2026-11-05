// 6.48: a pack converted with WMO groups cut into pieces (--wmo-chunk) must place exactly the triangles of the uncut pack in the world:
// every building triangle of both packs (model vertices through their placement: T * Rz * Ry * Rx) matched within 2 cm.
//   wmo_cut_test <uncut.w3dp> <cut.w3dp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "w3d_format.h"

using Tri = std::array<float, 9>;

static std::vector<Tri> worldTris(const char* path, size_t* placements) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    std::vector<Tri> out;
    *placements = 0;
    if (b.size() < sizeof(W3dPackHeader)) return out;
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = (const W3dEntry*)(b.data() + h.indexOffset);
    std::map<uint32_t, const uint8_t*> models;
    for (uint32_t i = 0; i < h.entryCount; ++i)
        if (e[i].type == W3D_ENTRY_MODEL) models[e[i].id] = b.data() + e[i].offset;
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        if (e[i].type != W3D_ENTRY_PLACEMENT) continue;
        W3dPlacementHeader ph;
        std::memcpy(&ph, b.data() + e[i].offset, sizeof ph);
        const W3dPlacement* p = (const W3dPlacement*)(b.data() + e[i].offset + sizeof ph);
        for (uint32_t k = 0; k < ph.count; ++k) {
            auto it = models.find(p[k].modelId);
            if (it == models.end()) continue;
            const uint8_t* m = it->second;
            const W3dModelHeader* mh = (const W3dModelHeader*)m;
            const W3dSubmesh* s = (const W3dSubmesh*)(m + mh->offSubmeshes);
            if (!mh->submeshCount || !(s[0].flags & W3D_SUBF_BUILDING)) continue;
            ++*placements;
            const W3dModelVertex* v = (const W3dModelVertex*)(m + mh->offVertices);
            const uint16_t* ix = (const uint16_t*)(m + mh->offIndices);
            const float cx = std::cos(p[k].rot[0]), sx = std::sin(p[k].rot[0]), cy = std::cos(p[k].rot[1]), sy = std::sin(p[k].rot[1]);
            const float cz = std::cos(p[k].rot[2]), sz = std::sin(p[k].rot[2]);
            auto place = [&](uint32_t vi, float* o) {
                float q[3] = {v[vi].x * mh->posScale[0] + mh->posOffset[0], v[vi].y * mh->posScale[1] + mh->posOffset[1],
                              v[vi].z * mh->posScale[2] + mh->posOffset[2]};
                float a[3] = {q[0], cx * q[1] - sx * q[2], sx * q[1] + cx * q[2]};          // Rx
                float c[3] = {cy * a[0] + sy * a[2], a[1], -sy * a[0] + cy * a[2]};          // Ry
                float d[3] = {cz * c[0] - sz * c[1], sz * c[0] + cz * c[1], c[2]};           // Rz
                for (int j = 0; j < 3; ++j) o[j] = d[j] * p[k].scale + p[k].pos[j];
            };
            for (uint32_t si = 0; si < mh->submeshCount; ++si)
                for (uint32_t t = s[si].firstIndex; t + 2 < s[si].firstIndex + s[si].indexCount; t += 3) {
                    Tri tr;
                    for (int c = 0; c < 3; ++c) place(ix[t + c], &tr[c * 3]);
                    out.push_back(tr);
                }
        }
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    size_t pa = 0, pb = 0;
    auto a = worldTris(argv[1], &pa), b = worldTris(argv[2], &pb);
    std::map<std::array<long, 3>, std::vector<size_t>> grid;
    for (size_t i = 0; i < a.size(); ++i) grid[{std::lround(a[i][0]), std::lround(a[i][1]), std::lround(a[i][2])}].push_back(i);
    std::vector<char> used(a.size(), 0);
    size_t miss = 0;
    for (const Tri& t : b) {
        bool hit = false;
        for (long dx = -1; dx <= 1 && !hit; ++dx) for (long dy = -1; dy <= 1 && !hit; ++dy) for (long dz = -1; dz <= 1 && !hit; ++dz) {
            auto it = grid.find({std::lround(t[0]) + dx, std::lround(t[1]) + dy, std::lround(t[2]) + dz});
            if (it == grid.end()) continue;
            for (size_t i : it->second) {
                if (used[i]) continue;
                bool same = true;
                for (int q = 0; q < 9 && same; ++q) same = std::fabs(a[i][q] - t[q]) < 0.02f;
                if (same) { used[i] = 1; hit = true; break; }
            }
        }
        if (!hit) ++miss;
    }
    const bool ok = a.size() == b.size() && miss == 0 && pb > pa;
    std::printf("WMO CUT: %zu building placements -> %zu, %zu triangles -> %zu, %zu not in place: %s\n", pa, pb, a.size(), b.size(), miss,
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
