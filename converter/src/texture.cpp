// BLP -> PICA200 texture encoding (RGB565 / RGBA4 / RGBA5551 / ETC1 / ETC1A4).
//
// GPU layout (matches devkitPro tex3ds, the reference encoder):
//  * image row 0 (top) is stored first, no vertical flip; PICA samples t=1 at
//    the top, so D3D-style UVs (v=0 top) are converted to v' = 1 - v by callers;
//  * the image is cut into 8x8 tiles, left->right, top->bottom;
//  * 16-bit formats: pixels inside a tile in Morton order (x bit0, y bit0, ...);
//  * ETC1: the four 4x4 blocks of a tile in order (0,0) (4,0) (0,4) (4,4),
//    each 64-bit block byte-reversed (rg_etc1 writes big-endian);
//  * ETC1A4: an 8-byte 4-bit alpha block precedes each ETC1 block, nibble
//    index = x*4 + y (column-major).
#include "conv.h"

#include "pipeline/blp_loader.hpp"
#include "rg_etc1.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <cstdio>

namespace w3d {

bool Image::hasAlpha() const {
    for (size_t i = 3; i < rgba.size(); i += 4)
        if (rgba[i] < 250) return true;
    return false;
}

bool decodeBlp(const std::vector<uint8_t>& bytes, Image& out) {
    auto img = wowee::pipeline::BLPLoader::load(bytes, /*keepCompressed=*/false);
    if (!img.isValid() || img.data.empty()) return false;
    out.w = img.width;
    out.h = img.height;
    out.rgba = img.data;
    return out.rgba.size() >= (size_t)out.w * out.h * 4;
}

static int floorPow2(int v) {
    int p = 1;
    while (p * 2 <= v) p *= 2;
    return p;
}

// Area-average resample (exact for the integer downscales that are the common case).
Image resizeImage(const Image& src, int w, int h) {
    Image d;
    d.w = w;
    d.h = h;
    d.rgba.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        int y0 = y * src.h / h, y1 = std::max(y0 + 1, (y + 1) * src.h / h);
        for (int x = 0; x < w; ++x) {
            int x0 = x * src.w / w, x1 = std::max(x0 + 1, (x + 1) * src.w / w);
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = &src.rgba[((size_t)sy * src.w + sx) * 4];
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    ++n;
                }
            for (int c = 0; c < 4; ++c) d.rgba[((size_t)y * w + x) * 4 + c] = (uint8_t)(acc[c] / n);
        }
    }
    return d;
}

static inline uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static void encode16(const Image& im, uint8_t fmt, std::vector<uint8_t>& out) {
    size_t base = out.size();
    out.resize(base + (size_t)im.w * im.h * 2);
    uint16_t* dst = reinterpret_cast<uint16_t*>(out.data() + base);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const uint8_t* p = &im.rgba[((size_t)y * im.w + x) * 4];
            uint16_t v;
            switch (fmt) {
                case W3D_TEX_RGBA4:
                    v = (uint16_t)(((p[0] >> 4) << 12) | ((p[1] >> 4) << 8) | ((p[2] >> 4) << 4) | (p[3] >> 4));
                    break;
                case W3D_TEX_RGBA5551:
                    v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >= 128 ? 1 : 0));
                    break;
                default:
                    v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
            }
            size_t tile = (size_t)(y >> 3) * (im.w >> 3) + (x >> 3);
            dst[tile * 64 + morton8(x & 7, y & 7)] = v;
        }
}

// rg_etc1 quality: low is ~14x faster than medium for ~0.3 dB PSNR (measured on mixed blocks), so it is the
// default; --tex-quality medium for the old behaviour. Big levels are split over threads by rows of
// 8x8 tiles; each thread writes its own rows, so the output is identical to a single-threaded run.
static int g_etcQuality = 0;
static TexTiming g_timing;
void setEtcQuality(int q) { g_etcQuality = q; }
TexTiming& textureTiming() { return g_timing; }

static void encodeEtc1Rows(const Image& im, bool alpha, int ty0, int ty1, uint8_t* dst) {
    rg_etc1::etc1_pack_params params;
    params.clear();
    params.m_quality = g_etcQuality >= 2 ? rg_etc1::cHighQuality : g_etcQuality == 1 ? rg_etc1::cMediumQuality : rg_etc1::cLowQuality;
    size_t o = 0;
    for (int ty = ty0; ty < ty1; ty += 8)
        for (int tx = 0; tx < im.w; tx += 8)
            for (int by = 0; by < 8; by += 4)
                for (int bx = 0; bx < 8; bx += 4) {
                    uint32_t px[16];
                    uint8_t a[8] = {0};
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) {
                            const uint8_t* p = &im.rgba[((size_t)(ty + by + y) * im.w + tx + bx + x) * 4];
                            px[y * 4 + x] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | 0xFF000000u;
                            uint8_t a4 = p[3] >> 4;
                            a[2 * x + y / 2] |= (y & 1) ? (uint8_t)(a4 << 4) : a4;
                        }
                    uint8_t blk[8];
                    rg_etc1::pack_etc1_block(blk, px, params);
                    if (alpha) { std::memcpy(dst + o, a, 8); o += 8; }
                    for (int i = 7; i >= 0; --i) dst[o++] = blk[i];
                }
}

static void encodeEtc1(const Image& im, bool alpha, std::vector<uint8_t>& out) {
    static std::once_flag once;
    std::call_once(once, [] { rg_etc1::pack_etc1_block_init(); });
    const size_t rowBytes = (size_t)(im.w / 8) * 4 * (alpha ? 16 : 8);   // one row of 8x8 tiles
    const int rows = im.h / 8;
    size_t base = out.size();
    out.resize(base + rowBytes * rows);
    static const unsigned threads = [] {
        const char* e = std::getenv("W3D_THREADS");   // override (tests compare 1 vs N)
        unsigned n = e ? (unsigned)std::atoi(e) : std::thread::hardware_concurrency();
        return std::max(1u, std::min(16u, n));
    }();
    unsigned nt = threads;
    if ((size_t)im.w * im.h < 128 * 128 || nt == 1) {
        encodeEtc1Rows(im, alpha, 0, im.h, out.data() + base);
        return;
    }
    nt = std::min<unsigned>(nt, (unsigned)rows);
    std::vector<std::thread> th;
    for (unsigned k = 0; k < nt; ++k) {
        int r0 = rows * k / nt, r1 = rows * (k + 1) / nt;
        th.emplace_back(encodeEtc1Rows, std::cref(im), alpha, r0 * 8, r1 * 8, out.data() + base + rowBytes * r0);
    }
    for (auto& t : th) t.join();
}

// Fraction of texels that pass an alpha test at 128 when alpha is scaled by k.
static float alphaCoverage(const Image& im, float k) {
    size_t pass = 0, n = (size_t)im.w * im.h;
    for (size_t i = 0; i < n; ++i) pass += im.rgba[i * 4 + 3] * k >= 127.5f;
    return n ? (float)pass / (float)n : 0.0f;
}

// Box-filtered mips of alpha-tested foliage lose coverage (leaves thin out and
// flicker with distance). Rescale each level's alpha so the share of texels
// passing the alpha test matches level 0.
static void preserveCoverage(Image& level, float target) {
    float lo = 0.0f, hi = 4.0f;
    for (int it = 0; it < 16; ++it) {
        float mid = 0.5f * (lo + hi);
        if (alphaCoverage(level, mid) < target) lo = mid; else hi = mid;
    }
    float k = 0.5f * (lo + hi);
    for (size_t i = 0; i < (size_t)level.w * level.h; ++i)
        level.rgba[i * 4 + 3] = (uint8_t)std::min(255.0f, level.rgba[i * 4 + 3] * k + 0.5f);
}

std::vector<uint8_t> encodeTexture(const Image& srcIn, int maxSize, uint8_t fmt, bool wantAlpha) {
    bool alpha = wantAlpha && srcIn.hasAlpha();
    // Formats that carry alpha only when the texture actually uses it.
    if (alpha) {
        if (fmt == W3D_TEX_ETC1) fmt = W3D_TEX_ETC1A4;
        else if (fmt == W3D_TEX_RGB565) fmt = W3D_TEX_RGBA4;
    } else {
        if (fmt == W3D_TEX_ETC1A4) fmt = W3D_TEX_ETC1;
        else if (fmt == W3D_TEX_RGBA4 || fmt == W3D_TEX_RGBA5551) fmt = W3D_TEX_RGB565;
    }

    int w = std::clamp(floorPow2(srcIn.w), 8, maxSize);
    int h = std::clamp(floorPow2(srcIn.h), 8, maxSize);
    Image level = resizeImage(srcIn, w, h);
    const float coverage0 = alpha ? alphaCoverage(level, 1.0f) : 0.0f;

    std::vector<uint8_t> data;
    int mips = 0;
    for (;;) {
        if (fmt == W3D_TEX_ETC1 || fmt == W3D_TEX_ETC1A4) encodeEtc1(level, fmt == W3D_TEX_ETC1A4, data);
        else encode16(level, fmt, data);
        ++mips;
        if (level.w <= 8 || level.h <= 8 || mips >= 8) break;
        level = resizeImage(level, level.w / 2, level.h / 2);
        if (alpha) preserveCoverage(level, coverage0);
    }

    W3dTexHeader th{};
    th.width = (uint16_t)w;
    th.height = (uint16_t)h;
    th.format = fmt;
    th.mipCount = (uint8_t)mips;
    th.flags = alpha ? W3D_TEXF_HAS_ALPHA : 0;
    th.dataSize = (uint32_t)data.size();
    std::vector<uint8_t> blob(W3D_TEX_DATA_OFFSET, 0);
    std::memcpy(blob.data(), &th, sizeof th);
    blob.insert(blob.end(), data.begin(), data.end());
    return blob;
}

// One level of an image whose size is already a power of two (UI pages), in exactly 'fmt' (W3D_TEX_A8 too).
std::vector<uint8_t> encodeTextureLevel(const Image& im, uint8_t fmt) {
    std::vector<uint8_t> data;
    if (fmt == W3D_TEX_ETC1 || fmt == W3D_TEX_ETC1A4) {
        encodeEtc1(im, fmt == W3D_TEX_ETC1A4, data);
    } else if (fmt == 5) {   // W3D_TEX_A8 (w3d_ui.h)
        data.resize((size_t)im.w * im.h);
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x)
                data[((size_t)(y >> 3) * (im.w >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)] = im.rgba[((size_t)y * im.w + x) * 4 + 3];
    } else {
        encode16(im, fmt, data);
    }
    W3dTexHeader th{};
    th.width = (uint16_t)im.w;
    th.height = (uint16_t)im.h;
    th.format = fmt;
    th.mipCount = 1;
    th.flags = fmt == W3D_TEX_ETC1 || fmt == W3D_TEX_RGB565 ? 0 : W3D_TEXF_HAS_ALPHA;
    th.dataSize = (uint32_t)data.size();
    std::vector<uint8_t> blob(W3D_TEX_DATA_OFFSET, 0);
    std::memcpy(blob.data(), &th, sizeof th);
    blob.insert(blob.end(), data.begin(), data.end());
    return blob;
}

uint32_t addTextureFromGame(PackWriter& pack, const DataDir& data, const std::string& gamePath,
                            int maxSize, uint8_t fmt, bool alphaCapable, bool verbose) {
    uint32_t id = w3d_fnv1a(DataDir::normalize(gamePath).c_str());
    if (pack.has(W3D_ENTRY_TEXTURE, id)) return id;
    std::vector<uint8_t> bytes;
    Image img;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = data.read(gamePath, bytes) && decodeBlp(bytes, img);
    auto t1 = std::chrono::steady_clock::now();
    g_timing.readDecodeSec += std::chrono::duration<double>(t1 - t0).count();
    if (!ok) {
        if (verbose) std::fprintf(stderr, "  texture missing/undecodable: %s\n", gamePath.c_str());
        return 0;
    }
    if (verbose && DataDir::normalize(gamePath).find("tileset") != std::string::npos) {   // terrain layers: what came out of the BLP
        double r = 0, g = 0, b = 0, aa = 0;
        const size_t n = img.rgba.size() / 4;
        for (size_t i = 0; i < n; ++i) { r += img.rgba[i * 4]; g += img.rgba[i * 4 + 1]; b += img.rgba[i * 4 + 2]; aa += img.rgba[i * 4 + 3]; }
        if (n) std::printf("  terrain texture %s %dx%d mean rgb %.0f %.0f %.0f alpha %.0f\n", gamePath.c_str(), img.w, img.h, r / n, g / n, b / n, aa / n);
    }
    pack.add(W3D_ENTRY_TEXTURE, id, encodeTexture(img, maxSize, fmt, alphaCapable));
    g_timing.encodeSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    ++g_timing.count;
    return id;
}

}  // namespace w3d
