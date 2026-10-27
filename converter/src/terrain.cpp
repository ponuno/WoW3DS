// ADT tile -> W3D terrain + doodad placements.
#include "conv.h"

#include "core/coordinates.hpp"
#include "pipeline/adt_alpha.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <array>
#include <functional>
#include <map>
#include <tuple>

namespace w3d {

namespace {
constexpr float TS = wowee::core::coords::TILE_SIZE;
constexpr float CHUNK = TS / 16.0f;
constexpr float kTexRepeatPerChunk = 4.0f;          // same as WoWee
const float kSun[3] = {0.30f, 0.40f, 0.866f};        // towards the sun, render space
constexpr float kAmbient = 0.50f, kDiffuse = 0.60f;

// Same Euler convention as WoWee's placementEuler()/placementModelMatrix().
void placementEuler(const float r[3], float out[3]) {
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    out[0] = r[2] * kDeg;            // about X
    out[1] = r[0] * kDeg;            // about Y
    out[2] = (r[1] + 180.0f) * kDeg; // about Z
}

uint8_t bakeLight(float nx, float ny, float nz) {
    float d = std::max(0.0f, nx * kSun[0] + ny * kSun[1] + nz * kSun[2]);
    return (uint8_t)std::lround(std::min(1.0f, kAmbient + kDiffuse * d) * 255.0f);
}
}  // namespace

uint32_t addPlaceholderTexture(PackWriter& pack, uint8_t fmt) {
    uint32_t id = w3d_fnv1a("<placeholder>");
    if (pack.has(W3D_ENTRY_TEXTURE, id)) return id;
    Image im;
    im.w = im.h = 8;
    im.rgba.resize(8 * 8 * 4);
    for (int i = 0; i < 64; ++i) {
        uint8_t g = ((i / 8 + i % 8) & 1) ? 150 : 110;
        im.rgba[i * 4 + 0] = im.rgba[i * 4 + 1] = im.rgba[i * 4 + 2] = g;
        im.rgba[i * 4 + 3] = 255;
    }
    pack.add(W3D_ENTRY_TEXTURE, id, encodeTexture(im, 8, fmt, false));
    return id;
}


// ---------------------------------------------------------------- water
// Procedural liquid textures (one per W3dLiquid), shared by all tiles.
static uint32_t addLiquidTexture(PackWriter& pack, int liquid, uint8_t fmt) {
    static const char* names[4] = {"<liquid-water>", "<liquid-ocean>", "<liquid-magma>", "<liquid-slime>"};
    static const uint8_t base[4][3] = {{60, 105, 150}, {35, 75, 125}, {230, 90, 20}, {80, 160, 40}};
    uint32_t id = w3d_fnv1a(names[liquid]);
    if (pack.has(W3D_ENTRY_TEXTURE, id)) return id;
    Image im;
    im.w = im.h = 64;
    im.rgba.resize(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            // Tileable ripples: sum of sines with integer periods.
            float t = 6.2831853f / 64.0f;
            float n = 0.5f + 0.18f * std::sin(t * (2 * x + 3 * y)) + 0.14f * std::sin(t * (5 * x - 2 * y)) +
                      0.10f * std::sin(t * (7 * y + x));
            uint8_t* p = &im.rgba[(y * 64 + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = (uint8_t)std::clamp(base[liquid][c] * (0.6f + 0.8f * n), 0.0f, 255.0f);
            p[3] = 255;
        }
    pack.add(W3D_ENTRY_TEXTURE, id, encodeTexture(im, 64, fmt, false));
    return id;
}

void emitWater(const Options& o, const wowee::pipeline::ADTTerrain& adt, float minX, float minY, PackWriter& pack,
               ConvStats& st) {
    struct WV { float p[3]; uint8_t a; float uv[2]; };
    std::vector<WV> verts;
    std::map<std::pair<int, int>, std::vector<uint32_t>> lists;  // (region, liquid)
    static const uint8_t kAlpha[4] = {150, 175, 255, 205};

    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            const auto& w = adt.waterData[cy * 16 + cx];
            for (const auto& L : w.layers) {
                int liquid = std::min<int>(L.liquidType, 3);
                int vw = L.width + 1, vh = L.height + 1;
                if ((int)L.heights.size() < vw * vh) continue;
                uint32_t base = (uint32_t)verts.size();
                for (int j = 0; j < vh; ++j)
                    for (int i = 0; i < vw; ++i) {
                        int row = L.y + j, col = L.x + i;
                        float gridX = (float)cy * 8.0f + (float)row, gridY = (float)cx * 8.0f + (float)col;
                        WV v{};
                        v.p[0] = (1.0f - gridX / 128.0f) * TS;
                        v.p[1] = (1.0f - gridY / 128.0f) * TS;
                        float hgt = L.heights[j * vw + i];
                        v.p[2] = std::isfinite(hgt) ? hgt : L.maxHeight;
                        v.a = kAlpha[liquid];
                        v.uv[0] = (TS - v.p[1]) / 8.0f;
                        v.uv[1] = v.p[0] / 8.0f;
                        verts.push_back(v);
                    }
                auto& lst = lists[{(cy / 4) * 4 + cx / 4, liquid}];
                for (int j = 0; j < L.height; ++j)
                    for (int i = 0; i < L.width; ++i) {
                        // mask: one byte per layer row, bit = column (layer-relative)
                        if (j < (int)L.mask.size() && !((L.mask[j] >> i) & 1)) continue;
                        uint32_t a = base + j * vw + i, b = a + 1, c = a + vw, d = c + 1;
                        for (auto tri : {std::array<uint32_t, 3>{a, c, b}, std::array<uint32_t, 3>{b, c, d}}) {
                            const float* p0 = verts[tri[0]].p; const float* p1 = verts[tri[1]].p; const float* p2 = verts[tri[2]].p;
                            float cz = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]);
                            if (cz < 0) std::swap(tri[1], tri[2]);
                            lst.insert(lst.end(), tri.begin(), tri.end());
                        }
                    }
            }
        }
    size_t total = 0;
    for (auto& [k, l] : lists) total += l.size();
    if (total == 0 || verts.size() > 65535) return;

    float zmin = 1e30f, zmax = -1e30f;
    for (auto& v : verts) { zmin = std::min(zmin, v.p[2]); zmax = std::max(zmax, v.p[2]); }
    W3dWaterHeader h{};
    h.tileX = (uint32_t)o.tileX; h.tileY = (uint32_t)o.tileY;
    h.origin[0] = minX; h.origin[1] = minY; h.origin[2] = zmin;
    float zr = std::max(zmax - zmin, 1.0f);
    h.posOffset[0] = TS * 0.5f; h.posOffset[1] = TS * 0.5f; h.posOffset[2] = zr * 0.5f;
    h.posScale[0] = TS * 0.5f / 32767.0f; h.posScale[1] = h.posScale[0]; h.posScale[2] = zr * 0.5f / 32767.0f;

    std::vector<uint32_t> texIds;
    std::map<int, uint16_t> liquidSlot;
    std::vector<W3dWaterBatch> batches;
    std::vector<uint16_t> indices;
    for (auto& [key, lst] : lists) {
        if (lst.empty()) continue;
        int liquid = key.second;
        if (!liquidSlot.count(liquid)) {
            liquidSlot[liquid] = (uint16_t)texIds.size();
            texIds.push_back(addLiquidTexture(pack, liquid, o.texFormat));
        }
        W3dWaterBatch b{};
        b.liquid = (uint16_t)liquid;
        b.tex = liquidSlot[liquid];
        b.firstIndex = (uint32_t)indices.size();
        b.indexCount = (uint32_t)lst.size();
        for (int c = 0; c < 3; ++c) { b.bmin[c] = 1e30f; b.bmax[c] = -1e30f; }
        for (uint32_t i : lst) {
            indices.push_back((uint16_t)i);
            for (int c = 0; c < 3; ++c) {
                float v = c == 2 ? verts[i].p[2] - zmin : verts[i].p[c];
                b.bmin[c] = std::min(b.bmin[c], v); b.bmax[c] = std::max(b.bmax[c], v);
            }
        }
        batches.push_back(b);
    }
    std::vector<W3dTerrainVertex> qv(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        auto& v = verts[i];
        auto q = [&](float val, int c) {
            return (int16_t)std::clamp((int)std::lround((val - h.posOffset[c]) / h.posScale[c]), -32767, 32767);
        };
        qv[i] = {q(v.p[0], 0), q(v.p[1], 1), q(v.p[2] - zmin, 2), 0, 255, 255, 255, v.a,
                 (int16_t)std::lround(v.uv[0] / W3D_TERRAIN_UV_SCALE), (int16_t)std::lround(v.uv[1] / W3D_TERRAIN_UV_SCALE)};
    }
    h.vertexCount = (uint32_t)qv.size();
    h.indexCount = (uint32_t)indices.size();
    h.batchCount = (uint32_t)batches.size();
    h.texCount = (uint32_t)texIds.size();
    Blob blob;
    blob.put(h);
    blob.align(4);
    size_t offTex = blob.putArray(texIds);
    blob.align(4);
    size_t offB = blob.putArray(batches);
    blob.align(16);
    size_t offV = blob.putArray(qv);
    blob.align(4);
    size_t offI = blob.putArray(indices);
    blob.align(4);
    auto* hp = blob.at<W3dWaterHeader>(0);
    hp->offTexIds = (uint32_t)offTex; hp->offBatches = (uint32_t)offB;
    hp->offVertices = (uint32_t)offV; hp->offIndices = (uint32_t)offI;
    pack.add(W3D_ENTRY_WATER, w3d_tile_id(o.tileX, o.tileY), std::move(blob.data));
    st.waterTris += indices.size() / 3;
}

struct TVert { float p[3]; uint8_t c[4]; float uv[2]; };

static bool claimPlacement(const Options& o, std::unordered_set<uint32_t>& seen, uint32_t uid, float x, float y);

bool convertTile(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st) {
    char adtPath[256];
    std::snprintf(adtPath, sizeof adtPath, "World\\Maps\\%s\\%s_%d_%d.adt", o.map.c_str(), o.map.c_str(), o.tileX,
                  o.tileY);
    std::vector<uint8_t> bytes;
    if (!data.read(adtPath, bytes)) {
        std::fprintf(stderr, "ADT not found: %s\n", adtPath);
        return false;
    }
    wowee::pipeline::ADTTerrain adt = wowee::pipeline::ADTLoader::load(bytes);
    if (!adt.isLoaded()) {
        std::fprintf(stderr, "ADT parse failed: %s\n", adtPath);
        return false;
    }

    // ---- texture table (MTEX index -> slot in tile table)
    std::vector<uint32_t> texIds{addPlaceholderTexture(pack, o.texFormat)};  // slot 0
    std::vector<uint16_t> mtexToSlot(adt.textures.size(), 0);
    for (size_t i = 0; i < adt.textures.size(); ++i) {
        uint32_t id = addTextureFromGame(pack, data, adt.textures[i], o.terrainTex, o.texFormat, false, o.verbose);
        if (id) {
            if (!pack.has(W3D_ENTRY_TEXTURE, id))
                std::fprintf(stderr, "  BUG: terrain texture %s (id %08x) is not in the pack after adding it\n", adt.textures[i].c_str(), id);
            ++st.texturesOk; mtexToSlot[i] = (uint16_t)texIds.size(); texIds.push_back(id);
        }
        else ++st.texturesFailed;
    }
    auto modelFor = [&](const std::string& name) { return addModelFromGame(pack, data, name, o, st); };
    std::function<void(std::vector<W3dPlacement>&)> wmoPlace;
    if (!o.noWmo)
        wmoPlace = [&](std::vector<W3dPlacement>& out) {
            for (const auto& p : adt.wmoPlacements) {
                if (p.nameId >= adt.wmoNames.size()) continue;
                auto w = wowee::core::coords::adtToWorld(p.position[0], p.position[1], p.position[2]);
                if (!claimPlacement(o, st.seenWmoUid, p.uniqueId, w.x, w.y)) { ++st.duplicatesSkipped; continue; }
                float pos[3] = {w.x, w.y, w.z}, rot[3];
                placementEuler(p.rotation, rot);
                placeWmo(pack, data, adt.wmoNames[p.nameId], pos, rot, p.doodadSet, o, st, modelFor, out);
            }
        };
    return emitTile(o, adt, texIds, mtexToSlot, modelFor, pack, st, wmoPlace);
}

bool placementOwner(const Options& o, float x, float y, int& tx, int& ty) {
    tx = (int)std::floor(32.0f - y / TS);
    ty = (int)std::floor(32.0f - x / TS);
    return tx >= o.tileX0 && tx <= o.tileX1 && ty >= o.tileY0 && ty <= o.tileY1;
}

// A placement listed by several ADTs is emitted by the tile that contains it, or -
// if that tile is outside the region - by the first tile that lists it.
static bool claimPlacement(const Options& o, std::unordered_set<uint32_t>& seen, uint32_t uid, float x, float y) {
    int tx, ty;
    bool inRegion = placementOwner(o, x, y, tx, ty);
    if (inRegion && (tx != o.tileX || ty != o.tileY)) return false;
    if (uid == 0) return true;
    return seen.insert(uid).second;
}

bool emitTile(const Options& o, const wowee::pipeline::ADTTerrain& adt, const std::vector<uint32_t>& texIds,
              const std::vector<uint16_t>& mtexToSlot, const std::function<uint32_t(const std::string&)>& modelFor,
              PackWriter& pack, ConvStats& st, const std::function<void(std::vector<W3dPlacement>&)>& wmoPlace) {
    // Tile min corner in render space (WoWee terrain_mesh.cpp convention).
    const float minX = (32.0f - (float)o.tileY - 1.0f) * TS;
    const float minY = (32.0f - (float)o.tileX - 1.0f) * TS;

    // ---- vertices + per-(region, texpair) index lists, one list per LOD
    const int g = o.grid;
    const int step = 8 / (g - 1);                      // outer-grid quads per vertex step
    const int lodStep[W3D_TERRAIN_LODS] = {1, std::min(2, g - 1), std::min(4, g - 1)};  // in vertex steps
    constexpr float kSkirtDepth = 4.0f;                // yards below the edge
    std::vector<TVert> verts;
    using LodLists = std::array<std::vector<uint32_t>, W3D_TERRAIN_LODS>;
    std::map<std::tuple<int, uint16_t, uint16_t>, LodLists> lists;  // (region, t0, t1)
    std::vector<uint8_t> alpha;

    auto pushTri = [&](std::vector<uint32_t>& lst, std::array<uint32_t, 3> tri) {
        // Force CCW seen from +Z so back-face culling is predictable.
        const float* p0 = verts[tri[0]].p; const float* p1 = verts[tri[1]].p; const float* p2 = verts[tri[2]].p;
        float cz = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]);
        if (cz < 0) std::swap(tri[1], tri[2]);
        lst.insert(lst.end(), tri.begin(), tri.end());
    };

    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            const auto& ch = adt.getChunk(cx, cy);
            if (!ch.hasHeightMap()) continue;

            uint16_t t0 = 0, t1 = 0;
            int blendLayer = -1;
            if (!ch.layers.empty() && ch.layers[0].textureId < mtexToSlot.size()) t0 = mtexToSlot[ch.layers[0].textureId];
            t1 = t0;
            // Keep the strongest overlay layer (the plan's 2-layers-per-chunk limit).
            float best = 0;
            std::vector<uint8_t> bestAlpha;
            for (size_t li = 1; li < ch.layers.size(); ++li) {
                if (!wowee::pipeline::decodeLayerAlpha(ch, li, alpha)) continue;
                float sum = 0;
                for (uint8_t a : alpha) sum += a;
                if (sum > best && ch.layers[li].textureId < mtexToSlot.size()) {
                    best = sum;
                    blendLayer = (int)li;
                    bestAlpha = alpha;
                    t1 = mtexToSlot[ch.layers[li].textureId];
                }
            }

            const uint32_t base = (uint32_t)verts.size();
            for (int j = 0; j < g; ++j)
                for (int i = 0; i < g; ++i) {
                    int col = i * step, row = j * step;           // 0..8 in the outer 9x9 grid
                    int index = row * 17 + col;
                    float h = ch.heightMap.heights[index];
                    if (!std::isfinite(h)) h = 0;
                    float gridX = (float)cy * 8.0f + (float)row;  // -> renderX
                    float gridY = (float)cx * 8.0f + (float)col;  // -> renderY
                    TVert v{};
                    v.p[0] = (1.0f - gridX / 128.0f) * TS;         // tile-local
                    v.p[1] = (1.0f - gridY / 128.0f) * TS;
                    v.p[2] = ch.position[2] + h;                   // absolute for now
                    float nx = ch.normals[index * 3 + 0] / 127.0f, ny = ch.normals[index * 3 + 1] / 127.0f,
                          nz = ch.normals[index * 3 + 2] / 127.0f;
                    float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (nl < 1e-4f) { nx = ny = 0; nz = 1; nl = 1; }
                    v.c[0] = v.c[1] = v.c[2] = bakeLight(nx / nl, ny / nl, nz / nl);
                    v.c[3] = 0;
                    if (blendLayer > 0) {
                        float au = ((float)col * (63.0f / 8.0f) + 0.5f) / 64.0f;
                        float av = ((float)row * (63.0f / 8.0f) + 0.5f) / 64.0f;
                        v.c[3] = (uint8_t)std::lround(std::clamp(wowee::pipeline::sampleAlpha(bestAlpha, au, av), 0.0f, 1.0f) * 255.0f);
                    }
                    const float k = kTexRepeatPerChunk / CHUNK;
                    v.uv[0] = (TS - v.p[1]) * k;
                    v.uv[1] = v.p[0] * k;
                    verts.push_back(v);
                }

            // Skirt vertices: copies of the chunk's edge vertices on a region border, lowered.
            // edges: 0 = row 0, 1 = row g-1, 2 = col 0, 3 = col g-1
            const bool onBorder[4] = {cy % 4 == 0, cy % 4 == 3, cx % 4 == 0, cx % 4 == 3};
            uint32_t skirtBase[4] = {0, 0, 0, 0};
            auto edgeVert = [&](int e, int k) -> uint32_t {   // k-th vertex along edge e
                int i = e < 2 ? k : (e == 2 ? 0 : g - 1);
                int j = e < 2 ? (e == 0 ? 0 : g - 1) : k;
                return base + (uint32_t)(j * g + i);
            };
            for (int e = 0; e < 4; ++e) {
                if (!onBorder[e]) continue;
                skirtBase[e] = (uint32_t)verts.size();
                for (int k = 0; k < g; ++k) {
                    TVert v = verts[edgeVert(e, k)];
                    v.p[2] -= kSkirtDepth;
                    verts.push_back(v);
                }
            }

            LodLists& L = lists[{(cy / 4) * 4 + cx / 4, t0, t1}];
            for (int lod = 0; lod < W3D_TERRAIN_LODS; ++lod) {
                const int s = lodStep[lod];
                for (int j = 0; j + s < g; j += s)
                    for (int i = 0; i + s < g; i += s) {
                        bool hole = false;
                        for (int qy = j * step; qy < (j + s) * step && !hole; ++qy)
                            for (int qx = i * step; qx < (i + s) * step && !hole; ++qx) hole = ch.isHole(qy, qx);
                        if (hole) continue;
                        uint32_t a = base + j * g + i, b = a + s, c = a + s * g, d = c + s;
                        pushTri(L[lod], {a, c, b});
                        pushTri(L[lod], {b, c, d});
                    }
                for (int e = 0; e < 4; ++e) {
                    if (!onBorder[e]) continue;
                    for (int k = 0; k + s < g; k += s) {
                        uint32_t e0 = edgeVert(e, k), e1 = edgeVert(e, k + s);
                        uint32_t b0 = skirtBase[e] + k, b1 = skirtBase[e] + k + s;
                        // both windings: a skirt must be visible from either side
                        uint32_t q[12] = {e0, b0, e1, e1, b0, b1, e0, e1, b0, e1, b1, b0};
                        L[lod].insert(L[lod].end(), q, q + 12);
                    }
                }
            }
        }
    if (verts.empty() || verts.size() > 65535) {
        std::fprintf(stderr, "terrain: bad vertex count %zu\n", verts.size());
        return false;
    }

    // ---- quantise
    float zmin = 1e30f, zmax = -1e30f;
    for (auto& v : verts) { zmin = std::min(zmin, v.p[2]); zmax = std::max(zmax, v.p[2]); }
    W3dTerrainHeader h{};
    h.tileX = (uint32_t)o.tileX;
    h.tileY = (uint32_t)o.tileY;
    h.origin[0] = minX; h.origin[1] = minY; h.origin[2] = zmin;
    float zr = std::max(zmax - zmin, 1.0f);
    h.posOffset[0] = TS * 0.5f; h.posOffset[1] = TS * 0.5f; h.posOffset[2] = zr * 0.5f;
    h.posScale[0] = TS * 0.5f / 32767.0f; h.posScale[1] = h.posScale[0]; h.posScale[2] = zr * 0.5f / 32767.0f;
    h.gridSize = (uint32_t)g;

    std::vector<W3dTerrainVertex> qv(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        auto& v = verts[i];
        v.p[2] -= zmin;
        auto q = [&](float val, int c) {
            return (int16_t)std::clamp((int)std::lround((val - h.posOffset[c]) / h.posScale[c]), -32767, 32767);
        };
        qv[i] = {q(v.p[0], 0), q(v.p[1], 1), q(v.p[2], 2), 0, v.c[0], v.c[1], v.c[2], v.c[3],
                 (int16_t)std::lround(v.uv[0] / W3D_TERRAIN_UV_SCALE), (int16_t)std::lround(v.uv[1] / W3D_TERRAIN_UV_SCALE)};
    }

    std::vector<uint16_t> indices;
    std::vector<W3dTerrainBatch> batches;
    for (auto& [key, L] : lists) {
        if (L[0].empty()) continue;
        W3dTerrainBatch b{};
        b.tex0 = std::get<1>(key);
        b.tex1 = std::get<2>(key);
        for (int c = 0; c < 3; ++c) { b.bmin[c] = 1e30f; b.bmax[c] = -1e30f; }
        for (int lod = 0; lod < W3D_TERRAIN_LODS; ++lod) {
            b.firstIndex[lod] = (uint32_t)indices.size();
            b.indexCount[lod] = (uint32_t)L[lod].size();
            for (uint32_t i : L[lod]) {
                indices.push_back((uint16_t)i);
                for (int c = 0; c < 3; ++c) { b.bmin[c] = std::min(b.bmin[c], verts[i].p[c]); b.bmax[c] = std::max(b.bmax[c], verts[i].p[c]); }
            }
        }
        batches.push_back(b);
    }
    h.vertexCount = (uint32_t)qv.size();
    h.indexCount = (uint32_t)indices.size();
    h.batchCount = (uint32_t)batches.size();
    h.texCount = (uint32_t)texIds.size();

    Blob blob;
    blob.put(h);
    blob.align(4);
    size_t offTex = blob.putArray(texIds);
    blob.align(4);
    size_t offB = blob.putArray(batches);
    blob.align(16);
    size_t offV = blob.putArray(qv);
    blob.align(4);
    size_t offI = blob.putArray(indices);
    blob.align(4);
    auto* hp = blob.at<W3dTerrainHeader>(0);
    hp->offTexIds = (uint32_t)offTex; hp->offBatches = (uint32_t)offB;
    hp->offVertices = (uint32_t)offV; hp->offIndices = (uint32_t)offI;
    pack.add(W3D_ENTRY_TERRAIN, w3d_tile_id(o.tileX, o.tileY), std::move(blob.data));
    for (auto& b : batches) st.terrainTris += b.indexCount[0] / 3;
    st.terrainVerts += qv.size();
    ++st.tiles;

    emitWater(o, adt, minX, minY, pack, st);

    // ---- doodads
    std::vector<W3dPlacement> placements;
    size_t limit = o.maxDoodads < 0 ? adt.doodadPlacements.size() : (size_t)o.maxDoodads;
    for (const auto& p : adt.doodadPlacements) {
        if (placements.size() >= limit) break;
        if (p.nameId >= adt.doodadNames.size()) continue;
        auto w = wowee::core::coords::adtToWorld(p.position[0], p.position[1], p.position[2]);
        if (!claimPlacement(o, st.seenDoodadUid, p.uniqueId, w.x, w.y)) { ++st.duplicatesSkipped; continue; }
        uint32_t mid = modelFor(adt.doodadNames[p.nameId]);
        if (!mid) continue;
        W3dPlacement pl{};
        pl.modelId = mid;
        pl.pos[0] = w.x; pl.pos[1] = w.y; pl.pos[2] = w.z;
        placementEuler(p.rotation, pl.rot);
        pl.scale = p.scale / 1024.0f;
        placements.push_back(pl);
        addPlacementCollision(o, st, pl);
    }
    size_t doodadCount = placements.size();
    if (wmoPlace) wmoPlace(placements);
    else st.wmoSkipped += adt.wmoPlacements.size();
    Blob pb;
    W3dPlacementHeader ph{};
    ph.count = (uint32_t)placements.size();
    pb.put(ph);
    pb.putArray(placements);
    pack.add(W3D_ENTRY_PLACEMENT, w3d_tile_id(o.tileX, o.tileY), std::move(pb.data));
    st.placements += doodadCount;
    return true;
}

}  // namespace w3d
