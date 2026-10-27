// w3dpreview — decodes every texture of a pack from PICA layout back to PNG
// and renders a top-down shaded view of the terrain with placements, so the
// converter output can be eyeballed without a 3DS.
#include "w3d_format.h"
#include "w3d_anim.h"
#include "w3d_character.h"
#include "w3d_liquid.h"
#include "rg_etc1.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static std::vector<uint8_t> decodeLevel0(const W3dTexHeader& t, const uint8_t* d) {
    int w = t.width, h = t.height;
    std::vector<uint8_t> rgba((size_t)w * h * 4, 255);
    if (t.format == W3D_TEX_ETC1 || t.format == W3D_TEX_ETC1A4) {
        bool a4 = t.format == W3D_TEX_ETC1A4;
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < w; tx += 8)
                for (int by = 0; by < 8; by += 4)
                    for (int bx = 0; bx < 8; bx += 4) {
                        uint8_t alpha[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                        if (a4) { std::memcpy(alpha, d, 8); d += 8; }
                        uint8_t blk[8];
                        for (int i = 0; i < 8; ++i) blk[i] = d[7 - i];
                        d += 8;
                        uint32_t px[16];
                        rg_etc1::unpack_etc1_block(blk, px);
                        for (int y = 0; y < 4; ++y)
                            for (int x = 0; x < 4; ++x) {
                                uint8_t* o = &rgba[((size_t)(ty + by + y) * w + tx + bx + x) * 4];
                                std::memcpy(o, &px[y * 4 + x], 3);
                                uint8_t nib = alpha[2 * x + y / 2];
                                o[3] = (uint8_t)(((y & 1) ? nib >> 4 : nib & 15) * 17);
                            }
                    }
        return rgba;
    }
    const uint16_t* s = reinterpret_cast<const uint16_t*>(d);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint16_t v = s[((size_t)(y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)];
            uint8_t* o = &rgba[((size_t)y * w + x) * 4];
            if (t.format == W3D_TEX_RGBA4) {
                o[0] = (v >> 12) * 17; o[1] = ((v >> 8) & 15) * 17; o[2] = ((v >> 4) & 15) * 17; o[3] = (v & 15) * 17;
            } else if (t.format == W3D_TEX_RGBA5551) {
                o[0] = (v >> 11) << 3; o[1] = ((v >> 6) & 31) << 3; o[2] = ((v >> 1) & 31) << 3; o[3] = (v & 1) * 255;
            } else {
                o[0] = (v >> 11) << 3; o[1] = ((v >> 5) & 63) << 2; o[2] = (v & 31) << 3;
            }
        }
    return rgba;
}


// ---- creature preview: CPU skinning (w3d_anim.h, like the 3DS) + a tiny z-buffered rasteriser.
// Writes <out>/creature_<display>.png: one row per kept animation, 8 frames each, side view.
// playerDisp/playerBody: a player (stage 5.5) instead of a CDSP display: its display record and
// its composited 128x128 RGBA body texture.
static int previewCreature(const std::vector<uint8_t>& b, const W3dEntry* e, uint32_t n, uint32_t displayId,
                           const std::string& outDir, const W3dDisplay* playerDisp = nullptr,
                           const std::vector<uint8_t>* playerBody = nullptr, const std::string& playerName = "") {
    auto find = [&](uint32_t type, uint32_t id) -> const W3dEntry* {
        for (uint32_t i = 0; i < n; ++i) if (e[i].type == type && e[i].id == id) return &e[i];
        return nullptr;
    };
    W3dDisplay d;
    if (playerDisp) {
        d = *playerDisp;
    } else {
        const W3dEntry* de = find(W3D_ENTRY_DISPLAY, displayId);
        if (!de) { std::printf("display %u not in pack\n", displayId); return 1; }
        std::memcpy(&d, b.data() + de->offset, sizeof d);
    }
    const W3dEntry* me = find(W3D_ENTRY_CREATURE, d.modelId);
    if (!me) { std::printf("model %08x not in pack\n", d.modelId); return 1; }
    const uint8_t* blob = b.data() + me->offset;
    const W3dCreatureHeader* h = w3d_creature(blob);
    const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(blob + h->offSubmeshes);
    const W3dBonePalette* pal = (const W3dBonePalette*)(blob + h->offPalettes);
    const W3dSkinVertex* vx = (const W3dSkinVertex*)(blob + h->offVertices);
    const uint16_t* ix = (const uint16_t*)(blob + h->offIndices);
    const W3dCreatureTex* ct = (const W3dCreatureTex*)(blob + h->offTex);
    const W3dAnimSeq* sq = (const W3dAnimSeq*)(blob + h->offSeqs);
    // textures per slot
    struct Img { int w = 0, h = 0; std::vector<uint8_t> rgba; };
    std::vector<Img> slotImg(h->texCount);
    for (uint32_t s = 0; s < h->texCount; ++s) {
        if (playerBody && ct[s].kind == W3D_CTEX_BODY) {
            slotImg[s].w = slotImg[s].h = W3D_CHAR_ATLAS;
            slotImg[s].rgba = *playerBody;
            continue;
        }
        uint32_t id = ct[s].kind == W3D_CTEX_FIXED ? ct[s].texId : (ct[s].kind >= 1 && ct[s].kind <= 5 ? d.tex[ct[s].kind - 1] : 0);
        const W3dEntry* te = id ? find(W3D_ENTRY_TEXTURE, id) : nullptr;
        if (!te) continue;
        W3dTexHeader th;
        std::memcpy(&th, b.data() + te->offset, sizeof th);
        slotImg[s].w = th.width; slotImg[s].h = th.height;
        slotImg[s].rgba = decodeLevel0(th, b.data() + te->offset + W3D_TEX_DATA_OFFSET);
    }
    auto geosetOn = [&](uint16_t g) {
        if (!d.geosetCount) return true;
        for (int k = 0; k < d.geosetCount; ++k) if (d.geosets[k] == g) return true;
        return false;
    };
    const int cell = h->seqCount > 24 ? 112 : 192, cols = 8;   // the animation stage keeps many: smaller cells
    int rows = std::max<int>(1, (int)h->seqCount);
    std::vector<uint8_t> img((size_t)cell * cols * cell * rows * 3, 0);
    std::vector<float> zb((size_t)cell * cols * cell * rows);
    for (size_t i = 0; i < img.size() / 3; ++i) { img[i * 3] = 60; img[i * 3 + 1] = 70; img[i * 3 + 2] = 90; }
    std::fill(zb.begin(), zb.end(), -1e30f);
    // fit: bind-pose bounds * display scale, a bit of room for animation
    float ctr[3], ext = 0;
    for (int c = 0; c < 3; ++c) {
        ctr[c] = 0.5f * (h->bmin[c] + h->bmax[c]);
        ext = std::max(ext, 0.5f * (h->bmax[c] - h->bmin[c]));
    }
    float px = cell * 0.34f / std::max(ext, 0.01f);
    const float ca = std::cos(0.7f), sa = std::sin(0.7f);   // 3/4 view: yaw 40 degrees
    std::vector<W3dMat34> pose(h->boneCount);
    const float L[3] = {0.45f, -0.6f, 0.66f};
    for (int r = 0; r < rows; ++r) {
        for (int f = 0; f < cols; ++f) {
            float t = h->seqCount ? sq[r].durationMs * (float)f / cols : 0;
            w3d_anim_pose(blob, h->seqCount ? r : -1, t, sq[r].animId != W3D_ANIM_DEATH, pose.data());
            int ox = f * cell, oy = r * cell;
            for (uint32_t s = 0; s < h->submeshCount; ++s) {
                if (!geosetOn(sm[s].geoset)) continue;
                const W3dBonePalette& pp = pal[sm[s].palette];
                const Img* ti = sm[s].texSlot < h->texCount && !slotImg[sm[s].texSlot].rgba.empty() ? &slotImg[sm[s].texSlot] : nullptr;
                for (uint32_t q = sm[s].firstIndex; q + 2 < sm[s].firstIndex + sm[s].indexCount; q += 3) {
                    float P[3][3], N[3][3], UV[3][2];
                    for (int k = 0; k < 3; ++k) {
                        const W3dSkinVertex& v = vx[ix[q + k]];
                        float bp[3] = {v.x * h->posScale[0] + h->posOffset[0], v.y * h->posScale[1] + h->posOffset[1],
                                       v.z * h->posScale[2] + h->posOffset[2]};
                        float bn[3] = {v.nx / 127.0f, v.ny / 127.0f, v.nz / 127.0f};
                        for (int c = 0; c < 3; ++c) { P[k][c] = 0; N[k][c] = 0; }
                        for (int j = 0; j < 2; ++j) {
                            float w = v.weight[j] / 255.0f;
                            if (w == 0) continue;
                            const float* M = pose[pp.bones[v.bone[j] / 3]].m;
                            for (int c = 0; c < 3; ++c) {
                                P[k][c] += w * (M[c * 4] * bp[0] + M[c * 4 + 1] * bp[1] + M[c * 4 + 2] * bp[2] + M[c * 4 + 3]);
                                N[k][c] += w * (M[c * 4] * bn[0] + M[c * 4 + 1] * bn[1] + M[c * 4 + 2] * bn[2]);
                            }
                        }
                        UV[k][0] = v.u * W3D_MODEL_UV_SCALE; UV[k][1] = v.v * W3D_MODEL_UV_SCALE;
                    }
                    // side view: image x = model x, image y = -(z - centre), depth = -y (camera on -y)
                    float sx[3], sy[3], sz[3];
                    for (int k = 0; k < 3; ++k) {
                        float rx = P[k][0] - ctr[0], ry = P[k][1] - ctr[1];
                        float vx = ca * rx - sa * ry, vy = sa * rx + ca * ry;
                        sx[k] = ox + cell * 0.5f + vx * px;
                        sy[k] = oy + cell * 0.5f - (P[k][2] - ctr[2]) * px;
                        sz[k] = -vy;
                    }
                    float den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
                    if (std::fabs(den) < 1e-6f) continue;
                    int x0 = std::max(ox, (int)std::floor(std::min({sx[0], sx[1], sx[2]})));
                    int x1 = std::min(ox + cell - 1, (int)std::ceil(std::max({sx[0], sx[1], sx[2]})));
                    int y0 = std::max(oy, (int)std::floor(std::min({sy[0], sy[1], sy[2]})));
                    int y1 = std::min(oy + cell - 1, (int)std::ceil(std::max({sy[0], sy[1], sy[2]})));
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x) {
                            float qx = x + 0.5f, qy = y + 0.5f;
                            float w0 = ((sy[1] - sy[2]) * (qx - sx[2]) + (sx[2] - sx[1]) * (qy - sy[2])) / den;
                            float w1 = ((sy[2] - sy[0]) * (qx - sx[2]) + (sx[0] - sx[2]) * (qy - sy[2])) / den;
                            float w2 = 1 - w0 - w1;
                            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                            float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                            size_t pi = (size_t)y * cell * cols + x;
                            if (z < zb[pi]) continue;
                            float col[3] = {200, 200, 200}, a = 255;
                            if (ti) {
                                float u = w0 * UV[0][0] + w1 * UV[1][0] + w2 * UV[2][0];
                                float vv = w0 * UV[0][1] + w1 * UV[1][1] + w2 * UV[2][1];
                                int tx = ((int)std::floor(u * ti->w) % ti->w + ti->w) % ti->w;
                                int ty = ((int)std::floor((1.0f - vv) * ti->h) % ti->h + ti->h) % ti->h;
                                const uint8_t* c = &ti->rgba[((size_t)ty * ti->w + tx) * 4];
                                col[0] = c[0]; col[1] = c[1]; col[2] = c[2]; a = c[3];
                            }
                            if (sm[s].blend == W3D_BLEND_ALPHAKEY && a < 128) continue;
                            float nn[3];
                            for (int c = 0; c < 3; ++c) nn[c] = w0 * N[0][c] + w1 * N[1][c] + w2 * N[2][c];
                            float nl = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]) + 1e-6f;
                            float lit = 0.45f + 0.55f * std::max(0.0f, (nn[0] * L[0] + nn[1] * L[1] + nn[2] * L[2]) / nl);
                            zb[pi] = z;
                            for (int c = 0; c < 3; ++c) img[pi * 3 + c] = (uint8_t)std::min(255.0f, col[c] * lit);
                        }
                }
            }
        }
    }
    std::string name = outDir + (playerDisp ? "/" + playerName + ".png" : "/creature_" + std::to_string(displayId) + ".png");
    stbi_write_png(name.c_str(), cell * cols, cell * rows, 3, img.data(), cell * cols * 3);
    std::printf("wrote %s: %u bones, %u submeshes, animations:", name.c_str(), h->boneCount, h->submeshCount);
    for (uint32_t s = 0; s < h->seqCount; ++s) std::printf(" %u(%u ms)", sq[s].animId, sq[s].durationMs);
    std::printf("\n");
    return 0;
}

static int previewLod = 0;  // which terrain LOD to draw (3rd argument)

// Rasterises tile-local triangles into the region image (north up, west left).
struct Canvas {
    int W, H, px;              // px per tile
    float x0, y0;              // render-space min of the region
    std::vector<uint8_t> rgb;
    void tri(const float P[3][2], const uint8_t col[3][3], float alpha) {
        // P = render x,y; image u = (maxY - y), v = (maxX - x) ... handled by caller
        int x0i = std::max(0, (int)std::floor(std::min({P[0][0], P[1][0], P[2][0]})));
        int x1i = std::min(W - 1, (int)std::ceil(std::max({P[0][0], P[1][0], P[2][0]})));
        int y0i = std::max(0, (int)std::floor(std::min({P[0][1], P[1][1], P[2][1]})));
        int y1i = std::min(H - 1, (int)std::ceil(std::max({P[0][1], P[1][1], P[2][1]})));
        float den = (P[1][1] - P[2][1]) * (P[0][0] - P[2][0]) + (P[2][0] - P[1][0]) * (P[0][1] - P[2][1]);
        if (std::fabs(den) < 1e-6f) return;
        for (int y = y0i; y <= y1i; ++y)
            for (int x = x0i; x <= x1i; ++x) {
                float qx = x + 0.5f, qy = y + 0.5f;
                float w0 = ((P[1][1] - P[2][1]) * (qx - P[2][0]) + (P[2][0] - P[1][0]) * (qy - P[2][1])) / den;
                float w1 = ((P[2][1] - P[0][1]) * (qx - P[2][0]) + (P[0][0] - P[2][0]) * (qy - P[2][1])) / den;
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                uint8_t* o = &rgb[((size_t)y * W + x) * 3];
                for (int c = 0; c < 3; ++c) {
                    float v = w0 * col[0][c] + w1 * col[1][c] + w2 * col[2][c];
                    o[c] = (uint8_t)(o[c] * (1 - alpha) + v * alpha);
                }
            }
    }
};

// Collision of one tile seen from above, 2 px per yard (north up, like region.png):
// floors blue, building walls red, doodad walls orange; grid cells faint.
// Stage 5.27: a map pack of 'w3dconv --all' keeps its models and textures in the pool next to it (common*.w3dp); their
// blobs are read from there when a picture needs them.
struct PoolFiles {
    std::vector<std::string> files;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<int, W3dEntry>> index;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> cache;
    void open(const std::string& mapPack, const std::vector<uint8_t>& b, const W3dEntry* e, uint32_t n) {
        W3dPackInfo pi{};
        bool have = false;
        for (uint32_t i = 0; i < n; ++i)
            if (e[i].type == W3D_ENTRY_PACKINFO) { std::memcpy(&pi, b.data() + e[i].offset, std::min<size_t>(sizeof pi, e[i].size)); have = true; }
        if (!have || pi.kind != W3D_PACK_MAP) return;
        std::string dir = mapPack;
        size_t cut = dir.find_last_of("/\\");
        dir = cut == std::string::npos ? std::string() : dir.substr(0, cut + 1);
        int parts = 1;
        for (int k = 0; k < parts; ++k) {
            std::string p = dir + (k ? "common" + std::to_string(k) + ".w3dp" : std::string("common.w3dp"));
            std::ifstream g(p, std::ios::binary);
            W3dPackHeader gh;
            if (!g.read(reinterpret_cast<char*>(&gh), sizeof gh) || gh.magic != W3DP_MAGIC) { std::printf("note: %s missing\n", p.c_str()); return; }
            std::vector<W3dEntry> idx(gh.entryCount);
            g.seekg(gh.indexOffset);
            g.read(reinterpret_cast<char*>(idx.data()), (std::streamsize)(idx.size() * sizeof(W3dEntry)));
            for (const W3dEntry& x : idx) {
                if (x.type == W3D_ENTRY_PACKINFO && k == 0) {
                    W3dPackInfo ci{};
                    g.seekg(x.offset);
                    g.read(reinterpret_cast<char*>(&ci), std::min<size_t>(sizeof ci, x.size));
                    parts = std::max<int>(1, ci.partCount);
                }
                index[{x.type, x.id}] = {(int)files.size(), x};
            }
            files.push_back(p);
        }
        std::printf("pool: %zu parts, %zu entries\n", files.size(), index.size());
    }
    const uint8_t* blob(uint32_t type, uint32_t id) {
        auto c = cache.find({type, id});
        if (c != cache.end()) return c->second.data();
        auto it = index.find({type, id});
        if (it == index.end()) return nullptr;
        std::ifstream g(files[it->second.first], std::ios::binary);
        std::vector<uint8_t> v(it->second.second.size);
        g.seekg(it->second.second.offset);
        if (!g.read(reinterpret_cast<char*>(v.data()), (std::streamsize)v.size())) return nullptr;
        return cache.emplace(std::make_pair(type, id), std::move(v)).first->second.data();
    }
};
static PoolFiles g_pool;

// Stage 5.26: the liquid query grid of one tile, 4 px per cell, north up like region.png: water blue, sea dark blue,
// lava orange, slime green; deep cells darker, cells from buildings striped. Brightness follows the surface height.
static int previewLiquid(const std::vector<uint8_t>& b, const W3dEntry& e, const std::string& outDir) {
    const uint8_t* p = b.data() + e.offset;
    if (!w3d_liquid_valid(p, e.size)) { std::printf("LIQD %u: invalid\n", e.id); return 1; }
    W3dLiquidHeader h;
    std::memcpy(&h, p, sizeof h);
    const int S = 4, W = 128 * S;
    std::vector<uint8_t> img((size_t)W * W * 3, 0);
    float lo = 1e30f, hi = -1e30f;
    size_t wet = 0;
    std::vector<float> lev(128 * 128, NAN);
    std::vector<uint8_t> fl(128 * 128, W3D_LIQ_DRY);
    for (int gx = 0; gx < 128; ++gx)
        for (int gy = 0; gy < 128; ++gy) {
            W3dLiquidHit hit;
            float x = h.maxX - (gx + 0.5f) * W3D_LIQ_CELL, y = h.maxY - (gy + 0.5f) * W3D_LIQ_CELL;
            if (!w3d_liquid_at(p, x, y, &hit)) continue;
            lev[gx * 128 + gy] = hit.level;
            fl[gx * 128 + gy] = hit.flags;
            lo = std::min(lo, hit.level); hi = std::max(hi, hit.level);
            ++wet;
        }
    static const uint8_t kCol[4][3] = {{60, 140, 255}, {20, 60, 190}, {255, 110, 20}, {100, 220, 40}};
    for (int gx = 0; gx < 128; ++gx)
        for (int gy = 0; gy < 128; ++gy) {
            const uint8_t f = fl[gx * 128 + gy];
            if (f == W3D_LIQ_DRY) continue;
            float L = hi > lo ? 0.55f + 0.45f * (lev[gx * 128 + gy] - lo) / (hi - lo) : 1.0f;
            if (f & W3D_LIQ_DEEP) L *= 0.6f;
            for (int yy = 0; yy < S; ++yy)
                for (int xx = 0; xx < S; ++xx) {
                    // image: x = column gy (west to the right like region.png), y = row gx (north up)
                    uint8_t* o = &img[((size_t)(gx * S + yy) * W + gy * S + xx) * 3];
                    bool stripe = (f & W3D_LIQ_WMO) && ((xx + yy) % 4 == 0);
                    for (int c = 0; c < 3; ++c) o[c] = stripe ? 255 : (uint8_t)(kCol[f & W3D_LIQ_KIND][c] * L);
                }
        }
    char name[512];
    std::snprintf(name, sizeof name, "%s/liquid_%u_%u.png", outDir.c_str(), h.tileX, h.tileY);
    stbi_write_png(name, W, W, 3, img.data(), W * 3);
    std::printf("wrote %s: %zu wet cells, surface %.2f .. %.2f, look water fog %u %u %u (%.0f yd), ocean fog %u %u %u (%.0f yd)\n",
                name, wet, wet ? lo : 0.0f, wet ? hi : 0.0f, h.look[0].fog[0], h.look[0].fog[1], h.look[0].fog[2], h.look[0].fogEnd,
                h.look[1].fog[0], h.look[1].fog[1], h.look[1].fog[2], h.look[1].fogEnd);
    return 0;
}

static int previewCollision(const std::vector<uint8_t>& b, const W3dEntry& e, const std::string& outDir) {
    const uint8_t* p = b.data() + e.offset;
    W3dCollHeader h;
    std::memcpy(&h, p, sizeof h);
    const W3dCollTri* t = reinterpret_cast<const W3dCollTri*>(p + h.offTris);
    const float TS = 533.33333f, S = W3D_COLL_XY_SCALE, ppy = 2.0f;
    Canvas cv;
    cv.px = (int)(TS * ppy);
    cv.W = cv.H = cv.px;
    cv.rgb.assign((size_t)cv.W * cv.H * 3, 18);
    auto toImg = [&](float lx, float ly, float out[2]) { out[0] = (TS - ly) * ppy; out[1] = (TS - lx) * ppy; };
    for (uint32_t c = 0; c <= h.gridN; ++c) {   // grid
        int k = std::min(cv.W - 1, (int)std::lround(c * (float)cv.W / h.gridN));
        for (int i = 0; i < cv.W; ++i) {
            for (uint8_t* o : {&cv.rgb[((size_t)k * cv.W + i) * 3], &cv.rgb[((size_t)i * cv.W + k) * 3]}) o[0] = o[1] = o[2] = 40;
        }
    }
    auto line = [&](const float a[2], const float c[2], const uint8_t col[3]) {
        int n = (int)std::ceil(std::max(std::fabs(c[0] - a[0]), std::fabs(c[1] - a[1]))) + 1;
        for (int i = 0; i <= n; ++i) {
            int x = (int)(a[0] + (c[0] - a[0]) * i / n), y = (int)(a[1] + (c[1] - a[1]) * i / n);
            if (x < 0 || y < 0 || x >= cv.W || y >= cv.H) continue;
            std::memcpy(&cv.rgb[((size_t)y * cv.W + x) * 3], col, 3);
        }
    };
    uint32_t walls = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < h.triCount; ++i) {
            bool wall = t[i].flags & W3D_COLL_WALL;
            if (wall != (pass == 1)) continue;
            float P[3][2];
            for (int k = 0; k < 3; ++k) toImg(t[i].x[k] * S, t[i].y[k] * S, P[k]);
            if (!wall) {
                uint8_t col[3][3] = {{60, 110, 220}, {60, 110, 220}, {60, 110, 220}};
                cv.tri(P, col, 0.5f);
            } else {
                ++walls;
                static const uint8_t red[3] = {235, 50, 40}, orange[3] = {250, 170, 30};
                const uint8_t* col = (t[i].flags & W3D_COLL_M2) ? orange : red;
                for (int k = 0; k < 3; ++k) line(P[k], P[(k + 1) % 3], col);
            }
        }
    char name[512];
    std::snprintf(name, sizeof name, "%s/coll_%u_%u.png", outDir.c_str(), h.tileX, h.tileY);
    stbi_write_png(name, cv.W, cv.H, 3, cv.rgb.data(), cv.W * 3);
    std::printf("wrote %s: %u triangles, %u walls (red = buildings, orange = doodads, blue = floors)\n", name,
                h.triCount, walls);
    return 0;
}

// A player look: the body texture as the 3DS composites it (player_<...>_body.png) and the
// race model wearing it, posed through its animations (player_<...>.png).
static int previewPlayer(const std::vector<uint8_t>& b, const W3dEntry* e, uint32_t n, const W3dLook& look,
                         const std::string& outDir, const uint32_t* equip) {
    auto find = [&](uint32_t type, uint32_t id) -> const W3dEntry* {
        for (uint32_t i = 0; i < n; ++i) if (e[i].type == type && e[i].id == id) return &e[i];
        return nullptr;
    };
    const W3dEntry* ce = find(W3D_ENTRY_CHARACTER, w3d_char_id(look.race, look.sex));
    if (!ce) { std::printf("race %u sex %u not in pack (convert with --players)\n", look.race, look.sex); return 1; }
    const uint8_t* chrs = b.data() + ce->offset;
    W3dCharResolved r;
    w3d_char_resolve(chrs, &look, &r);
    const W3dEntry* me = find(W3D_ENTRY_CREATURE, w3d_char(chrs)->modelId);
    if (!me) { std::printf("race model not in pack\n"); return 1; }
    const uint8_t* mb = b.data() + me->offset;
    const W3dCreatureHeader* mh = w3d_creature(mb);
    const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(mb + mh->offSubmeshes);
    std::vector<uint16_t> all;
    for (uint32_t i = 0; i < mh->submeshCount; ++i) all.push_back(sm[i].geoset);
    // worn armour (--equip): ITEM entries of the body slots, like the client resolves them
    const W3dItemDisplay* worn[W3D_EQ_SLOTS] = {};
    for (int s = 0; s < W3D_EQ_SLOTS; ++s) {
        if (!equip[s]) continue;
        const W3dEntry* ie = find(W3D_ENTRY_ITEM, equip[s]);
        if (!ie) { std::printf("item display %u (slot %d) not in the pack\n", equip[s], s); continue; }
        worn[s] = (const W3dItemDisplay*)(b.data() + ie->offset);
    }
    W3dEquipResolved er;
    w3d_equip_resolve(worn, look.sex, &er);
    if (er.hideHair) r.scalp = 1;
    W3dDisplay d{};
    d.modelId = w3d_char(chrs)->modelId;
    d.scale = w3d_char(chrs)->scale;
    d.tex[4] = r.hairTex;
    d.geosetCount = (uint16_t)w3d_humanoid_geosets(all.data(), (int)all.size(), look.race, r.scalp, r.f100, r.f200, r.f300, &er.geo,
                                                   d.geosets, W3D_DISPLAY_GEOSETS);
    const uint8_t* L[7 + W3D_MAX_EQ_LAYERS];
    int nl = 0, nLayers = 0;
    for (int k = 0; k < 7; ++k) {
        const W3dEntry* le = r.layers[k] ? find(W3D_ENTRY_CHARIMG, r.layers[k]) : nullptr;
        L[nLayers++] = le ? b.data() + le->offset : nullptr;
        nl += le != nullptr;
    }
    for (int k = 0; k < er.layerCount; ++k)
        if (const W3dEntry* le = find(W3D_ENTRY_CHARIMG, er.layers[k])) { L[nLayers++] = b.data() + le->offset; ++nl; }
    const int A = W3D_CHAR_ATLAS;
    std::vector<uint8_t> rgb((size_t)A * A * 3), rgba((size_t)A * A * 4, 255);
    std::vector<uint16_t> tex((size_t)A * A);
    w3d_char_composite(L, nLayers, rgb.data(), tex.data());
    for (size_t i = 0; i < (size_t)A * A; ++i)
        for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = rgb[i * 3 + c];
    char base[96];
    std::snprintf(base, sizeof base, "player_%u_%u_%u_%u_%u_%u_%u", look.race, look.sex, look.skin, look.face,
                  look.hairStyle, look.hairColor, look.facialHair);
    std::string bodyName = outDir + "/" + base + "_body.png";
    stbi_write_png(bodyName.c_str(), A, A, 4, rgba.data(), A * 4);
    std::printf("wrote %s: %d layers%s%s%s, hair texture %s, %d geosets, scale %.2f\n", bodyName.c_str(), nl,
                r.haveSkin ? "" : " (skin colour not in the tables: first skin)",
                r.exactFace ? "" : " (face approximated)", r.haveHair ? "" : " (hair approximated)",
                r.hairTex ? "yes" : "none", d.geosetCount, d.scale);
    return previewCreature(b, e, n, 0, outDir, &d, &rgba, base);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: w3dpreview <pack.w3dp> <outdir> [lod]\n       w3dpreview <pack.w3dp> <outdir> --creature <displayId | 0 [count]>\n"
                  "       w3dpreview <pack.w3dp> <outdir> --coll [tileX tileY]   (floors and walls, 2 px per yard)\n"
                  "       w3dpreview <pack.w3dp> <outdir> --liquid [tileX tileY] (where the liquids are: swimming, 4 px per cell)\n"
                  "       w3dpreview <pack.w3dp> <outdir> --player race sex skin face hairStyle hairColor facialHair [--equip slot=display,...]");
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string outDir = argv[2];
    previewLod = argc > 3 && argv[3][0] != '-' ? std::clamp(std::atoi(argv[3]), 0, W3D_TERRAIN_LODS - 1) : 0;
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = reinterpret_cast<const W3dEntry*>(b.data() + h.indexOffset);
    if (argc > 10 && std::string(argv[3]) == "--player") {
        W3dLook look;
        uint8_t* f[7] = {&look.race, &look.sex, &look.skin, &look.face, &look.hairStyle, &look.hairColor, &look.facialHair};
        for (int k = 0; k < 7; ++k) *f[k] = (uint8_t)std::atoi(argv[4 + k]);
        uint32_t equip[W3D_EQ_SLOTS] = {};
        for (int k = 11; k + 1 < argc; ++k)   // --equip 4=7101,7=7002 (W3dEquipSlot=ItemDisplayInfo id)
            if (std::string(argv[k]) == "--equip")
                for (const char* q = argv[k + 1]; *q;) {
                    int slot = std::atoi(q);
                    const char* eq = std::strchr(q, '=');
                    if (!eq) break;
                    if (slot >= 0 && slot < W3D_EQ_SLOTS) equip[slot] = (uint32_t)std::strtoul(eq + 1, nullptr, 10);
                    q = std::strchr(eq, ',');
                    if (!q) break;
                    ++q;
                }
        return previewPlayer(b, e, h.entryCount, look, outDir, equip);
    }
    if (argc > 3 && std::string(argv[3]) == "--liquid") {
        int n = 0;
        for (uint32_t i = 0; i < h.entryCount; ++i) {
            if (e[i].type != W3D_ENTRY_LIQUID) continue;
            if (argc > 5 && e[i].id != w3d_tile_id((uint32_t)std::atoi(argv[4]), (uint32_t)std::atoi(argv[5]))) continue;
            n += previewLiquid(b, e[i], outDir) == 0;
        }
        if (!n) std::puts("no matching LIQD entries in the pack");
        return n ? 0 : 1;
    }
    if (argc > 3 && std::string(argv[3]) == "--coll") {
        int n = 0;
        for (uint32_t i = 0; i < h.entryCount; ++i) {
            if (e[i].type != W3D_ENTRY_COLLISION) continue;
            if (argc > 5 && e[i].id != w3d_tile_id((uint32_t)std::atoi(argv[4]), (uint32_t)std::atoi(argv[5]))) continue;
            n += previewCollision(b, e[i], outDir) == 0;
        }
        if (!n) std::puts("no matching COLL entries in the pack");
        return n ? 0 : 1;
    }
    if (argc > 4 && std::string(argv[3]) == "--creature") {
        uint32_t id = (uint32_t)std::strtoul(argv[4], nullptr, 10);
        if (id) return previewCreature(b, e, h.entryCount, id, outDir);
        // 0 = a sample: 'count' displays spread evenly over the pack's display ids
        int count = argc > 5 ? std::max(1, std::atoi(argv[5])) : 6;
        std::vector<uint32_t> ids;
        for (uint32_t i = 0; i < h.entryCount; ++i) if (e[i].type == W3D_ENTRY_DISPLAY) ids.push_back(e[i].id);
        std::sort(ids.begin(), ids.end());
        if (ids.empty()) { std::puts("no creature displays in the pack (convert with --creatures)"); return 1; }
        int bad = 0;
        for (int k = 0; k < count && k < (int)ids.size(); ++k)
            bad += previewCreature(b, e, h.entryCount, ids[(size_t)k * ids.size() / std::min<size_t>(count, ids.size())], outDir) != 0;
        return bad ? 1 : 0;
    }
    const float TS = 533.33333f;
    g_pool.open(argv[1], b, e, h.entryCount);

    FILE* texList = std::fopen((outDir + "/textures.txt").c_str(), "w");   // id, size, format of every texture in the pack
    // Region bounds from terrain headers.
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        const uint8_t* p = b.data() + e[i].offset;
        if (e[i].type == W3D_ENTRY_TEXTURE) {
            W3dTexHeader t;
            std::memcpy(&t, p, sizeof t);
            auto rgba = decodeLevel0(t, p + W3D_TEX_DATA_OFFSET);
            char name[512];
            std::snprintf(name, sizeof name, "%s/tex_%08x.png", outDir.c_str(), e[i].id);
            stbi_write_png(name, t.width, t.height, 4, rgba.data(), t.width * 4);
            if (texList) std::fprintf(texList, "%08x %dx%d format %d mips %d\n", e[i].id, (int)t.width, (int)t.height, (int)t.format, (int)t.mipCount);
        } else if (e[i].type == W3D_ENTRY_TERRAIN) {
            W3dTerrainHeader t;
            std::memcpy(&t, p, sizeof t);
            minX = std::min(minX, t.origin[0]); minY = std::min(minY, t.origin[1]);
            maxX = std::max(maxX, t.origin[0] + TS); maxY = std::max(maxY, t.origin[1] + TS);
        }
    }
    if (texList) std::fclose(texList);
    if (minX > maxX) { std::puts("no terrain"); return 1; }
    Canvas cv;
    cv.px = 256;
    cv.W = (int)std::lround((maxY - minY) / TS) * cv.px;
    cv.H = (int)std::lround((maxX - minX) / TS) * cv.px;
    cv.rgb.assign((size_t)cv.W * cv.H * 3, 0);
    auto toImg = [&](float rx, float ry, float out[2]) {
        out[0] = (maxY - ry) / TS * cv.px;   // west (small y... ) -> right: matches top-down north-up view
        out[1] = (maxX - rx) / TS * cv.px;
    };

    std::map<uint32_t, std::array<float, 3>> means;
    auto texMean = [&](uint32_t id, float out[3]) {
        auto it = means.find(id);
        if (it == means.end()) {
            std::array<float, 3> m = {200, 200, 200};
            for (uint32_t j = 0; j < h.entryCount; ++j) {
                if (e[j].type != W3D_ENTRY_TEXTURE || e[j].id != id) continue;
                W3dTexHeader t;
                std::memcpy(&t, b.data() + e[j].offset, sizeof t);
                auto rgba = decodeLevel0(t, b.data() + e[j].offset + W3D_TEX_DATA_OFFSET);
                double acc[3] = {0, 0, 0};
                const size_t n = rgba.size() / 4;
                for (size_t k = 0; k < n; ++k) for (int c = 0; c < 3; ++c) acc[c] += rgba[k * 4 + c];
                if (n) for (int c = 0; c < 3; ++c) m[c] = (float)(acc[c] / n);
                break;
            }
            if (const uint8_t* pb = m[0] == 200 && m[1] == 200 && m[2] == 200 ? g_pool.blob(W3D_ENTRY_TEXTURE, id) : nullptr) {
                W3dTexHeader t;
                std::memcpy(&t, pb, sizeof t);
                auto rgba = decodeLevel0(t, pb + W3D_TEX_DATA_OFFSET);
                double acc[3] = {0, 0, 0};
                const size_t n = rgba.size() / 4;
                for (size_t k = 0; k < n; ++k) for (int c = 0; c < 3; ++c) acc[c] += rgba[k * 4 + c];
                if (n) for (int c = 0; c < 3; ++c) m[c] = (float)(acc[c] / n);
            }
            it = means.emplace(id, m).first;
        }
        for (int c = 0; c < 3; ++c) out[c] = it->second[c];
    };
    for (int pass = 0; pass < 3; ++pass)
        for (uint32_t i = 0; i < h.entryCount; ++i) {
            const uint8_t* p = b.data() + e[i].offset;
            if (pass == 0 && e[i].type == W3D_ENTRY_TERRAIN) {
                W3dTerrainHeader t;
                std::memcpy(&t, p, sizeof t);
                const W3dTerrainVertex* v = reinterpret_cast<const W3dTerrainVertex*>(p + t.offVertices);
                const uint16_t* idx = reinterpret_cast<const uint16_t*>(p + t.offIndices);
                const W3dTerrainBatch* tb = reinterpret_cast<const W3dTerrainBatch*>(p + t.offBatches);
                const int lod = previewLod;
                for (uint32_t bi = 0; bi < t.batchCount; ++bi)
                for (uint32_t k = tb[bi].firstIndex[lod]; k + 2 < tb[bi].firstIndex[lod] + tb[bi].indexCount[lod]; k += 3) {
                    float P[3][2];
                    uint8_t col[3][3];
                    for (int c = 0; c < 3; ++c) {
                        const W3dTerrainVertex& q = v[idx[k + c]];
                        toImg(t.origin[0] + q.x * t.posScale[0] + t.posOffset[0], t.origin[1] + q.y * t.posScale[1] + t.posOffset[1], P[c]);
                        float L = q.r / 255.0f, A = q.a / 255.0f;
                        col[c][0] = (uint8_t)(L * (70 + A * 60)); col[c][1] = (uint8_t)(L * (120 - A * 25)); col[c][2] = (uint8_t)(L * (45 + A * 20));
                    }
                    cv.tri(P, col, 1.0f);
                }
            } else if (pass == 1 && e[i].type == W3D_ENTRY_WATER) {
                // stage 5.26: the colour the 3DS shows - the first frame's mean colour times the vertex colour, its opacity
                W3dWaterHeader t;
                std::memcpy(&t, p, sizeof t);
                const W3dTerrainVertex* v = reinterpret_cast<const W3dTerrainVertex*>(p + t.offVertices);
                const uint16_t* idx = reinterpret_cast<const uint16_t*>(p + t.offIndices);
                const W3dWaterBatch* wb = reinterpret_cast<const W3dWaterBatch*>(p + t.offBatches);
                const uint32_t* tids = reinterpret_cast<const uint32_t*>(p + t.offTexIds);
                for (uint32_t bi = 0; bi < t.batchCount; ++bi) {
                    float mean[3] = {200, 200, 200};
                    if (wb[bi].tex < t.texCount) texMean(tids[wb[bi].tex], mean);
                    for (uint32_t k = wb[bi].firstIndex; k + 2 < wb[bi].firstIndex + wb[bi].indexCount; k += 3) {
                        float P[3][2];
                        uint8_t col[3][3];
                        float a = 0;
                        for (int c = 0; c < 3; ++c) {
                            const W3dTerrainVertex& q = v[idx[k + c]];
                            toImg(t.origin[0] + q.x * t.posScale[0] + t.posOffset[0], t.origin[1] + q.y * t.posScale[1] + t.posOffset[1], P[c]);
                            col[c][0] = (uint8_t)(mean[0] * q.r / 255.0f);
                            col[c][1] = (uint8_t)(mean[1] * q.g / 255.0f);
                            col[c][2] = (uint8_t)(mean[2] * q.b / 255.0f);
                            a += q.a / (3 * 255.0f);
                        }
                        cv.tri(P, col, std::max(0.35f, a));
                    }
                }
            } else if (pass == 2 && e[i].type == W3D_ENTRY_PLACEMENT) {
                // Draw every placed model from above (buildings and doodads), shaded by facing.
                W3dPlacementHeader ph;
                std::memcpy(&ph, p, sizeof ph);
                const W3dPlacement* pl = reinterpret_cast<const W3dPlacement*>(p + sizeof ph);
                for (uint32_t k = 0; k < ph.count; ++k) {
                    const W3dEntry* me = nullptr;
                    for (uint32_t j = 0; j < h.entryCount; ++j)
                        if (e[j].type == W3D_ENTRY_MODEL && e[j].id == pl[k].modelId) { me = &e[j]; break; }
                    const uint8_t* poolModel = me ? nullptr : g_pool.blob(W3D_ENTRY_MODEL, pl[k].modelId);
                    if (!me && !poolModel) {   // unresolved model: magenta dot
                        float q[2];
                        toImg(pl[k].pos[0], pl[k].pos[1], q);
                        int x = (int)q[0], y = (int)q[1];
                        if (x >= 0 && y >= 0 && x < cv.W && y < cv.H) {
                            uint8_t* o = &cv.rgb[((size_t)y * cv.W + x) * 3];
                            o[0] = 255; o[1] = 40; o[2] = 200;
                        }
                        continue;
                    }
                    const uint8_t* mb = me ? b.data() + me->offset : poolModel;
                    W3dModelHeader mh;
                    std::memcpy(&mh, mb, sizeof mh);
                    const W3dModelVertex* mv = reinterpret_cast<const W3dModelVertex*>(mb + mh.offVertices);
                    const uint16_t* mi = reinterpret_cast<const uint16_t*>(mb + mh.offIndices);
                    // R = Rz * Ry * Rx (W3dPlacement convention)
                    float cx = std::cos(pl[k].rot[0]), sx = std::sin(pl[k].rot[0]);
                    float cy = std::cos(pl[k].rot[1]), sy = std::sin(pl[k].rot[1]);
                    float cz = std::cos(pl[k].rot[2]), sz = std::sin(pl[k].rot[2]);
                    const float R[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                                           {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                                           {-sy, cy * sx, cy * cx}};
                    auto world = [&](const W3dModelVertex& v, float out[3]) {
                        float l[3] = {v.x * mh.posScale[0] + mh.posOffset[0], v.y * mh.posScale[1] + mh.posOffset[1],
                                      v.z * mh.posScale[2] + mh.posOffset[2]};
                        for (int r = 0; r < 3; ++r)
                            out[r] = pl[k].pos[r] + pl[k].scale * (R[r][0] * l[0] + R[r][1] * l[1] + R[r][2] * l[2]);
                    };
                    for (uint32_t t = 0; t + 2 < mh.indexCount; t += 3) {
                        float w[3][3], P[3][2];
                        for (int c = 0; c < 3; ++c) { world(mv[mi[t + c]], w[c]); toImg(w[c][0], w[c][1], P[c]); }
                        float ax = w[1][0] - w[0][0], ay = w[1][1] - w[0][1], az = w[1][2] - w[0][2];
                        float bx = w[2][0] - w[0][0], by = w[2][1] - w[0][1], bz = w[2][2] - w[0][2];
                        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                        float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (nl < 1e-9f) continue;
                        float L = 0.35f + 0.65f * std::fabs(nz / nl);
                        uint8_t col[3][3];
                        for (int c = 0; c < 3; ++c) { col[c][0] = (uint8_t)(220 * L); col[c][1] = (uint8_t)(200 * L); col[c][2] = (uint8_t)(170 * L); }
                        cv.tri(P, col, 0.85f);
                    }
                }
            }
        }
    // Tile grid lines to make seams obvious.
    for (int y = 0; y < cv.H; ++y)
        for (int x = 0; x < cv.W; ++x)
            if (x % cv.px == 0 || y % cv.px == 0) {
                uint8_t* o = &cv.rgb[((size_t)y * cv.W + x) * 3];
                o[0] = o[1] = o[2] = 255;
            }
    std::string name = outDir + (previewLod ? "/region_lod" + std::to_string(previewLod) + ".png" : std::string("/region.png"));
    stbi_write_png(name.c_str(), cv.W, cv.H, 3, cv.rgb.data(), cv.W * 3);
    std::printf("wrote %s (%dx%d)\n", name.c_str(), cv.W, cv.H);
    return 0;
}
