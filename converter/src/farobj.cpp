// farobj.cpp — the far buildings (FARO, stage 6.29; format in common/w3d_farobj.h).
//
// buildFarShell() turns a building's outside surface into a coarse shell by vertex clustering: the vertices are merged per
// cell of a grid over the building (the cell grows until the triangles fit the budget), each cluster at the mean of its
// vertices, coloured with the area-weighted mean colour of the triangles around it; triangles that collapse are dropped and
// the ones that end up twice (both sides of a wall) kept once - the client draws shells from both sides. emitFarObjects()
// writes every placement of a shelled building of the map, sorted by tile, with the shells used and a palette of their colours.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#include "conv.h"
#include "w3d_farobj.h"
#include "w3d_format.h"

namespace w3d {

std::vector<uint8_t> encodeTextureLevel(const Image& im, uint8_t fmt);   // texture.cpp

bool buildFarShell(const std::vector<float>& tris, const std::vector<float>& triRgb, float minExtent, int maxTris,
                   ConvStats::FarShellSrc& out) {
    const size_t nt = tris.size() / 9;
    if (!nt || triRgb.size() < nt * 3) return false;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < nt * 3; ++i)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], tris[i * 3 + k]);
            hi[k] = std::max(hi[k], tris[i * 3 + k]);
        }
    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
    if (!(extent >= minExtent) || extent > 4000.0f) return false;
    // per triangle: area and normal (the clusters' colours and normals are weighted by it)
    std::vector<float> area(nt), fn(nt * 3);
    for (size_t t = 0; t < nt; ++t) {
        const float* p = &tris[t * 9];
        const float e1[3] = {p[3] - p[0], p[4] - p[1], p[5] - p[2]}, e2[3] = {p[6] - p[0], p[7] - p[1], p[8] - p[2]};
        const float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        area[t] = 0.5f * l;
        for (int k = 0; k < 3; ++k) fn[t * 3 + k] = l > 1e-12f ? c[k] / l : 0.0f;
    }
    static const float kGrid[] = {40, 32, 26, 21, 17, 14, 11, 9, 7, 5, 4, 3, 2};
    for (float g : kGrid) {
        const float cell = extent / g;
        std::unordered_map<uint64_t, uint32_t> cid;
        struct Acc { double p[3] = {0, 0, 0}; double rgb[3] = {0, 0, 0}; double n[3] = {0, 0, 0}; double w = 0; uint32_t cnt = 0; };
        std::vector<Acc> acc;
        std::vector<uint32_t> vc(nt * 3);
        for (size_t i = 0; i < nt * 3; ++i) {
            const float* p = &tris[i * 3];
            const uint64_t ix = (uint64_t)std::max(0.0f, std::floor((p[0] - lo[0]) / cell));
            const uint64_t iy = (uint64_t)std::max(0.0f, std::floor((p[1] - lo[1]) / cell));
            const uint64_t iz = (uint64_t)std::max(0.0f, std::floor((p[2] - lo[2]) / cell));
            const uint64_t key = (ix << 42) | (iy << 21) | iz;
            auto [it, fresh] = cid.emplace(key, (uint32_t)acc.size());
            if (fresh) acc.emplace_back();
            Acc& a = acc[it->second];
            for (int k = 0; k < 3; ++k) a.p[k] += p[k];
            ++a.cnt;
            vc[i] = it->second;
        }
        // triangles after the merge: no collapsed ones, each set of three clusters once
        std::vector<uint32_t> idx;
        std::unordered_map<uint64_t, bool> seen;
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t a = vc[t * 3], b = vc[t * 3 + 1], c = vc[t * 3 + 2];
            // the triangle's colour and normal go to its clusters (also when it collapses: a thin wall still colours its edge)
            for (uint32_t v : {a, b, c}) {
                Acc& q = acc[v];
                const double w = std::max(area[t], 1e-6f);
                for (int k = 0; k < 3; ++k) { q.rgb[k] += w * triRgb[t * 3 + k]; q.n[k] += w * fn[t * 3 + k]; }
                q.w += w;
            }
            if (a == b || b == c || a == c) continue;
            uint32_t s[3] = {a, b, c};
            std::sort(s, s + 3);
            const uint64_t key = ((uint64_t)s[0] << 42) ^ ((uint64_t)s[1] << 21) ^ (uint64_t)s[2];
            if (!seen.emplace(key, true).second) continue;
            idx.insert(idx.end(), {a, b, c});
        }
        if (idx.size() / 3 > (size_t)maxTris && g > kGrid[sizeof kGrid / sizeof kGrid[0] - 1]) continue;
        if (idx.empty()) return false;
        // compact: only the clusters used
        std::vector<int32_t> remap(acc.size(), -1);
        out = ConvStats::FarShellSrc();
        for (uint32_t& v : idx) {
            if (remap[v] < 0) {
                remap[v] = (int32_t)(out.pos.size() / 3);
                const Acc& q = acc[v];
                float p[3], n[3], c[3];
                for (int k = 0; k < 3; ++k) {
                    p[k] = (float)(q.p[k] / q.cnt);
                    c[k] = q.w > 0 ? (float)(q.rgb[k] / q.w) : 0.5f;
                    n[k] = (float)q.n[k];
                }
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl > 1e-6f) for (float& x : n) x /= nl;
                else { n[0] = 0; n[1] = 0; n[2] = 1; }
                out.pos.insert(out.pos.end(), p, p + 3);
                out.nrm.insert(out.nrm.end(), n, n + 3);
                out.rgb.insert(out.rgb.end(), c, c + 3);
                out.radius = std::max(out.radius, std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
            }
            v = (uint32_t)remap[v];
        }
        if (out.pos.size() / 3 > 65535) return false;
        out.idx = std::move(idx);
        return true;
    }
    return false;
}

void emitFarObjects(const Options& o, PackWriter& pack, ConvStats& st) {
    if (o.noFarObjects || st.farObjs.empty()) { st.farObjs.clear(); st.farShells.clear(); return; }
    // the objects by tile (the tile whose PLCM holds the building's groups: the client hides a shell when that tile shows it)
    struct Obj { int tile; const ConvStats::FarObjSrc* src; };
    std::vector<Obj> objs;
    for (const auto& f : st.farObjs) {
        if (!st.farShells.count(f.key)) continue;
        if (f.tile < 0 || f.tile >= 64 * 64) continue;
        objs.push_back({f.tile, &f});
    }
    std::stable_sort(objs.begin(), objs.end(), [](const Obj& a, const Obj& b) { return a.tile < b.tile; });
    // the shells used, the palette (4-bit channels; 3-bit when that is too many)
    std::map<std::string, uint32_t> shellOf;
    std::vector<const ConvStats::FarShellSrc*> shells;
    for (const Obj& ob : objs)
        if (shellOf.emplace(ob.src->key, (uint32_t)shells.size()).second) shells.push_back(&st.farShells[ob.src->key]);
    auto quant = [](const float* c, int bits) {
        uint32_t k = 0;
        for (int q = 0; q < 3; ++q) k = (k << bits) | (uint32_t)std::clamp((int)std::lround(c[q] * ((1 << bits) - 1)), 0, (1 << bits) - 1);
        return k;
    };
    int bits = 4;
    std::map<uint32_t, uint32_t> pal;
    for (;;) {
        pal.clear();
        for (const auto* s : shells)
            for (size_t v = 0; v < s->rgb.size() / 3; ++v) pal.emplace(quant(&s->rgb[v * 3], bits), 0);
        if (pal.size() <= 1024 || bits == 3) break;
        bits = 3;
    }
    uint32_t n = 0;
    for (auto& [k, i] : pal) i = n++;
    const int palW = 128, perRow = palW / W3D_FAROBJ_CELL;
    int palH = 8;
    while (palH < (int)((n + perRow - 1) / perRow) * W3D_FAROBJ_CELL) palH *= 2;
    Image img;
    img.w = palW;
    img.h = palH;
    img.rgba.assign((size_t)palW * palH * 4, 255);
    for (auto& [k, i] : pal) {
        const int cx = (int)(i % perRow) * W3D_FAROBJ_CELL, cy = (int)(i / perRow) * W3D_FAROBJ_CELL;
        uint8_t rgb[3];
        for (int q = 0; q < 3; ++q) {
            const uint32_t v = (k >> ((2 - q) * bits)) & ((1u << bits) - 1);
            rgb[q] = (uint8_t)std::lround(v * 255.0 / ((1 << bits) - 1));
        }
        for (int y = 0; y < W3D_FAROBJ_CELL; ++y)
            for (int x = 0; x < W3D_FAROBJ_CELL; ++x) {
                uint8_t* px = &img.rgba[((size_t)(cy + y) * palW + cx + x) * 4];
                px[0] = rgb[0]; px[1] = rgb[1]; px[2] = rgb[2]; px[3] = 255;
            }
    }
    // the entry
    W3dFarObjHeader hd{};
    hd.magic = W3D_FAROBJ_MAGIC;
    hd.shellCount = (uint32_t)shells.size();
    for (const auto* s : shells) { hd.vertCount += (uint32_t)(s->pos.size() / 3); hd.indexCount += (uint32_t)std::min<size_t>(s->idx.size(), 65535 / 3 * 3); }
    hd.objectCount = (uint32_t)objs.size();
    hd.palW = (uint16_t)palW;
    hd.palH = (uint16_t)palH;
    {
        size_t k = 0;
        for (int t = 0; t <= 64 * 64; ++t) {
            while (k < objs.size() && objs[k].tile < t) ++k;
            hd.tileStart[t] = (uint32_t)k;
        }
    }
    std::vector<uint8_t> blob(w3d_farobj_size(&hd), 0);
    W3dFarShell* so = (W3dFarShell*)(blob.data() + w3d_farobj_offset_shells());
    W3dFarShellVert* vo = (W3dFarShellVert*)(blob.data() + w3d_farobj_offset_verts(&hd));
    uint16_t* io = (uint16_t*)(blob.data() + w3d_farobj_offset_indices(&hd));
    W3dFarObject* oo = (W3dFarObject*)(blob.data() + w3d_farobj_offset_objects(&hd));
    uint32_t fv = 0, fi = 0;
    size_t tris = 0;
    for (size_t i = 0; i < shells.size(); ++i) {
        const auto& s = *shells[i];
        const uint32_t nv = (uint32_t)(s.pos.size() / 3);
        so[i].firstVert = fv;
        so[i].firstIndex = fi;
        so[i].vertCount = (uint16_t)nv;
        so[i].indexCount = (uint16_t)std::min<size_t>(s.idx.size(), 65535 / 3 * 3);
        so[i].radius = s.radius;
        for (uint32_t v = 0; v < nv; ++v) {
            W3dFarShellVert& d = vo[fv + v];
            d.x = (int16_t)std::clamp(std::lround(s.pos[v * 3] * W3D_FAROBJ_POS_UNIT), -32767L, 32767L);
            d.y = (int16_t)std::clamp(std::lround(s.pos[v * 3 + 1] * W3D_FAROBJ_POS_UNIT), -32767L, 32767L);
            d.z = (int16_t)std::clamp(std::lround(s.pos[v * 3 + 2] * W3D_FAROBJ_POS_UNIT), -32767L, 32767L);
            d.nx = (int8_t)std::clamp(std::lround(s.nrm[v * 3] * 127.0f), -127L, 127L);
            d.ny = (int8_t)std::clamp(std::lround(s.nrm[v * 3 + 1] * 127.0f), -127L, 127L);
            d.nz = (int8_t)std::clamp(std::lround(s.nrm[v * 3 + 2] * 127.0f), -127L, 127L);
            d.colour = (uint16_t)pal[quant(&s.rgb[v * 3], bits)];
        }
        for (uint32_t q = 0; q < so[i].indexCount; ++q) io[fi + q] = (uint16_t)s.idx[q];
        fv += nv;
        fi += so[i].indexCount;
        tris += so[i].indexCount / 3;
    }
    for (size_t i = 0; i < objs.size(); ++i) {
        const auto& f = *objs[i].src;
        W3dFarObject& d = oo[i];
        d.shell = shellOf[f.key];
        std::memcpy(d.pos, f.pos, sizeof d.pos);
        std::memcpy(d.rot, f.rot, sizeof d.rot);
        d.groupModel = f.groupModel;
        std::memcpy(d.groupPos, f.groupPos, sizeof d.groupPos);
    }
    const uint32_t texId = pack.keyId(W3D_ENTRY_TEXTURE, "farobj|" + DataDir::normalize(o.map) + "|" + std::to_string(n) + "|" +
                                                         std::to_string(hd.objectCount));
    hd.paletteTexId = texId;
    std::memcpy(blob.data(), &hd, sizeof hd);
    pack.add(W3D_ENTRY_TEXTURE, texId, encodeTextureLevel(img, W3D_TEX_RGB565));
    std::printf("  far buildings: %u placements of %u buildings, %zu triangles in the shells, %u colours, %.1f KB\n", hd.objectCount,
                hd.shellCount, tris, n, (blob.size() + (size_t)palW * palH * 2) / 1024.0);
    st.farObjects += hd.objectCount;
    st.farShellsOut += hd.shellCount;
    st.farShellTris += tris;
    st.farObjBytes += blob.size() + (size_t)palW * palH * 2;
    pack.add(W3D_ENTRY_FAROBJ, 0, std::move(blob));
    st.farObjs.clear();
    st.farShells.clear();
}

}  // namespace w3d
