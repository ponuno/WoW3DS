#include "ui_raster.hpp"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <map>

#include "rg_etc1.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {
uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

struct Tex { int w = 0, h = 0; std::vector<uint8_t> rgba; bool ok = false; };
std::map<uint16_t, Tex> g_cache;
std::string g_cachePath;

std::map<uint16_t, Tex> g_external;   // UI stage 8: run-time textures (the portraits) the tests stand in for

const Tex& texture(uint16_t id) {
    auto ex = g_external.find(id);
    if (ex != g_external.end()) return ex->second;
    const ui::Assets& a = ui::assets();
    if (g_cachePath != a.path()) { g_cache.clear(); g_cachePath = a.path(); }
    auto it = g_cache.find(id);
    if (it != g_cache.end()) return it->second;
    Tex& t = g_cache[id];
    if (id == ui::kTexBuiltinFont) {
        std::vector<uint8_t> a8;
        ui::Assets::builtinFontA8(a8, &t.w, &t.h);
        t.rgba.resize(a8.size() * 4);
        for (size_t i = 0; i < a8.size(); ++i) {
            t.rgba[i * 4] = t.rgba[i * 4 + 1] = t.rgba[i * 4 + 2] = 255;
            t.rgba[i * 4 + 3] = a8[i];
        }
        t.ok = true;
    } else {
        std::vector<uint8_t> blob;
        t.ok = a.readTexture(id, blob) && decodeTexBlob(blob, &t.w, &t.h, t.rgba);
    }
    return t;
}
}  // namespace

void rasterSetExternal(uint16_t id, int w, int h, const std::vector<uint8_t>& rgba) {
    Tex& t = g_external[id];
    t.w = w; t.h = h; t.rgba = rgba; t.ok = true;
}

bool decodeTexBlob(const std::vector<uint8_t>& blob, int* pw, int* ph, std::vector<uint8_t>& rgba) {
    if (blob.size() < W3D_TEX_DATA_OFFSET) return false;
    W3dTexHeader t;
    memcpy(&t, blob.data(), sizeof t);
    const int w = t.width, h = t.height;
    const uint8_t* d = blob.data() + W3D_TEX_DATA_OFFSET;
    *pw = w;
    *ph = h;
    rgba.assign((size_t)w * h * 4, 255);
    if (t.format == W3D_TEX_ETC1 || t.format == W3D_TEX_ETC1A4) {
        static bool init = false;
        if (!init) { rg_etc1::pack_etc1_block_init(); init = true; }
        const bool a4 = t.format == W3D_TEX_ETC1A4;
        if ((size_t)w * h / (a4 ? 1 : 2) > blob.size() - W3D_TEX_DATA_OFFSET) return false;
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < w; tx += 8)
                for (int by = 0; by < 8; by += 4)
                    for (int bx = 0; bx < 8; bx += 4) {
                        uint8_t alpha[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                        if (a4) { memcpy(alpha, d, 8); d += 8; }
                        uint8_t blk[8];
                        for (int i = 0; i < 8; ++i) blk[i] = d[7 - i];
                        d += 8;
                        uint32_t px[16];
                        rg_etc1::unpack_etc1_block(blk, px);
                        for (int y = 0; y < 4; ++y)
                            for (int x = 0; x < 4; ++x) {
                                uint8_t* o = &rgba[((size_t)(ty + by + y) * w + tx + bx + x) * 4];
                                memcpy(o, &px[y * 4 + x], 3);
                                uint8_t nib = alpha[2 * x + y / 2];
                                o[3] = (uint8_t)(((y & 1) ? nib >> 4 : nib & 15) * 17);
                            }
                    }
        return true;
    }
    if (t.format == W3D_TEX_A8) {
        if ((size_t)w * h > blob.size() - W3D_TEX_DATA_OFFSET) return false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* o = &rgba[((size_t)y * w + x) * 4];
                o[3] = d[((size_t)(y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)];
            }
        return true;
    }
    if ((size_t)w * h * 2 > blob.size() - W3D_TEX_DATA_OFFSET) return false;
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
    return true;
}

Canvas::Canvas(int w_, int h_, uint32_t color) : w(w_), h(h_), rgba((size_t)w_ * h_ * 4) { fill(color); }

void Canvas::fill(uint32_t c) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = c & 0xFF; rgba[i + 1] = (c >> 8) & 0xFF; rgba[i + 2] = (c >> 16) & 0xFF; rgba[i + 3] = 255;
    }
}

void Canvas::backdrop() {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* o = &rgba[((size_t)y * w + x) * 4];
            if (y < h * 55 / 100) {   // sky
                o[0] = (uint8_t)(120 + y / 3); o[1] = (uint8_t)(170 + y / 4); o[2] = 230;
            } else {                  // grass with some noise
                int n = ((x * 73 + y * 151) ^ (x * y)) & 15;
                o[0] = (uint8_t)(60 + n); o[1] = (uint8_t)(110 + n * 2); o[2] = (uint8_t)(40 + n);
            }
            o[3] = 255;
        }
}

void Canvas::draw(const HudList& l) {
    for (const HudQuad& q : l.q) {
        const Tex* t = q.mode != kHudFlat ? &texture(q.tex) : nullptr;
        if (t && !t->ok) t = nullptr;
        const int cr = q.color & 0xFF, cg = (q.color >> 8) & 0xFF, cb = (q.color >> 16) & 0xFF, ca = q.color >> 24;
        const int x0 = std::max<int>(0, q.x0), x1 = std::min<int>(w, q.x1), y0 = std::max<int>(0, q.y0), y1 = std::min<int>(h, q.y1);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                int r = cr, g = cg, b = cb, a = ca;
                if (q.mode != kHudFlat) {
                    if (!t) { a = a / 3; r = 255; g = 0; b = 255; }   // a missing texture shows magenta
                    else {
                        float fu = q.u0 + ((x - q.x0) + 0.5f) * (float)(q.u1 - q.u0) / (float)(q.x1 - q.x0);
                        float fv = q.v0 + ((y - q.y0) + 0.5f) * (float)(q.v1 - q.v0) / (float)(q.y1 - q.y0);
                        int u = std::clamp((int)floorf(fu), 0, t->w - 1), v = std::clamp((int)floorf(fv), 0, t->h - 1);
                        const uint8_t* s = &t->rgba[((size_t)v * t->w + u) * 4];
                        if (q.mode == kHudImage) { r = r * s[0] / 255; g = g * s[1] / 255; b = b * s[2] / 255; }
                        a = a * s[3] / 255;
                    }
                }
                uint8_t* o = &rgba[((size_t)y * w + x) * 4];
                o[0] = (uint8_t)((r * a + o[0] * (255 - a)) / 255);
                o[1] = (uint8_t)((g * a + o[1] * (255 - a)) / 255);
                o[2] = (uint8_t)((b * a + o[2] * (255 - a)) / 255);
            }
    }
}

void Canvas::blit(const Canvas& c, int x, int y) {
    for (int j = 0; j < c.h; ++j)
        for (int i = 0; i < c.w; ++i) {
            int tx = x + i, ty = y + j;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            memcpy(&rgba[((size_t)ty * w + tx) * 4], &c.rgba[((size_t)j * c.w + i) * 4], 4);
        }
}

bool Canvas::png(const std::string& path) const { return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0; }
