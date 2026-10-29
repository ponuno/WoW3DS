// Liquids (stage 5.26): lakes, rivers, the sea, lava and slime of the terrain (ADT MCLQ) and of buildings (WMO MLIQ)
// -> WATR (what the 3DS draws, with the game's animated textures and Light.dbc colours) + LIQD (where the surfaces
// are: swimming, water walking, falling into water, the underwater view).
//
// MCLQ (vanilla, inside MCNK; vMaNGOS extractor adt.h): MCNK header +0x60 ofsLiquid / +0x64 sizeLiquid, offsets from
// the MCNK chunk start, pointing at a "MCLQ" chunk header whose own size is unusable. One 804-byte layer per liquid
// flag of the MCNK in the order river 0x04, ocean 0x08, magma 0x10, slime 0x20:
//   f32 min, max; 9 x 9 { u32 (water: u8 depth, flow0, flow1, filler | magma: u16 s, t), f32 height };
//   u8 tiles[8][8] (0x0F = no liquid, 0x80 = "dark water": deep, fatigue); 84 bytes of flow data.
// WoWee's parser keeps one layer, and hides the 0x80 cells (the open sea), so the converter reads MCLQ itself.
#include "conv.h"

#include "pipeline/adt_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace w3d {

namespace {
constexpr float TS = 533.33333f;
constexpr float kCell = W3D_LIQ_CELL;
constexpr float kUvRepeat = TS / 64.0f;   // yards per texture repeat: two liquid cells

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
float rdf(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

struct McnkLiquid { int index; uint32_t flags; const uint8_t* layers; int count; };

// The MCNK chunks of an ADT in file order, each with its MCLQ layers (count 0 = none).
std::vector<McnkLiquid> scanMclq(const std::vector<uint8_t>& b, ConvStats& st) {
    std::vector<McnkLiquid> out;
    int index = 0;
    for (size_t p = 0; p + 8 <= b.size();) {
        uint32_t len = rd32(&b[p + 4]);
        if (len > b.size() - p - 8) break;
        if (!std::memcmp(&b[p], "KNCM", 4) && len >= 0x68) {
            const uint8_t* h = &b[p + 8];
            const uint32_t flags = rd32(h), ofs = rd32(h + 0x60), size = rd32(h + 0x64);
            McnkLiquid m{index, flags, nullptr, 0};
            const int wanted = __builtin_popcount(flags & 0x3C);
            if (ofs && wanted) {
                const size_t end = p + 8 + len;   // MCLQ lies inside the MCNK
                size_t at = 0;
                if (p + ofs + 8 <= end && !std::memcmp(&b[p + ofs], "QLCM", 4)) at = p + ofs + 8;           // the real layout
                else if (p + 8 + ofs + 8 <= end && !std::memcmp(&b[p + 8 + ofs], "QLCM", 4)) at = p + 16 + ofs;   // payload-relative
                else at = p + ofs + 8;
                int fit = at < end ? (int)((end - at) / 804) : 0;
                if (size > 8 && (size - 8) / 804 < (uint32_t)fit) fit = (int)((size - 8) / 804);
                m.count = std::min(wanted, fit);
                if (m.count < wanted) ++st.liquidMclqBad;
                if (m.count > 0) m.layers = &b[at];
            }
            out.push_back(m);
            ++index;
        }
        p += 8 + len;
    }
    return out;
}

void initChunk(ConvStats::LiquidChunk& c) {
    std::memset(c.cell, W3D_LIQ_DRY, sizeof c.cell);
    for (auto& cell : c.h) for (float& v : cell) v = -1e30f;
    for (auto& cell : c.depth) for (float& v : cell) v = 10.0f;
}

// Corner q (0 (r,k), 1 (r,k+1), 2 (r+1,k), 3 (r+1,k+1)) of cell (r, k) as an index into a chunk's 9 x 9 vertices.
inline int cornerVertex(int r, int k, int q) { return (r + (q >> 1)) * 9 + k + (q & 1); }
float cellTop(const ConvStats::LiquidChunk& c, int ci) { return std::max({c.h[ci][0], c.h[ci][1], c.h[ci][2], c.h[ci][3]}); }

// Terrain height at the outer vertex (r, c) of a chunk (WoWee heights: 9 outer + 8 inner per row pair).
bool terrainAt(const wowee::pipeline::ADTTerrain& adt, int ci, int r, int c, float* z) {
    const auto& ch = adt.getChunk(ci % 16, ci / 16);
    if (!ch.hasHeightMap()) return false;
    float h = ch.heightMap.heights[r * 17 + c];
    if (!std::isfinite(h)) return false;
    *z = ch.position[2] + h;
    return true;
}

// One layer's visible cells into the chunk: a cell takes the layer whose surface there is the highest, with that
// layer's own corners (only the corners of visible cells are valid: the client leaves the others uninitialised).
void mergeLayer(ConvStats::LiquidChunk& c, int kind, const float* heights, const uint8_t* tiles, ConvStats& st) {
    for (int r = 0; r < 8; ++r)
        for (int k = 0; k < 8; ++k) {
            const uint8_t t = tiles[r * 8 + k];
            if ((t & 0x0F) == 0x0F) continue;
            float top = -1e30f, hq[4];
            bool ok = true;
            for (int q = 0; q < 4; ++q) {
                hq[q] = heights[cornerVertex(r, k, q)];
                if (!std::isfinite(hq[q]) || std::fabs(hq[q]) > 20000.0f) ok = false;
                else top = std::max(top, hq[q]);
            }
            if (!ok) continue;
            const int ci = r * 8 + k;
            if (c.cell[ci] != W3D_LIQ_DRY && cellTop(c, ci) >= top) continue;
            if (c.cell[ci] == W3D_LIQ_DRY) ++st.liquidCells;
            c.cell[ci] = (uint8_t)(kind | ((t & 0x80) ? W3D_LIQ_DEEP : 0));
            for (int q = 0; q < 4; ++q) c.h[ci][q] = hq[q];
        }
}

}  // namespace

// ------------------------------------------------------------------ terrain liquids
void collectAdtLiquids(const Options& o, const std::vector<uint8_t>* adtBytes, const wowee::pipeline::ADTTerrain& adt,
                       ConvStats& st) {
    std::map<int, ConvStats::LiquidChunk> chunks;
    auto chunkOf = [&](int ci) -> ConvStats::LiquidChunk& {
        auto it = chunks.find(ci);
        if (it != chunks.end()) return it->second;
        ConvStats::LiquidChunk& c = chunks[ci];
        initChunk(c);
        return c;
    };
    if (adtBytes) {   // the real file: every layer, deep cells
        static const int kKinds[4] = {W3D_LIQUID_WATER, W3D_LIQUID_OCEAN, W3D_LIQUID_MAGMA, W3D_LIQUID_SLIME};
        for (const McnkLiquid& m : scanMclq(*adtBytes, st)) {
            if (!m.count || m.index >= 256) continue;
            ConvStats::LiquidChunk& c = chunkOf(m.index);
            int layer = 0;
            for (int bit = 0; bit < 4 && layer < m.count; ++bit) {
                if (!(m.flags & (0x04u << bit))) continue;
                const uint8_t* L = m.layers + (size_t)layer * 804;
                ++layer;
                float heights[81];
                for (int v = 0; v < 81; ++v) heights[v] = rdf(L + 8 + v * 8 + 4);
                mergeLayer(c, kKinds[bit], heights, L + 8 + 648, st);
                ++st.liquidLayers;
            }
        }
    } else {   // WoWee's layers (synthetic tiles): one per MCNK, 0x80 cells already dropped
        for (int ci = 0; ci < 256; ++ci)
            for (const auto& L : adt.waterData[ci].layers) {
                if (L.width < 1 || L.height < 1 || L.x + L.width > 8 || L.y + L.height > 8) continue;
                const int vw = L.width + 1;
                if ((int)L.heights.size() < vw * (L.height + 1)) continue;
                float heights[81];
                uint8_t tiles[64];
                std::fill(heights, heights + 81, L.maxHeight);
                std::fill(tiles, tiles + 64, 0x0F);
                for (int j = 0; j <= L.height; ++j)
                    for (int i = 0; i < vw; ++i) heights[(L.y + j) * 9 + L.x + i] = L.heights[j * vw + i];
                for (int j = 0; j < L.height; ++j)
                    for (int i = 0; i < L.width; ++i)
                        if (j >= (int)L.mask.size() || ((L.mask[j] >> i) & 1)) tiles[(L.y + j) * 8 + L.x + i] = 0;
                ConvStats::LiquidChunk& c = chunkOf(ci);
                mergeLayer(c, std::min<int>(L.liquidType, 3), heights, tiles, st);
                ++st.liquidLayers;
            }
    }
    if (chunks.empty()) return;
    for (auto& [ci, c] : chunks) {
        for (int cell = 0; cell < 64; ++cell) {
            if (c.cell[cell] == W3D_LIQ_DRY) continue;
            for (int q = 0; q < 4; ++q) {
                float tz;
                const int v = cornerVertex(cell / 8, cell % 8, q);
                if (terrainAt(adt, ci, v / 9, v % 9, &tz)) c.depth[cell][q] = c.h[cell][q] - tz;
            }
        }
        for (uint8_t f : c.cell) if (f != W3D_LIQ_DRY && (f & W3D_LIQ_DEEP)) ++st.liquidDeepCells;
    }
    auto& tl = st.liquids[w3d_tile_id(o.tileX, o.tileY)];
    for (auto& [ci, c] : chunks) {
        bool any = false;
        for (uint8_t f : c.cell) any |= f != W3D_LIQ_DRY;
        if (any) tl.chunks[ci] = c;
    }
}

// ------------------------------------------------------------------ building liquids
int wmoLiquidKind(uint32_t rootFlags, uint32_t rootId, uint32_t groupFlags, uint32_t groupLiquid, const uint8_t* tiles,
                  size_t tileCount) {
    // vMaNGOS contrib/vmap_extractor/vmapextract/wmo.cpp (after wowdev): LiquidType.dbc entry 1 water, 2 ocean,
    // 3 magma, 4 slime (21: Naxxramas slime).
    uint32_t e;
    if (rootFlags & 4) e = groupLiquid;
    else if (groupLiquid == 15) e = 0;
    else e = groupLiquid + 1;
    if (!e) {
        for (size_t i = 0; i < tileCount; ++i)
            if ((tiles[i] & 0x0F) != 0x0F) { e = (tiles[i] & 0x0F) + 1u; break; }
    }
    if (e && e < 21) {
        switch ((e - 1) & 3) {
            case 0: e = (groupFlags & 0x80000) ? 2 : 1; break;
            case 1: e = 2; break;
            case 2: e = 3; break;
            default: e = rootId == 4489 ? 21 : 4; break;
        }
    }
    switch (e) {
        case 1: return W3D_LIQUID_WATER;
        case 2: return W3D_LIQUID_OCEAN;
        case 3: return W3D_LIQUID_MAGMA;
        case 4: case 21: return W3D_LIQUID_SLIME;
        default: return -1;
    }
}

namespace {

// ------------------------------------------------------------------ colours (Light.dbc at noon)
// Light (1.12, WoWee classic layout): ID 0, map 1, x 2, z 3, y 4 (36ths of a yard on the tile grid's axes), falloff
// start 5, end 6, LightParams 7 (clear), 8 (rain), 9 (underwater). LightIntBand / LightFloatBand: ID = (params - 1) *
// 18 (6) + channel + 1, count 1, times 2..17 (half minutes), values 18..33 (BGRA / float).
// Colour channels: 7 fog, 14 / 15 ocean shallow / deep, 16 / 17 river shallow / deep (the last four of a block).
// Float channel 0: fog end (36ths of a yard).
struct Rgb { float r, g, b; };
struct LightVol { float x, y, inner, outer; uint32_t clear, under; };
struct LightDb {
    bool loaded = false, have = false;
    std::vector<LightVol> vols;
    std::map<uint32_t, uint32_t> intRow, floatRow;   // band id -> row
    DbcTable ib, fb;
};

Rgb bgra(uint32_t v) { return {(float)((v >> 16) & 255), (float)((v >> 8) & 255), (float)(v & 255)}; }

bool sampleInt(const LightDb& L, uint32_t params, int ch, Rgb* out) {
    if (!params) return false;
    auto it = L.intRow.find((params - 1) * 18 + (uint32_t)ch + 1);
    if (it == L.intRow.end()) return false;
    const uint32_t r = it->second, n = std::min<uint32_t>(L.ib.u32(r, 1), 16);
    if (!n) return false;
    const uint32_t noon = 1440;
    uint32_t k = 0;
    while (k + 1 < n && L.ib.u32(r, 2 + k + 1) <= noon) ++k;
    Rgb a = bgra(L.ib.u32(r, 18 + k));
    if (k + 1 < n) {
        uint32_t t0 = L.ib.u32(r, 2 + k), t1 = L.ib.u32(r, 2 + k + 1);
        if (t1 > t0 && noon > t0) {
            float f = std::min(1.0f, (float)(noon - t0) / (float)(t1 - t0));
            Rgb b = bgra(L.ib.u32(r, 18 + k + 1));
            a = {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
        }
    }
    *out = a;
    return true;
}

bool sampleFloat(const LightDb& L, uint32_t params, int ch, float* out) {
    if (!params) return false;
    auto it = L.floatRow.find((params - 1) * 6 + (uint32_t)ch + 1);
    if (it == L.floatRow.end()) return false;
    const uint32_t r = it->second, n = std::min<uint32_t>(L.fb.u32(r, 1), 16);
    if (!n) return false;
    uint32_t k = 0;
    while (k + 1 < n && L.fb.u32(r, 2 + k + 1) <= 1440) ++k;
    *out = L.fb.f32(r, 18 + k);
    return std::isfinite(*out);
}

const LightDb& lightDb(const Options& o, const DataDir* data) {
    static std::map<std::string, LightDb> cache;
    LightDb& L = cache[o.map];
    if (L.loaded) return L;
    L.loaded = true;
    if (!data) return L;
    DbcTable maps, light;
    if (!maps.load(*data, "Map.dbc") || !light.load(*data, "Light.dbc") || light.fields < 10 ||
        !L.ib.load(*data, "LightIntBand.dbc") || L.ib.fields < 34)
        return L;
    L.fb.load(*data, "LightFloatBand.dbc");
    uint32_t mapId = 0xFFFFFFFFu;
    for (uint32_t r = 0; r < maps.records; ++r) {
        std::string d = maps.str(r, 1);
        if (d.size() == o.map.size() && std::equal(d.begin(), d.end(), o.map.begin(),
                                                    [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
            mapId = maps.u32(r, 0);
    }
    if (mapId == 0xFFFFFFFFu) return L;
    const float zp = 32.0f * TS;
    for (uint32_t r = 0; r < light.records; ++r) {
        if (light.u32(r, 1) != mapId) continue;
        LightVol v;
        const float dx = light.f32(r, 2), dy = light.f32(r, 4);
        v.x = zp - dy / 36.0f;
        v.y = zp - dx / 36.0f;
        v.inner = light.f32(r, 5) / 36.0f;
        v.outer = light.f32(r, 6) / 36.0f;
        v.clear = light.u32(r, 7);
        v.under = light.u32(r, 9);
        if (dx == 0 && dy == 0 && v.outer <= 0) { v.x = v.y = 0; v.outer = 0; }   // the map's global light
        L.vols.push_back(v);
    }
    for (uint32_t r = 0; r < L.ib.records; ++r) L.intRow[L.ib.u32(r, 0)] = r;
    for (uint32_t r = 0; r < L.fb.records; ++r) L.floatRow[L.fb.u32(r, 0)] = r;
    L.have = !L.vols.empty();
    return L;
}

uint8_t c8(float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 255.0f)); }

// The look of the four liquids for one tile: the light volume around its centre (else the map's global one).
void tileLook(const Options& o, const DataDir* data, int tx, int ty, W3dLiquidLook look[4], bool* fromDbc) {
    // defaults (no Light.dbc): Elwynn-like water, darker sea, lava and slime glowing in their own colour
    const Rgb defClose[2] = {{70, 125, 140}, {45, 95, 130}}, defFar[2] = {{25, 60, 80}, {15, 40, 75}};
    const Rgb defFog[4] = {{28, 62, 78}, {18, 45, 72}, {190, 70, 15}, {70, 120, 35}};
    const float defEnd[4] = {45, 70, 6, 10};
    Rgb cl[4] = {defClose[0], defClose[1], {255, 255, 255}, {255, 255, 255}};
    Rgb fa[4] = {defFar[0], defFar[1], {255, 255, 255}, {255, 255, 255}};
    Rgb fog[4] = {defFog[0], defFog[1], defFog[2], defFog[3]};
    float fogEnd[4] = {defEnd[0], defEnd[1], defEnd[2], defEnd[3]};
    *fromDbc = false;
    const LightDb& L = lightDb(o, data);
    if (L.have) {
        const float cx = (32.0f - ty - 0.5f) * TS, cy = (32.0f - tx - 0.5f) * TS;
        const LightVol* best = nullptr;
        float bestK = 1e30f;
        for (const LightVol& v : L.vols) {
            float k;
            if (v.outer <= 0) k = 1e20f;   // global: only when no zone light covers the tile
            else {
                float d = std::hypot(cx - v.x, cy - v.y);
                if (d > v.outer) continue;
                k = d / v.outer;
            }
            if (k < bestK) { bestK = k; best = &v; }
        }
        if (best) {
            Rgb c;
            bool any = false;
            if (sampleInt(L, best->clear, 16, &c)) { cl[0] = c; any = true; }
            if (sampleInt(L, best->clear, 17, &c)) { fa[0] = c; any = true; }
            if (sampleInt(L, best->clear, 14, &c)) { cl[1] = c; any = true; }
            if (sampleInt(L, best->clear, 15, &c)) { fa[1] = c; any = true; }
            if (sampleInt(L, best->under, 7, &c)) { fog[0] = fog[1] = c; any = true; }
            else { fog[0] = {fa[0].r * 0.8f, fa[0].g * 0.8f, fa[0].b * 0.8f}; fog[1] = {fa[1].r * 0.8f, fa[1].g * 0.8f, fa[1].b * 0.8f}; }
            float e;
            if (sampleFloat(L, best->under, 0, &e) && e > 0) fogEnd[0] = fogEnd[1] = std::clamp(e / 36.0f, 12.0f, 150.0f);
            *fromDbc = any;
        }
    }
    for (int k = 0; k < 4; ++k) {
        look[k] = W3dLiquidLook{};
        look[k].close[0] = c8(cl[k].r); look[k].close[1] = c8(cl[k].g); look[k].close[2] = c8(cl[k].b); look[k].close[3] = 255;
        look[k].far_[0] = c8(fa[k].r); look[k].far_[1] = c8(fa[k].g); look[k].far_[2] = c8(fa[k].b); look[k].far_[3] = 255;
        look[k].fog[0] = c8(fog[k].r); look[k].fog[1] = c8(fog[k].g); look[k].fog[2] = c8(fog[k].b); look[k].fog[3] = 255;
        look[k].fogEnd = fogEnd[k];
    }
}

// ------------------------------------------------------------------ textures
// Procedural stand-in (no client data / a frame missing): tileable ripples in the liquid's colour.
uint32_t proceduralLiquid(PackWriter& pack, int liquid, uint8_t fmt) {
    static const char* names[4] = {"<liquid-water>", "<liquid-ocean>", "<liquid-magma>", "<liquid-slime>"};
    static const uint8_t base[4][3] = {{150, 185, 200}, {130, 165, 195}, {230, 90, 20}, {80, 160, 40}};
    uint32_t id = pack.keyId(W3D_ENTRY_TEXTURE, names[liquid]);
    if (pack.has(W3D_ENTRY_TEXTURE, id)) return id;
    Image im;
    im.w = im.h = 64;
    im.rgba.resize(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
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

struct Frames { std::vector<uint32_t> ids; float tint = 1.0f; bool game = false; };

// The liquid's animation (XTextures\river\lake_a.1..30 etc.), every 30/frames-th frame; tint = how much the Light.dbc
// colour multiplies it: a grey texture (the ripples only) takes the colour fully, an already coloured one a little.
const Frames& liquidFrames(const Options& o, const DataDir* data, PackWriter& pack, int liquid, ConvStats& st) {
    static Frames cache[4];
    Frames& f = cache[liquid];
    if (!f.ids.empty() && textureInPack(pack, f.ids[0])) return f;   // (a new pack, e.g. the next dungeon: again)
    f = Frames();
    static const char* kFmt[4] = {"XTextures\\river\\lake_a.%d.blp", "XTextures\\ocean\\ocean_h.%d.blp",
                                  "XTextures\\lava\\lava.%d.blp", "XTextures\\slime\\slime.%d.blp"};
    int want = std::clamp(o.waterFrames, 1, 30);
    while (30 % want) --want;
    const int step = 30 / want;
    if (data) {
        for (int i = 1; i <= 30; i += step) {
            char path[64];
            std::snprintf(path, sizeof path, kFmt[liquid], i);
            if (f.ids.empty()) {   // the first frame: its colour decides the tint
                std::vector<uint8_t> bytes;
                Image img;
                if (data->read(path, bytes) && decodeBlp(bytes, img) && !img.rgba.empty()) {
                    double sat = 0, lum = 0;
                    const size_t n = img.rgba.size() / 4;
                    double mr = 0, mg = 0, mb = 0;
                    for (size_t k = 0; k < n; ++k) {
                        float r = img.rgba[k * 4], g = img.rgba[k * 4 + 1], b = img.rgba[k * 4 + 2];
                        float mx = std::max({r, g, b}), mn = std::min({r, g, b});
                        sat += mx > 0 ? (mx - mn) / mx : 0;
                        lum += (r + g + b) / 3;
                        mr += r; mg += g; mb += b;
                    }
                    sat /= n; lum /= n;
                    f.tint = o.waterTint >= 0 ? std::min(1.0f, o.waterTint) : (sat < 0.18 ? 1.0f : sat < 0.35 ? 0.6f : 0.3f);
                    if (liquid >= W3D_LIQUID_MAGMA) f.tint = 0;   // lava and slime keep their own colours
                    std::printf("  liquid %d texture %s: %dx%d, mean rgb %.0f %.0f %.0f, saturation %.2f -> Light.dbc tint %.1f\n",
                                liquid, path, img.w, img.h, mr / n, mg / n, mb / n, sat, f.tint);
                    (void)lum;
                }
            }
            uint32_t id = addTextureFromGame(pack, *data, path, o.waterTex, o.texFormat, false, o.verbose);
            if (!id) { ++st.liquidFramesMissing; break; }   // keep the frames up to the first missing one
            f.ids.push_back(id);
            ++st.liquidFramesOk;
        }
    }
    if (f.ids.empty()) {
        f.ids.push_back(proceduralLiquid(pack, liquid, o.texFormat));
        f.tint = liquid >= W3D_LIQUID_MAGMA ? 0.0f : 0.85f;
        f.game = false;
    } else {
        f.game = true;
    }
    return f;
}

// ------------------------------------------------------------------ mesh
struct MeshV { float p[3]; uint8_t c[4]; };

// Shallow -> deep: colour and opacity of a corner (depth in yards; buildings: medium).
void shade(const W3dLiquidLook& lk, int liquid, float depth, float tint, uint8_t out[4]) {
    static const float kA0[4] = {0.40f, 0.55f, 1.0f, 0.80f}, kA1[4] = {0.82f, 0.90f, 1.0f, 0.92f};
    static const float kDeep[4] = {6.0f, 9.0f, 1.0f, 3.0f};
    const float f = std::clamp(depth / kDeep[liquid], 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c) {
        float col = lk.close[c] + (lk.far_[c] - (float)lk.close[c]) * f;
        out[c] = c8(255.0f + (col - 255.0f) * tint);
    }
    out[3] = c8(255.0f * (kA0[liquid] + (kA1[liquid] - kA0[liquid]) * f));
}

}  // namespace

void emitLiquids(const Options& o, const DataDir* data, PackWriter& pack, ConvStats& st) {
    for (auto& [tid, tl] : st.liquids) {
        if (tl.chunks.empty() && tl.quads.empty()) continue;
        if (!pack.has(W3D_ENTRY_TERRAIN, tid)) continue;   // the client only loads tiles that have terrain
        const int tx = (int)(tid / 64), ty = (int)(tid % 64);
        const float minX = (32.0f - ty - 1.0f) * TS, minY = (32.0f - tx - 1.0f) * TS;
        const float maxX = minX + TS, maxY = minY + TS;
        W3dLiquidLook look[4];
        bool fromDbc = false;
        tileLook(o, data, tx, ty, look, &fromDbc);

        // ---- query grid: the terrain's chunks, then the buildings' cells rasterised over them
        std::map<int, ConvStats::LiquidChunk> grid = tl.chunks;
        for (const auto& q : tl.quads) {
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
            for (auto& p : q.p) { x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]); y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]); }
            const int gx0 = std::max(0, (int)std::floor((maxX - x1) / kCell)), gx1 = std::min(127, (int)std::floor((maxX - x0) / kCell));
            const int gy0 = std::max(0, (int)std::floor((maxY - y1) / kCell)), gy1 = std::min(127, (int)std::floor((maxY - y0) / kCell));
            for (int gx = gx0; gx <= gx1; ++gx)
                for (int gy = gy0; gy <= gy1; ++gy) {
                    const float px = maxX - (gx + 0.5f) * kCell, py = maxY - (gy + 0.5f) * kCell;
                    // inside one of the quad's triangles (0 1 2, 0 2 3)? height by the plane of that triangle
                    float z = 0;
                    bool in = false;
                    for (int t = 0; t < 2 && !in; ++t) {
                        const float* a = q.p[0]; const float* b = q.p[t + 1]; const float* c = q.p[t + 2];
                        float den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
                        if (std::fabs(den) < 1e-6f) continue;
                        float w0 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / den;
                        float w1 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / den;
                        float w2 = 1 - w0 - w1;
                        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                        z = w0 * a[2] + w1 * b[2] + w2 * c[2];
                        in = true;
                    }
                    if (!in) continue;
                    const int ci = (gx / 8) * 16 + gy / 8, cell = (gx % 8) * 8 + gy % 8;
                    auto it = grid.find(ci);
                    if (it == grid.end()) { it = grid.emplace(ci, ConvStats::LiquidChunk{}).first; initChunk(it->second); }
                    ConvStats::LiquidChunk& c = it->second;
                    if (c.cell[cell] != W3D_LIQ_DRY && cellTop(c, cell) >= z) continue;   // a higher surface is there
                    c.cell[cell] = (uint8_t)(q.kind | W3D_LIQ_WMO);
                    for (float& hv : c.h[cell]) hv = z;
                }
        }

        // ---- LIQD: a chunk is flat (one height), shared (9 x 9 corners every wet cell agrees on) or per cell
        {
            std::vector<uint16_t> index(256, 0xFFFF);
            std::vector<W3dLiquidChunk> recs;
            std::vector<int16_t> heights;
            std::vector<uint32_t> recHeights;   // per record: W3D_LIQ_FLAT, or index into 'heights' (| W3D_LIQ_PERCELL)
            size_t perCell = 0;
            for (auto& [ci, c] : grid) {
                if (ci < 0 || ci >= 256) continue;
                float lo = 1e30f, hi = -1e30f, shared[81];
                bool set[81] = {}, agree = true;
                for (int cell = 0; cell < 64; ++cell) {
                    if (c.cell[cell] == W3D_LIQ_DRY) continue;
                    for (int q = 0; q < 4; ++q) {
                        const float hv = c.h[cell][q];
                        const int v = cornerVertex(cell / 8, cell % 8, q);
                        lo = std::min(lo, hv); hi = std::max(hi, hv);
                        if (!set[v]) { set[v] = true; shared[v] = hv; }
                        else if (std::fabs(shared[v] - hv) > 1.0f / 64.0f) agree = false;
                    }
                }
                if (lo > hi) continue;
                W3dLiquidChunk rec{};
                std::memcpy(rec.cell, c.cell, 64);
                rec.base = lo;
                index[ci] = (uint16_t)recs.size();
                auto q16 = [&](float hv) { return (int16_t)std::clamp((int)std::lround((hv - lo) * 32.0f), 0, 32767); };
                if (hi - lo < 1.0f / 64.0f) {
                    recHeights.push_back(W3D_LIQ_FLAT);
                } else if (agree) {
                    recHeights.push_back((uint32_t)heights.size());
                    for (int v = 0; v < 81; ++v) heights.push_back(set[v] ? q16(shared[v]) : 0);
                } else {
                    recHeights.push_back((uint32_t)heights.size() | W3D_LIQ_PERCELL);
                    for (int cell = 0; cell < 64; ++cell)
                        for (int q = 0; q < 4; ++q) heights.push_back(c.cell[cell] == W3D_LIQ_DRY ? 0 : q16(c.h[cell][q]));
                    ++perCell;
                }
                recs.push_back(rec);
            }
            if (!recs.empty()) {
                W3dLiquidHeader h{};
                h.tileX = (uint32_t)tx; h.tileY = (uint32_t)ty;
                h.maxX = maxX; h.maxY = maxY;
                h.chunkCount = (uint32_t)recs.size();
                for (int k = 0; k < 4; ++k) h.look[k] = look[k];
                Blob b;
                b.put(h);
                b.align(4);
                size_t offIdx = b.putArray(index);
                b.align(4);
                size_t offRec = b.putArray(recs);
                b.align(4);
                size_t offH = b.putArray(heights);
                b.align(4);
                auto* hp = b.at<W3dLiquidHeader>(0);
                hp->offChunkIndex = (uint32_t)offIdx;
                hp->offChunks = (uint32_t)offRec;
                auto* rp = b.at<W3dLiquidChunk>(offRec);
                for (size_t i = 0; i < recs.size(); ++i) {
                    if (recHeights[i] == W3D_LIQ_FLAT) { rp[i].offHeights = W3D_LIQ_FLAT; continue; }
                    rp[i].offHeights = (uint32_t)(offH + (recHeights[i] & ~W3D_LIQ_PERCELL) * 2) | (recHeights[i] & W3D_LIQ_PERCELL);
                }
                st.liquidBytes += b.data.size();
                if (o.verbose && perCell) std::printf("  liquids tile %d_%d: %zu chunks with surfaces meeting (heights per cell)\n", tx, ty, perCell);
                pack.add(W3D_ENTRY_LIQUID, tid, std::move(b.data));
            }
        }

        // ---- WATR: merged quads per (4x4-chunk region, liquid)
        std::vector<MeshV> verts;
        std::map<std::pair<int, int>, std::vector<uint32_t>> lists;
        auto quad = [&](int region, int liquid, const float p[4][3], const uint8_t col[4][4]) {
            uint32_t base = (uint32_t)verts.size();
            for (int k = 0; k < 4; ++k) {
                MeshV v;
                v.p[0] = p[k][0] - minX; v.p[1] = p[k][1] - minY; v.p[2] = p[k][2];
                std::memcpy(v.c, col[k], 4);
                verts.push_back(v);
            }
            auto& l = lists[{region, liquid}];
            const uint32_t t[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
            for (int i = 0; i < 6; i += 3) {   // CCW from above (the renderer draws water two-sided anyway)
                const float* a = verts[t[i]].p; const float* b = verts[t[i + 1]].p; const float* c = verts[t[i + 2]].p;
                float cz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                if (cz < 0) { l.push_back(t[i]); l.push_back(t[i + 2]); l.push_back(t[i + 1]); }
                else l.insert(l.end(), t + i, t + i + 3);
            }
        };
        float tint[4] = {1, 1, 1, 1};
        {
            bool used[4] = {false, false, false, false};
            for (auto& [ci, c] : tl.chunks) for (uint8_t f : c.cell) if (f != W3D_LIQ_DRY) used[f & W3D_LIQ_KIND] = true;
            for (const auto& q : tl.quads) used[q.kind & 3] = true;
            for (int k = 0; k < 4; ++k) if (used[k]) tint[k] = liquidFrames(o, data, pack, k, st).tint;
        }
        for (auto& [ci, c] : tl.chunks) {
            const int cy = ci / 16, cx = ci % 16, region = (cy / 4) * 4 + cx / 4;
            // corner colours per cell, then greedy rectangles over cells of one liquid, flat at one height, one colour
            uint8_t col[64][4][4];
            for (int cell = 0; cell < 64; ++cell) {
                if (c.cell[cell] == W3D_LIQ_DRY) continue;
                const int kind = c.cell[cell] & W3D_LIQ_KIND;
                for (int q = 0; q < 4; ++q) shade(look[kind], kind, c.depth[cell][q], tint[kind], col[cell][q]);
            }
            bool done[64] = {};
            auto uniform = [&](int cell) -> bool {   // flat, one colour: it can merge with its like
                if (c.cell[cell] == W3D_LIQ_DRY) return false;
                for (int q = 1; q < 4; ++q)
                    if (c.h[cell][q] != c.h[cell][0] || std::memcmp(col[cell][q], col[cell][0], 4)) return false;
                return true;
            };
            for (int r = 0; r < 8; ++r)
                for (int k = 0; k < 8; ++k) {
                    const int cell = r * 8 + k;
                    if (done[cell] || c.cell[cell] == W3D_LIQ_DRY) continue;
                    const int kind = c.cell[cell] & W3D_LIQ_KIND;
                    int r1 = r + 1, k1 = k + 1;
                    if (uniform(cell)) {
                        auto same = [&](int rr, int kk) {
                            const int o2 = rr * 8 + kk;
                            return !done[o2] && uniform(o2) && (c.cell[o2] & W3D_LIQ_KIND) == kind && c.h[o2][0] == c.h[cell][0] &&
                                   !std::memcmp(col[o2][0], col[cell][0], 4);
                        };
                        while (k1 < 8 && same(r, k1)) ++k1;
                        bool grow = true;
                        while (grow && r1 < 8) {
                            for (int kk = k; kk < k1 && grow; ++kk) grow = same(r1, kk);
                            if (grow) ++r1;
                        }
                    }
                    for (int rr = r; rr < r1; ++rr)
                        for (int kk = k; kk < k1; ++kk) done[rr * 8 + kk] = true;
                    // corners of the rectangle (r,k) (r,k1) (r1,k1) (r1,k), each from the cell it belongs to
                    const int vr[4] = {r, r, r1, r1}, vk[4] = {k, k1, k1, k};
                    const int fromCell[4] = {r * 8 + k, r * 8 + k1 - 1, (r1 - 1) * 8 + k1 - 1, (r1 - 1) * 8 + k};
                    const int fromQ[4] = {0, 1, 3, 2};
                    float p[4][3];
                    uint8_t cc[4][4];
                    for (int q = 0; q < 4; ++q) {
                        p[q][0] = maxX - (cy * 8 + vr[q]) * kCell;
                        p[q][1] = maxY - (cx * 8 + vk[q]) * kCell;
                        p[q][2] = c.h[fromCell[q]][fromQ[q]];
                        std::memcpy(cc[q], col[fromCell[q]][fromQ[q]], 4);
                    }
                    quad(region, kind, p, cc);
                }
        }
        for (const auto& q : tl.quads) {
            float mx = 0, my = 0;
            for (auto& p : q.p) { mx += 0.25f * p[0]; my += 0.25f * p[1]; }
            const int gx = std::clamp((int)((maxX - mx) / kCell), 0, 127), gy = std::clamp((int)((maxY - my) / kCell), 0, 127);
            const int region = (gx / 32) * 4 + gy / 32;
            uint8_t cc[4][4];
            for (int k = 0; k < 4; ++k) shade(look[q.kind], q.kind, 3.5f, tint[q.kind], cc[k]);
            quad(region, q.kind, q.p, cc);
        }
        if (verts.empty() || verts.size() > 65535) {
            if (verts.size() > 65535) std::fprintf(stderr, "  liquids of tile %d_%d: %zu vertices, too many (skipped)\n", tx, ty, verts.size());
            continue;
        }
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (auto& v : verts)
            for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], v.p[c]); hi[c] = std::max(hi[c], v.p[c]); }
        W3dWaterHeader h{};
        h.tileX = (uint32_t)tx; h.tileY = (uint32_t)ty;
        h.origin[0] = minX; h.origin[1] = minY; h.origin[2] = lo[2];
        for (int c = 0; c < 3; ++c) {
            float l = c == 2 ? 0.0f : lo[c], u = c == 2 ? hi[2] - lo[2] : hi[c];
            float range = std::max(u - l, 1.0f);
            h.posOffset[c] = 0.5f * (l + u);
            h.posScale[c] = range * 0.5f / 32767.0f;
        }
        std::vector<uint32_t> texIds;
        std::vector<W3dWaterBatch> batches;
        std::vector<uint16_t> indices;
        std::map<int, uint16_t> firstTex;
        for (auto& [key, l] : lists) {
            if (l.empty()) continue;
            const int liquid = key.second;
            const Frames& fr = liquidFrames(o, data, pack, liquid, st);
            if (!firstTex.count(liquid)) {
                firstTex[liquid] = (uint16_t)texIds.size();
                texIds.insert(texIds.end(), fr.ids.begin(), fr.ids.end());
            }
            W3dWaterBatch b{};
            b.liquid = (uint8_t)liquid;
            b.frames = (uint8_t)(fr.ids.size() > 1 ? fr.ids.size() : 0);
            b.tex = firstTex[liquid];
            b.firstIndex = (uint32_t)indices.size();
            b.indexCount = (uint32_t)l.size();
            for (int c = 0; c < 3; ++c) { b.bmin[c] = 1e30f; b.bmax[c] = -1e30f; }
            for (uint32_t i : l) {
                indices.push_back((uint16_t)i);
                for (int c = 0; c < 3; ++c) {
                    float v = c == 2 ? verts[i].p[2] - lo[2] : verts[i].p[c];
                    b.bmin[c] = std::min(b.bmin[c], v); b.bmax[c] = std::max(b.bmax[c], v);
                }
            }
            batches.push_back(b);
        }
        std::vector<W3dTerrainVertex> qv(verts.size());
        for (size_t i = 0; i < verts.size(); ++i) {
            const MeshV& v = verts[i];
            auto q = [&](float val, int c) {
                return (int16_t)std::clamp((int)std::lround((val - h.posOffset[c]) / h.posScale[c]), -32767, 32767);
            };
            // world-aligned tiling: the same texel at the same place in every tile
            const float u = (TS - v.p[1]) / kUvRepeat, w = v.p[0] / kUvRepeat;
            qv[i] = {q(v.p[0], 0), q(v.p[1], 1), q(v.p[2] - lo[2], 2), 0, v.c[0], v.c[1], v.c[2], v.c[3],
                     (int16_t)std::clamp((int)std::lround(u / W3D_TERRAIN_UV_SCALE), -32767, 32767),
                     (int16_t)std::clamp((int)std::lround(w / W3D_TERRAIN_UV_SCALE), -32767, 32767)};
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
        pack.add(W3D_ENTRY_WATER, tid, std::move(blob.data));
        st.waterTris += indices.size() / 3;
        ++st.liquidTiles;
        if (o.verbose)
            std::printf("  liquids tile %d_%d: %zu chunks, %zu building cells, %zu triangles, colours %s\n", tx, ty, tl.chunks.size(),
                        tl.quads.size(), indices.size() / 3, fromDbc ? "Light.dbc" : "default");
    }
}

}  // namespace w3d
