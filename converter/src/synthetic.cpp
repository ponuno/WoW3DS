// Synthetic test tile: lets the 3DS prototype and the pack format be exercised
// without any game data. Geometry/texture budgets mirror a busy forest tile.
#include "conv.h"

#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <cstdio>
#include <random>

namespace w3d {
namespace {

constexpr float TS = wowee::core::coords::TILE_SIZE;
constexpr float ZP = wowee::core::coords::ZEROPOINT;

float hash2(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
float valueNoise(float x, float y) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
float fbm(float x, float y) {
    float s = 0, amp = 1, f = 1;
    for (int i = 0; i < 4; ++i) { s += amp * valueNoise(x * f, y * f); amp *= 0.5f; f *= 2.0f; }
    return s / 1.875f;
}
// Height of tile-local (lx, ly) in yards.
// World render coordinates (continuous across tiles).
float heightAt(float wx, float wy) { return 40.0f + 60.0f * fbm(wx / 120.0f, wy / 120.0f); }
float dirtAt(float lx, float ly) { return std::clamp((fbm(lx / 40.0f + 9.1f, ly / 40.0f) - 0.45f) * 4.0f, 0.0f, 1.0f); }

Image noiseTexture(int size, uint8_t r, uint8_t g, uint8_t b, float contrast, int seed) {
    Image im;
    im.w = im.h = size;
    im.rgba.resize((size_t)size * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            // Periodic noise so the texture tiles without seams.
            float n = 0, amp = 1;
            for (int o = 0, f = 4; o < 4; ++o, f *= 2, amp *= 0.5f) {
                int cell = size / f;
                float fx = (float)x / cell, fy = (float)y / cell;
                int x0 = (int)fx, y0 = (int)fy;
                float tx = fx - x0, ty = fy - y0;
                auto hv = [&](int a, int bb) { return hash2((a % f) + seed * 101, (bb % f) + o * 977); };
                float v = (hv(x0, y0) * (1 - tx) + hv(x0 + 1, y0) * tx) * (1 - ty) +
                          (hv(x0, y0 + 1) * (1 - tx) + hv(x0 + 1, y0 + 1) * tx) * ty;
                n += amp * v;
            }
            float k = 1.0f + contrast * (n / 1.875f - 0.5f) * 2.0f;
            uint8_t* p = &im.rgba[((size_t)y * size + x) * 4];
            p[0] = (uint8_t)std::clamp(r * k, 0.0f, 255.0f);
            p[1] = (uint8_t)std::clamp(g * k, 0.0f, 255.0f);
            p[2] = (uint8_t)std::clamp(b * k, 0.0f, 255.0f);
            p[3] = 255;
        }
    return im;
}

// Leaf card texture with hard alpha (exercises ETC1A4 + alpha test).
Image leafTexture(int size) {
    Image im = noiseTexture(size, 60, 120, 45, 0.6f, 7);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float u = (x + 0.5f) / size * 2 - 1, v = (y + 0.5f) / size * 2 - 1;
            float r = std::sqrt(u * u + v * v) + 0.25f * (hash2(x / 4, y / 4) - 0.5f);
            im.rgba[((size_t)y * size + x) * 4 + 3] = r < 0.9f ? 255 : 0;
        }
    return im;
}

void addTexture(PackWriter& pack, const char* name, const Image& im, int maxSize, uint8_t fmt, bool alpha,
                std::vector<uint32_t>& slots) {
    uint32_t id = w3d_fnv1a(name);
    pack.add(W3D_ENTRY_TEXTURE, id, encodeTexture(im, maxSize, fmt, alpha));
    slots.push_back(id);
}

// Tree: 8-sided trunk + 3 crossed leaf cards per tier (alpha-keyed), Z up.
ModelSource makeTree(uint32_t barkSlot, uint32_t leafSlot) {
    ModelSource m;
    auto addV = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        m.verts.push_back({{x, y, z}, {nx, ny, nz}, {u, v}});
        return (uint32_t)m.verts.size() - 1;
    };
    ModelSource::Sub trunk{barkSlot, W3D_BLEND_OPAQUE, 0, {}};
    const int sides = 8;
    for (int i = 0; i < sides; ++i) {
        float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
        float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        uint32_t v0 = addV(0.6f * c0, 0.6f * s0, 0, c0, s0, 0, (float)i / sides, 1);
        uint32_t v1 = addV(0.6f * c1, 0.6f * s1, 0, c1, s1, 0, (float)(i + 1) / sides, 1);
        uint32_t v2 = addV(0.35f * c0, 0.35f * s0, 9, c0, s0, 0, (float)i / sides, 0);
        uint32_t v3 = addV(0.35f * c1, 0.35f * s1, 9, c1, s1, 0, (float)(i + 1) / sides, 0);
        trunk.idx.insert(trunk.idx.end(), {v0, v1, v2, v2, v1, v3});
    }
    ModelSource::Sub leaves{leafSlot, W3D_BLEND_ALPHAKEY, W3D_SUBF_TWO_SIDED, {}};
    for (int tier = 0; tier < 3; ++tier) {
        float z0 = 4.0f + tier * 2.5f, r = 4.5f - tier * 1.1f;
        for (int k = 0; k < 3; ++k) {
            float a = 1.0472f * k + tier * 0.5f, c = std::cos(a) * r, s = std::sin(a) * r;
            uint32_t v0 = addV(-c, -s, z0, -s, c, 0.3f, 0, 1), v1 = addV(c, s, z0, -s, c, 0.3f, 1, 1);
            uint32_t v2 = addV(-c, -s, z0 + 2 * r, -s, c, 0.3f, 0, 0), v3 = addV(c, s, z0 + 2 * r, -s, c, 0.3f, 1, 0);
            leaves.idx.insert(leaves.idx.end(), {v0, v1, v2, v2, v1, v3});
        }
        // horizontal card
        uint32_t h0 = addV(-r, -r, z0 + r, 0, 0, 1, 0, 1), h1 = addV(r, -r, z0 + r, 0, 0, 1, 1, 1);
        uint32_t h2 = addV(-r, r, z0 + r, 0, 0, 1, 0, 0), h3 = addV(r, r, z0 + r, 0, 0, 1, 1, 0);
        leaves.idx.insert(leaves.idx.end(), {h0, h1, h2, h2, h1, h3});
    }
    m.subs = {trunk, leaves};
    return m;
}

// Rock: subdivided octahedron with noisy radius.
ModelSource makeRock(uint32_t slot) {
    ModelSource m;
    std::vector<std::array<float, 3>> p = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<uint32_t> tri = {0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 4, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 5};
    for (int sub = 0; sub < 2; ++sub) {
        std::vector<uint32_t> nt;
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> mid;
        auto midpoint = [&](uint32_t a, uint32_t b) {
            auto key = std::minmax(a, b);
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            std::array<float, 3> q{(p[a][0] + p[b][0]) / 2, (p[a][1] + p[b][1]) / 2, (p[a][2] + p[b][2]) / 2};
            float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            for (auto& c : q) c /= l;
            p.push_back(q);
            return mid[key] = (uint32_t)p.size() - 1;
        };
        for (size_t i = 0; i < tri.size(); i += 3) {
            uint32_t a = tri[i], b = tri[i + 1], c = tri[i + 2];
            uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            nt.insert(nt.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
        }
        tri.swap(nt);
    }
    for (size_t i = 0; i < p.size(); ++i) {
        float r = 1.5f * (0.8f + 0.4f * hash2((int)i, 3));
        m.verts.push_back({{p[i][0] * r, p[i][1] * r, p[i][2] * r * 0.7f + 0.3f},
                           {p[i][0], p[i][1], p[i][2]},
                           {std::atan2(p[i][1], p[i][0]) / 6.2831853f + 0.5f, p[i][2] * 0.5f + 0.5f}});
    }
    m.subs = {{slot, W3D_BLEND_OPAQUE, 0, tri}};
    return m;
}

}  // namespace

bool buildSynthetic(const Options& o, PackWriter& pack, ConvStats& st) {
    // ---- shared textures and models (added once for the whole region)
    std::vector<uint32_t> texIds{addPlaceholderTexture(pack, o.texFormat)};
    std::vector<uint32_t> modelTex;
    const bool first = !pack.has(W3D_ENTRY_TEXTURE, w3d_fnv1a("synthetic/grass.blp"));
    if (first) {
        addTexture(pack, "synthetic/grass.blp", noiseTexture(128, 70, 115, 40, 0.5f, 1), o.terrainTex, o.texFormat, false, texIds);
        addTexture(pack, "synthetic/dirt.blp", noiseTexture(128, 120, 95, 65, 0.6f, 2), o.terrainTex, o.texFormat, false, texIds);
        addTexture(pack, "synthetic/bark.blp", noiseTexture(64, 95, 70, 45, 0.7f, 3), o.modelTex, o.texFormat, false, modelTex);
        addTexture(pack, "synthetic/leaf.blp", leafTexture(64), o.modelTex, o.texFormat, true, modelTex);
        addTexture(pack, "synthetic/rock.blp", noiseTexture(64, 120, 120, 115, 0.5f, 4), o.modelTex, o.texFormat, false, modelTex);
        st.texturesOk += 5;
        auto emitModel = [&](const char* name, ModelSource m, const std::vector<uint32_t>& slots) {
            m.texIds = slots;
            size_t tris = 0;
            pack.add(W3D_ENTRY_MODEL, w3d_fnv1a(name), buildModelBlob(m, o.maxModelTris, &tris));
            st.modelTrisTotal += tris;
            ++st.modelsOk;
        };
        emitModel("synthetic/tree.m2", makeTree(0, 1), {modelTex[0], modelTex[1]});
        emitModel("synthetic/rock.m2", makeRock(0), {modelTex[2]});
    } else {
        texIds.push_back(w3d_fnv1a("synthetic/grass.blp"));
        texIds.push_back(w3d_fnv1a("synthetic/dirt.blp"));
    }

    // Terrain functions take WORLD render coordinates so neighbouring tiles match.
    const float minX = (32.0f - (float)o.tileY - 1.0f) * TS, minY = (32.0f - (float)o.tileX - 1.0f) * TS;
    const float kWaterLevel = 52.0f;
    auto H = [&](float lx, float ly) { return heightAt(minX + lx, minY + ly); };

    // ---- ADT in memory
    wowee::pipeline::ADTTerrain adt;
    adt.loaded = true;
    adt.textures = {"synthetic/grass.blp", "synthetic/dirt.blp"};
    adt.doodadNames = {"synthetic/tree.m2", "synthetic/rock.m2"};
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            auto& ch = adt.getChunk(cx, cy);
            ch.position[2] = 0;
            for (int idx = 0; idx < 145; ++idx) {
                int row = idx / 17, col = idx % 17;
                float fr = (float)row, fc = (float)col;
                if (col > 8) { fr += 0.5f; fc -= 8.5f; }
                float lx = (1.0f - (cy * 8 + fr) / 128.0f) * TS, ly = (1.0f - (cx * 8 + fc) / 128.0f) * TS;
                ch.heightMap.heights[idx] = H(lx, ly);
                const float e = 1.0f;
                float dx = H(lx + e, ly) - H(lx - e, ly), dy = H(lx, ly + e) - H(lx, ly - e);
                float nx = -dx / (2 * e), ny = -dy / (2 * e), nz = 1, l = std::sqrt(nx * nx + ny * ny + nz * nz);
                ch.normals[idx * 3 + 0] = (int8_t)std::lround(nx / l * 127);
                ch.normals[idx * 3 + 1] = (int8_t)std::lround(ny / l * 127);
                ch.normals[idx * 3 + 2] = (int8_t)std::lround(nz / l * 127);
            }
            ch.heightMap.loaded = true;
            ch.layers.push_back({0, 0, 0, 0});
            ch.layers.push_back({1, 0x100, 0, 0});
            ch.alphaMap.resize(4096);
            for (int ay = 0; ay < 64; ++ay)
                for (int ax = 0; ax < 64; ++ax) {
                    float lx = (1.0f - (cy * 8 + ay / 63.0f * 8.0f) / 128.0f) * TS;
                    float ly = (1.0f - (cx * 8 + ax / 63.0f * 8.0f) / 128.0f) * TS;
                    ch.alphaMap[ay * 64 + ax] = (uint8_t)(dirtAt(minX + lx, minY + ly) * 255);
                }
            // Lakes: an MCLQ-style layer wherever a quad dips below the water level.
            wowee::pipeline::ADTTerrain::WaterLayer L{};
            L.liquidType = 0;
            L.width = L.height = 8;
            L.minHeight = L.maxHeight = kWaterLevel;
            L.heights.assign(81, kWaterLevel);
            L.mask.assign(8, 0);
            bool any = false;
            for (int qy = 0; qy < 8; ++qy)
                for (int qx = 0; qx < 8; ++qx) {
                    float lo = 1e9f;
                    for (int k = 0; k < 4; ++k) lo = std::min(lo, ch.heightMap.heights[(qy + k / 2) * 17 + qx + k % 2]);
                    if (lo < kWaterLevel) { L.mask[qy] |= (uint8_t)(1 << qx); any = true; }
                }
            if (any) adt.waterData[cy * 16 + cx].layers.push_back(std::move(L));
        }

    // ---- placements: forest density similar to Elwynn, nothing under water
    std::mt19937 rng(1234u + (uint32_t)w3d_tile_id(o.tileX, o.tileY));
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    int want = o.maxDoodads < 0 ? 600 : o.maxDoodads;
    for (int i = 0, tries = 0; i < want && tries < want * 4; ++tries) {
        float lx = U(rng) * TS, ly = U(rng) * TS, h = H(lx, ly);
        if (h < kWaterLevel + 0.5f) continue;
        wowee::pipeline::ADTTerrain::DoodadPlacement p{};
        p.nameId = (i % 5 == 4) ? 1 : 0;
        p.uniqueId = (uint32_t)i;
        float rx = minX + lx, ry = minY + ly, rz = h - 0.3f;
        p.position[0] = ZP - ry;   // inverse of coords::adtToWorld
        p.position[1] = rz;
        p.position[2] = ZP - rx;
        p.rotation[1] = U(rng) * 360.0f;
        p.scale = (uint16_t)(1024 * (0.8f + 0.5f * U(rng)));
        adt.doodadPlacements.push_back(p);
        ++i;
    }

    std::vector<uint16_t> mtexToSlot = {1, 2};
    return emitTile(o, adt, texIds, mtexToSlot,
                    [](const std::string& n) { return w3d_fnv1a(DataDir::normalize(n).c_str()); }, pack, st);
}

}  // namespace w3d
