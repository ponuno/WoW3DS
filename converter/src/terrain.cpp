// ADT tile -> W3D terrain + doodad placements.
#include "conv.h"

#include "w3d_ground.h"
#include "core/coordinates.hpp"
#include "pipeline/adt_alpha.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    uint32_t id = pack.keyId(W3D_ENTRY_TEXTURE, "<placeholder>");
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


struct TVert { float p[3]; uint8_t c[4]; float uv[2]; };

static bool claimPlacement(const Options& o, std::unordered_set<uint32_t>& seen, uint32_t uid, float x, float y);

bool convertTile(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st) {
    trace("map %s tile %d_%d", o.map.c_str(), o.tileX, o.tileY);
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
            if (!textureInPack(pack, id))
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
    if (!o.noFarLod) collectFarTile(o, &data, adt, st);   // 6.24: the far ground's heights and colours of this tile
    return emitTile(o, adt, texIds, mtexToSlot, modelFor, pack, st, wmoPlace, &bytes);
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

// ---------------------------------------------------------------------------------------------------------------------
// Ground splat (stage 5.47 / 5.50). A batch draws three textures: the chunk's base and its two strongest overlays; a vertex
// carries two weights (first overlay a, second overlay g): colour = lerp(lerp(T0, T1, a), T2, g), i.e. the weights of the
// three slots are w0 = (1-a)(1-g), w1 = a(1-g), w2 = g. Chunks are drawn on their own, so where two neighbours do not have
// the same textures / alphas along their shared edge the ground tears along a straight line (the base of one chunk is the
// road, the other has no road at all: report 5.50, photo of the road in Northshire). harmonizeSplats makes the vertices of a
// shared edge look the same from both sides and fades the change over the next two vertex rows.
struct ChunkSplat {
    bool ok = false;
    uint16_t t[3] = {0, 0, 0};       // texture slots (indices of the tile's texture table); a repeated one is a free slot
    std::vector<float> w;            // g*g vertices x 3 slot weights (a repeated slot has weight 0, its first copy takes it)
};

static ChunkSplat computeSplat(const wowee::pipeline::MapChunk& ch, const std::vector<uint16_t>& mtexToSlot, int g, int step) {
    ChunkSplat sp;
    sp.ok = true;
    uint16_t t0 = 0;
    if (!ch.layers.empty() && ch.layers[0].textureId < mtexToSlot.size()) t0 = mtexToSlot[ch.layers[0].textureId];
    // 5.47: base + the two strongest overlays (WoW splats up to four layers). Overlays keep their layer order.
    struct Ov { size_t li; float sum; std::vector<uint8_t> a; };
    std::vector<Ov> cand;
    std::vector<uint8_t> alpha;
    for (size_t li = 1; li < ch.layers.size(); ++li) {
        if (ch.layers[li].textureId >= mtexToSlot.size()) continue;
        if (!wowee::pipeline::decodeLayerAlpha(ch, li, alpha)) continue;
        float sum = 0;
        for (uint8_t a : alpha) sum += a;
        if (sum > 0) cand.push_back({li, sum, alpha});
    }
    std::stable_sort(cand.begin(), cand.end(), [](const Ov& x, const Ov& y) { return x.sum > y.sum; });
    if (cand.size() > 2) cand.resize(2);
    std::sort(cand.begin(), cand.end(), [](const Ov& x, const Ov& y) { return x.li < y.li; });
    const std::vector<uint8_t>* ovA[2] = {nullptr, nullptr};
    uint16_t t1 = t0, t2 = t0;
    for (size_t k = 0; k < cand.size(); ++k) {
        ovA[k] = &cand[k].a;
        (k == 0 ? t1 : t2) = mtexToSlot[ch.layers[cand[k].li].textureId];
    }
    if (cand.size() == 1) t2 = t1;
    sp.t[0] = t0; sp.t[1] = t1; sp.t[2] = t2;
    sp.w.assign((size_t)g * g * 3, 0.0f);
    for (int j = 0; j < g; ++j)
        for (int i = 0; i < g; ++i) {
            const int col = i * step, row = j * step;
            const float au = ((float)col * (63.0f / 8.0f) + 0.5f) / 64.0f;
            const float av = ((float)row * (63.0f / 8.0f) + 0.5f) / 64.0f;
            float a = 0, gg = 0;
            if (ovA[0]) a = std::clamp(wowee::pipeline::sampleAlpha(*ovA[0], au, av), 0.0f, 1.0f);
            if (ovA[1]) gg = std::clamp(wowee::pipeline::sampleAlpha(*ovA[1], au, av), 0.0f, 1.0f);
            float* w = &sp.w[(size_t)(j * g + i) * 3];
            w[0] = (1 - a) * (1 - gg); w[1] = a * (1 - gg); w[2] = gg;
            // a repeated slot is a free one: its weight belongs to the first slot with that texture
            if (sp.t[1] == sp.t[0]) { w[0] += w[1]; w[1] = 0; }
            if (sp.t[2] == sp.t[0]) { w[0] += w[2]; w[2] = 0; }
            else if (sp.t[2] == sp.t[1]) { w[1] += w[2]; w[2] = 0; }
        }
    return sp;
}

using TexWeights = std::vector<std::pair<uint16_t, float>>;   // texture -> weight, a texture once
static TexWeights texWeights(const ChunkSplat& sp, const float* w) {
    TexWeights r;
    for (int k = 0; k < 3; ++k) {
        if (w[k] <= 0) continue;
        bool done = false;
        for (auto& e : r) if (e.first == sp.t[k]) { e.second += w[k]; done = true; }
        if (!done) r.push_back({sp.t[k], w[k]});
    }
    return r;
}
static float weightOf(const TexWeights& m, uint16_t t) {
    for (const auto& e : m) if (e.first == t) return e.second;
    return 0;
}

// Gives the chunk's vertex 'w' the look 'm' as far as its three slots allow (a texture it has no slot for is taken in by a
// free slot - one that repeats another - if there is one, otherwise dropped); false if nothing could be set.
static bool applyLook(ChunkSplat& sp, float* w, const TexWeights& m, std::vector<std::pair<int, uint16_t>>& newSlots) {
    for (const auto& e : m) {
        if (e.second < 0.02f) continue;
        bool have = false;
        for (int k = 0; k < 3; ++k) have = have || sp.t[k] == e.first;
        if (have) continue;
        for (int k = 1; k < 3; ++k) {   // a free slot: it repeats an earlier one (its weight is 0 everywhere)
            bool dup = false;
            for (int q = 0; q < k; ++q) dup = dup || sp.t[q] == sp.t[k];
            if (!dup) continue;
            sp.t[k] = e.first;
            newSlots.push_back({k, e.first});
            break;
        }
    }
    float nw[3] = {0, 0, 0}, sum = 0;
    for (int k = 0; k < 3; ++k) {
        bool first = true;
        for (int q = 0; q < k; ++q) first = first && sp.t[q] != sp.t[k];
        if (first) nw[k] = weightOf(m, sp.t[k]);
        sum += nw[k];
    }
    if (sum < 0.05f) return false;
    for (int k = 0; k < 3; ++k) w[k] = nw[k] / sum;
    return true;
}

static void harmonizeSplats(std::vector<ChunkSplat>& sp, int g) {
    const int n = g - 1;
    struct Member { int chunk, vi; };
    std::map<std::pair<int, int>, std::vector<Member>> groups;   // vertex position on the tile's grid -> chunk vertices there
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            if (!sp[cy * 16 + cx].ok) continue;
            for (int j = 0; j < g; ++j)
                for (int i = 0; i < g; ++i)
                    if (i == 0 || i == n || j == 0 || j == n) groups[{cx * n + i, cy * n + j}].push_back({cy * 16 + cx, j * g + i});
        }
    // the change made at each border vertex (per slot), kept to fade it inwards
    std::vector<std::vector<float>> delta(256);
    for (int c = 0; c < 256; ++c) if (sp[c].ok) delta[c].assign((size_t)g * g * 3, 0.0f);
    for (auto& [pos, mem] : groups) {
        if (mem.size() < 2) continue;
        std::vector<TexWeights> look;
        for (const Member& m : mem) look.push_back(texWeights(sp[m.chunk], &sp[m.chunk].w[(size_t)m.vi * 3]));
        TexWeights mean;   // the average look of the vertex over its chunks
        for (const auto& l : look)
            for (const auto& e : l) {
                bool done = false;
                for (auto& x : mean) if (x.first == e.first) { x.second += e.second / (float)mem.size(); done = true; }
                if (!done) mean.push_back({e.first, e.second / (float)mem.size()});
            }
        float worst = 0;
        for (const auto& l : look) {
            float d = 0;
            for (const auto& e : mean) d += std::fabs(e.second - weightOf(l, e.first));
            worst = std::max(worst, d);
        }
        if (worst < 0.08f) continue;   // the same look already (the usual case along a road or a meadow)
        // Only textures every chunk here can show count (a chunk has three slots; one with all three in use cannot take a
        // fourth): strongest first, a texture a chunk has no slot for takes one of its free slots (a slot repeating another).
        {
            std::vector<int> freeSlots;
            for (const Member& m : mem) {
                const ChunkSplat& c = sp[m.chunk];
                int f = 0;
                for (int k = 1; k < 3; ++k) {
                    bool dup = false;
                    for (int q = 0; q < k; ++q) dup = dup || c.t[q] == c.t[k];
                    f += dup;
                }
                freeSlots.push_back(f);
            }
            std::sort(mean.begin(), mean.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            TexWeights accepted;
            for (const auto& e : mean) {
                std::vector<int> need(mem.size(), 0);
                bool ok = true;
                for (size_t q = 0; q < mem.size() && ok; ++q) {
                    const ChunkSplat& c = sp[mem[q].chunk];
                    bool has = false;
                    for (int k = 0; k < 3; ++k) has = has || c.t[k] == e.first;
                    if (!has) { need[q] = 1; ok = freeSlots[q] > 0; }
                }
                if (!ok) continue;
                for (size_t q = 0; q < mem.size(); ++q) freeSlots[q] -= need[q];
                accepted.push_back(e);
            }
            if (accepted.empty()) continue;
            float tot = 0;
            for (const auto& e : accepted) tot += e.second;
            for (auto& e : accepted) e.second /= tot;
            mean = accepted;
        }
        for (const Member& m : mem) {
            ChunkSplat& c = sp[m.chunk];
            float* w = &c.w[(size_t)m.vi * 3];
            float old[3] = {w[0], w[1], w[2]};
            std::vector<std::pair<int, uint16_t>> added;
            if (!applyLook(c, w, mean, added)) continue;
            for (int k = 0; k < 3; ++k) delta[m.chunk][(size_t)m.vi * 3 + k] = w[k] - old[k];
        }
    }
    // fade over the next two rows: the vertex takes f of the change of the border vertex nearest to it
    static const float kFade[3] = {0.0f, 0.5f, 0.25f};
    for (int c = 0; c < 256; ++c) {
        if (!sp[c].ok) continue;
        ChunkSplat& s = sp[c];
        for (int j = 1; j < n; ++j)
            for (int i = 1; i < n; ++i) {
                const int d = std::min(std::min(i, n - i), std::min(j, n - j));
                if (d >= 3) continue;
                int bi = i, bj = j;   // the nearest border vertex
                if (i == d) bi = 0; else if (n - i == d) bi = n; else if (j == d) bj = 0; else bj = n;
                const float* dl = &delta[c][(size_t)(bj * g + bi) * 3];
                float* w = &s.w[(size_t)(j * g + i) * 3];
                float sum = 0;
                for (int k = 0; k < 3; ++k) { w[k] = std::max(0.0f, w[k] + kFade[d] * dl[k]); sum += w[k]; }
                if (sum > 1e-4f) for (int k = 0; k < 3; ++k) w[k] /= sum;
            }
    }
}

// 6.18a: GRND (w3d_ground.h): per chunk the 145 heights (corners and centres, as in the ADT) and the hole mask.
// Empty if the tile has no chunk with a height map.
std::vector<uint8_t> buildGround(const Options& o, const wowee::pipeline::ADTTerrain& adt, float minX, float minY) {
    float zmin = 1e30f, zmax = -1e30f;
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            const auto& ch = adt.getChunk(cx, cy);
            if (!ch.hasHeightMap()) continue;
            for (int i = 0; i < 145; ++i) {
                float z = ch.position[2] + ch.heightMap.heights[i];
                if (!std::isfinite(z)) z = 0;
                zmin = std::min(zmin, z);
                zmax = std::max(zmax, z);
            }
        }
    if (zmin > zmax) return {};
    W3dGroundHeader gh{};
    gh.tileX = (uint32_t)o.tileX;
    gh.tileY = (uint32_t)o.tileY;
    gh.origin[0] = minX; gh.origin[1] = minY; gh.origin[2] = zmin;
    gh.zScale = std::max(zmax - zmin, 1.0f) / 65535.0f;
    gh.chunkCount = 256;
    std::vector<W3dGroundChunk> chunks(256);
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            const auto& ch = adt.getChunk(cx, cy);
            W3dGroundChunk& out = chunks[cy * 16 + cx];
            std::memset(&out, 0, sizeof out);
            if (!ch.hasHeightMap()) continue;
            out.flags = W3D_GROUND_CHUNK_PRESENT;
            out.holes = (uint16_t)ch.holes;
            for (int i = 0; i < 145; ++i) {
                float z = ch.position[2] + ch.heightMap.heights[i];
                if (!std::isfinite(z)) z = 0;
                out.h[i] = (uint16_t)std::clamp((int)std::lround((z - zmin) / gh.zScale), 0, 65535);
            }
        }
    Blob b;
    b.put(gh);
    b.putArray(chunks);
    return std::move(b.data);
}

bool emitTile(const Options& o, const wowee::pipeline::ADTTerrain& adt, const std::vector<uint32_t>& texIds,
              const std::vector<uint16_t>& mtexToSlot, const std::function<uint32_t(const std::string&)>& modelFor,
              PackWriter& pack, ConvStats& st, const std::function<void(std::vector<W3dPlacement>&)>& wmoPlace,
              const std::vector<uint8_t>* adtBytes) {
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
    std::map<std::tuple<int, uint16_t, uint16_t, uint16_t>, LodLists> lists;  // (region, t0, t1, t2)

    auto pushTri = [&](std::vector<uint32_t>& lst, std::array<uint32_t, 3> tri) {
        // Force CCW seen from +Z so back-face culling is predictable.
        const float* p0 = verts[tri[0]].p; const float* p1 = verts[tri[1]].p; const float* p2 = verts[tri[2]].p;
        float cz = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]);
        if (cz < 0) std::swap(tri[1], tri[2]);
        lst.insert(lst.end(), tri.begin(), tri.end());
    };

    // ---- 5.50: the splat of every chunk (3 textures + a weight per texture per vertex), made to agree across chunk borders
    std::vector<ChunkSplat> splat(256);
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            const auto& ch = adt.getChunk(cx, cy);
            if (ch.hasHeightMap()) splat[cy * 16 + cx] = computeSplat(ch, mtexToSlot, g, step);
        }
    harmonizeSplats(splat, g);

    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            const auto& ch = adt.getChunk(cx, cy);
            if (!ch.hasHeightMap()) continue;
            const ChunkSplat& sp = splat[cy * 16 + cx];
            const uint16_t t0 = sp.t[0], t1 = sp.t[1], t2 = sp.t[2];
            const uint32_t base = (uint32_t)verts.size();
            // one ground vertex: (gridX, gridY) in squares over the tile, the height and normal of ADT vertex 'nidx', splat weights w
            auto mkVert = [&](float gridX, float gridY, float h, int nidx, const float* w) {
                if (!std::isfinite(h)) h = 0;
                TVert v{};
                v.p[0] = (1.0f - gridX / 128.0f) * TS;         // tile-local
                v.p[1] = (1.0f - gridY / 128.0f) * TS;
                v.p[2] = ch.position[2] + h;                   // absolute for now
                float nx = ch.normals[nidx * 3 + 0] / 127.0f, ny = ch.normals[nidx * 3 + 1] / 127.0f,
                      nz = ch.normals[nidx * 3 + 2] / 127.0f;
                float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (nl < 1e-4f) { nx = ny = 0; nz = 1; nl = 1; }
                const uint8_t light = bakeLight(nx / nl, ny / nl, nz / nl);
                v.c[0] = v.c[2] = light;
                v.c[1] = v.c[3] = 0;                           // g = second overlay weight, a = first overlay weight
                {   // (5.50: the weights come from the splat, see harmonizeSplats)
                    const float gg = w[2];
                    const float aa = gg < 0.9999f ? w[1] / (1.0f - gg) : 0.0f;
                    v.c[1] = (uint8_t)std::lround(std::clamp(gg, 0.0f, 1.0f) * 255.0f);
                    v.c[3] = (uint8_t)std::lround(std::clamp(aa, 0.0f, 1.0f) * 255.0f);
                }
                const float k = kTexRepeatPerChunk / CHUNK;
                v.uv[0] = (TS - v.p[1]) * k;
                v.uv[1] = v.p[0] * k;
                verts.push_back(v);
            };
            for (int j = 0; j < g; ++j)
                for (int i = 0; i < g; ++i) {
                    int col = i * step, row = j * step;           // 0..8 in the outer 9x9 grid
                    int index = row * 17 + col;
                    mkVert((float)cy * 8.0f + (float)row, (float)cx * 8.0f + (float)col, ch.heightMap.heights[index], index,
                           &sp.w[(size_t)(j * g + i) * 3]);       // gridX -> renderX, gridY -> renderY
                }
            // 6.18a: the centre vertex of every square (the 145-height layout; the walker's ground is the four triangles around it)
            const bool centers = o.terrainCenters && step == 1;
            const uint32_t centerBase = (uint32_t)verts.size();
            if (centers)
                for (int j = 0; j < g - 1; ++j)
                    for (int i = 0; i < g - 1; ++i) {
                        float w[3];   // the mean of the square's corners
                        for (int k = 0; k < 3; ++k)
                            w[k] = 0.25f * (sp.w[(size_t)(j * g + i) * 3 + k] + sp.w[(size_t)(j * g + i + 1) * 3 + k] +
                                            sp.w[(size_t)((j + 1) * g + i) * 3 + k] + sp.w[(size_t)((j + 1) * g + i + 1) * 3 + k]);
                        const int index = j * 17 + 9 + i;
                        mkVert((float)cy * 8.0f + (float)j + 0.5f, (float)cx * 8.0f + (float)i + 0.5f, ch.heightMap.heights[index], index, w);
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

            LodLists& L = lists[{(cy / 4) * 4 + cx / 4, t0, t1, t2}];
            for (int lod = 0; lod < W3D_TERRAIN_LODS; ++lod) {
                const int s = lodStep[lod];
                // A block of s x s vertex steps; one touching a hole is split into halves down to single steps (6.13: it was
                // dropped whole - beyond 140 yd the ground around every building's footprint vanished, objects hung in the air)
                std::function<void(int, int, int)> block = [&](int i, int j, int sz) {
                    bool hole = false;
                    for (int qy = j * step; qy < (j + sz) * step && !hole; ++qy)
                        for (int qx = i * step; qx < (i + sz) * step && !hole; ++qx) hole = ch.isHole(qy, qx);
                    if (hole) {
                        if (sz == 1) return;
                        const int h = sz / 2;
                        block(i, j, h); block(i + h, j, h); block(i, j + h, h); block(i + h, j + h, h);
                        return;
                    }
                    uint32_t a = base + j * g + i, b = a + sz, c = a + sz * g, d = c + sz;
                    if (centers && lod == 0 && sz == 1) {   // four triangles around the centre, as the game draws the square
                        const uint32_t m = centerBase + j * (g - 1) + i;
                        pushTri(L[lod], {m, a, b});
                        pushTri(L[lod], {m, b, d});
                        pushTri(L[lod], {m, d, c});
                        pushTri(L[lod], {m, c, a});
                        return;
                    }
                    pushTri(L[lod], {a, c, b});
                    pushTri(L[lod], {b, c, d});
                };
                for (int j = 0; j + s < g; j += s)
                    for (int i = 0; i + s < g; i += s) block(i, j, s);
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
    std::vector<uint16_t> tex2;
    for (auto& [key, L] : lists) {
        if (L[0].empty()) continue;
        W3dTerrainBatch b{};
        b.tex0 = std::get<1>(key);
        b.tex1 = std::get<2>(key);
        tex2.push_back(std::get<3>(key));
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
    blob.align(4);
    size_t offT2 = blob.putArray(tex2);
    blob.align(16);
    size_t offV = blob.putArray(qv);
    blob.align(4);
    size_t offI = blob.putArray(indices);
    blob.align(4);
    auto* hp = blob.at<W3dTerrainHeader>(0);
    hp->offTexIds = (uint32_t)offTex; hp->offBatches = (uint32_t)offB;
    hp->offVertices = (uint32_t)offV; hp->offIndices = (uint32_t)offI; hp->offTex2 = (uint32_t)offT2;
    pack.add(W3D_ENTRY_TERRAIN, w3d_tile_id(o.tileX, o.tileY), std::move(blob.data));
    if (o.exactGround) {   // 6.18a: the ADT's own heights and holes - what the walker stands on
        std::vector<uint8_t> gr = buildGround(o, adt, minX, minY);
        if (!gr.empty()) { ++st.groundTiles; st.groundBytes += gr.size(); pack.add(W3D_ENTRY_GROUND, w3d_tile_id(o.tileX, o.tileY), std::move(gr)); }
    }
    for (auto& b : batches) st.terrainTris += b.indexCount[0] / 3;
    st.terrainVerts += qv.size();
    ++st.tiles;

    collectAdtLiquids(o, adtBytes, adt, st);   // written with the buildings' liquids by emitLiquids() after all tiles

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
