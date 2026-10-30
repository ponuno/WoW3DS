// farlod.cpp — the far ground of a map (stage 6.24, common/w3d_farlod.h): the FARL entry and its colour atlas.
//
// collectFarTile() runs for every converted ADT: the heights at the corners of its 16 x 16 chunks (exactly the detailed
// ground's corners) and a colour per chunk - the ground textures' mean colours blended the way the game blends its layers
// (each layer over the ones before it by its alpha), with the alpha maps' mean coverage.
// emitFarLod() runs once per map after its tiles: the map's WDL gives the coarse heights of every other tile (a region
// conversion still gets a far ground all around it); colours of those tiles are the mean of the converted ones.
#include "conv.h"

#include "w3d_farlod.h"
#include "pipeline/adt_alpha.hpp"
#include "pipeline/adt_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace w3d {

std::vector<uint8_t> encodeTextureLevel(const Image& im, uint8_t fmt);   // texture.cpp

namespace {

constexpr float kTS = 533.33333f;
constexpr float kZScale = 0.25f;   // yards per height unit: +-8192 yards

// Mean colour of a game texture (decoded once per path for the whole run).
bool meanColour(const DataDir* data, const std::string& path, float rgb[3]) {
    static std::mutex m;
    static std::unordered_map<std::string, std::array<float, 4>> cache;   // [3] = 1 ok / 0 failed
    const std::string key = DataDir::normalize(path);
    {
        std::lock_guard<std::mutex> l(m);
        auto it = cache.find(key);
        if (it != cache.end()) {
            for (int k = 0; k < 3; ++k) rgb[k] = it->second[k];
            return it->second[3] > 0.5f;
        }
    }
    std::array<float, 4> v{0.45f, 0.42f, 0.35f, 0.0f};
    std::vector<uint8_t> bytes;
    Image img;
    if (data && data->read(path, bytes) && decodeBlp(bytes, img) && img.w > 0 && img.h > 0) {
        double s[3] = {0, 0, 0};
        const size_t n = (size_t)img.w * img.h;
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) s[k] += img.rgba[i * 4 + k];
        for (int k = 0; k < 3; ++k) v[k] = (float)(s[k] / (double)n / 255.0);
        v[3] = 1.0f;
    }
    std::lock_guard<std::mutex> l(m);
    cache[key] = v;
    for (int k = 0; k < 3; ++k) rgb[k] = v[k];
    return v[3] > 0.5f;
}

// The map's WDL: per tile 17 x 17 outer heights (int16, yards). Returns the tiles found ([tileY * 64 + tileX] -> heights).
// MAOF holds 64 x 64 file offsets of MARE chunks; which index is which tile and which way the 17 x 17 grid runs is checked
// against the converted ADTs (see emitFarLod), so both readings are kept.
bool readWdl(const DataDir* data, const std::string& map, std::vector<std::array<int16_t, 289>>& grids, std::vector<int>& at) {
    if (!data) return false;
    std::vector<uint8_t> b;
    if (!data->read("World\\Maps\\" + map + "\\" + map + ".wdl", b)) return false;
    at.assign(4096, -1);
    size_t p = 0;
    const uint8_t* maof = nullptr;
    while (p + 8 <= b.size()) {
        uint32_t magic, size;
        std::memcpy(&magic, &b[p], 4);
        std::memcpy(&size, &b[p + 4], 4);
        if (p + 8 + (size_t)size > b.size()) break;
        if (magic == 0x4D414F46u /* "MAOF" stored reversed: 'F','O','A','M' */ && size >= 4096 * 4) { maof = &b[p + 8]; break; }
        p += 8 + size;
    }
    if (!maof) return false;
    for (int k = 0; k < 4096; ++k) {
        uint32_t off;
        std::memcpy(&off, maof + k * 4, 4);
        if (!off || (size_t)off + 8 + 545 * 2 > b.size()) continue;
        uint32_t magic;
        std::memcpy(&magic, &b[off], 4);
        if (magic != 0x4D415245u /* "MARE" */) continue;
        std::array<int16_t, 289> g;
        std::memcpy(g.data(), &b[off + 8], 289 * 2);
        at[k] = (int)grids.size();
        grids.push_back(g);
    }
    return !grids.empty();
}

}  // namespace

bool farMeanColour(const DataDir* data, const std::string& path, float rgb[3]) { return meanColour(data, path, rgb); }

void collectFarTile(const Options& o, const DataDir* data, const wowee::pipeline::ADTTerrain& adt, ConvStats& st) {
    ConvStats::FarSrc f;
    bool any = false;
    for (int i = 0; i < 17; ++i)
        for (int j = 0; j < 17; ++j) {
            // corner (i, j) = chunk (cx = min(j, 15), cy = min(i, 15)), its corner row / column 0 or 8
            const int cy = std::min(i, 15), cx = std::min(j, 15), r = i == 16 ? 8 : 0, c = j == 16 ? 8 : 0;
            const auto& ch = adt.getChunk(cx, cy);
            float z = 0;
            if (ch.hasHeightMap()) {
                z = ch.position[2] + ch.heightMap.heights[r * 17 + c];
                any = true;
            }
            f.h[i * 17 + j] = std::isfinite(z) ? z : 0.0f;
        }
    if (!any) return;
    {   // corners of chunks without a height map take the mean of those that have one (no far ground dropping to z 0)
        double s = 0;
        int k = 0;
        bool has[289];
        for (int i = 0; i < 17; ++i)
            for (int j = 0; j < 17; ++j) {
                has[i * 17 + j] = adt.getChunk(std::min(j, 15), std::min(i, 15)).hasHeightMap();
                if (has[i * 17 + j]) { s += f.h[i * 17 + j]; ++k; }
            }
        for (int q = 0; q < 289; ++q)
            if (!has[q]) f.h[q] = (float)(s / k);
    }
    int colours = 0;
    std::vector<uint8_t> alpha;
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            const auto& ch = adt.getChunk(cx, cy);
            float rgb[3] = {0.45f, 0.42f, 0.35f};
            bool have = false;
            for (size_t li = 0; li < ch.layers.size(); ++li) {
                const uint32_t ti = ch.layers[li].textureId;
                if (ti >= adt.textures.size()) continue;
                float tc[3];
                if (!meanColour(data, adt.textures[ti], tc)) continue;
                float a = 1.0f;
                if (li > 0) {
                    if (!wowee::pipeline::decodeLayerAlpha(ch, li, alpha)) continue;
                    double s = 0;
                    for (uint8_t v : alpha) s += v;
                    a = alpha.empty() ? 0.0f : (float)(s / (double)alpha.size() / 255.0);
                }
                if (!have) { for (int k = 0; k < 3; ++k) rgb[k] = tc[k]; have = true; continue; }
                for (int k = 0; k < 3; ++k) rgb[k] = rgb[k] * (1.0f - a) + tc[k] * a;
            }
            if (have) ++colours;
            for (int k = 0; k < 3; ++k) f.rgb[cy * 16 + cx][k] = rgb[k];
        }
    f.haveColor = colours > 0;
    st.far[w3d_tile_id(o.tileX, o.tileY)] = f;
}

void emitFarLod(const Options& o, const DataDir* data, PackWriter& pack, ConvStats& st) {
    if (o.noFarLod) { st.far.clear(); return; }
    if (st.far.empty()) return;   // no ADT converted: a dungeon (WMO-only map) or nothing - no far ground
    std::map<int, ConvStats::FarSrc> tiles;   // [tileY * 64 + tileX]
    for (auto& [tid, f] : st.far) {
        const int tx = (int)(tid / 64u), ty = (int)(tid % 64u);   // (w3d_tile_id = tileX * 64 + tileY)
        if (tx < 0 || ty < 0 || tx > 63 || ty > 63) continue;
        tiles[ty * 64 + tx] = f;
    }
    st.far.clear();
    // the converted tiles' mean colour, for the tiles known from the WDL only
    double mean[3] = {0, 0, 0};
    size_t meanN = 0;
    for (auto& [k, f] : tiles)
        if (f.haveColor)
            for (int c = 0; c < 256; ++c) {
                for (int q = 0; q < 3; ++q) mean[q] += f.rgb[c][q];
                ++meanN;
            }
    float meanRgb[3] = {0.42f, 0.40f, 0.33f};
    if (meanN)
        for (int q = 0; q < 3; ++q) meanRgb[q] = (float)(mean[q] / (double)meanN);

    // the WDL: the other tiles of the map. Four readings (tile index [y][x] or [x][y], grid rows along ADT rows or columns);
    // the one that agrees best with the converted ADTs at their chunk corners is used (else the plain one).
    std::vector<std::array<int16_t, 289>> grids;
    std::vector<int> at;
    size_t wdlAdded = 0;
    if (readWdl(data, o.map, grids, at)) {
        double err[4] = {0, 0, 0, 0};
        int n[4] = {0, 0, 0, 0};
        for (auto& [k, f] : tiles) {
            const int tx = k % 64, ty = k / 64;
            for (int mode = 0; mode < 4; ++mode) {
                const int idx = (mode & 1) ? tx * 64 + ty : ty * 64 + tx;
                if (at[idx] < 0) continue;
                const auto& g = grids[at[idx]];
                for (int i = 0; i < 17; ++i)
                    for (int j = 0; j < 17; ++j) {
                        const int s = (mode & 2) ? j * 17 + i : i * 17 + j;
                        err[mode] += std::fabs((double)g[s] - f.h[i * 17 + j]);
                        ++n[mode];
                    }
            }
        }
        int best = 0;
        double bestErr = 1e30;
        for (int mode = 0; mode < 4; ++mode)
            if (n[mode] && err[mode] / n[mode] < bestErr) { bestErr = err[mode] / n[mode]; best = mode; }
        if (o.verbose || n[best])
            std::printf("  far ground: WDL %zu tiles, reading %d (%s index, %s grid), mean difference to the ADT corners %.2f yd over %d points\n",
                        grids.size(), best, (best & 1) ? "[x][y]" : "[y][x]", (best & 2) ? "transposed" : "plain",
                        n[best] ? bestErr : 0.0, n[best]);
        for (int ty = 0; ty < 64; ++ty)
            for (int tx = 0; tx < 64; ++tx) {
                const int k = ty * 64 + tx;
                if (tiles.count(k)) continue;
                const int idx = (best & 1) ? tx * 64 + ty : ty * 64 + tx;
                if (at[idx] < 0) continue;
                ConvStats::FarSrc f;
                const auto& g = grids[at[idx]];
                for (int i = 0; i < 17; ++i)
                    for (int j = 0; j < 17; ++j) f.h[i * 17 + j] = (float)g[(best & 2) ? j * 17 + i : i * 17 + j];
                for (int c = 0; c < 256; ++c)
                    for (int q = 0; q < 3; ++q) f.rgb[c][q] = meanRgb[q];
                f.haveColor = false;
                tiles[k] = f;
                ++wdlAdded;
            }
    }
    if (tiles.empty()) return;

    // the colour atlas: 16 x 16 texels per tile, 32 tiles a row
    const int n = (int)tiles.size();
    const int rows = (n + 31) / 32;
    int atlasH = 16;
    while (atlasH < rows * W3D_FAR_CELL) atlasH *= 2;
    const int atlasW = 512;
    if (atlasH > 1024) {
        std::fprintf(stderr, "  far ground: %d tiles do not fit the atlas, skipped\n", n);
        return;
    }
    Image img;
    img.w = atlasW;
    img.h = atlasH;
    img.rgba.assign((size_t)atlasW * atlasH * 4, 255);
    std::vector<uint8_t> blob(sizeof(W3dFarHeader) + (size_t)n * sizeof(W3dFarTile), 0);
    W3dFarHeader* hd = (W3dFarHeader*)blob.data();
    hd->magic = W3D_FAR_MAGIC;
    hd->tileCount = (uint32_t)n;
    hd->atlasW = (uint16_t)atlasW;
    hd->atlasH = (uint16_t)atlasH;
    hd->zScale = kZScale;
    W3dFarTile* rec = (W3dFarTile*)(blob.data() + sizeof(W3dFarHeader));
    int r = 0;
    for (auto& [k, f] : tiles) {
        W3dFarTile& t = rec[r];
        t.tileX = (uint8_t)(k % 64);
        t.tileY = (uint8_t)(k / 64);
        t.source = f.haveColor ? W3D_FAR_FROM_ADT : W3D_FAR_FROM_WDL;
        t.cellX = (uint16_t)((r % 32) * W3D_FAR_CELL);
        t.cellY = (uint16_t)((r / 32) * W3D_FAR_CELL);
        for (int i = 0; i < 289; ++i)
            t.h[i] = (int16_t)std::clamp((long)std::lround(f.h[i] / kZScale), -32767L, 32767L);
        for (int cy = 0; cy < 16; ++cy)
            for (int cx = 0; cx < 16; ++cx) {
                uint8_t* px = &img.rgba[((size_t)(t.cellY + cy) * atlasW + t.cellX + cx) * 4];
                for (int q = 0; q < 3; ++q) px[q] = (uint8_t)std::clamp((int)std::lround(f.rgb[cy * 16 + cx][q] * 255.0f), 0, 255);
                px[3] = 255;
            }
        hd->index[k] = (uint16_t)(r + 1);
        ++r;
    }
    const uint32_t texId = pack.keyId(W3D_ENTRY_TEXTURE, "farlod|" + DataDir::normalize(o.map) + "|" + std::to_string(n));
    hd->texId = texId;
    pack.add(W3D_ENTRY_TEXTURE, texId, encodeTextureLevel(img, W3D_TEX_ETC1));
    st.farTiles += (size_t)n;
    st.farFromWdl += wdlAdded;
    st.farBytes += blob.size() + (size_t)atlasW * atlasH / 2;
    std::printf("  far ground: %d tiles (%zu from the WDL), atlas %dx%d, %.1f KB\n", n, wdlAdded, atlasW, atlasH,
                (blob.size() + (size_t)atlasW * atlasH / 2) / 1024.0);
    pack.add(W3D_ENTRY_FARLOD, 0, std::move(blob));
}

}  // namespace w3d
