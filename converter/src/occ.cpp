// w3docc — 6.53: how much of what the 3DS client draws from one spot is actually seen; 6.54: --pvs builds map<ID>.pvs, the precomputed
// visibility the client uses to skip doodads and ground hidden behind buildings and the ground (common/w3d_pvs.h).
//
// Reads only what is needed from a pack (the tiles around the area, their placements, the models - from the map pack or its pool
// common*.w3dp) and draws on the CPU into a depth + object-id buffer, with the client's rules: its frustum, doodads to 40 + range x
// radius, outdoor WMO groups to the draw distance + radius, indoor groups within radius + 25 (radius + 70 when the camera is inside
// that building: the portal walk), the level of detail by distance / screen error, back faces culled. Occluders: the ground and opaque
// parts; alpha-keyed / blended parts never hide anything (their holes are not known) but can be seen. Seen = at least one pixel.
//
//   w3docc <map.w3dp> <outdir> x y z yawDeg pitchDeg [--dist 533] [--range 45] [--narrow] [--lodpx 2]
//       one view (x y z yaw pitch as gputest.txt prints them): occ_report.txt (drawn vs seen; with map<ID>.pvs next to the pack also
//       what the PVS keeps and whether it misses anything), occ_view.png, occ_hidden.png
//   w3docc --pvs <map.w3dp> --around x y radius [--around ...] [--cell 16] [--res 256] [--threads N] [--check 300]
//       [--occrange 25] [--maxrange 120] [--dist 533] [--near 25] [-o file.pvs]
//       cells of ~16 yd over the area; floors found by a ray down through the ground, buildings and bigger doodads (walkable,
//       1.8 yd free above); eyes 2 and 8 yd over every floor at each cell's corners and centre, six 90-degree faces each; a cell's
//       floors clustered into height levels (a gap over 3 yd starts a new one), each level the union of its eyes' views. Occluders
//       only what the client surely draws (doodads within 40 + occrange x radius); candidates within 40 + maxrange x radius. Then
//       a check: random cameras in the area in the client's view, the file read back - "MISSING" lists what was seen but culled.
//       Writes map<ID>.pvs and map<ID>.pvs.txt.
//   w3docc --hlod <map.w3dp> [--around x y radius ...] [--switch 160] [-o file.hlod]
//       6.57: the horizon - per 133-yd block of every tile (or of the tiles in the areas) one coarse mesh of its buildings (outside
//       groups, interior groups with portals) and large doodads, two levels, coloured from the textures (common/w3d_hlod.h).
//       Writes map<ID>.hlod and map<ID>.hlod.txt.
#include "w3d_format.h"
#include "w3d_pvs.h"
#include "w3d_farobj.h"
#include "w3d_ground.h"
#include "w3d_hlod.h"
#include "wmo_portals.hpp"   // 6.57c: the client's portal graph (proto3ds/source/wmo_portals.cpp)
#include "w3d_farlod.h"
#include "rg_etc1.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

static bool seek64(FILE* f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

struct PackFile {
    std::string path, err;
    FILE* f = nullptr;
    std::vector<W3dEntry> idx;
    ~PackFile() { if (f) std::fclose(f); }
    bool open(const std::string& p) {
        path = p;
        f = std::fopen(p.c_str(), "rb");
        if (!f) { err = "cannot open"; return false; }
        W3dPackHeader h{};
        if (std::fread(&h, sizeof h, 1, f) != 1 || h.magic != W3DP_MAGIC) { err = "not a W3DP pack"; return false; }
        if (h.version != W3DP_VERSION) { err = "pack version " + std::to_string(h.version) + ", this tool reads " + std::to_string(W3DP_VERSION); return false; }
        idx.resize(h.entryCount);
        if (!seek64(f, h.indexOffset) || std::fread(idx.data(), sizeof(W3dEntry), idx.size(), f) != idx.size()) { err = "cannot read its index"; return false; }
        return true;
    }
    bool read(const W3dEntry& e, std::vector<uint8_t>& out, uint32_t max = 0xFFFFFFFFu) {
        out.resize(std::min(e.size, max));
        if (out.empty()) return true;
        return seek64(f, e.offset) && std::fread(out.data(), 1, out.size(), f) == out.size();
    }
};

struct Packs {
    std::vector<std::unique_ptr<PackFile>> files;   // [0] the map pack, then the pool parts
    std::map<std::pair<uint32_t, uint32_t>, std::pair<int, W3dEntry>> index;
    std::string poolNote;                             // for the report: which pool files were read (or why not)
    uint32_t readFails = 0;
    bool open(const std::string& mapPack) {
        files.emplace_back(new PackFile);
        if (!files[0]->open(mapPack)) { std::fprintf(stderr, "%s: %s\n", mapPack.c_str(), files[0]->err.c_str()); return false; }
        W3dPackInfo mi{};
        for (const W3dEntry& x : files[0]->idx) {
            index[{x.type, x.id}] = {0, x};
            if (x.type == W3D_ENTRY_PACKINFO) {
                std::vector<uint8_t> b;
                if (files[0]->read(x, b)) std::memcpy(&mi, b.data(), std::min(sizeof mi, b.size()));
            }
        }
        if (mi.kind != W3D_PACK_MAP) { poolNote = "none (a standalone pack: its models are inside it)"; return true; }
        std::string dir = mapPack;
        const size_t cut = dir.find_last_of("/\\");
        dir = cut == std::string::npos ? std::string() : dir.substr(0, cut + 1);
        int parts = 1;
        for (int k = 0; k < parts; ++k) {
            const std::string p = dir + (k ? "common" + std::to_string(k % 10) + ".w3dp" : std::string("common.w3dp"));
            std::unique_ptr<PackFile> pf(new PackFile);
            if (!pf->open(p)) {
                poolNote += (poolNote.empty() ? "" : "; ") + p + ": " + pf->err + " - MODELS FROM IT ARE MISSING";
                std::fprintf(stderr, "%s: %s\n", p.c_str(), pf->err.c_str());
                break;
            }
            for (const W3dEntry& x : pf->idx) {
                if (x.type == W3D_ENTRY_PACKINFO && k == 0) {
                    std::vector<uint8_t> b;
                    W3dPackInfo ci{};
                    if (pf->read(x, b)) std::memcpy(&ci, b.data(), std::min(sizeof ci, b.size()));
                    parts = std::max<int>(1, ci.partCount);
                    if (ci.buildId != mi.buildId) poolNote += (poolNote.empty() ? "" : "; ") + p + " is from another conversion than the map pack";
                }
                if (!index.count({x.type, x.id})) index[{x.type, x.id}] = {(int)files.size(), x};
            }
            std::printf("pool part %s: %zu entries\n", p.c_str(), pf->idx.size());
            poolNote += (poolNote.empty() ? "" : "; ") + p + " (" + std::to_string(pf->idx.size()) + " entries)";
            files.push_back(std::move(pf));
        }
        return true;
    }
    bool read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
        auto it = index.find({type, id});
        if (it == index.end()) return false;
        if (!files[it->second.first]->read(it->second.second, out)) { ++readFails; return false; }
        return true;
    }
};


// ------------------------------------------------------------------ the scene
struct ModelData {
    bool ok = false;
    std::vector<uint8_t> blob;
    const W3dModelHeader* h = nullptr;
    const W3dSubmesh* subs = nullptr;
    const W3dSubLod* lods = nullptr;
    bool building = false, interior = false, portals = false;
    float lodErr[2] = {0, 0};
};

enum Kind : uint8_t { kDoodad = 0, kExterior = 1, kInterior = 2 };

struct Inst {
    const ModelData* m;
    float pos[3], R[3][3], scale, radius, rot[3];
    int tile;
    uint32_t pl;        // index in the tile's PLCM
    uint8_t kind;
    uint32_t bkey;      // the building (placements sharing rotation and scale), for the "inside" rule
    uint32_t modelId = 0;
    bool linked = false;   // 6.57c: a group whose building (portal graph) holds an outdoor group - as the client's Node::outdoorLinked
    bool sealed = false;   // 6.57f: an interior group without portals in a tile whose buildings have portal tables: seen only from inside it
};

struct TileData {
    int tx, ty;
    std::vector<uint8_t> terr;
    const W3dTerrainHeader* th = nullptr;
};

struct TB { int tile; uint32_t b; float c[3], rad; };   // a ground batch

constexpr float kPi = 3.14159265358979f;
constexpr float kTile = 533.33333f;

struct Scene {
    std::vector<TileData> tiles;
    std::vector<Inst> inst;
    std::vector<TB> tb;
    std::map<uint32_t, ModelData> models;
    float windSign = 1.0f, modelSign = 1.0f;
    double normalsAgree = 1.0;
    uint32_t placements = 0, missing = 0;
};

struct View {
    float cam[3], f[3], r[3], u[3];
    float tanV, tanH;
    int W, H;
    float n[6][3], d[6], maxDist;
    // margin: the client's frustum is 3 degrees wider than its view (Frustum::build)
    void build(const float c[3], float yawDeg, float pitchDeg, float dist, int w, int h, float tanVert, float tanHorz, float marginDeg) {
        std::memcpy(cam, c, sizeof cam);
        const float yaw = yawDeg * kPi / 180.0f, pitch = pitchDeg * kPi / 180.0f;
        f[0] = cosf(pitch) * cosf(yaw); f[1] = cosf(pitch) * sinf(yaw); f[2] = sinf(pitch);
        r[0] = f[1]; r[1] = -f[0]; r[2] = 0;
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
        if (rl < 1e-4f) { r[0] = sinf(yaw); r[1] = -cosf(yaw); rl = 1; }   // (straight up / down: right from the yaw)
        r[0] /= rl; r[1] /= rl;
        u[0] = r[1] * f[2] - r[2] * f[1]; u[1] = r[2] * f[0] - r[0] * f[2]; u[2] = r[0] * f[1] - r[1] * f[0];
        tanV = tanVert; tanH = tanHorz;
        W = w; H = h;
        const float m = marginDeg * kPi / 180.0f;
        const float hv = atanf(tanV) + m, hh = atanf(tanH) + m;
        const float sh = sinf(hh), ch = cosf(hh), sv = sinf(hv), cv = cosf(hv);
        for (int k = 0; k < 3; ++k) {
            n[0][k] = f[k] * sh + r[k] * ch; n[1][k] = f[k] * sh - r[k] * ch;
            n[2][k] = f[k] * sv + u[k] * cv; n[3][k] = f[k] * sv - u[k] * cv;
            n[4][k] = f[k]; n[5][k] = -f[k];
        }
        d[0] = d[1] = d[2] = d[3] = 0; d[4] = -1.0f; d[5] = dist;
        maxDist = dist;
    }
    void client(const float c[3], float yawDeg, float pitchDeg, float dist, bool wide) {   // the top screen
        const float tv = tanf(30.0f * kPi / 180.0f);
        build(c, yawDeg, pitchDeg, dist, wide ? 800 : 400, 240, tv, tv * 400.0f / 240.0f, 3.0f);
    }
    bool sphere(const float c[3], float rad) const {
        const float p[3] = {c[0] - cam[0], c[1] - cam[1], c[2] - cam[2]};
        const float lim = maxDist + rad;
        if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] > lim * lim) return false;
        for (int i = 0; i < 6; ++i)
            if (n[i][0] * p[0] + n[i][1] * p[1] + n[i][2] * p[2] + d[i] < -rad) return false;
        return true;
    }
    void toView(const float w[3], float o[3]) const {
        const float p[3] = {w[0] - cam[0], w[1] - cam[1], w[2] - cam[2]};
        o[0] = p[0] * r[0] + p[1] * r[1] + p[2] * r[2];
        o[1] = p[0] * u[0] + p[1] * u[1] + p[2] * u[2];
        o[2] = p[0] * f[0] + p[1] * f[1] + p[2] * f[2];
    }
};

struct Target {
    int W = 0, H = 0;
    std::vector<float> inv;      // 1 / view z (larger = nearer), 0 = nothing
    std::vector<uint32_t> id;    // 0 nothing; 1 + instance; 0x80000000 | ground batch
    // 6.54e: blocks of 8x8 pixels - the farthest depth drawn in each (recomputed when written to): whole blocks behind are skipped
    int BW = 0, BH = 0;
    mutable std::vector<float> bfar;
    mutable std::vector<uint8_t> bdirty;
    void clear(int w, int h) {
        W = w; H = h; inv.assign((size_t)w * h, 0.0f); id.assign((size_t)w * h, 0u);
        BW = (w + 7) / 8; BH = (h + 7) / 8;
        bfar.assign((size_t)BW * BH, 0.0f); bdirty.assign((size_t)BW * BH, 0);
    }
    void dirty(int x0, int y0, int x1, int y1) {   // (pixels, inclusive)
        for (int by = y0 >> 3; by <= (y1 >> 3); ++by)
            for (int bx = x0 >> 3; bx <= (x1 >> 3); ++bx) bdirty[(size_t)by * BW + bx] = 1;
    }
    float blockFar(int bx, int by) const {   // the smallest 1/z in the block (0: something open there)
        const size_t b = (size_t)by * BW + bx;
        if (bdirty[b]) {
            float m = 1e30f;
            const int xe = std::min(W, bx * 8 + 8), ye = std::min(H, by * 8 + 8);
            for (int y = by * 8; y < ye && m > 0; ++y)
                for (int x = bx * 8; x < xe; ++x) m = std::min(m, inv[(size_t)y * W + x]);
            bfar[b] = m;
            bdirty[b] = 0;
        }
        return bfar[b];
    }
};

// 6.54e: the pixels x of a row (px = x + 0.5) where a + b px >= lim, within [x0, x1]; worked in double, with one pixel of slack
// each side. The caller still tests each pixel with its own float sums, so lim carries a margin for their rounding: the span only
// leaves out pixels that test would fail anyway.
inline void edgeSpan(double a, double b, double lim, int& x0, int& x1) {
    if (x0 > x1) return;
    if (b == 0.0) { if (a < lim) x1 = x0 - 1; return; }
    const double p = (lim - a) / b - 0.5;   // x where it equals lim
    if (!(p == p)) return;
    if (b > 0) { if (p > (double)x1 + 1.0) x1 = x0 - 1; else if (p > (double)x0) x0 = (int)std::floor(p) - 1 > x0 ? (int)std::floor(p) - 1 : x0; }
    else { if (p < (double)x0 - 1.0) x1 = x0 - 1; else if (p < (double)x1) x1 = (int)std::ceil(p) + 1 < x1 ? (int)std::ceil(p) + 1 : x1; }
}
constexpr double kFloatErr = 4e-6;   // (a few float roundings of a sum of products, relative to its terms)

constexpr float kNear = 1.0f;
constexpr uint32_t kGround = 0x80000000u;

// A view-space triangle, clipped at the near plane. write: depth + id. Else a test: the pixels that pass (stopAtFirst: 1 at most).
uint32_t rasterTri(Target& t, const View& v, const float a[3], const float b[3], const float c[3], uint32_t id, bool write,
                   bool stopAtFirst = false) {
    if (a[2] < kNear && b[2] < kNear && c[2] < kNear) return 0;
    float poly[4][3];
    int np = 0;
    const float* in[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        const float* p = in[i];
        const float* q = in[(i + 1) % 3];
        const bool pin = p[2] >= kNear, qin = q[2] >= kNear;
        if (pin) std::memcpy(poly[np++], p, sizeof(float) * 3);
        if (pin != qin) {
            const float s = (kNear - p[2]) / (q[2] - p[2]);
            for (int k = 0; k < 3; ++k) poly[np][k] = p[k] + (q[k] - p[k]) * s;
            poly[np][2] = kNear;
            ++np;
        }
    }
    if (np < 3) return 0;
    float sx[4], sy[4], iz[4];
    const float kx = 0.5f * (float)t.W / v.tanH, ky = 0.5f * (float)t.H / v.tanV;
    for (int i = 0; i < np; ++i) {
        const float z = poly[i][2];
        iz[i] = 1.0f / z;
        sx[i] = 0.5f * (float)t.W + poly[i][0] * iz[i] * kx;
        sy[i] = 0.5f * (float)t.H - poly[i][1] * iz[i] * ky;
    }
    uint32_t passed = 0;
    for (int k = 1; k + 1 < np; ++k) {
        const int i0 = 0, i1 = k, i2 = k + 1;
        const float area = (sx[i1] - sx[i0]) * (sy[i2] - sy[i0]) - (sx[i2] - sx[i0]) * (sy[i1] - sy[i0]);
        if (fabsf(area) < 1e-12f) continue;
        const float minx = std::min({sx[i0], sx[i1], sx[i2]}), maxx = std::max({sx[i0], sx[i1], sx[i2]});
        const float miny = std::min({sy[i0], sy[i1], sy[i2]}), maxy = std::max({sy[i0], sy[i1], sy[i2]});
        if (maxx < 0 || maxy < 0 || minx > (float)t.W || miny > (float)t.H) continue;
        const int x0 = std::max(0, (int)floorf(minx)), x1 = std::min(t.W - 1, (int)ceilf(maxx));
        const int y0 = std::max(0, (int)floorf(miny)), y1 = std::min(t.H - 1, (int)ceilf(maxy));
        if (x0 > x1 || y0 > y1) continue;
        // 6.54e: a small triangle wholly behind what its blocks hold already passes no pixel (its depth never exceeds its nearest
        // corner's, but for the edge tolerance's hair)
        const float izm = std::max({iz[i0], iz[i1], iz[i2]}) * 1.0012f;
        if ((x1 >> 3) - (x0 >> 3) < 4 && (y1 >> 3) - (y0 >> 3) < 4) {
            bool hidden = true;
            for (int by = y0 >> 3; by <= (y1 >> 3) && hidden; ++by)
                for (int bx = x0 >> 3; bx <= (x1 >> 3) && hidden; ++bx) hidden = t.blockFar(bx, by) * 1.0005f >= izm;
            if (hidden) continue;
        }
        const float ia = 1.0f / area;
        for (int y = y0; y <= y1; ++y) {
            const float py = (float)y + 0.5f;
            // 6.54e: only the part of the row inside the three edges (w = a + b px along the row; the margin covers the float sums below)
            const double X = std::max(fabsf((float)x0), fabsf((float)x1)) + 1.0, ad = fabs((double)ia);
            const double d1 = (double)sy[i1] - py, d2 = (double)sy[i2] - py, d0 = (double)sy[i0] - py;
            const double a0 = ((double)sx[i1] * d2 - (double)sx[i2] * d1) * ia, b0 = ((double)sy[i1] - sy[i2]) * ia;
            const double a1 = ((double)sx[i2] * d0 - (double)sx[i0] * d2) * ia, b1 = ((double)sy[i2] - sy[i0]) * ia;
            const double e0 = kFloatErr * ad * ((fabs(sx[i1]) + X) * fabs(d2) + (fabs(sx[i2]) + X) * fabs(d1)) + 1e-6;
            const double e1 = kFloatErr * ad * ((fabs(sx[i2]) + X) * fabs(d0) + (fabs(sx[i0]) + X) * fabs(d2)) + 1e-6;
            int xs = x0, xe = x1;
            edgeSpan(a0, b0, -2e-4 - e0, xs, xe);
            edgeSpan(a1, b1, -2e-4 - e1, xs, xe);
            edgeSpan(1.0 - a0 - a1, -b0 - b1, -2e-4 - e0 - e1 - 1e-6, xs, xe);
            const size_t brow = (size_t)(y >> 3) * t.BW;
            for (int x = xs; x <= xe;) {
                // 6.54e: a block not written since its farthest depth was found, all of it nearer than this triangle: skipped whole
                const size_t blk = brow + (size_t)(x >> 3);
                const int bend = std::min(xe, (x | 7));
                if (!t.bdirty[blk] && t.bfar[blk] * 1.0005f >= izm) { x = bend + 1; continue; }
                bool wroteHere = false;
                for (; x <= bend; ++x) {
                    const float px = (float)x + 0.5f;
                    const float w0 = ((sx[i1] - px) * (sy[i2] - py) - (sx[i2] - px) * (sy[i1] - py)) * ia;
                    const float w1 = ((sx[i2] - px) * (sy[i0] - py) - (sx[i0] - px) * (sy[i2] - py)) * ia;
                    const float w2 = 1.0f - w0 - w1;
                    // 6.54d: a hair of overlap - a pixel on the edge two triangles share was missed by both (rounding): 1-px holes in walls
                    if (w0 < -2e-4f || w1 < -2e-4f || w2 < -2e-4f) continue;
                    const float z = w0 * iz[i0] + w1 * iz[i1] + w2 * iz[i2];
                    const size_t o = (size_t)y * t.W + x;
                    if (z <= t.inv[o] * 1.0005f) continue;   // (a hair of bias: lying on another part counts as behind it)
                    ++passed;
                    if (write) { t.inv[o] = z; t.id[o] = id; wroteHere = true; }
                    else if (stopAtFirst) return passed;
                }
                if (wroteHere) t.bdirty[blk] = 1;
            }
        }
    }
    return passed;
}

// 6.54: a conservative test (the PVS's eyes): a pixel counts when the triangle touches any part of it and the triangle's nearest point
// is in front of what is drawn there. The cube faces are coarser than the client's screen; without this a far sliver of ground one
// pixel high over a ridge (seen on the 3DS) was missed at 256x256 and only caught at 1024x1024.
bool conservativeSeen(const Target& t, const View& v, const float a[3], const float b[3], const float c[3]) {
    if (a[2] < kNear && b[2] < kNear && c[2] < kNear) return false;
    float poly[4][3];
    int np = 0;
    const float* in[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        const float* p = in[i];
        const float* q = in[(i + 1) % 3];
        const bool pin = p[2] >= kNear, qin = q[2] >= kNear;
        if (pin) std::memcpy(poly[np++], p, sizeof(float) * 3);
        if (pin != qin) {
            const float s = (kNear - p[2]) / (q[2] - p[2]);
            for (int k = 0; k < 3; ++k) poly[np][k] = p[k] + (q[k] - p[k]) * s;
            poly[np][2] = kNear;
            ++np;
        }
    }
    if (np < 3) return false;
    float sx[4], sy[4], izMax = 0;
    const float kx = 0.5f * (float)t.W / v.tanH, ky = 0.5f * (float)t.H / v.tanV;
    for (int i = 0; i < np; ++i) {
        const float iz = 1.0f / poly[i][2];
        izMax = std::max(izMax, iz);
        sx[i] = 0.5f * (float)t.W + poly[i][0] * iz * kx;
        sy[i] = 0.5f * (float)t.H - poly[i][1] * iz * ky;
    }
    for (int k = 1; k + 1 < np; ++k) {
        const int i0 = 0, i1 = k, i2 = k + 1;
        float area = (sx[i1] - sx[i0]) * (sy[i2] - sy[i0]) - (sx[i2] - sx[i0]) * (sy[i1] - sy[i0]);
        if (fabsf(area) < 1e-12f) continue;
        const float sg = area > 0 ? 1.0f : -1.0f;
        // edge functions E(x, y) = A x + B y + C, >= 0 inside (oriented by the sign of the area); a pixel square touches the half
        // plane when E(centre) + 0.5 (|A| + |B|) >= 0
        const int vi[3] = {i0, i1, i2};
        float A[3], B[3], C[3];
        for (int e = 0; e < 3; ++e) {
            const int p0 = vi[(e + 1) % 3], p1 = vi[(e + 2) % 3];
            A[e] = sg * (sy[p0] - sy[p1]);
            B[e] = sg * (sx[p1] - sx[p0]);
            C[e] = sg * (sx[p0] * sy[p1] - sx[p1] * sy[p0]);
        }
        const float minx = std::min({sx[i0], sx[i1], sx[i2]}), maxx = std::max({sx[i0], sx[i1], sx[i2]});
        const float miny = std::min({sy[i0], sy[i1], sy[i2]}), maxy = std::max({sy[i0], sy[i1], sy[i2]});
        const int x0 = std::max(0, (int)floorf(minx) - 1), x1 = std::min(t.W - 1, (int)ceilf(maxx) + 1);
        const int y0 = std::max(0, (int)floorf(miny) - 1), y1 = std::min(t.H - 1, (int)ceilf(maxy) + 1);
        // 6.54e: blocks wholly in front of the triangle's nearest point are skipped; rows only where all three edges touch
        for (int by = y0 >> 3; by <= (y1 >> 3); ++by)
            for (int bx = x0 >> 3; bx <= (x1 >> 3); ++bx) {
                if (t.blockFar(bx, by) * 1.0005f >= izMax) continue;
                const int ya = std::max(y0, by * 8), yb = std::min(y1, by * 8 + 7);
                for (int y = ya; y <= yb; ++y) {
                    const float py = (float)y + 0.5f;
                    int xs = std::max(x0, bx * 8), xe = std::min(x1, bx * 8 + 7);
                    for (int e = 0; e < 3; ++e) {
                        const double h = 0.5 * (fabs(A[e]) + fabs(B[e]));
                        const double err = kFloatErr * (fabs(A[e]) * ((double)xe + 1.0) + fabs(B[e]) * py + fabs(C[e]) + h);
                        edgeSpan((double)B[e] * py + C[e] + h, A[e], -err, xs, xe);
                    }
                    for (int x = xs; x <= xe; ++x) {
                        const float px = (float)x + 0.5f;
                        bool touch = true;
                        for (int e = 0; e < 3 && touch; ++e) touch = A[e] * px + B[e] * py + C[e] + 0.5f * (fabsf(A[e]) + fabsf(B[e])) >= 0;
                        if (!touch) continue;
                        if (t.inv[(size_t)y * t.W + x] * 1.0005f < izMax) return true;
                    }
                }
            }
    }
    return false;
}

// ------------------------------------------------------------------ the client's rules
struct Rules {
    float dist = 533.0f;       // draw distance
    float range = 45.0f;       // doodads drawn to 40 + range x radius (also their LOD rules)
    float occRange = 45.0f;    // doodads hide others within 40 + occRange x radius (their opaque parts)
    float targetRange = 45.0f; // doodads that are asked about: within 40 + targetRange x radius
    float lodpx = 2.0f;
    bool wide = true;
    // 6.56d: interior groups WITH portals from outside to the draw distance (Stormwind's streets / canal walls); those without
    // (unreachable: inside-only scenery, the cathedral's stand-in spires) only near. --interior-near: none far (v3)
    bool interiorFar = true;
};

uint8_t lodForError(float dn, const float err[2], float scale, const Rules& o) {   // world.h w3d_lod_for_error
    const float ppr = o.wide ? 416.0f : 208.0f;
    const float lim = dn * std::max(o.lodpx, 0.1f) / ppr;
    return err[1] > 0 && err[1] * scale <= lim ? 2 : err[0] > 0 && err[0] * scale <= lim ? 1 : 0;
}
uint8_t lodForDist(float d2, float r) {   // world.h w3d_lod_for
    const float r1 = r * 12.0f, r2 = r * 30.0f;
    return d2 > r2 * r2 ? 2 : d2 > r1 * r1 ? 1 : 0;
}

void worldVert(const Inst& in, const W3dModelVertex& q, float out[3]) {
    const W3dModelHeader* h = in.m->h;
    const float l[3] = {q.x * h->posScale[0] + h->posOffset[0], q.y * h->posScale[1] + h->posOffset[1], q.z * h->posScale[2] + h->posOffset[2]};
    for (int r = 0; r < 3; ++r) out[r] = in.pos[r] + in.scale * (in.R[r][0] * l[0] + in.R[r][1] * l[1] + in.R[r][2] * l[2]);
}
void terrVert(const W3dTerrainHeader* h, const W3dTerrainVertex& q, float out[3]) {
    out[0] = h->origin[0] + q.x * h->posScale[0] + h->posOffset[0];
    out[1] = h->origin[1] + q.y * h->posScale[1] + h->posOffset[1];
    out[2] = h->origin[2] + q.z * h->posScale[2] + h->posOffset[2];
}
bool frontFacing(const float a[3], const float b[3], const float c[3], const float eye[3], float sign) {
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
    return sign * (n[0] * (eye[0] - a[0]) + n[1] * (eye[1] - a[1]) + n[2] * (eye[2] - a[2])) > 0;
}
void submeshRange(const ModelData& m, uint32_t s, uint8_t lod, uint32_t& first, uint32_t& count) {
    first = m.subs[s].firstIndex; count = m.subs[s].indexCount;
    if (lod && m.lods)
        for (int k = lod; k >= 1; --k)
            if (m.lods[s].indexCount[k - 1]) { first = m.lods[s].firstIndex[k - 1]; count = m.lods[s].indexCount[k - 1]; break; }
}
// Is the point inside the (turned, scaled) box of the instance's model?
bool insideBox(const Inst& in, const float p[3], float pad) {
    const float d[3] = {p[0] - in.pos[0], p[1] - in.pos[1], p[2] - in.pos[2]};
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > (in.radius + pad) * (in.radius + pad)) return false;
    const float s = in.scale > 0 ? in.scale : 1.0f;
    for (int k = 0; k < 3; ++k) {
        const float l = (in.R[0][k] * d[0] + in.R[1][k] * d[1] + in.R[2][k] * d[2]) / s;   // R^T d
        if (l < in.m->h->bmin[k] - pad || l > in.m->h->bmax[k] + pad) return false;
    }
    return true;
}
// The client's portal walk culls (and draws reached indoor groups to radius + 70) when the camera is in an indoor group with portals
// and in no outdoor group of the same building; returns that building's key, or ~0u.
uint32_t insideKey(const Scene& sc, const float p[3], const std::vector<uint32_t>* cand = nullptr) {   // (cand: 6.54e, all near p)
    const size_t n = cand ? cand->size() : sc.inst.size();
    auto at = [&](size_t k) -> const Inst& { return sc.inst[cand ? (*cand)[k] : k]; };
    uint32_t key = ~0u;
    for (size_t k = 0; k < n; ++k) {
        const Inst& in = at(k);
        if (in.kind == kInterior && in.m->portals && insideBox(in, p, 0.0f)) { key = in.bkey; break; }
    }
    if (key == ~0u) return key;
    for (size_t k = 0; k < n; ++k) {
        const Inst& in = at(k);
        if (in.kind == kExterior && in.bkey == key && in.m->portals && insideBox(in, p, 0.0f)) return ~0u;   // a doorway
    }
    return key;
}

struct Pick { bool drawn = false, occluder = false, target = false; uint8_t lod = 0; };
Pick pickInstance(const Inst& in, const View& v, const Rules& o, uint32_t inside) {
    Pick p;
    const ModelData& m = *in.m;
    const float dx = in.pos[0] - v.cam[0], dy = in.pos[1] - v.cam[1], dz = in.pos[2] - v.cam[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (in.kind == kDoodad) {
        const float far = 40.0f + std::max({o.range, o.occRange, o.targetRange}) * in.radius;
        if (d2 > far * far || !v.sphere(in.pos, in.radius)) return p;
        const float dr = 40.0f + o.range * in.radius, oc = 40.0f + o.occRange * in.radius, tg = 40.0f + o.targetRange * in.radius;
        p.drawn = d2 <= dr * dr;
        p.occluder = d2 <= oc * oc;
        p.target = d2 <= tg * tg;
    } else {
        // 6.57f: a sealed interior group (no portals): only from inside its own box, as the client
        if (in.sealed && !insideBox(in, v.cam, 1.0f)) return p;
        // 6.56: an interior group from outside to the draw distance, as the client now (WoWee: no culling outdoors)
        float maxD = in.kind == kExterior || (o.interiorFar && in.m->portals && in.linked) ? v.maxDist + in.radius : in.radius + 25.0f;
        if (in.kind == kInterior && inside != ~0u && in.bkey == inside) maxD = in.radius + 70.0f;
        if (d2 > maxD * maxD || !v.sphere(in.pos, in.radius)) return p;
        p.drawn = p.occluder = true;
    }
    if (!p.drawn && !p.occluder && !p.target) return p;
    if (m.lods && m.lodErr[0] > 0) p.lod = lodForError(std::max(0.0f, sqrtf(d2) - in.radius), m.lodErr, in.scale > 0 ? in.scale : 1.0f, o);
    else p.lod = m.lods ? lodForDist(d2, in.radius) : 0;
    return p;
}

// 6.54e: the instances that can be picked from a point (pickInstance's distances), by 32-yd cells: an eye gathers its own once
// instead of every face testing the whole scene (tens of thousands of doodads around Stormwind)
struct CandGrid {
    float x0 = 0, y0 = 0, cs = 32.0f;
    int nx = 0, ny = 0;
    std::vector<std::vector<uint32_t>> cell;
    std::vector<float> reach, cellReach;
    float maxReach = 0;
    void build(const Scene& sc, const Rules& o) {
        float a[2] = {1e30f, 1e30f}, b[2] = {-1e30f, -1e30f};
        reach.resize(sc.inst.size());
        for (size_t i = 0; i < sc.inst.size(); ++i) {
            const Inst& in = sc.inst[i];
            for (int k = 0; k < 2; ++k) { a[k] = std::min(a[k], in.pos[k]); b[k] = std::max(b[k], in.pos[k]); }
            reach[i] = (in.kind == kDoodad ? 40.0f + std::max({o.range, o.occRange, o.targetRange}) * in.radius
                        : in.kind == kExterior || (o.interiorFar && sc.inst[i].m->portals && sc.inst[i].linked) ? o.dist + in.radius : in.radius + 70.0f) + 1.0f;
        }
        if (sc.inst.empty()) return;
        x0 = a[0]; y0 = a[1];
        nx = (int)((b[0] - a[0]) / cs) + 1; ny = (int)((b[1] - a[1]) / cs) + 1;
        cell.assign((size_t)nx * ny, {});
        cellReach.assign((size_t)nx * ny, -1.0f);
        for (size_t i = 0; i < sc.inst.size(); ++i) {
            const Inst& in = sc.inst[i];
            const int cx = std::min(nx - 1, (int)((in.pos[0] - x0) / cs)), cy = std::min(ny - 1, (int)((in.pos[1] - y0) / cs));
            const size_t c = (size_t)cy * nx + cx;
            cell[c].push_back((uint32_t)i);
            const float ox = in.pos[0] - (x0 + (cx + 0.5f) * cs), oy = in.pos[1] - (y0 + (cy + 0.5f) * cs);
            cellReach[c] = std::max(cellReach[c], reach[i] + sqrtf(ox * ox + oy * oy));
            maxReach = std::max(maxReach, cellReach[c]);
        }
    }
    void gather(const Scene& sc, const float p[3], std::vector<uint32_t>& out) const {
        out.clear();
        if (cell.empty()) return;
        // (only the cells a reach can come from: several towns far apart make a wide grid)
        const int cx0 = std::max(0, (int)floorf((p[0] - maxReach - x0) / cs)), cx1 = std::min(nx - 1, (int)floorf((p[0] + maxReach - x0) / cs));
        const int cy0 = std::max(0, (int)floorf((p[1] - maxReach - y0) / cs)), cy1 = std::min(ny - 1, (int)floorf((p[1] + maxReach - y0) / cs));
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx) {
                const size_t c = (size_t)cy * nx + cx;
                if (cell[c].empty()) continue;
                const float dx = p[0] - (x0 + (cx + 0.5f) * cs), dy = p[1] - (y0 + (cy + 0.5f) * cs);
                if (dx * dx + dy * dy > cellReach[c] * cellReach[c]) continue;
                for (uint32_t i : cell[c]) {
                    const Inst& in = sc.inst[i];
                    const float ex = in.pos[0] - p[0], ey = in.pos[1] - p[1], ez = in.pos[2] - p[2];
                    if (ex * ex + ey * ey + ez * ez <= reach[i] * reach[i]) out.push_back(i);
                }
            }
        std::sort(out.begin(), out.end());   // (the scene's order, as without the grid)
    }
};

// What one view sees. detail: pixel counts of everything drawn (the report); else only ids seen (the PVS), with early outs.
struct EyeResult {
    std::vector<uint8_t> drawn, lod;   // per instance (detail)
    std::vector<uint32_t> pix;         // per instance (detail)
    std::vector<uint8_t> tbDrawn;      // per ground batch (detail)
    std::vector<uint32_t> tbPix;       // per ground batch (detail)
};

template <class F>
void forTris(const Inst& in, uint32_t s, uint8_t lod, F&& fn) {
    const ModelData& m = *in.m;
    const W3dModelVertex* mv = reinterpret_cast<const W3dModelVertex*>(m.blob.data() + m.h->offVertices);
    const uint16_t* mi = reinterpret_cast<const uint16_t*>(m.blob.data() + m.h->offIndices);
    uint32_t first, count;
    submeshRange(m, s, lod, first, count);
    for (uint32_t k = first; k + 2 < first + count; k += 3) {
        float w[3][3];
        for (int q = 0; q < 3; ++q) worldVert(in, mv[mi[k + q]], w[q]);
        if (!fn(w)) return;
    }
}

// The sphere's nearest point is behind everything already drawn where it would show: it cannot be seen (a cheap test first)
bool sphereHidden(const Target& t, const View& v, const float c[3], float r, int pad = 0) {
    float vc[3];
    v.toView(c, vc);
    if (vc[2] - r <= kNear * 1.01f) return false;
    const float zn = vc[2] - r, zf = vc[2] + r;
    const float kx = 0.5f * (float)t.W / v.tanH, ky = 0.5f * (float)t.H / v.tanV;
    const float xa = std::min((vc[0] - r) / zn, (vc[0] - r) / zf), xb = std::max((vc[0] + r) / zn, (vc[0] + r) / zf);
    const float ya = std::min((vc[1] - r) / zn, (vc[1] - r) / zf), yb = std::max((vc[1] + r) / zn, (vc[1] + r) / zf);
    // (pad: extra pixels around, if ever wanted)
    const int x0 = std::max(0, (int)floorf(0.5f * t.W + xa * kx) - pad), x1 = std::min(t.W - 1, (int)ceilf(0.5f * t.W + xb * kx) + pad);
    const int y0 = std::max(0, (int)floorf(0.5f * t.H - yb * ky) - pad), y1 = std::min(t.H - 1, (int)ceilf(0.5f * t.H - ya * ky) + pad);
    if (x0 > x1 || y0 > y1) return true;   // off the screen
    const float invNear = 1.0f / zn;
    for (int by = y0 >> 3; by <= (y1 >> 3); ++by)   // 6.54e: by blocks first
        for (int bx = x0 >> 3; bx <= (x1 >> 3); ++bx) {
            if (t.blockFar(bx, by) * 1.0005f >= invNear) continue;
            const int ya2 = std::max(y0, by * 8), yb2 = std::min(y1, by * 8 + 7), xa2 = std::max(x0, bx * 8), xb2 = std::min(x1, bx * 8 + 7);
            for (int y = ya2; y <= yb2; ++y)
                for (int x = xa2; x <= xb2; ++x)
                    if (t.inv[(size_t)y * t.W + x] * 1.0005f < invNear) return false;
        }
    return true;
}

// Renders one view. seen (not detail): marks seen doodads (instance index) and ground batches (kGround | index).
void renderEye(const Scene& sc, const View& v, const Rules& o, uint32_t inside, Target& t, bool detail, EyeResult* er,
               std::vector<uint32_t>* seen, const std::vector<uint32_t>* cand = nullptr) {
    t.clear(v.W, v.H);
    const size_t ni = sc.inst.size();
    if (detail) {
        er->drawn.assign(ni, 0); er->lod.assign(ni, 0); er->pix.assign(ni, 0);
        er->tbDrawn.assign(sc.tb.size(), 0); er->tbPix.assign(sc.tb.size(), 0);
    }
    // 6.54e: everything that writes the picture, nearest first; one whose sphere is already hidden is not drawn (it would leave no
    // pixel: what is behind stays behind) - Stormwind's buildings behind its walls cost nothing then
    struct Occ { float key; uint32_t i; uint8_t lod; bool ground; };
    std::vector<Occ> occ;
    struct Later { uint32_t i; uint8_t lod; };
    std::vector<Later> later;
    for (size_t bi = 0; bi < sc.tb.size(); ++bi) {
        const TB& b = sc.tb[bi];
        if (!v.sphere(b.c, b.rad)) continue;
        if (detail) er->tbDrawn[bi] = 1;
        const float dx = b.c[0] - v.cam[0], dy = b.c[1] - v.cam[1], dz = b.c[2] - v.cam[2];
        occ.push_back({sqrtf(dx * dx + dy * dy + dz * dz) - b.rad, (uint32_t)bi, 0, true});
    }
    for (size_t ci = 0, cn = cand ? cand->size() : ni; ci < cn; ++ci) {
        const size_t i = cand ? (*cand)[ci] : ci;
        const Inst& in = sc.inst[i];
        const Pick p = pickInstance(in, v, o, inside);
        if (!p.drawn && !p.occluder && !p.target) continue;
        if (detail && p.drawn) { er->drawn[i] = 1; er->lod[i] = p.lod; }
        if (p.occluder) {
            const float dx = in.pos[0] - v.cam[0], dy = in.pos[1] - v.cam[1], dz = in.pos[2] - v.cam[2];
            occ.push_back({sqrtf(dx * dx + dy * dy + dz * dz) - in.radius, (uint32_t)i, p.lod, false});
        }
        if (in.kind == kDoodad && (detail ? p.drawn : p.target)) later.push_back({(uint32_t)i, p.lod});
        else if (!detail && in.kind != kDoodad && p.drawn) later.push_back({(uint32_t)i, p.lod});   // 6.54d: WMO groups are in the PVS too
    }
    std::sort(occ.begin(), occ.end(), [](const Occ& a, const Occ& b) { return a.key < b.key || (a.key == b.key && a.ground > b.ground); });
    for (const Occ& oc : occ) {
        if (!oc.ground) {
            const Inst& in = sc.inst[oc.i];
            if (sphereHidden(t, v, in.pos, in.radius)) continue;
            const ModelData& m = *in.m;
            for (uint32_t s = 0; s < m.h->submeshCount; ++s) {
                if (m.subs[s].blend != W3D_BLEND_OPAQUE) continue;
                const bool cull = !(m.subs[s].flags & W3D_SUBF_TWO_SIDED);
                forTris(in, s, oc.lod, [&](float (*w)[3]) {
                    if (cull && !frontFacing(w[0], w[1], w[2], v.cam, sc.modelSign)) return true;
                    float vv[3][3];
                    for (int q = 0; q < 3; ++q) v.toView(w[q], vv[q]);
                    rasterTri(t, v, vv[0], vv[1], vv[2], oc.i + 1, true);
                    return true;
                });
            }
            continue;
        }
        const size_t bi = oc.i;
        const TB& b = sc.tb[bi];
        if (sphereHidden(t, v, b.c, b.rad)) continue;
        const TileData& td = sc.tiles[b.tile];
        const W3dTerrainHeader* h = td.th;
        const W3dTerrainVertex* tv = reinterpret_cast<const W3dTerrainVertex*>(td.terr.data() + h->offVertices);
        const uint16_t* ix = reinterpret_cast<const uint16_t*>(td.terr.data() + h->offIndices);
        const W3dTerrainBatch& bt = reinterpret_cast<const W3dTerrainBatch*>(td.terr.data() + h->offBatches)[b.b];
        const float dx = b.c[0] - v.cam[0], dy = b.c[1] - v.cam[1], dz = b.c[2] - v.cam[2];
        const float dn = sqrtf(dx * dx + dy * dy + dz * dz) - b.rad;
        const int lod = dn < 40.0f ? 0 : dn < 140.0f ? 1 : 2;   // (the client's W3D_TERRAIN_LOD1 / LOD2_DIST)
        const uint32_t first = bt.firstIndex[lod], count = bt.indexCount[lod];
        for (uint32_t k = first; k + 2 < first + count; k += 3) {
            float w[3][3], vv[3][3];
            for (int q = 0; q < 3; ++q) { terrVert(h, tv[ix[k + q]], w[q]); v.toView(w[q], vv[q]); }
            if (!frontFacing(w[0], w[1], w[2], v.cam, sc.windSign)) continue;
            rasterTri(t, v, vv[0], vv[1], vv[2], kGround | (uint32_t)bi, true);
        }
    }
    // what is seen of the written ones
    // (6.54e: kept per thread and cleared where set - not the whole scene's size per face)
    thread_local std::vector<uint8_t> mark, tbMark;
    thread_local std::vector<uint32_t> setList;
    if (seen) {
        if (mark.size() < ni) mark.assign(ni, 0);
        if (tbMark.size() < sc.tb.size()) tbMark.assign(sc.tb.size(), 0);
        setList.clear();
    }
    for (size_t q = 0; q < t.id.size(); ++q) {
        const uint32_t id = t.id[q];
        if (!id) continue;
        if (id & kGround) {
            if (detail) ++er->tbPix[id & ~kGround];
            if (seen && !tbMark[id & ~kGround]) { tbMark[id & ~kGround] = 1; setList.push_back(id); }
        } else {
            if (detail) ++er->pix[id - 1];
            if (seen && !mark[id - 1]) { mark[id - 1] = 1; setList.push_back(id); }
        }
    }
    // (the PVS) the ground batches in view that left no pixel: the conservative test against the finished picture
    if (seen && !detail)
        for (size_t bi = 0; bi < sc.tb.size(); ++bi) {
            if (tbMark[bi]) continue;
            const TB& b = sc.tb[bi];
            if (!v.sphere(b.c, b.rad) || sphereHidden(t, v, b.c, b.rad)) continue;   // (6.54e: hidden whole)
            const TileData& td = sc.tiles[b.tile];
            const W3dTerrainHeader* h = td.th;
            const W3dTerrainVertex* tv = reinterpret_cast<const W3dTerrainVertex*>(td.terr.data() + h->offVertices);
            const uint16_t* ix = reinterpret_cast<const uint16_t*>(td.terr.data() + h->offIndices);
            const W3dTerrainBatch& bt = reinterpret_cast<const W3dTerrainBatch*>(td.terr.data() + h->offBatches)[b.b];
            const float dx = b.c[0] - v.cam[0], dy = b.c[1] - v.cam[1], dz = b.c[2] - v.cam[2];
            const float dn = sqrtf(dx * dx + dy * dy + dz * dz) - b.rad;
            const int lod = dn < 40.0f ? 0 : dn < 140.0f ? 1 : 2;
            for (uint32_t k = bt.firstIndex[lod]; k + 2 < bt.firstIndex[lod] + bt.indexCount[lod] && !tbMark[bi]; k += 3) {
                float w[3][3], vv[3][3];
                for (int q = 0; q < 3; ++q) { terrVert(h, tv[ix[k + q]], w[q]); v.toView(w[q], vv[q]); }
                if (!frontFacing(w[0], w[1], w[2], v.cam, sc.windSign)) continue;
                if (conservativeSeen(t, v, vv[0], vv[1], vv[2])) { tbMark[bi] = 1; setList.push_back(kGround | (uint32_t)bi); }
            }
        }
    // the doodads (6.54d: and WMO groups) asked about: every part tested (a cut-out / blended part, or an opaque one that left no pixel)
    for (const Later& l : later) {
        const Inst& in = sc.inst[l.i];
        if (seen && mark[l.i]) continue;
        if (!detail && sphereHidden(t, v, in.pos, in.radius)) continue;
        const ModelData& m = *in.m;
        const Pick p = pickInstance(in, v, o, inside);
        uint32_t px = 0;
        for (uint32_t s = 0; s < m.h->submeshCount && (detail || !px); ++s) {
            if (detail && m.subs[s].blend == W3D_BLEND_OPAQUE && p.occluder) continue;   // (written above: counted already)
            const bool cull = m.subs[s].blend == W3D_BLEND_OPAQUE && !(m.subs[s].flags & W3D_SUBF_TWO_SIDED);
            forTris(in, s, l.lod, [&](float (*w)[3]) {
                if (cull && !frontFacing(w[0], w[1], w[2], v.cam, sc.modelSign)) return true;
                float vv[3][3];
                for (int q = 0; q < 3; ++q) v.toView(w[q], vv[q]);
                if (detail) px += rasterTri(t, v, vv[0], vv[1], vv[2], 0, false);
                else if (conservativeSeen(t, v, vv[0], vv[1], vv[2])) px = 1;
                return detail || px == 0;
            });
        }
        if (detail) er->pix[l.i] += px;
        if (seen && px) { mark[l.i] = 1; setList.push_back(l.i + 1); }
    }
    if (seen)   // (6.54d: doodads and WMO groups by instance index, ground batches as kGround | index); the marks cleared again
        for (uint32_t id : setList) {
            if (id & kGround) { seen->push_back(id); tbMark[id & ~kGround] = 0; }
            else { seen->push_back(id - 1); mark[id - 1] = 0; }
        }
}

// Could something past the draw distance show at this pixel of a view: nothing nearer than farR there, and the direction low
// enough for the far ground / far buildings (their tops: sinLim, farSinLim) to reach it - 6.54d
bool farPixel(const Target& t, const View& v, int x, int y, float farR, float sinLim) {
    const size_t o = (size_t)y * t.W + x;
    const float xn = ((float)x + 0.5f) / (float)t.W * 2.0f - 1.0f, yn = 1.0f - ((float)y + 0.5f) / (float)t.H * 2.0f;
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = v.f[k] + v.r[k] * xn * v.tanH + v.u[k] * yn * v.tanV;
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (d[2] / len > sinLim) return false;   // (above the far things' tops)
    return !t.id[o] || t.inv[o] <= 0 || len / t.inv[o] > farR;
}

// 6.54e: the far things around (the far ground's tiles, the far buildings), to know how high over the horizon they can reach
// nsub (6.54e): the next nsub boxes are its parts (a tile's 133-yd blocks, from its exact heights): looked at only when the
// whole box could raise the limit
struct FarBox { float x0, y0, x1, y1, top; uint32_t nsub = 0; };
// The highest a far thing can show from p, as the sine of its angle over the horizon: each at its real distance (but no nearer than
// farR: what is nearer is drawn anyway). -2: nothing far around.
float farSinLim(const std::vector<FarBox>& fb, const float p[3], float farR) {
    float s = -2.0f;
    auto sinOf = [&](const FarBox& b) {
        const float gx = std::max({0.0f, b.x0 - p[0], p[0] - b.x1}), gy = std::max({0.0f, b.y0 - p[1], p[1] - b.y1});
        const float fx = std::max(fabsf(p[0] - b.x0), fabsf(p[0] - b.x1)), fy = std::max(fabsf(p[1] - b.y0), fabsf(p[1] - b.y1));
        if (fx * fx + fy * fy <= farR * farR) return -2.0f;   // (wholly nearer than farR)
        const float d = std::max(sqrtf(gx * gx + gy * gy), farR), h = b.top - p[2];
        return h / sqrtf(h * h + d * d);
    };
    for (size_t i = 0; i < fb.size(); i += 1 + fb[i].nsub) {
        const float whole = sinOf(fb[i]);
        if (whole <= s) continue;
        if (!fb[i].nsub) { s = whole; continue; }
        for (size_t k = i + 1; k <= i + fb[i].nsub; ++k) s = std::max(s, sinOf(fb[k]));
    }
    return s;
}

// All directions from a point: six 90-degree faces. farSeen (6.54d): could anything past farR be seen from here (below the far
// things' tops)?
void renderCube(const Scene& sc, const float eye[3], const Rules& o, int res, Target& t, std::vector<uint32_t>& out, float farR = 0,
                const std::vector<FarBox>* far = nullptr, bool* farSeen = nullptr, const CandGrid* grid = nullptr) {
    thread_local std::vector<uint32_t> cand;
    if (grid) grid->gather(sc, eye, cand);
    const uint32_t inside = insideKey(sc, eye, grid ? &cand : nullptr);
    static const float faces[6][2] = {{0, 0}, {90, 0}, {180, 0}, {270, 0}, {0, 90}, {0, -90}};
    // 6.54e: each face's pixel directions (the same for every eye): the sine over the horizon and the length per unit of view z
    thread_local int tabRes = 0;
    thread_local std::vector<float> tabSin[6], tabLen[6];
    out.clear();
    if (farSeen) *farSeen = false;
    const float sinLim = farSeen && far ? farSinLim(*far, eye, farR) : -2.0f;
    for (int fi = 0; fi < 6; ++fi) {
        const auto& fc = faces[fi];
        View v;
        v.build(eye, fc[0], fc[1], o.dist, res, res, 1.0f, 1.0f, 1.0f);
        renderEye(sc, v, o, inside, t, false, nullptr, &out, grid ? &cand : nullptr);
        if (!farSeen || *farSeen || sinLim <= -1.5f) continue;
        if (tabRes != res) {
            for (int k = 0; k < 6; ++k) {
                View w;
                w.build(eye, faces[k][0], faces[k][1], o.dist, res, res, 1.0f, 1.0f, 1.0f);
                tabSin[k].resize((size_t)res * res);
                tabLen[k].resize((size_t)res * res);
                for (int y = 0; y < res; ++y)
                    for (int x = 0; x < res; ++x) {
                        const float xn = ((float)x + 0.5f) / (float)res * 2.0f - 1.0f, yn = 1.0f - ((float)y + 0.5f) / (float)res * 2.0f;
                        float d[3];
                        for (int q = 0; q < 3; ++q) d[q] = w.f[q] + w.r[q] * xn * w.tanH + w.u[q] * yn * w.tanV;
                        const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        tabSin[k][(size_t)y * res + x] = d[2] / len;
                        tabLen[k][(size_t)y * res + x] = len;
                    }
            }
            tabRes = res;
        }
        const float* ts = tabSin[fi].data();
        const float* tl = tabLen[fi].data();
        for (size_t q = 0, n = (size_t)res * res; q < n; ++q) {   // (as farPixel)
            if (ts[q] > sinLim) continue;
            if (!t.id[q] || t.inv[q] <= 0 || tl[q] > farR * t.inv[q]) { *farSeen = true; break; }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// ------------------------------------------------------------------ loading
// boxes (6.54e): instead of the one box, tiles within reach of any of these (x0, y0, x1, y1) - several towns far apart
bool loadScene(Packs& packs, Scene& sc, float x0, float y0, float x1, float y1, float reach,
               const std::vector<std::array<float, 4>>* boxes = nullptr) {
    std::vector<uint8_t> buf;
    for (const W3dEntry& e : packs.files[0]->idx) {
        if (e.type != W3D_ENTRY_TERRAIN) continue;
        if (!packs.files[0]->read(e, buf, sizeof(W3dTerrainHeader))) continue;
        W3dTerrainHeader th;
        std::memcpy(&th, buf.data(), std::min(sizeof th, buf.size()));
        // distance from the box [x0,x1]x[y0,y1] to the tile's square
        auto nearBox = [&](float a0, float b0, float a1, float b1) {
            const float gx = std::max({0.0f, th.origin[0] - a1, a0 - (th.origin[0] + kTile)});
            const float gy = std::max({0.0f, th.origin[1] - b1, b0 - (th.origin[1] + kTile)});
            return gx * gx + gy * gy <= reach * reach;
        };
        bool want = !boxes && nearBox(x0, y0, x1, y1);
        if (boxes) for (const auto& b : *boxes) want = want || nearBox(b[0], b[1], b[2], b[3]);
        if (!want) continue;
        TileData td;
        td.tx = (int)th.tileX; td.ty = (int)th.tileY;
        if (!packs.files[0]->read(e, td.terr)) continue;
        td.th = reinterpret_cast<const W3dTerrainHeader*>(td.terr.data());
        sc.tiles.push_back(std::move(td));
    }
    if (sc.tiles.empty()) return false;
    for (size_t ti = 0; ti < sc.tiles.size(); ++ti) {
        const W3dTerrainHeader* h = sc.tiles[ti].th;
        const W3dTerrainBatch* bt = reinterpret_cast<const W3dTerrainBatch*>(sc.tiles[ti].terr.data() + h->offBatches);
        for (uint32_t b = 0; b < h->batchCount; ++b) {
            TB x;
            x.tile = (int)ti; x.b = b;
            float r2 = 0;
            for (int k = 0; k < 3; ++k) {
                x.c[k] = h->origin[k] + 0.5f * (bt[b].bmin[k] + bt[b].bmax[k]);
                const float e = 0.5f * (bt[b].bmax[k] - bt[b].bmin[k]);
                r2 += e * e;
            }
            x.rad = sqrtf(r2);
            sc.tb.push_back(x);
        }
    }
    {   // the ground's winding (front = up)
        double up = 0, down = 0;
        for (const TileData& td : sc.tiles) {
            const W3dTerrainHeader* h = td.th;
            const W3dTerrainVertex* tv = reinterpret_cast<const W3dTerrainVertex*>(td.terr.data() + h->offVertices);
            const uint16_t* ix = reinterpret_cast<const uint16_t*>(td.terr.data() + h->offIndices);
            for (uint32_t k = 0; k + 2 < h->indexCount && k < 30000; k += 3) {
                float w[3][3];
                for (int q = 0; q < 3; ++q) terrVert(h, tv[ix[k + q]], w[q]);
                const float nz = (w[1][0] - w[0][0]) * (w[2][1] - w[0][1]) - (w[1][1] - w[0][1]) * (w[2][0] - w[0][0]);
                (nz > 0 ? up : down) += 1;
            }
        }
        sc.windSign = up >= down ? 1.0f : -1.0f;
    }
    std::map<std::vector<float>, uint32_t> bkeys;
    for (size_t ti = 0; ti < sc.tiles.size(); ++ti) {
        const uint32_t tid = w3d_tile_id((uint32_t)sc.tiles[ti].tx, (uint32_t)sc.tiles[ti].ty);
        if (!packs.read(W3D_ENTRY_PLACEMENT, tid, buf) || buf.size() < sizeof(W3dPlacementHeader)) continue;
        W3dPlacementHeader ph;
        std::memcpy(&ph, buf.data(), sizeof ph);
        const size_t n = std::min<size_t>(ph.count, (buf.size() - sizeof ph) / sizeof(W3dPlacement));
        std::vector<W3dPlacement> pl(n);
        std::memcpy(pl.data(), buf.data() + sizeof ph, n * sizeof(W3dPlacement));
        for (size_t pi = 0; pi < n; ++pi) {
            const W3dPlacement& p = pl[pi];
            ++sc.placements;
            ModelData& m = sc.models[p.modelId];
            if (!m.ok && m.blob.empty()) {
                if (packs.read(W3D_ENTRY_MODEL, p.modelId, m.blob) && m.blob.size() >= sizeof(W3dModelHeader)) {
                    m.h = reinterpret_cast<const W3dModelHeader*>(m.blob.data());
                    const uint64_t subEnd = (uint64_t)m.h->offSubmeshes + (uint64_t)m.h->submeshCount * sizeof(W3dSubmesh);
                    const uint64_t vEnd = (uint64_t)m.h->offVertices + (uint64_t)m.h->vertexCount * sizeof(W3dModelVertex);
                    const uint64_t iEnd = (uint64_t)m.h->offIndices + (uint64_t)m.h->indexCount * 2;
                    if (subEnd <= m.blob.size() && vEnd <= m.blob.size() && iEnd <= m.blob.size()) {
                        m.subs = reinterpret_cast<const W3dSubmesh*>(m.blob.data() + m.h->offSubmeshes);
                        m.lods = w3d_model_lods(m.blob.data(), (uint32_t)m.blob.size());
                        m.building = m.h->submeshCount && (m.subs[0].flags & W3D_SUBF_BUILDING);
                        m.interior = m.h->submeshCount && (m.subs[0].flags & W3D_SUBF_INTERIOR);
                        m.portals = m.h->offTexIds >= W3D_MODEL_HDR_PORTAL_END && m.h->offSubmeshes >= W3D_MODEL_HDR_PORTAL_END && m.h->offPortals;
                        m.lodErr[0] = w3d_model_lod_err(m.h, 1);
                        m.lodErr[1] = w3d_model_lod_err(m.h, 2);
                        m.ok = true;
                        const uint16_t* ix = reinterpret_cast<const uint16_t*>(m.blob.data() + m.h->offIndices);
                        for (uint32_t k = 0; k < m.h->indexCount && m.ok; ++k) m.ok = ix[k] < m.h->vertexCount;
                        for (uint32_t s = 0; s < m.h->submeshCount && m.ok; ++s)
                            m.ok = (uint64_t)m.subs[s].firstIndex + m.subs[s].indexCount <= m.h->indexCount;
                    }
                }
                if (m.blob.empty()) m.blob.push_back(0);   // (tried)
            }
            if (!m.ok) { ++sc.missing; continue; }
            Inst in;
            in.m = &m;
            std::memcpy(in.pos, p.pos, sizeof in.pos);
            std::memcpy(in.rot, p.rot, sizeof in.rot);
            const float cx = cosf(p.rot[0]), sx = sinf(p.rot[0]), cy = cosf(p.rot[1]), sy = sinf(p.rot[1]);
            const float cz = cosf(p.rot[2]), sz = sinf(p.rot[2]);
            const float R[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                                   {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                                   {-sy, cy * sx, cy * cx}};
            std::memcpy(in.R, R, sizeof R);
            in.scale = p.scale;
            in.radius = m.h->radius * p.scale;
            in.tile = (int)ti;
            in.pl = (uint32_t)pi;
            in.kind = m.interior ? kInterior : m.building ? kExterior : kDoodad;
            in.modelId = p.modelId;
            const std::vector<float> key = {p.rot[0], p.rot[1], p.rot[2], p.scale};
            auto it = bkeys.find(key);
            in.bkey = it != bkeys.end() ? it->second : (bkeys[key] = (uint32_t)bkeys.size());
            if (in.radius <= 0) continue;
            sc.inst.push_back(in);
        }
    }
    {   // 6.57c: the portal graph of the buildings, as the client's (wmoportal::buildGraph): which interior groups are linked to the outside
        std::vector<wmoportal::Node> nodes;
        std::vector<size_t> of;
        for (size_t i = 0; i < sc.inst.size(); ++i) {
            const Inst& in = sc.inst[i];
            if (in.kind == kDoodad || !in.m->portals) continue;
            const W3dPortalTable* pt = w3d_model_portals(in.m->blob.data(), (uint32_t)in.m->blob.size());
            if (!pt) continue;
            wmoportal::Node n;
            n.modelId = in.modelId;
            n.portals = w3d_portals_of(pt);
            n.nPortals = pt->count;
            n.hasTable = true;
            n.interior = in.kind == kInterior;
            std::memcpy(n.pos, in.pos, sizeof n.pos);
            std::memcpy(n.rot, in.rot, sizeof n.rot);
            std::memcpy(n.bmin, in.m->h->bmin, sizeof n.bmin);
            std::memcpy(n.bmax, in.m->h->bmax, sizeof n.bmax);
            n.radius = in.radius;
            n.inst = (uint32_t)i;
            nodes.push_back(std::move(n));
            of.push_back(i);
        }
        wmoportal::buildGraph(nodes);
        for (size_t k = 0; k < nodes.size(); ++k) sc.inst[of[k]].linked = nodes[k].outdoorLinked;
        // (6.57g: the client decides by the pack - any model with a portal table loaded - not per tile)
        bool packHasPortals = false;
        for (const Inst& in : sc.inst) packHasPortals = packHasPortals || (in.kind != kDoodad && in.m->portals);
        for (Inst& in : sc.inst) in.sealed = in.kind == kInterior && !in.m->portals && packHasPortals;
    }
    {   // models' winding against their normals
        double agree = 0, total = 0;
        for (const auto& [id, m] : sc.models) {
            if (!m.ok) continue;
            const W3dModelVertex* mv = reinterpret_cast<const W3dModelVertex*>(m.blob.data() + m.h->offVertices);
            const uint16_t* mi = reinterpret_cast<const uint16_t*>(m.blob.data() + m.h->offIndices);
            for (uint32_t k = 0; k + 2 < m.h->indexCount && k < 600; k += 3) {
                float l[3][3], nn[3] = {0, 0, 0};
                for (int q = 0; q < 3; ++q) {
                    const W3dModelVertex& x = mv[mi[k + q]];
                    l[q][0] = x.x * m.h->posScale[0]; l[q][1] = x.y * m.h->posScale[1]; l[q][2] = x.z * m.h->posScale[2];
                    nn[0] += x.nx; nn[1] += x.ny; nn[2] += x.nz;
                }
                const float u[3] = {l[1][0] - l[0][0], l[1][1] - l[0][1], l[1][2] - l[0][2]}, w[3] = {l[2][0] - l[0][0], l[2][1] - l[0][1], l[2][2] - l[0][2]};
                const float c[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
                const float dp = sc.windSign * (c[0] * nn[0] + c[1] * nn[1] + c[2] * nn[2]);
                if (dp == 0) continue;
                total += 1;
                if (dp > 0) agree += 1;
            }
        }
        sc.normalsAgree = total > 0 ? agree / total : 1.0;
        sc.modelSign = sc.normalsAgree >= 0.5 ? sc.windSign : -sc.windSign;
    }
    return true;
}

// ------------------------------------------------------------------ pictures
void writePicture(const Scene& sc, const Target& t, const std::string& path) {
    std::vector<uint8_t> rgb((size_t)t.W * t.H * 3, 0);
    for (int y = 0; y < t.H; ++y)
        for (int x = 0; x < t.W; ++x) {
            const size_t o = (size_t)y * t.W + x;
            uint8_t* p = &rgb[o * 3];
            const uint32_t id = t.id[o];
            if (!id) { p[0] = 150; p[1] = 190; p[2] = 230; continue; }
            const float z = t.inv[o] > 0 ? 1.0f / t.inv[o] : 1e9f;
            const float L = std::max(0.25f, 1.0f - z / 600.0f);
            float c[3];
            if (id & kGround) { c[0] = 150; c[1] = 120; c[2] = 80; }
            else if (sc.inst[id - 1].kind != kDoodad) { c[0] = 90; c[1] = 120; c[2] = 220; }
            else { c[0] = 80; c[1] = 190; c[2] = 80; }
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::min(255.0f, c[k] * L);
        }
    stbi_write_png(path.c_str(), t.W, t.H, 3, rgb.data(), t.W * 3);
}
void writeHidden(const Scene& sc, const View& v, const Target& seen, const EyeResult& r, const std::string& path) {
    std::vector<uint8_t> rgb((size_t)seen.W * seen.H * 3, 0);
    for (size_t i = 0; i < seen.id.size(); ++i) { const uint8_t g = seen.id[i] ? 70 : 30; rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = g; }
    Target t;
    t.clear(seen.W, seen.H);
    for (size_t i = 0; i < sc.inst.size(); ++i) {
        if (!r.drawn[i] || r.pix[i]) continue;
        const Inst& in = sc.inst[i];
        for (uint32_t s = 0; s < in.m->h->submeshCount; ++s)
            forTris(in, s, r.lod[i], [&](float (*w)[3]) {
                float vv[3][3];
                for (int q = 0; q < 3; ++q) v.toView(w[q], vv[q]);
                rasterTri(t, v, vv[0], vv[1], vv[2], (uint32_t)i + 1, true);
                return true;
            });
    }
    for (size_t i = 0; i < t.id.size(); ++i)
        if (t.id[i]) {
            const bool b = sc.inst[t.id[i] - 1].kind != kDoodad;
            rgb[i * 3] = 230; rgb[i * 3 + 1] = b ? 60 : 150; rgb[i * 3 + 2] = b ? 60 : 40;
        }
    stbi_write_png(path.c_str(), seen.W, seen.H, 3, rgb.data(), seen.W * 3);
}

struct Tally {
    uint32_t inst = 0, tris = 0, draws = 0;
    void add(const Inst& in, uint8_t lod, int blendFilter = -1) {
        const ModelData& m = *in.m;
        bool any = false;
        for (uint32_t s = 0; s < m.h->submeshCount; ++s) {
            const int b = m.subs[s].blend == W3D_BLEND_ADD ? 2 : m.subs[s].blend;
            if (blendFilter >= 0 && b != blendFilter) continue;
            uint32_t first, count;
            submeshRange(m, s, lod, first, count);
            tris += count / 3;
            ++draws;
            any = true;
        }
        if (any) ++inst;
    }
};

}  // namespace

namespace {

// ------------------------------------------------------------------ PVS: floors
struct RayGrid {
    float x0 = 0, y0 = 0, cs = 4.0f;
    int nx = 0, ny = 0;
    std::vector<std::array<float, 9>> tris;
    std::vector<uint8_t> walk;   // |n.z| >= cos 50 degrees
    std::vector<std::vector<uint32_t>> bins;
    void init(float ax, float ay, float bx, float by) {
        x0 = ax; y0 = ay;
        nx = std::max(1, (int)ceilf((bx - ax) / cs));
        ny = std::max(1, (int)ceilf((by - ay) / cs));
        bins.assign((size_t)nx * ny, {});
    }
    void add(float (*w)[3]) {
        const float mnx = std::min({w[0][0], w[1][0], w[2][0]}), mxx = std::max({w[0][0], w[1][0], w[2][0]});
        const float mny = std::min({w[0][1], w[1][1], w[2][1]}), mxy = std::max({w[0][1], w[1][1], w[2][1]});
        const int bx0 = (int)floorf((mnx - x0) / cs), bx1 = (int)floorf((mxx - x0) / cs);
        const int by0 = (int)floorf((mny - y0) / cs), by1 = (int)floorf((mxy - y0) / cs);
        if (bx1 < 0 || by1 < 0 || bx0 >= nx || by0 >= ny) return;
        const float u[3] = {w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2]}, v[3] = {w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2]};
        const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-9f || fabsf(n[2]) < 1e-6f * nl) return;   // (degenerate / vertical: a ray down never hits it)
        const uint32_t id = (uint32_t)tris.size();
        tris.push_back({w[0][0], w[0][1], w[0][2], w[1][0], w[1][1], w[1][2], w[2][0], w[2][1], w[2][2]});
        walk.push_back(fabsf(n[2]) / nl >= 0.64f ? 1 : 0);
        for (int y = std::max(0, by0); y <= std::min(ny - 1, by1); ++y)
            for (int x = std::max(0, bx0); x <= std::min(nx - 1, bx1); ++x) bins[(size_t)y * nx + x].push_back(id);
    }
    // the floors at (x, y): walkable surfaces with 1.8 yd free above them, lowest first
    std::vector<float> floors(float x, float y) const {
        std::vector<float> out;
        const int bx = (int)floorf((x - x0) / cs), by = (int)floorf((y - y0) / cs);
        if (bx < 0 || by < 0 || bx >= nx || by >= ny) return out;
        std::vector<std::pair<float, bool>> hits;
        for (uint32_t id : bins[(size_t)by * nx + bx]) {
            const auto& t = tris[id];
            const float d = (t[4] - t[7]) * (t[0] - t[6]) + (t[6] - t[3]) * (t[1] - t[7]);
            if (fabsf(d) < 1e-12f) continue;
            const float a = ((t[4] - t[7]) * (x - t[6]) + (t[6] - t[3]) * (y - t[7])) / d;
            const float b = ((t[7] - t[1]) * (x - t[6]) + (t[0] - t[6]) * (y - t[7])) / d;
            const float c = 1.0f - a - b;
            if (a < -1e-5f || b < -1e-5f || c < -1e-5f) continue;
            hits.push_back({a * t[2] + b * t[5] + c * t[8], walk[id] != 0});
        }
        std::sort(hits.begin(), hits.end());
        for (size_t i = 0; i < hits.size(); ++i) {
            if (!hits[i].second) continue;
            const float z = hits[i].first;
            bool clear = true;
            for (size_t j = i + 1; j < hits.size() && clear; ++j)
                if (hits[j].first > z + 0.05f && hits[j].first < z + 1.8f) clear = false;
            if (!clear) continue;
            if (!out.empty() && z - out.back() < 0.5f) { out.back() = z; continue; }
            out.push_back(z);
        }
        return out;
    }
};

void putVarint(std::vector<uint8_t>& o, uint32_t v) {
    while (v >= 0x80) { o.push_back((uint8_t)(v | 0x80)); v >>= 7; }
    o.push_back((uint8_t)v);
}
template <class T> void putPod(std::vector<uint8_t>& o, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    o.insert(o.end(), p, p + sizeof v);
}

struct PvsCell { int tile, cx, cy; };
struct PvsLevelOut { float zmin, zmax; std::vector<uint8_t> bytes; };

// The set of one level, decoded back (tile index in the scene -> doodad PLCM indices / batch indices)
struct DecodedSet {
    std::set<std::pair<int, uint32_t>> dood, batch, bldg;   // (6.54d: bldg = WMO groups, PLCM index)
    std::set<int> tiles;   // covered tiles
    bool farHidden = true; // 6.54d: every level used says nothing past the draw distance can be seen
};
bool decodeLevel(const Scene& sc, int cellTile, const uint8_t* p, const uint8_t* end, DecodedSet& out) {
    const uint32_t flags = w3d_pvs_varint(&p, end);   // (v3)
    if (!(flags & W3D_PVS_FAR_HIDDEN)) out.farHidden = false;
    const uint32_t groups = w3d_pvs_varint(&p, end);
    for (uint32_t g = 0; g < groups; ++g) {
        if (end - p < 2) return false;
        const int dtx = (int8_t)*p++, dty = (int8_t)*p++;
        const int tx = sc.tiles[cellTile].tx + dtx, ty = sc.tiles[cellTile].ty + dty;
        int ti = -1;
        for (size_t k = 0; k < sc.tiles.size(); ++k) if (sc.tiles[k].tx == tx && sc.tiles[k].ty == ty) ti = (int)k;
        if (ti >= 0) out.tiles.insert(ti);
        for (int kind = 0; kind < 3; ++kind) {
            const uint32_t n = w3d_pvs_varint(&p, end);
            uint32_t prev = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t v = w3d_pvs_varint(&p, end);
                const uint32_t idx = i == 0 ? v : prev + 1 + v;
                prev = idx;
                if (ti >= 0) (kind == 0 ? out.dood : kind == 1 ? out.batch : out.bldg).insert({ti, idx});
            }
        }
    }
    return p <= end;
}

// The PVS file in memory, for the check and the measure mode
struct PvsFile {
    std::vector<uint8_t> data;
    const W3dPvsHeader* h = nullptr;
    bool load(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        data.resize(n > 0 ? (size_t)n : 0);
        const bool ok = n > (long)sizeof(W3dPvsHeader) && std::fread(data.data(), 1, data.size(), f) == data.size();
        std::fclose(f);
        if (!ok) return false;
        h = reinterpret_cast<const W3dPvsHeader*>(data.data());
        return h->magic == W3D_PVS_MAGIC && (h->version == W3D_PVS_VERSION || h->version == 3u);
    }
    // the levels matching a camera; tile = the scene's tile the camera is over. false: no data there (the PVS is off)
    bool lookup(const Scene& sc, int tile, const float cam[3], DecodedSet& out, int* levelsUsed) const {
        *levelsUsed = 0;
        const W3dPvsTile* tt = reinterpret_cast<const W3dPvsTile*>(data.data() + h->tileTableOff);
        for (uint32_t i = 0; i < h->tileCount; ++i) {
            if (tt[i].tx != sc.tiles[tile].tx || tt[i].ty != sc.tiles[tile].ty) continue;
            const float cs = h->cellSize;
            const int cx = (int)floorf((cam[0] - tt[i].origin[0]) / cs), cy = (int)floorf((cam[1] - tt[i].origin[1]) / cs);
            const int N = (int)h->cellsPerSide;
            if (cx < 0 || cy < 0 || cx >= N || cy >= N) return false;
            uint32_t off;
            std::memcpy(&off, data.data() + tt[i].cellTableOff + 4 * (size_t)(cx + cy * N), 4);
            W3dPvsLevel lv[16];
            const int nl = off ? w3d_pvs_levels(data.data(), (uint32_t)data.size(), off, lv, 16) : 0;
            for (int k = 0; k < nl; ++k) {
                if (cam[2] < lv[k].zlo || cam[2] > lv[k].zhi) continue;
                decodeLevel(sc, tile, lv[k].data, lv[k].data + lv[k].bytes, out);
                ++*levelsUsed;
            }
            return *levelsUsed > 0;
        }
        return false;
    }
};

// 6.54e: the far things within 4000 yd of the area (what the client may draw past its distance): the ground's tiles (TERR headers and
// batch boxes) and the far buildings' shells (FARO)
std::vector<FarBox> farBoxes(Packs& packs, float ax, float ay, float bx, float by) {
    std::vector<FarBox> out;
    std::vector<uint8_t> b;
    std::map<uint32_t, const W3dEntry*> grnd;   // 6.54e: the exact heights (GRND) of each tile
    for (const W3dEntry& e : packs.files[0]->idx)
        if (e.type == W3D_ENTRY_GROUND) grnd[e.id] = &e;
    auto nearArea = [&](float x0, float y0, float x1, float y1) {
        const float gx = std::max({0.0f, x0 - bx, ax - x1}), gy = std::max({0.0f, y0 - by, ay - y1});
        return gx * gx + gy * gy <= 4000.0f * 4000.0f;
    };
    for (const W3dEntry& e : packs.files[0]->idx) {
        if (e.type == W3D_ENTRY_TERRAIN) {
            if (!packs.files[0]->read(e, b, sizeof(W3dTerrainHeader))) continue;
            W3dTerrainHeader th;
            std::memcpy(&th, b.data(), std::min(sizeof th, b.size()));
            if (!nearArea(th.origin[0], th.origin[1], th.origin[0] + kTile, th.origin[1] + kTile)) continue;
            const uint32_t need = th.offBatches + th.batchCount * (uint32_t)sizeof(W3dTerrainBatch);
            if (need > e.size || !packs.files[0]->read(e, b, need)) continue;
            const W3dTerrainBatch* bt = reinterpret_cast<const W3dTerrainBatch*>(b.data() + th.offBatches);
            float top = -1e30f;
            for (uint32_t k = 0; k < th.batchCount; ++k) top = std::max(top, th.origin[2] + bt[k].bmax[2]);
            if (top <= -1e29f) continue;
            out.push_back({th.origin[0], th.origin[1], th.origin[0] + kTile, th.origin[1] + kTile, top + 2.0f});
            // 6.54e: its 4 x 4 blocks of 4 x 4 chunks (133 yd) with their highest exact ground (+2: the drawn meshes and the far
            // ground are made of these heights or fewer)
            const auto g = grnd.find(e.id);
            std::vector<uint8_t> gb;
            if (g == grnd.end() || !packs.files[0]->read(*g->second, gb) || !w3d_ground_valid(gb.data(), (uint32_t)gb.size())) continue;
            const W3dGroundHeader* gh = (const W3dGroundHeader*)gb.data();
            const W3dGroundChunk* ch = (const W3dGroundChunk*)(gb.data() + sizeof(W3dGroundHeader));
            const size_t tileAt = out.size() - 1;
            float tileTop = -1e30f;
            bool layoutOk = true;
            for (int by4 = 0; by4 < 4; ++by4)       // (chunk rows cy: x)
                for (int bx4 = 0; bx4 < 4; ++bx4) { // (chunk columns cx: y)
                    float hmax = -1e30f;
                    for (int cy = by4 * 4; cy < by4 * 4 + 4; ++cy)
                        for (int cx = bx4 * 4; cx < bx4 * 4 + 4; ++cx) {
                            const W3dGroundChunk& c = ch[cy * 16 + cx];
                            if (!(c.flags & W3D_GROUND_CHUNK_PRESENT)) continue;
                            for (uint16_t v : c.h) hmax = std::max(hmax, gh->origin[2] + v * gh->zScale);
                        }
                    if (hmax <= -1e29f) continue;
                    const float x0 = gh->origin[0] + (1.0f - (by4 + 1) / 4.0f) * kTile, x1 = gh->origin[0] + (1.0f - by4 / 4.0f) * kTile;
                    const float y0 = gh->origin[1] + (1.0f - (bx4 + 1) / 4.0f) * kTile, y1 = gh->origin[1] + (1.0f - bx4 / 4.0f) * kTile;
                    // (the layout checked against w3d_ground_at - what the client walks on: points of the block lie under its top)
                    for (float fx : {0.1f, 0.5f, 0.9f})
                        for (float fy : {0.1f, 0.5f, 0.9f}) {
                            W3dGroundHit hit;
                            if (w3d_ground_at(gb.data(), x0 + fx * (x1 - x0), y0 + fy * (y1 - y0), &hit) && hit.z > hmax + 0.05f) layoutOk = false;
                        }
                    out.push_back({x0 - 0.5f, y0 - 0.5f, x1 + 0.5f, y1 + 0.5f, hmax + 2.0f});
                    tileTop = std::max(tileTop, hmax + 2.0f);
                }
            out[tileAt].nsub = (uint32_t)(out.size() - 1 - tileAt);
            // (the whole tile's top: the higher of the mesh's box and the heights - its blocks hold the exact parts)
            out[tileAt].top = std::max(out[tileAt].top, tileTop);
            if (out[tileAt].nsub) {   // the mesh may stand higher than the heights somewhere (a mesh's box over them): keep it safe
                const float meshTop = top + 2.0f;
                if (meshTop > tileTop + 0.5f || !layoutOk) {
                    out.resize(tileAt + 1);   // the heights do not explain the mesh: the whole tile as before
                    out[tileAt].nsub = 0;
                    out[tileAt].top = meshTop;
                }
            }
        } else if (e.type == W3D_ENTRY_FAROBJ) {
            if (!packs.files[0]->read(e, b, e.size) || !w3d_farobj_valid(b.data(), (uint32_t)b.size())) continue;
            const W3dFarObjHeader* h = (const W3dFarObjHeader*)b.data();
            const W3dFarShell* sh = w3d_farobj_shells(b.data());
            const W3dFarObject* ob = w3d_farobj_objects(b.data());
            for (uint32_t k = 0; k < h->objectCount; ++k) {
                const float r = sh[ob[k].shell].radius;
                const float* p = ob[k].pos;
                if (nearArea(p[0] - r, p[1] - r, p[0] + r, p[1] + r)) out.push_back({p[0] - r, p[1] - r, p[0] + r, p[1] + r, p[2] + r});
            }
        }
    }
    return out;
}

int tileOf(const Scene& sc, float x, float y) {
    for (size_t k = 0; k < sc.tiles.size(); ++k) {
        const float* o = sc.tiles[k].th->origin;
        if (x >= o[0] && x < o[0] + kTile && y >= o[1] && y < o[1] + kTile) return (int)k;
    }
    return -1;
}

template <class F>
void parallelFor(size_t n, int threads, F&& fn) {
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= n) break;
                fn(i, t);
            }
        });
    for (std::thread& th : pool) th.join();
}

struct Around { float x, y, r; };

// What a camera would see vs what its PVS cell lets through (doodads beyond nearAlways, ground batches): the check of a built set
struct CheckStat {
    uint32_t views = 0, noData = 0, missingObjects = 0, missingBatches = 0, worstPixels = 0;
    uint32_t viewsLow = 0, missLowViews = 0, missHighViews = 0;   // camera up to 4 yd over its floor / higher: views with something missing
    double bandDrawn[3] = {0, 0, 0}, bandKept[3] = {0, 0, 0};       // doodad draws by the camera's band (4.75 / 7.25 / 10.25 yd)
    uint32_t bandViews[3] = {0, 0, 0}, bandMissViews[3] = {0, 0, 0};
    double drawnDraws = 0, keptDraws = 0, drawnBatches = 0, keptBatches = 0, drawnTris = 0, keptTris = 0;
    double bldgDrawnTris = 0, bldgKeptTris = 0, bldgDrawnDraws = 0, bldgKeptDraws = 0;   // 6.54d: WMO groups
    uint32_t missingBldg = 0, farHiddenViews = 0, farMissViews = 0;                        // 6.54d
    std::string worst, farWorst;
};
void checkView(const Scene& sc, const PvsFile& pv, const float cam[3], float yaw, float pitch, const Rules& truth, CheckStat& st, Target& t,
               int band = -1, int hband = -1, const std::vector<FarBox>* far = nullptr) {
    const int tile = tileOf(sc, cam[0], cam[1]);
    if (tile < 0) return;
    DecodedSet ds;
    int lv = 0;
    ++st.views;
    if (!pv.lookup(sc, tile, cam, ds, &lv)) { ++st.noData; return; }
    const uint32_t missBefore = st.missingObjects + st.missingBatches + st.missingBldg;
    const double drawnBefore = st.drawnDraws, keptBefore = st.keptDraws;
    if (band == 0) ++st.viewsLow;
    View v;
    v.client(cam, yaw, pitch, truth.dist, truth.wide);
    EyeResult er;
    renderEye(sc, v, truth, insideKey(sc, cam), t, true, &er, nullptr);
    const float nearA = pv.h->nearAlways;
    for (size_t i = 0; i < sc.inst.size(); ++i) {   // 6.54d: WMO groups
        const Inst& in = sc.inst[i];
        if (in.kind == kDoodad || !er.drawn[i]) continue;
        const float dx = in.pos[0] - cam[0], dy = in.pos[1] - cam[1], dz = in.pos[2] - cam[2];
        const bool near = sqrtf(dx * dx + dy * dy + dz * dz) - in.radius < nearA;
        const bool kept = near || !ds.tiles.count(in.tile) || ds.bldg.count({in.tile, in.pl}) > 0;
        Tally tl;
        tl.add(in, er.lod[i]);
        st.bldgDrawnTris += tl.tris; st.bldgDrawnDraws += tl.draws;
        if (kept) { st.bldgKeptTris += tl.tris; st.bldgKeptDraws += tl.draws; }
        if (!kept && er.pix[i]) {
            ++st.missingBldg;
            if (std::getenv("PVS_DEBUG"))
                std::printf("missing WMO group %08x at %.1f %.1f %.1f r %.1f kind %d, %u px, camera %.1f %.1f %.1f yaw %.0f pitch %.0f\n",
                            0u, in.pos[0], in.pos[1], in.pos[2], in.radius, in.kind, er.pix[i], cam[0], cam[1], cam[2], yaw, pitch);
            if (er.pix[i] > st.worstPixels) {
                st.worstPixels = er.pix[i];
                char b[256];
                std::snprintf(b, sizeof b, "a WMO group at %.1f %.1f %.1f (radius %.1f, %u px) from camera %.1f %.1f %.1f yaw %.0f pitch %.0f",
                              in.pos[0], in.pos[1], in.pos[2], in.radius, er.pix[i], cam[0], cam[1], cam[2], yaw, pitch);
                st.worst = b;
            }
        }
    }
    if (ds.farHidden) {   // 6.54d: nothing past the distance may show where the far ground / buildings would be
        ++st.farHiddenViews;
        const float fr = pv.h->maxDist - 2.0f * pv.h->cellSize;   // (the client skips what lies wholly past this)
        const float sinLim = far ? farSinLim(*far, cam, fr) : 1.0f;
        uint32_t n = 0;
        for (int y = 0; y < t.H; ++y)
            for (int x = 0; x < t.W; ++x) n += farPixel(t, v, x, y, fr, sinLim);
        if (n) {
            ++st.farMissViews;
            char b[200];
            std::snprintf(b, sizeof b, "%u px open past the distance from camera %.1f %.1f %.1f yaw %.0f pitch %.0f", n, cam[0], cam[1], cam[2], yaw, pitch);
            if (st.farWorst.empty()) st.farWorst = b;
        }
    }
    for (size_t i = 0; i < sc.inst.size(); ++i) {
        const Inst& in = sc.inst[i];
        if (in.kind != kDoodad || !er.drawn[i]) continue;
        const float dx = in.pos[0] - cam[0], dy = in.pos[1] - cam[1], dz = in.pos[2] - cam[2];
        const bool near = sqrtf(dx * dx + dy * dy + dz * dz) - in.radius < nearA;
        const bool covered = ds.tiles.count(in.tile) > 0;
        const bool kept = near || !covered || ds.dood.count({in.tile, in.pl}) > 0;
        Tally tl;
        tl.add(in, er.lod[i]);
        st.drawnDraws += tl.draws; st.drawnTris += tl.tris;
        if (kept) { st.keptDraws += tl.draws; st.keptTris += tl.tris; }
        if (!kept && er.pix[i]) {
            ++st.missingObjects;
            if (er.pix[i] > st.worstPixels) {
                st.worstPixels = er.pix[i];
                char b[256];
                std::snprintf(b, sizeof b, "a doodad at %.1f %.1f %.1f (radius %.1f, %u px) from camera %.1f %.1f %.1f yaw %.0f pitch %.0f",
                              in.pos[0], in.pos[1], in.pos[2], in.radius, er.pix[i], cam[0], cam[1], cam[2], yaw, pitch);
                st.worst = b;
            }
        }
    }
    for (size_t b = 0; b < sc.tb.size(); ++b) {
        if (!er.tbDrawn[b]) continue;
        const TB& x = sc.tb[b];
        const float dx = x.c[0] - cam[0], dy = x.c[1] - cam[1], dz = x.c[2] - cam[2];
        const bool near = sqrtf(dx * dx + dy * dy + dz * dz) - x.rad < nearA;
        const bool kept = near || !ds.tiles.count(x.tile) || ds.batch.count({x.tile, x.b}) > 0;
        st.drawnBatches += 1;
        if (kept) st.keptBatches += 1;
        if (!kept && er.tbPix[b]) {
            ++st.missingBatches;
            if (er.tbPix[b] > st.worstPixels) {
                st.worstPixels = er.tbPix[b];
                char s[256];
                std::snprintf(s, sizeof s, "a ground batch around %.1f %.1f %.1f (%u px) from camera %.1f %.1f %.1f yaw %.0f pitch %.0f",
                              x.c[0], x.c[1], x.c[2], er.tbPix[b], cam[0], cam[1], cam[2], yaw, pitch);
                st.worst = s;
            }
        }
    }
    if (st.missingObjects + st.missingBatches + st.missingBldg > missBefore) ++(band == 0 ? st.missLowViews : st.missHighViews);
    if (hband >= 0 && hband < 3) {
        ++st.bandViews[hband];
        st.bandDrawn[hband] += st.drawnDraws - drawnBefore;
        st.bandKept[hband] += st.keptDraws - keptBefore;
        if (st.missingObjects + st.missingBatches + st.missingBldg > missBefore) ++st.bandMissViews[hband];
    }
}

std::string pvsPathFor(const std::string& pack) {
    const size_t dot = pack.find_last_of('.');
    const size_t sl = pack.find_last_of("/\\");
    return (dot != std::string::npos && (sl == std::string::npos || dot > sl) ? pack.substr(0, dot) : pack) + ".pvs";
}

int buildPvs(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: w3docc --pvs <map.w3dp> --around x y radius [--around ...] [--cell 16] [--res 256] [--threads N] [--check 300]\n"
                  "       [--occrange 25] [--maxrange 120] [--dist 533] [--near 25] [-o out.pvs]");
        return 1;
    }
    const std::string packPath = argv[2];
    std::vector<Around> area;
    float cellWant = 16.0f, occRange = 15.0f,   // (6.54d: 15 - the PVS holds for doodadwide down to 0.34)
          maxRange = 120.0f, dist = 533.0f, nearAlways = 25.0f;
    int res = 256, checks = 300, sub = 4, margin = 1;   // (6.54d: sub 4 - the synthetic town's misses through diagonal gaps 12 -> 1)
    int threads = (int)std::max(1u, std::thread::hardware_concurrency());
    bool interiorNear = false;
    bool thinNone = false;   // 6.56e: --thin-none drops the narrow heights (no level there) as before   // (6.56d: interiors with portals far by default)
    std::string outPath = pvsPathFor(packPath);
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--around" && i + 3 < argc) { area.push_back({(float)std::atof(argv[i + 1]), (float)std::atof(argv[i + 2]), (float)std::atof(argv[i + 3])}); i += 3; }
        else if (a == "--cell" && i + 1 < argc) cellWant = (float)std::atof(argv[++i]);
        else if (a == "--res" && i + 1 < argc) res = std::max(32, std::atoi(argv[++i]));
        else if (a == "--sub" && i + 1 < argc) sub = std::max(2, std::min(9, std::atoi(argv[++i])));
        else if (a == "--margin" && i + 1 < argc) margin = std::max(0, std::min(1, std::atoi(argv[++i])));
        else if (a == "--threads" && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else if (a == "--thin-none") thinNone = true;
        else if (a == "--interior-near") interiorNear = true;   // 6.56: interiors from outside only near (v3; pad.txt interiors 0)
        else if (a == "--check" && i + 1 < argc) checks = std::max(0, std::atoi(argv[++i]));
        else if (a == "--occrange" && i + 1 < argc) occRange = (float)std::atof(argv[++i]);
        else if (a == "--maxrange" && i + 1 < argc) maxRange = (float)std::atof(argv[++i]);
        else if (a == "--dist" && i + 1 < argc) dist = (float)std::atof(argv[++i]);
        else if (a == "--near" && i + 1 < argc) nearAlways = (float)std::atof(argv[++i]);
        else if (a == "-o" && i + 1 < argc) outPath = argv[++i];
    }
    if (area.empty()) { std::fprintf(stderr, "--pvs: give the area: --around x y radius (yards, render space)\n"); return 1; }
    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    Packs packs;
    if (!packs.open(packPath)) return 1;
    W3dPackInfo mi{};
    for (const W3dEntry& x : packs.files[0]->idx)
        if (x.type == W3D_ENTRY_PACKINFO) { std::vector<uint8_t> b; if (packs.files[0]->read(x, b)) std::memcpy(&mi, b.data(), std::min(sizeof mi, b.size())); }
    {   // 6.54e: an area without ground tiles in this pack (a town outside the converted tiles) is left out, not an error
        std::vector<Around> kept;
        std::vector<uint8_t> b;
        for (const Around& a : area) {
            bool any = false;
            for (const W3dEntry& e : packs.files[0]->idx) {
                if (e.type != W3D_ENTRY_TERRAIN || !packs.files[0]->read(e, b, sizeof(W3dTerrainHeader))) continue;
                W3dTerrainHeader th;
                std::memcpy(&th, b.data(), std::min(sizeof th, b.size()));
                const float gx = std::max({0.0f, th.origin[0] - a.x, a.x - (th.origin[0] + kTile)});
                const float gy = std::max({0.0f, th.origin[1] - a.y, a.y - (th.origin[1] + kTile)});
                if (gx * gx + gy * gy <= a.r * a.r) { any = true; break; }
            }
            if (any) kept.push_back(a);
            else std::printf("pvs: the area around %.0f %.0f (radius %.0f) has no ground tiles in this pack - left out\n", a.x, a.y, a.r);
        }
        area = kept;
        if (area.empty()) { std::printf("pvs: none of the areas is in this pack - no PVS made (nothing to do)\n"); return 0; }
    }
    float ax = 1e30f, ay = 1e30f, bx = -1e30f, by = -1e30f;
    std::vector<std::array<float, 4>> boxes;   // 6.54e: each area's own box (towns far apart: not everything between them)
    for (const Around& a : area) {
        ax = std::min(ax, a.x - a.r); ay = std::min(ay, a.y - a.r); bx = std::max(bx, a.x + a.r); by = std::max(by, a.y + a.r);
        boxes.push_back({a.x - a.r, a.y - a.r, a.x + a.r, a.y + a.r});
    }
    const float reach = dist + 150.0f;   // (a doodad is drawn to the draw distance + its radius at most: the frustum's far plane)
    Scene sc;
    if (!loadScene(packs, sc, ax, ay, bx, by, reach, &boxes)) { std::fprintf(stderr, "no ground tiles in that area\n"); return 1; }
    std::printf("pvs: %zu tiles, %u placements (%u without a model), %zu instances; pool: %s\n", sc.tiles.size(), sc.placements, sc.missing,
                sc.inst.size(), packs.poolNote.c_str());
    if (sc.placements && sc.missing * 2 > sc.placements) { std::fprintf(stderr, "most placements have no model (is common.w3dp next to the map pack?)\n"); return 1; }
    const int N = std::max(1, (int)lroundf(kTile / cellWant));
    const float cs = kTile / (float)N;
    // the cells
    std::vector<PvsCell> cells;
    for (size_t ti = 0; ti < sc.tiles.size(); ++ti) {
        const float* o = sc.tiles[ti].th->origin;
        for (int cy = 0; cy < N; ++cy)
            for (int cx = 0; cx < N; ++cx) {
                const float x = o[0] + (cx + 0.5f) * cs, y = o[1] + (cy + 0.5f) * cs;
                for (const Around& a : area)
                    if ((x - a.x) * (x - a.x) + (y - a.y) * (y - a.y) <= a.r * a.r) { cells.push_back({(int)ti, cx, cy}); break; }
            }
    }
    std::printf("pvs: %zu cells of %.2f yd (%d per tile side) in %zu tiles\n", cells.size(), cs, N, sc.tiles.size());
    // the floors: ground, buildings, the opaque parts of bigger doodads
    RayGrid rg;
    rg.init(ax - 2 * cs, ay - 2 * cs, bx + 2 * cs, by + 2 * cs);
    for (const TileData& td : sc.tiles) {
        const W3dTerrainHeader* h = td.th;
        const W3dTerrainVertex* tv = reinterpret_cast<const W3dTerrainVertex*>(td.terr.data() + h->offVertices);
        const uint16_t* ix = reinterpret_cast<const uint16_t*>(td.terr.data() + h->offIndices);
        const W3dTerrainBatch* bt = reinterpret_cast<const W3dTerrainBatch*>(td.terr.data() + h->offBatches);
        for (uint32_t b = 0; b < h->batchCount; ++b)
            for (uint32_t k = bt[b].firstIndex[0]; k + 2 < bt[b].firstIndex[0] + bt[b].indexCount[0]; k += 3) {
                float w[3][3];
                for (int q = 0; q < 3; ++q) terrVert(h, tv[ix[k + q]], w[q]);
                rg.add(w);
            }
    }
    for (const Inst& in : sc.inst) {
        if (in.kind == kDoodad && in.radius < 2.0f) continue;
        if (in.pos[0] + in.radius < rg.x0 || in.pos[1] + in.radius < rg.y0 || in.pos[0] - in.radius > rg.x0 + rg.nx * rg.cs ||
            in.pos[1] - in.radius > rg.y0 + rg.ny * rg.cs) continue;
        for (uint32_t s = 0; s < in.m->h->submeshCount; ++s) {
            if (in.m->subs[s].blend != W3D_BLEND_OPAQUE) continue;
            forTris(in, s, 0, [&](float (*w)[3]) { rg.add(w); return true; });
        }
    }
    std::printf("pvs: %zu triangles for the floors (%.0f s)\n", rg.tris.size(), secs());
    // the points: cell corners and centres
    std::map<std::pair<long long, long long>, uint32_t> pointIdx;
    std::vector<std::array<float, 2>> points;
    auto pointAt = [&](float x, float y) {
        const std::pair<long long, long long> k = {llroundf(x * 8.0f), llroundf(y * 8.0f)};
        auto it = pointIdx.find(k);
        if (it != pointIdx.end()) return it->second;
        const uint32_t id = (uint32_t)points.size();
        points.push_back({x, y});
        pointIdx[k] = id;
        return id;
    };
    // a cell's points: a sub x sub grid over it, edges included (shared with the neighbours); --sub 3 = corners, edge middles, centre
    auto gridOf = [&](float x0, float y0, std::vector<uint32_t>& out) {
        for (int j = 0; j < sub; ++j)
            for (int i = 0; i < sub; ++i) out.push_back(pointAt(x0 + cs * i / (sub - 1), y0 + cs * j / (sub - 1)));
    };
    std::vector<std::vector<uint32_t>> cellPoints(cells.size());
    for (size_t c = 0; c < cells.size(); ++c) {
        const float* o = sc.tiles[cells[c].tile].th->origin;
        const float x0 = o[0] + cells[c].cx * cs, y0 = o[1] + cells[c].cy * cs;
        gridOf(x0, y0, cellPoints[c]);
    }
    // the points of the 8 neighbouring cells (in the area or not: their floors are found too)
    std::vector<std::vector<uint32_t>> nbPoints(cells.size());
    for (size_t c = 0; c < cells.size(); ++c) {
        const float* o = sc.tiles[cells[c].tile].th->origin;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                gridOf(o[0] + (cells[c].cx + dx) * cs, o[1] + (cells[c].cy + dy) * cs, nbPoints[c]);
            }
        std::sort(nbPoints[c].begin(), nbPoints[c].end());
        nbPoints[c].erase(std::unique(nbPoints[c].begin(), nbPoints[c].end()), nbPoints[c].end());
    }
    std::vector<std::vector<float>> pointFloors(points.size());
    parallelFor(points.size(), threads, [&](size_t i, int) { pointFloors[i] = rg.floors(points[i][0], points[i][1]); });
    // the eyes: 2, 4.5, 7 and 10 yd over every floor of every point
    // 6.54b: four eye heights; the camera's bands use the eyes up to their top (band b: eyes 0 .. b + 1)
    static const float kEyes[4] = {2.0f, 4.5f, 7.0f, 10.0f};
    constexpr int kNE = 4, kBands = 3;
    static const float kBandTop[kBands] = {4.75f, 7.25f, 10.25f};
    struct Eye { uint32_t point; float z; };
    std::vector<Eye> eyes;
    std::vector<std::vector<uint32_t>> eyeOf(points.size());   // per point: per floor the first eye index
    for (uint32_t p = 0; p < points.size(); ++p)
        for (float f : pointFloors[p]) {
            eyeOf[p].push_back((uint32_t)eyes.size());
            for (float e : kEyes) eyes.push_back({p, f + e});
        }
    // 6.56e: a height found at too few of a cell's points (a bridge, a ledge, a narrow stair: on the bridge over the Trade District's
    // canal the PVS was off and everything drawn) made no level. Now it gets eyes in the air too: at its top, over a 2 x 2 grid of the
    // cell - an eye in the open sees at least what a camera on the narrow floor there does.
    const int minPoints = std::min(3, sub * sub);
    struct Thin { uint32_t cell; float top; uint32_t eye; };   // eye: the first of its 4 x kNE air eyes
    std::vector<Thin> thins;
    const size_t eyesOnFloors = eyes.size();
    for (size_t c = 0; c < cells.size(); ++c) {
        std::vector<std::pair<float, uint32_t>> fl;
        for (uint32_t p : cellPoints[c])
            for (float z : pointFloors[p]) fl.push_back({z, p});
        std::sort(fl.begin(), fl.end());
        const float* o = sc.tiles[cells[c].tile].th->origin;
        for (size_t a = 0; a < fl.size();) {
            size_t b = a + 1;
            while (b < fl.size() && fl[b].first - fl[b - 1].first <= 3.0f && fl[b].first - fl[a].first <= 4.0f) ++b;   // (as the levels)
            std::set<uint32_t> pts;
            for (size_t k = a; k < b; ++k) pts.insert(fl[k].second);
            if ((int)pts.size() < minPoints && !thinNone) {
                thins.push_back({(uint32_t)c, fl[b - 1].first, (uint32_t)eyes.size()});
                for (int q = 0; q < 4; ++q) {
                    const uint32_t ap = (uint32_t)points.size();
                    points.push_back({o[0] + (cells[c].cx + 0.25f + 0.5f * (q & 1)) * cs, o[1] + (cells[c].cy + 0.25f + 0.5f * (q >> 1)) * cs});
                    for (float e : kEyes) eyes.push_back({ap, fl[b - 1].first + e});
                }
            }
            a = b;
        }
    }
    std::printf("pvs: %zu points, %zu eyes (%zu in the air over %zu narrow heights) ; 6 faces of %dx%d each, %d threads\n", points.size(), eyes.size(),
                eyes.size() - eyesOnFloors, thins.size(), res, res, threads);
    Rules rules;
    // (the eyes see 2 cells farther than the draw distance: a camera elsewhere in the cell or its neighbours sees that much past an eye)
    rules.dist = dist + 2.0f * cs; rules.range = 45.0f; rules.occRange = occRange; rules.targetRange = maxRange; rules.wide = true;
    rules.interiorFar = !interiorNear;
    CandGrid grid;   // 6.54e
    grid.build(sc, rules);
    std::vector<std::vector<uint32_t>> eyeSeen(eyes.size());
    std::vector<uint8_t> eyeFar(eyes.size(), 1);   // 6.54d: could something past the draw distance be seen from this eye
    const float farR = dist - 2.0f * cs;
    const std::vector<FarBox> farB = farBoxes(packs, ax, ay, bx, by);   // 6.54e: each at its own distance and height
    float farHigh = -1e30f;
    for (const FarBox& f : farB) farHigh = std::max(farHigh, f.top);
    size_t farTop = 0, farParts = 0;
    for (size_t i = 0; i < farB.size(); i += 1 + farB[i].nsub) { ++farTop; farParts += farB[i].nsub; }
    {
        std::vector<Target> tg(threads);
        const double tEyes = secs();
        std::atomic<size_t> done{0};
        std::mutex pm;
        size_t lastPct = 0;
        parallelFor(eyes.size(), threads, [&](size_t i, int th) {
            const float eye[3] = {points[eyes[i].point][0], points[eyes[i].point][1], eyes[i].z};
            bool fs = true;
            renderCube(sc, eye, rules, res, tg[th], eyeSeen[i], farR, &farB, &fs, &grid);
            eyeFar[i] = fs ? 1 : 0;
            const size_t d = ++done;
            const size_t pct = d * 20 / std::max<size_t>(1, eyes.size());
            std::lock_guard<std::mutex> g(pm);
            if (pct > lastPct) {
                lastPct = pct;
                const double el = secs() - tEyes;
                std::printf("pvs: eyes %zu%% (%.0f s, about %.0f s left)\n", pct * 5, secs(), el * (20.0 - (double)pct) / (double)pct);
                std::fflush(stdout);
            }
        });
    }
    // the cells: levels by floor height, each the union of its eyes
    std::vector<std::vector<PvsLevelOut>> cellLevels(cells.size());
    size_t levelsTotal = 0, itemsTotal = 0, emptyCells = 0, thinLevels = 0, bandItems[3] = {0, 0, 0}, farHiddenLevels = 0, bldgItems = 0;
    size_t thinAt = 0;   // (6.56e: the narrow heights come in the same order as above)
    for (size_t c = 0; c < cells.size(); ++c) {
        struct F { float z; uint32_t eye, point; };
        std::vector<F> fl;
        for (uint32_t p : cellPoints[c])
            for (size_t k = 0; k < pointFloors[p].size(); ++k) fl.push_back({pointFloors[p][k], eyeOf[p][k], p});
        if (fl.empty()) { ++emptyCells; continue; }
        std::sort(fl.begin(), fl.end(), [](const F& a, const F& b) { return a.z < b.z; });
        const float* o = sc.tiles[cells[c].tile].th->origin;
        const float ccx = o[0] + (cells[c].cx + 0.5f) * cs, ccy = o[1] + (cells[c].cy + 0.5f) * cs;
        for (size_t a = 0; a < fl.size();) {
            size_t b = a + 1;
            while (b < fl.size() && fl[b].z - fl[b - 1].z <= 3.0f && fl[b].z - fl[a].z <= 4.0f) ++b;   // (6.54b: a level spans 4 yd at most)
            const Thin* thin = nullptr;
            {   // a height found at too few of the cell's points (a bridge, the top of a wall, a beam): 6.56e its eyes in the air too
                std::set<uint32_t> pts;
                for (size_t k = a; k < b; ++k) pts.insert(fl[k].point);
                if ((int)pts.size() < minPoints) {
                    ++thinLevels;
                    if (thinNone || thinAt >= thins.size() || thins[thinAt].cell != c) { a = b; continue; }   // (--thin-none: none, as before)
                    thin = &thins[thinAt++];
                }
            }
            for (int band = 0; band < kBands; ++band) {
                const int ne = band + 2;   // the eyes of this band: 0 .. band + 1
                std::vector<uint32_t> u;
                bool far = false;   // 6.54d
                for (size_t k = a; k < b; ++k)
                    for (int e = 0; e < ne; ++e) {
                        u.insert(u.end(), eyeSeen[fl[k].eye + e].begin(), eyeSeen[fl[k].eye + e].end());
                        far = far || eyeFar[fl[k].eye + e];
                    }
                if (thin)   // 6.56e: the air eyes over the cell
                    for (int q = 0; q < 4; ++q)
                        for (int e = 0; e < ne; ++e) {
                            const uint32_t ei = thin->eye + (uint32_t)(q * kNE + e);
                            u.insert(u.end(), eyeSeen[ei].begin(), eyeSeen[ei].end());
                            far = far || eyeFar[ei];
                        }
                // margin: the eyes of the 8 neighbouring cells' floors at this level's heights (+-3 yd)
                if (margin)
                for (uint32_t p : nbPoints[c])
                    for (size_t k = 0; k < pointFloors[p].size(); ++k) {
                        const float z = pointFloors[p][k];
                        if (z < fl[a].z - 3.0f || z > fl[b - 1].z + 3.0f) continue;
                        for (int e = 0; e < ne; ++e) {
                            u.insert(u.end(), eyeSeen[eyeOf[p][k] + e].begin(), eyeSeen[eyeOf[p][k] + e].end());
                            far = far || eyeFar[eyeOf[p][k] + e];
                        }
                    }
                std::sort(u.begin(), u.end());
                u.erase(std::unique(u.begin(), u.end()), u.end());
                itemsTotal += u.size();
                bandItems[band] += u.size();
                // per tile: the doodads' PLCM indices, the batches
                std::map<int, std::array<std::vector<uint32_t>, 3>> byTile;   // doodads, ground batches, WMO groups (6.54d)
                for (size_t ti = 0; ti < sc.tiles.size(); ++ti) {   // every tile that could matter is listed (empty = nothing of it seen)
                    const float* to = sc.tiles[ti].th->origin;
                    const float gx = std::max({0.0f, to[0] - ccx, ccx - (to[0] + kTile)}), gy = std::max({0.0f, to[1] - ccy, ccy - (to[1] + kTile)});
                    if (gx * gx + gy * gy <= reach * reach) byTile[(int)ti];
                }
                for (uint32_t id : u) {
                    if (id & kGround) { const TB& x = sc.tb[id & ~kGround]; byTile[x.tile][1].push_back(x.b); }
                    else { const Inst& in = sc.inst[id]; byTile[in.tile][in.kind == kDoodad ? 0 : 2].push_back(in.pl); }
                }
                if (!far) ++farHiddenLevels;
                for (uint32_t id : u) if (!(id & kGround) && sc.inst[id].kind != kDoodad) ++bldgItems;
                PvsLevelOut lo;
                lo.zmin = band == 0 ? fl[a].z - 1.0f : fl[b - 1].z + kBandTop[band - 1];   // (the camera's range: written as zlo / zhi)
                lo.zmax = fl[b - 1].z + kBandTop[band];
                putVarint(lo.bytes, far ? 0u : (uint32_t)W3D_PVS_FAR_HIDDEN);
                putVarint(lo.bytes, (uint32_t)byTile.size());
                for (auto& [ti, lists] : byTile) {
                    lo.bytes.push_back((uint8_t)(int8_t)(sc.tiles[ti].tx - sc.tiles[cells[c].tile].tx));
                    lo.bytes.push_back((uint8_t)(int8_t)(sc.tiles[ti].ty - sc.tiles[cells[c].tile].ty));
                    for (std::vector<uint32_t>* l : {&lists[0], &lists[1], &lists[2]}) {
                        std::sort(l->begin(), l->end());
                        l->erase(std::unique(l->begin(), l->end()), l->end());
                        putVarint(lo.bytes, (uint32_t)l->size());
                        for (size_t i = 0; i < l->size(); ++i) putVarint(lo.bytes, i == 0 ? (*l)[i] : (*l)[i] - (*l)[i - 1] - 1);
                    }
                }
                cellLevels[c].push_back(std::move(lo));
                ++levelsTotal;
            }
            a = b;
        }
        if (cellLevels[c].size() > 255) cellLevels[c].resize(255);
    }
    // the file
    std::vector<uint8_t> out;
    W3dPvsHeader hd{};
    hd.magic = W3D_PVS_MAGIC; hd.version = interiorNear ? 3u : W3D_PVS_VERSION;   // (6.56: v3 = interiors only near)
    hd.buildId = mi.buildId; hd.cellSize = cs; hd.cellsPerSide = (uint32_t)N;
    hd.occRange = occRange; hd.maxRange = maxRange; hd.maxDist = dist; hd.nearAlways = nearAlways; hd.eyeAbove = kEyes[kNE - 1];
    std::map<std::pair<int, int>, int> tilesUsed;   // (tx, ty) -> scene tile
    for (const PvsCell& c : cells) tilesUsed[{sc.tiles[c.tile].tx, sc.tiles[c.tile].ty}] = c.tile;
    hd.tileCount = (uint32_t)tilesUsed.size();
    hd.tileTableOff = sizeof hd;
    putPod(out, hd);
    const size_t tableAt = out.size();
    out.resize(out.size() + tilesUsed.size() * sizeof(W3dPvsTile));
    std::map<int, size_t> cellTableAt;
    {
        size_t k = 0;
        for (auto& [key, ti] : tilesUsed) {
            W3dPvsTile t{};
            t.tx = (uint16_t)key.first; t.ty = (uint16_t)key.second;
            t.origin[0] = sc.tiles[ti].th->origin[0]; t.origin[1] = sc.tiles[ti].th->origin[1];
            t.cellTableOff = (uint32_t)out.size();
            std::memcpy(out.data() + tableAt + k * sizeof t, &t, sizeof t);
            cellTableAt[ti] = out.size();
            out.resize(out.size() + (size_t)N * N * 4, 0);
            ++k;
        }
    }
    for (size_t c = 0; c < cells.size(); ++c) {
        if (cellLevels[c].empty()) continue;
        const uint32_t off = (uint32_t)out.size();
        std::memcpy(out.data() + cellTableAt[cells[c].tile] + 4 * (size_t)(cells[c].cx + cells[c].cy * N), &off, 4);
        out.push_back((uint8_t)cellLevels[c].size());
        for (const PvsLevelOut& l : cellLevels[c]) {
            putPod(out, l.zmin); putPod(out, l.zmax); putPod(out, (uint32_t)l.bytes.size());
            out.insert(out.end(), l.bytes.begin(), l.bytes.end());
        }
    }
    if (FILE* f = std::fopen(outPath.c_str(), "wb")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
    else { std::fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    std::string rep;
    char line[600];
    auto P = [&](const char* fmt, auto... a) { std::snprintf(line, sizeof line, fmt, a...); rep += line; rep += "\n"; std::printf("%s\n", line); };
    P("pvs (6.54): %s -> %s, %zu KB, build %08x", packPath.c_str(), outPath.c_str(), out.size() / 1024, (unsigned)mi.buildId);
    for (const Around& a : area) P("  area: around %.1f %.1f, radius %.0f yd", a.x, a.y, a.r);
    P("  %zu cells of %.2f yd (%zu without floors), %zu levels (%.2f a cell), %zu points, %zu eyes (2 / 4.5 / 7 / 10 yd over the floors; a level also takes its 8 neighbours' eyes); %.0f items a level on average",
      cells.size(), cs, emptyCells, levelsTotal, cells.size() ? (double)levelsTotal / cells.size() : 0.0, points.size(), eyes.size(),
      levelsTotal ? (double)itemsTotal / levelsTotal : 0.0);
    P("  items a level by the camera's band (up to 4.75 / 7.25 / 10.25 yd over the floor): %.0f / %.0f / %.0f",
      levelsTotal ? 3.0 * bandItems[0] / levelsTotal : 0.0, levelsTotal ? 3.0 * bandItems[1] / levelsTotal : 0.0, levelsTotal ? 3.0 * bandItems[2] / levelsTotal : 0.0);
    P("  WMO groups in a level on average %.0f; levels from which nothing past %.0f yd can be seen (far ground / far buildings skipped): %zu of %zu"
      " (%zu far things within 4000 yd, %zu of their 133-yd blocks, up to %.0f high, each at its own distance)",
      levelsTotal ? (double)bldgItems / levelsTotal : 0.0, farR, farHiddenLevels, levelsTotal, farTop, farParts, farB.empty() ? 0.0 : (double)farHigh);
    P("  %zu narrow heights (found at fewer than %d of a cell's points: bridges, ledges)%s", thinLevels, minPoints,
      thinNone ? " dropped: no PVS for a camera there" : " - levels with eyes in the air over the cell too (6.56e)");
    P("  occluders: ground, buildings as the client draws them, doodads' opaque parts within 40 + %.0f x radius; candidates within 40 + %.0f x radius,"
      " draw distance %.0f; doodads within %.0f yd always drawn; cube faces %dx%d", occRange, maxRange, dist, nearAlways, res, res);
    P("  models: front side agrees with their normals for %.0f%% of sampled triangles; pool: %s", 100.0 * sc.normalsAgree, packs.poolNote.c_str());
    P("  built in %.0f s", secs());
    // the check: random cameras in the area, the client's view (800x240), the file read back
    if (checks > 0) {
        PvsFile pv;
        if (!pv.load(outPath)) { P("CHECK: cannot read the file back"); }
        else {
            Rules truth;
            truth.dist = dist; truth.range = 45.0f; truth.occRange = 45.0f; truth.targetRange = 45.0f; truth.wide = true;
            truth.interiorFar = !interiorNear;
            std::vector<CheckStat> stats(threads);
            std::vector<Target> tg(threads);
            parallelFor((size_t)checks, threads, [&](size_t i, int th) {
                uint32_t s = (uint32_t)(i * 2654435761u + 12345u);
                auto rnd = [&] { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / 16777216.0f; };
                const PvsCell& c = cells[(size_t)(rnd() * cells.size()) % cells.size()];
                const float* o = sc.tiles[c.tile].th->origin;
                const float x = o[0] + (c.cx + rnd()) * cs, y = o[1] + (c.cy + rnd()) * cs;
                const std::vector<float> fl = rg.floors(x, y);
                if (fl.empty()) return;
                const float over = 1.5f + 8.5f * rnd();
                const float cam[3] = {x, y, fl[(size_t)(rnd() * fl.size()) % fl.size()] + over};
                checkView(sc, pv, cam, 360.0f * rnd(), -25.0f + 50.0f * rnd(), truth, stats[th], tg[th], over <= 4.0f ? 0 : 1,
                          over <= 4.75f ? 0 : over <= 7.25f ? 1 : 2, &farB);
            });
            CheckStat all;
            for (const CheckStat& s : stats) {
                all.views += s.views; all.noData += s.noData; all.missingObjects += s.missingObjects; all.missingBatches += s.missingBatches;
                all.drawnDraws += s.drawnDraws; all.keptDraws += s.keptDraws; all.drawnBatches += s.drawnBatches; all.keptBatches += s.keptBatches;
                all.drawnTris += s.drawnTris; all.keptTris += s.keptTris;
                all.viewsLow += s.viewsLow; all.missLowViews += s.missLowViews; all.missHighViews += s.missHighViews;
                all.bldgDrawnTris += s.bldgDrawnTris; all.bldgKeptTris += s.bldgKeptTris; all.bldgDrawnDraws += s.bldgDrawnDraws;
                all.bldgKeptDraws += s.bldgKeptDraws; all.missingBldg += s.missingBldg; all.farHiddenViews += s.farHiddenViews;
                all.farMissViews += s.farMissViews;
                if (all.farWorst.empty()) all.farWorst = s.farWorst;
                for (int k = 0; k < 3; ++k) { all.bandDrawn[k] += s.bandDrawn[k]; all.bandKept[k] += s.bandKept[k]; all.bandViews[k] += s.bandViews[k]; all.bandMissViews[k] += s.bandMissViews[k]; }
                if (s.worstPixels > all.worstPixels) { all.worstPixels = s.worstPixels; all.worst = s.worst; }
            }
            const uint32_t used = all.views - all.noData;
            P("check: %u random cameras in the area (the client's 800x240 view, doodad range 45): %u with a PVS level, %u without (PVS off there)",
              all.views, used, all.noData);
            P("  with the PVS the client would draw %.0f%% of the doodad draws (%.0f -> %.0f a view), %.0f%% of their triangles, %.0f%% of the ground batches",
              all.drawnDraws > 0 ? 100.0 * all.keptDraws / all.drawnDraws : 0.0, used ? all.drawnDraws / used : 0.0, used ? all.keptDraws / used : 0.0,
              all.drawnTris > 0 ? 100.0 * all.keptTris / all.drawnTris : 0.0, all.drawnBatches > 0 ? 100.0 * all.keptBatches / all.drawnBatches : 0.0);
            P("  WMO groups (6.54d): the client would draw %.0f%% of their triangles, %.0f%% of their draws", all.bldgDrawnTris > 0 ? 100.0 * all.bldgKeptTris / all.bldgDrawnTris : 0.0,
              all.bldgDrawnDraws > 0 ? 100.0 * all.bldgKeptDraws / all.bldgDrawnDraws : 0.0);
            P("  far ground / far buildings skipped in %u of %u views; views where something past the distance would show although skipped: %u%s",
              all.farHiddenViews, used, all.farMissViews, all.farWorst.empty() ? "" : (" (" + all.farWorst + ")").c_str());
            P("  MISSING (seen in the view but culled by the PVS): %u doodads, %u ground batches, %u WMO groups in %u views%s", all.missingObjects,
              all.missingBatches, all.missingBldg, used, all.missingObjects + all.missingBatches + all.missingBldg ? "" : " - none");
            P("  views with something missing: %u of %u with the camera up to 4 yd over its floor, %u of %u higher (4..10 yd)", all.missLowViews,
              all.viewsLow, all.missHighViews, used - all.viewsLow);
            static const char* const kBandName[3] = {"camera up to 4.75 yd over its floor", "4.75 .. 7.25 yd", "7.25 .. 10 yd"};
            for (int k = 0; k < 3; ++k)
                P("  %-36s: %4u views, draws %.0f -> %.0f a view (%.0f%%), %u views with something missing", kBandName[k], all.bandViews[k],
                  all.bandViews[k] ? all.bandDrawn[k] / all.bandViews[k] : 0.0, all.bandViews[k] ? all.bandKept[k] / all.bandViews[k] : 0.0,
                  all.bandDrawn[k] > 0 ? 100.0 * all.bandKept[k] / all.bandDrawn[k] : 0.0, all.bandMissViews[k]);
            if (all.worstPixels) P("  worst: %s", all.worst.c_str());
        }
    }
    if (FILE* f = std::fopen((outPath + ".txt").c_str(), "w")) { std::fputs(rep.c_str(), f); std::fclose(f); }
    return 0;
}


// ------------------------------------------------------------------ 6.57: the horizon (HLOD)
// A texture as a 16 x 16 RGBA picture (level 0 boxed down): the colours of a mesh's triangles are taken from it
struct SmallTex { bool ok = false; uint8_t px[16 * 16 * 4]; };
uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}
bool decodeSmall(const std::vector<uint8_t>& b, SmallTex& out) {
    if (b.size() < W3D_TEX_DATA_OFFSET) return false;
    W3dTexHeader t;
    std::memcpy(&t, b.data(), sizeof t);
    const int w = t.width, h = t.height;
    if (w < 8 || h < 8 || w > 1024 || h > 1024) return false;
    const uint8_t* d = b.data() + W3D_TEX_DATA_OFFSET;
    const size_t avail = b.size() - W3D_TEX_DATA_OFFSET;
    std::vector<uint8_t> rgba((size_t)w * h * 4, 255);
    if (t.format == W3D_TEX_ETC1 || t.format == W3D_TEX_ETC1A4) {
        const bool a4 = t.format == W3D_TEX_ETC1A4;
        if (avail < (size_t)w * h / (a4 ? 1 : 2)) return false;
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
                                const uint8_t nib = alpha[2 * x + y / 2];
                                o[3] = (uint8_t)(((y & 1) ? nib >> 4 : nib & 15) * 17);
                            }
                    }
    } else {
        if (avail < (size_t)w * h * 2) return false;
        const uint16_t* s16 = reinterpret_cast<const uint16_t*>(d);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint16_t v = s16[((size_t)(y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)];
                uint8_t* o = &rgba[((size_t)y * w + x) * 4];
                if (t.format == W3D_TEX_RGBA4) { o[0] = (v >> 12) * 17; o[1] = ((v >> 8) & 15) * 17; o[2] = ((v >> 4) & 15) * 17; o[3] = (v & 15) * 17; }
                else if (t.format == W3D_TEX_RGBA5551) { o[0] = (v >> 11) << 3; o[1] = ((v >> 6) & 31) << 3; o[2] = ((v >> 1) & 31) << 3; o[3] = (v & 1) * 255; }
                else { o[0] = (v >> 11) << 3; o[1] = ((v >> 5) & 63) << 2; o[2] = (v & 31) << 3; }
            }
    }
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const int x0 = x * w / 16, x1 = std::max(x0 + 1, (x + 1) * w / 16), y0 = y * h / 16, y1 = std::max(y0 + 1, (y + 1) * h / 16);
            double acc[4] = {0, 0, 0, 0};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = &rgba[((size_t)yy * w + xx) * 4];
                    const double a = p[3] / 255.0;
                    for (int k = 0; k < 3; ++k) acc[k] += p[k] * a;
                    acc[3] += a;
                }
            const int n = (x1 - x0) * (y1 - y0);
            uint8_t* o = &out.px[(y * 16 + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = (uint8_t)std::clamp(acc[3] > 1e-6 ? acc[k] / acc[3] : 128.0, 0.0, 255.0);
            o[3] = (uint8_t)std::clamp(255.0 * acc[3] / n, 0.0, 255.0);
        }
    out.ok = true;
    return true;
}

// The source triangles of a block: world positions, colour (0..1) and alpha coverage
struct HTri { float p[9]; float rgb[3]; };
struct HMesh { std::vector<float> pos, nrm, rgb; std::vector<uint32_t> idx; };

// Vertex clustering on a grid of 'cell' yards (as the far shells, farobj.cpp): a cluster at the mean of its vertices, coloured and
// turned by the area-weighted triangles around it; collapsed triangles dropped, the same three clusters kept once (drawn two-sided)
void clusterMesh(const std::vector<HTri>& tris, float cell, HMesh& out) {
    out = HMesh();
    std::unordered_map<uint64_t, uint32_t> cid;
    struct Acc { double p[3] = {0, 0, 0}, rgb[3] = {0, 0, 0}, n[3] = {0, 0, 0}, w = 0; uint32_t cnt = 0; };
    std::vector<Acc> acc;
    std::vector<uint32_t> vc(tris.size() * 3);
    for (size_t t = 0; t < tris.size(); ++t)
        for (int q = 0; q < 3; ++q) {
            const float* p = &tris[t].p[q * 3];
            const int64_t ix = (int64_t)std::floor(p[0] / cell) + (1 << 20), iy = (int64_t)std::floor(p[1] / cell) + (1 << 20),
                          iz = (int64_t)std::floor(p[2] / cell) + (1 << 20);
            const uint64_t key = ((uint64_t)ix << 42) | ((uint64_t)iy << 21) | (uint64_t)iz;
            auto [it, fresh] = cid.emplace(key, (uint32_t)acc.size());
            if (fresh) acc.emplace_back();
            Acc& a = acc[it->second];
            for (int k = 0; k < 3; ++k) a.p[k] += p[k];
            ++a.cnt;
            vc[t * 3 + q] = it->second;
        }
    std::unordered_map<uint64_t, uint8_t> seen;
    std::vector<uint32_t> idx;
    for (size_t t = 0; t < tris.size(); ++t) {
        const float* p = tris[t].p;
        const float e1[3] = {p[3] - p[0], p[4] - p[1], p[5] - p[2]}, e2[3] = {p[6] - p[0], p[7] - p[1], p[8] - p[2]};
        const float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        const double w = std::max(0.5 * l, 1e-6);
        const uint32_t a = vc[t * 3], b = vc[t * 3 + 1], d = vc[t * 3 + 2];
        for (uint32_t v : {a, b, d}) {
            Acc& q = acc[v];
            for (int k = 0; k < 3; ++k) { q.rgb[k] += w * tris[t].rgb[k]; if (l > 1e-12f) q.n[k] += w * c[k] / l; }
            q.w += w;
        }
        if (a == b || b == d || a == d) continue;
        uint32_t s3[3] = {a, b, d};
        std::sort(s3, s3 + 3);
        const uint64_t key = ((uint64_t)s3[0] << 42) ^ ((uint64_t)s3[1] << 21) ^ (uint64_t)s3[2];
        if (!seen.emplace(key, 1).second) continue;
        idx.insert(idx.end(), {a, b, d});
    }
    std::vector<int32_t> remap(acc.size(), -1);
    for (uint32_t& v : idx) {
        if (remap[v] < 0) {
            remap[v] = (int32_t)(out.pos.size() / 3);
            const Acc& q = acc[v];
            double nl = std::sqrt(q.n[0] * q.n[0] + q.n[1] * q.n[1] + q.n[2] * q.n[2]);
            for (int k = 0; k < 3; ++k) {
                out.pos.push_back((float)(q.p[k] / q.cnt));
                out.rgb.push_back(q.w > 0 ? (float)(q.rgb[k] / q.w) : 0.5f);
                out.nrm.push_back(nl > 1e-6 ? (float)(q.n[k] / nl) : (k == 2 ? 1.0f : 0.0f));
            }
        }
        v = (uint32_t)remap[v];
    }
    out.idx = std::move(idx);
}

static_assert(sizeof(W3dHlodBlock) == 96, "W3dHlodBlock layout");
static_assert(sizeof(W3dHlodHeader) == 64, "W3dHlodHeader layout");
// 6.57b: one colour per triangle. The colour is a palette cell picked by the vertices' u / v: across a triangle whose corners point
// at different cells the GPU sweeps the u / v through every cell in between - unrelated colours (the palette is laid out by r, g, b),
// rainbow noise far off on the console. So a triangle's corners all get its colour (the mean of the three, as quantised for the
// palette) and a vertex is shared only by triangles of the same colour; the light (vertex r) still varies smoothly.
void flatColours(HMesh& m) {
    HMesh o;
    std::unordered_map<uint64_t, uint32_t> at;
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
        float c[3] = {0, 0, 0};
        for (int q = 0; q < 3; ++q)
            for (int k = 0; k < 3; ++k) c[k] += m.rgb[m.idx[t + q] * 3 + k] / 3.0f;
        uint32_t key = 0;
        for (int k = 0; k < 3; ++k) key = (key << 4) | (uint32_t)std::clamp((int)std::lround(c[k] * 15.0f), 0, 15);
        for (int q = 0; q < 3; ++q) {
            const uint32_t v = m.idx[t + q];
            auto [it, fresh] = at.emplace(((uint64_t)v << 12) | key, (uint32_t)(o.pos.size() / 3));
            if (fresh)
                for (int k = 0; k < 3; ++k) {
                    o.pos.push_back(m.pos[v * 3 + k]);
                    o.nrm.push_back(m.nrm[v * 3 + k]);
                    o.rgb.push_back((float)((key >> (4 * (2 - k))) & 15) / 15.0f);
                }
            o.idx.push_back(it->second);
        }
    }
    m = std::move(o);
}

int buildHlod(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: w3docc --hlod <map.w3dp> [--around x y radius ...] [--switch 160] [-o file.hlod]");
        return 1;
    }
    const std::string packPath = argv[2];
    std::vector<Around> area;
    float switchDist = 160.0f;
    std::string outPath;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--around" && i + 3 < argc) { area.push_back({(float)std::atof(argv[i + 1]), (float)std::atof(argv[i + 2]), (float)std::atof(argv[i + 3])}); i += 3; }
        else if (a == "--switch" && i + 1 < argc) switchDist = std::clamp((float)std::atof(argv[++i]), 60.0f, 600.0f);
        else if (a == "-o" && i + 1 < argc) outPath = argv[++i];
        else { std::fprintf(stderr, "--hlod: unknown option %s\n", a.c_str()); return 1; }
    }
    if (outPath.empty()) { outPath = packPath; const size_t d = outPath.rfind('.'); if (d != std::string::npos) outPath.resize(d); outPath += ".hlod"; }
    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    Packs packs;
    if (!packs.open(packPath)) return 1;
    W3dPackInfo mi{};
    for (const W3dEntry& x : packs.files[0]->idx)
        if (x.type == W3D_ENTRY_PACKINFO) { std::vector<uint8_t> b; if (packs.files[0]->read(x, b)) std::memcpy(&mi, b.data(), std::min(sizeof mi, b.size())); }
    // the tiles: every ground tile of the pack (or those touching the areas)
    struct TileRef { int tx, ty; float ox, oy; };
    std::vector<TileRef> tiles;
    {
        std::vector<uint8_t> b;
        for (const W3dEntry& e : packs.files[0]->idx) {
            if (e.type != W3D_ENTRY_TERRAIN || !packs.files[0]->read(e, b, sizeof(W3dTerrainHeader))) continue;
            W3dTerrainHeader th;
            std::memcpy(&th, b.data(), std::min(sizeof th, b.size()));
            bool want = area.empty();
            for (const Around& a : area) {
                const float gx = std::max({0.0f, th.origin[0] - a.x, a.x - (th.origin[0] + kTile)});
                const float gy = std::max({0.0f, th.origin[1] - a.y, a.y - (th.origin[1] + kTile)});
                want = want || gx * gx + gy * gy <= a.r * a.r;
            }
            if (want) tiles.push_back({(int)th.tileX, (int)th.tileY, th.origin[0], th.origin[1]});
        }
        std::sort(tiles.begin(), tiles.end(), [](const TileRef& a, const TileRef& b) { return a.tx != b.tx ? a.tx < b.tx : a.ty < b.ty; });
    }
    std::printf("hlod: %zu tiles, switch %.0f yd\n", tiles.size(), switchDist);
    // the levels: level 0 for a camera at the switch distance, about 2.5 px of error on the wide top screen (416 px a radian); level 1
    // from twice that on, twice as coarse; a block over its budget gets a coarser grid
    // 6.57d: three levels (from the switch, 2x and 4x it), each grid about 6 px on the wide screen at its distance, and budgets a 3DS
    // can carry with tens of blocks in view (v1: 2.5 px, 12000 / 5000 - 2100 triangles a block, 150k a frame, a 240 MB file)
    const float levelFrom[W3D_HLOD_LEVELS] = {switchDist, 2.0f * switchDist, 4.0f * switchDist};
    const float baseCell[W3D_HLOD_LEVELS] = {levelFrom[0] * 6.0f / 416.0f, levelFrom[1] * 6.0f / 416.0f, levelFrom[2] * 6.0f / 416.0f};
    const size_t budget[W3D_HLOD_LEVELS] = {2500, 800, 250};
    const float doodadMinR = (switchDist - 40.0f) / 45.0f;   // (a doodad the client draws at the switch distance with range 45)
    // the palette: every 4-bit colour (4096) in 4 x 4 cells of a 256 x 256 RGB565 texture
    constexpr int kPalW = 256, kPalH = 256, kCellT = 4, kPerRow = kPalW / kCellT;
    std::map<uint32_t, SmallTex> texCache;
    Scene sc;
    std::vector<uint8_t> buf;
    struct TileOut { int tx, ty; std::vector<uint8_t> data; };
    std::vector<TileOut> outTiles;
    size_t nBlocks = 0, nMembers = 0, srcTris = 0, outTris[W3D_HLOD_LEVELS] = {0, 0, 0}, texMissing = 0, skippedTris = 0, coarsened = 0;
    size_t nBldg = 0, nDood = 0;
    double chkBoth[W3D_HLOD_LEVELS] = {0, 0, 0}, chkEither[W3D_HLOD_LEVELS] = {0, 0, 0}, worstIoU[W3D_HLOD_LEVELS] = {1, 1, 1};
    size_t chkViews[W3D_HLOD_LEVELS] = {0, 0, 0};
    size_t chkSerial = 0, mixedTris = 0, allTris = 0;
    const size_t chkEvery = tiles.size() > 30 ? 8 : 1;
    for (size_t ti = 0; ti < tiles.size(); ++ti) {
        const TileRef& tr = tiles[ti];
        sc.tiles.clear(); sc.inst.clear(); sc.tb.clear();
        // (a box just inside the tile, no reach: only this tile's ground and placements)
        if (!loadScene(packs, sc, tr.ox + 1, tr.oy + 1, tr.ox + kTile - 1, tr.oy + kTile - 1, 0.0f)) continue;
        int own = -1;
        for (size_t k = 0; k < sc.tiles.size(); ++k) if (sc.tiles[k].tx == tr.tx && sc.tiles[k].ty == tr.ty) own = (int)k;
        if (own < 0) continue;
        std::vector<HTri> src[16];
        std::vector<uint32_t> members[16];
        float bmin[16][3], bmax[16][3];
        for (int b = 0; b < 16; ++b) for (int k = 0; k < 3; ++k) { bmin[b][k] = 1e30f; bmax[b][k] = -1e30f; }
        for (const Inst& in : sc.inst) {
            if (in.tile != own) continue;
            const bool take = in.kind == kExterior || (in.kind == kInterior && in.m->portals && in.linked) || (in.kind == kDoodad && in.radius >= doodadMinR);
            if (!take) continue;
            const int bx = std::clamp((int)std::floor((in.pos[0] - tr.ox) / (kTile / W3D_HLOD_BLOCKS)), 0, W3D_HLOD_BLOCKS - 1);
            const int by = std::clamp((int)std::floor((in.pos[1] - tr.oy) / (kTile / W3D_HLOD_BLOCKS)), 0, W3D_HLOD_BLOCKS - 1);
            const int bi = by * W3D_HLOD_BLOCKS + bx;
            const ModelData& m = *in.m;
            const uint32_t* texIds = m.h->texCount && (uint64_t)m.h->offTexIds + m.h->texCount * 4ull <= m.blob.size()
                                     ? reinterpret_cast<const uint32_t*>(m.blob.data() + m.h->offTexIds) : nullptr;
            const W3dModelVertex* mv = reinterpret_cast<const W3dModelVertex*>(m.blob.data() + m.h->offVertices);
            const uint16_t* mi = reinterpret_cast<const uint16_t*>(m.blob.data() + m.h->offIndices);
            size_t added = 0;
            for (uint32_t s = 0; s < m.h->submeshCount; ++s) {
                const W3dSubmesh& sm = m.subs[s];
                if (sm.blend != W3D_BLEND_OPAQUE && sm.blend != W3D_BLEND_ALPHAKEY) continue;
                const SmallTex* tex = nullptr;
                if (texIds && sm.tex != W3D_NO_TEXTURE && sm.tex < m.h->texCount) {
                    const uint32_t id = texIds[sm.tex];
                    auto it = texCache.find(id);
                    if (it == texCache.end()) {
                        SmallTex st;
                        if (!packs.read(W3D_ENTRY_TEXTURE, id, buf) || !decodeSmall(buf, st)) ++texMissing;
                        it = texCache.emplace(id, st).first;
                    }
                    if (it->second.ok) tex = &it->second;
                }
                uint32_t first = sm.firstIndex, count = sm.indexCount;
                for (uint32_t k = first; k + 2 < first + count; k += 3) {
                    HTri t;
                    float uv[2] = {0, 0};
                    for (int q = 0; q < 3; ++q) {
                        const W3dModelVertex& x = mv[mi[k + q]];
                        worldVert(in, x, &t.p[q * 3]);
                        uv[0] += x.u * W3D_MODEL_UV_SCALE / 3.0f;
                        uv[1] += x.v * W3D_MODEL_UV_SCALE / 3.0f;
                    }
                    if (tex) {   // (v is flipped for the GPU: row 0 of the picture is v = 1)
                        const float fu = uv[0] - std::floor(uv[0]), fv = 1.0f - (uv[1] - std::floor(uv[1]));
                        const int px = std::clamp((int)(fu * 16.0f), 0, 15), py = std::clamp((int)(fv * 16.0f), 0, 15);
                        const uint8_t* c = &tex->px[(py * 16 + px) * 4];
                        if (sm.blend == W3D_BLEND_ALPHAKEY && c[3] < 128) { ++skippedTris; continue; }
                        for (int q = 0; q < 3; ++q) t.rgb[q] = c[q] / 255.0f;
                    } else {
                        t.rgb[0] = t.rgb[1] = t.rgb[2] = 0.6f;
                    }
                    for (int q = 0; q < 3; ++q)
                        for (int a = 0; a < 3; ++a) { bmin[bi][a] = std::min(bmin[bi][a], t.p[q * 3 + a]); bmax[bi][a] = std::max(bmax[bi][a], t.p[q * 3 + a]); }
                    src[bi].push_back(t);
                    ++added;
                }
            }
            if (added) {
                members[bi].push_back(in.pl);
                (in.kind == kDoodad ? nDood : nBldg) += 1;
            }
        }
        // the tile's data
        std::vector<uint8_t> d(16, 0);
        std::vector<W3dHlodBlock> blocks;
        std::vector<std::vector<uint8_t>> payload;   // per block: its arrays (offsets fixed below)
        for (int bi = 0; bi < 16; ++bi) {
            if (src[bi].empty()) continue;
            W3dHlodBlock hb{};
            hb.bx = (uint8_t)(bi % W3D_HLOD_BLOCKS);
            hb.by = (uint8_t)(bi / W3D_HLOD_BLOCKS);
            for (int k = 0; k < 3; ++k) { hb.origin[k] = bmin[bi][k]; hb.bmin[k] = bmin[bi][k]; hb.bmax[k] = bmax[bi][k]; }
            srcTris += src[bi].size();
            HMesh mesh[W3D_HLOD_LEVELS];
            for (int l = 0; l < W3D_HLOD_LEVELS; ++l) {
                float cell = baseCell[l];
                for (int tries = 0; tries < 12; ++tries) {
                    clusterMesh(src[bi], cell, mesh[l]);
                    flatColours(mesh[l]);
                    if (mesh[l].idx.size() / 3 <= budget[l] && mesh[l].pos.size() / 3 <= 65535) break;
                    cell *= 1.3f;
                    ++coarsened;
                }
                outTris[l] += mesh[l].idx.size() / 3;
            }
            blocks.push_back(hb);
            payload.emplace_back();
            // write the arrays into the block's payload with offsets relative to the payload start (fixed when placed)
            std::vector<uint8_t>& P = payload.back();
            W3dHlodBlock& B = blocks.back();
            for (int l = 0; l < W3D_HLOD_LEVELS; ++l) {
                while (P.size() % 16) P.push_back(0);
                B.vertOff[l] = (uint32_t)P.size();
                B.vertCount[l] = (uint32_t)(mesh[l].pos.size() / 3);
                for (uint32_t v = 0; v < B.vertCount[l]; ++v) {
                    W3dTerrainVertex tv{};
                    long q[3];
                    for (int k = 0; k < 3; ++k) q[k] = std::lround((mesh[l].pos[v * 3 + k] - B.origin[k]) / W3D_HLOD_POS_UNIT);
                    tv.x = (int16_t)std::clamp(q[0], -32767L, 32767L);
                    tv.y = (int16_t)std::clamp(q[1], -32767L, 32767L);
                    tv.z = (int16_t)std::clamp(q[2], -32767L, 32767L);
                    const uint8_t light = w3d_far_light(mesh[l].nrm[v * 3], mesh[l].nrm[v * 3 + 1], mesh[l].nrm[v * 3 + 2]);
                    tv.r = light; tv.b = light; tv.g = 0; tv.a = 0;
                    uint32_t c = 0;
                    for (int k = 0; k < 3; ++k) c = (c << 4) | (uint32_t)std::clamp((int)std::lround(mesh[l].rgb[v * 3 + k] * 15.0f), 0, 15);
                    const float cx = (float)((c % kPerRow) * kCellT) + kCellT * 0.5f, cy = (float)((c / kPerRow) * kCellT) + kCellT * 0.5f;
                    tv.u = (int16_t)std::lround(cx / (float)kPalW / W3D_FAR_UV_SCALE);
                    tv.v = (int16_t)std::lround((1.0f - cy / (float)kPalH) / W3D_FAR_UV_SCALE);
                    putPod(P, tv);
                }
                B.idxOff[l] = (uint32_t)P.size();
                B.idxCount[l] = (uint32_t)mesh[l].idx.size();
                for (uint32_t ix : mesh[l].idx) putPod(P, (uint16_t)ix);
            }
            while (P.size() % 4) P.push_back(0);
            B.memberOff = (uint32_t)P.size();
            B.memberCount = (uint32_t)members[bi].size();
            for (uint32_t m : members[bi]) putPod(P, m);
            nMembers += members[bi].size();
        }
        if (blocks.empty()) continue;
        // layout: count, blocks, then each payload at a 16-aligned offset
        *(uint32_t*)d.data() = (uint32_t)blocks.size();
        d.resize(16 + blocks.size() * sizeof(W3dHlodBlock), 0);
        for (size_t b = 0; b < blocks.size(); ++b) {
            while (d.size() % 16) d.push_back(0);
            const uint32_t base = (uint32_t)d.size();
            W3dHlodBlock& B = blocks[b];
            for (int l = 0; l < W3D_HLOD_LEVELS; ++l) { B.vertOff[l] += base; B.idxOff[l] += base; }
            B.memberOff += base;
            d.insert(d.end(), payload[b].begin(), payload[b].end());
        }
        std::memcpy(d.data() + 16, blocks.data(), blocks.size() * sizeof(W3dHlodBlock));
        if (!w3d_hlod_tile_valid(d.data(), (uint32_t)d.size())) { std::fprintf(stderr, "hlod: tile %d_%d: bad layout (a bug)\n", tr.tx, tr.ty); return 1; }
        {   // 6.57b: every triangle one palette cell (its corners' u / v equal)
            const W3dHlodBlock* wb = w3d_hlod_blocks(d.data());
            for (uint32_t b = 0; b < blocks.size(); ++b)
                for (int l = 0; l < W3D_HLOD_LEVELS; ++l) {
                    const W3dTerrainVertex* hv = (const W3dTerrainVertex*)(d.data() + wb[b].vertOff[l]);
                    const uint16_t* hi = (const uint16_t*)(d.data() + wb[b].idxOff[l]);
                    for (uint32_t k = 0; k + 2 < wb[b].idxCount[l]; k += 3) {
                        const W3dTerrainVertex &a = hv[hi[k]], &b2 = hv[hi[k + 1]], &c2 = hv[hi[k + 2]];
                        mixedTris += !(a.u == b2.u && a.u == c2.u && a.v == b2.v && a.v == c2.v);
                        ++allTris;
                    }
                }
        }
        // the check: each block from its level's distance, 4 sides, in the client's view - the members' pixels against the mesh's,
        // the mesh decoded from the bytes just written as the client does (origin + vertex * unit)
        {
            const W3dHlodBlock* wb = w3d_hlod_blocks(d.data());
            for (uint32_t b = 0; b < blocks.size(); ++b) {
                if (chkSerial++ % chkEvery) continue;   // (a sample: a whole continent is thousands of blocks)
                const int bi = wb[b].by * W3D_HLOD_BLOCKS + wb[b].bx;
                float c[3], ext = 0;
                for (int k = 0; k < 3; ++k) { c[k] = 0.5f * (wb[b].bmin[k] + wb[b].bmax[k]); ext = std::max(ext, wb[b].bmax[k] - wb[b].bmin[k]); }
                for (int l = 0; l < W3D_HLOD_LEVELS; ++l) {
                    const float dist = levelFrom[l] + 0.5f * ext;
                    const W3dTerrainVertex* hv = (const W3dTerrainVertex*)(d.data() + wb[b].vertOff[l]);
                    const uint16_t* hi = (const uint16_t*)(d.data() + wb[b].idxOff[l]);
                    for (int side = 0; side < 4; ++side) {
                        const float yaw = 90.0f * side + 45.0f, ya = yaw * kPi / 180.0f;
                        const float camp[3] = {c[0] - cosf(ya) * dist, c[1] - sinf(ya) * dist, c[2] + 15.0f};
                        const float pitch = -atan2f(15.0f, dist) * 180.0f / kPi;
                        View v;
                        v.client(camp, yaw, pitch, 2000.0f, true);
                        Target ta, tb;
                        ta.clear(v.W, v.H);
                        tb.clear(v.W, v.H);
                        for (const HTri& t : src[bi]) {
                            float vv[3][3];
                            for (int q = 0; q < 3; ++q) v.toView(&t.p[q * 3], vv[q]);
                            rasterTri(ta, v, vv[0], vv[1], vv[2], 1, true);
                        }
                        for (uint32_t k = 0; k + 2 < wb[b].idxCount[l]; k += 3) {
                            float w[3], vv[3][3];
                            for (int q = 0; q < 3; ++q) {
                                const W3dTerrainVertex& x = hv[hi[k + q]];
                                w[0] = wb[b].origin[0] + x.x * W3D_HLOD_POS_UNIT;
                                w[1] = wb[b].origin[1] + x.y * W3D_HLOD_POS_UNIT;
                                w[2] = wb[b].origin[2] + x.z * W3D_HLOD_POS_UNIT;
                                v.toView(w, vv[q]);
                            }
                            rasterTri(tb, v, vv[0], vv[1], vv[2], 1, true);
                        }
                        size_t both = 0, either = 0;
                        for (size_t q = 0; q < ta.id.size(); ++q) { both += ta.id[q] && tb.id[q]; either += ta.id[q] || tb.id[q]; }
                        if (either) { chkBoth[l] += both; chkEither[l] += either; ++chkViews[l]; worstIoU[l] = std::min(worstIoU[l], (double)both / either); }
                    }
                }
            }
        }
        nBlocks += blocks.size();
        outTiles.push_back({tr.tx, tr.ty, std::move(d)});
        if ((ti + 1) % 20 == 0 || ti + 1 == tiles.size()) {
            std::printf("hlod: %zu of %zu tiles (%.0f s)\n", ti + 1, tiles.size(), secs());
            std::fflush(stdout);
        }
    }
    // the file
    std::vector<uint8_t> f(sizeof(W3dHlodHeader), 0);
    W3dHlodHeader hd{};
    hd.magic = W3D_HLOD_MAGIC; hd.version = W3D_HLOD_VERSION; hd.buildId = mi.buildId;
    hd.switchDist = switchDist; hd.levelDist[0] = levelFrom[1]; hd.levelDist[1] = levelFrom[2];
    for (int l = 0; l < W3D_HLOD_LEVELS; ++l) hd.cell[l] = baseCell[l];
    hd.palW = kPalW; hd.palH = kPalH;
    while (f.size() % 16) f.push_back(0);
    hd.palOff = (uint32_t)f.size();
    {
        std::vector<uint16_t> pal((size_t)kPalW * kPalH, 0);
        for (uint32_t c = 0; c < 4096; ++c) {
            const uint32_t r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
            const uint16_t v = (uint16_t)(((r * 31 + 7) / 15) << 11 | ((g * 63 + 7) / 15) << 5 | ((b * 31 + 7) / 15));
            const int cx = (int)(c % kPerRow) * kCellT, cy = (int)(c / kPerRow) * kCellT;
            for (int y = cy; y < cy + kCellT; ++y)
                for (int x = cx; x < cx + kCellT; ++x) pal[((size_t)(y >> 3) * (kPalW >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)] = v;
        }
        hd.palBytes = (uint32_t)(pal.size() * 2);
        const uint8_t* pb = reinterpret_cast<const uint8_t*>(pal.data());
        f.insert(f.end(), pb, pb + hd.palBytes);
    }
    while (f.size() % 16) f.push_back(0);
    hd.tileTableOff = (uint32_t)f.size();
    hd.tileCount = (uint32_t)outTiles.size();
    f.resize(f.size() + outTiles.size() * sizeof(W3dHlodTile), 0);
    for (size_t i = 0; i < outTiles.size(); ++i) {
        while (f.size() % 16) f.push_back(0);
        W3dHlodTile t{outTiles[i].tx, outTiles[i].ty, (uint32_t)f.size(), (uint32_t)outTiles[i].data.size()};
        std::memcpy(f.data() + hd.tileTableOff + i * sizeof(W3dHlodTile), &t, sizeof t);
        f.insert(f.end(), outTiles[i].data.begin(), outTiles[i].data.end());
    }
    std::memcpy(f.data(), &hd, sizeof hd);
    FILE* fo = std::fopen(outPath.c_str(), "wb");
    if (!fo || std::fwrite(f.data(), 1, f.size(), fo) != f.size()) { std::fprintf(stderr, "cannot write %s\n", outPath.c_str()); if (fo) std::fclose(fo); return 1; }
    std::fclose(fo);
    std::string rep;
    char line[512];
    auto P = [&](const char* fmt, auto... a) { std::snprintf(line, sizeof line, fmt, a...); rep += line; rep += "\n"; std::printf("%s\n", line); };
    P("hlod (6.57): %s -> %s, %.0f KB, build %08x", packPath.c_str(), outPath.c_str(), f.size() / 1024.0, (unsigned)mi.buildId);
    P("  %zu tiles with %zu blocks of 133 yd; members %zu (%zu building groups, %zu doodads of radius >= %.1f)", outTiles.size(), nBlocks,
      nMembers, nBldg, nDood, doodadMinR);
    P("  triangles: %zu in the members -> level 0 %zu (grid %.2f yd, from %.0f yd), level 1 %zu (grid %.2f yd, from %.0f yd); %zu grids made coarser for the budget",
      srcTris, outTris[0], baseCell[0], switchDist, outTris[1], baseCell[1], levelFrom[1], coarsened);
    P("  level 2: %zu triangles (grid %.2f yd, from %.0f yd)", outTris[2], baseCell[2], levelFrom[2]);
    P("  per block on average: level 0 %.0f, level 1 %.0f, level 2 %.0f triangles", nBlocks ? (double)outTris[0] / nBlocks : 0.0, nBlocks ? (double)outTris[1] / nBlocks : 0.0, nBlocks ? (double)outTris[2] / nBlocks : 0.0);
    P("  textures: %zu read, %zu missing (grey); %zu cut-out triangles left out (their texels mostly clear)", texCache.size(), texMissing, skippedTris);
    for (int l = 0; l < W3D_HLOD_LEVELS; ++l)
        P("  check, level %d: %zu views of the blocks from %.0f yd (+ half the block) - the mesh covers the members' pixels: %.1f%% overlap (IoU), worst view %.1f%%",
          l, chkViews[l], levelFrom[l], chkEither[l] > 0 ? 100.0 * chkBoth[l] / chkEither[l] : 100.0, 100.0 * worstIoU[l]);
    P("  colours: %zu of %zu triangles with corners in different palette cells (must be 0: one colour a triangle)", mixedTris, allTris);
    P("  built in %.0f s", secs());
    if (mixedTris) { std::fprintf(stderr, "hlod: triangles with mixed colours (a bug)\n"); return 1; }
    if (FILE* r = std::fopen((outPath + ".txt").c_str(), "wb")) { std::fwrite(rep.data(), 1, rep.size(), r); std::fclose(r); }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--pvs") return buildPvs(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--hlod") return buildHlod(argc, argv);
    if (argc < 8) {
        std::puts("usage: w3docc <map.w3dp> <outdir> <x> <y> <z> <yawDeg> <pitchDeg> [--dist 533] [--range 45] [--narrow] [--lodpx 2]\n"
                  "         x y z yaw pitch as gputest.txt prints them; with map<ID>.pvs next to the pack: what the PVS keeps there\n"
                  "       w3docc --pvs <map.w3dp> --around x y radius [...]   (builds map<ID>.pvs, see the source)");
        return 1;
    }
    const std::string packPath = argv[1], outDir = argv[2];
    const float cam[3] = {(float)std::atof(argv[3]), (float)std::atof(argv[4]), (float)std::atof(argv[5])};
    const float yaw = (float)std::atof(argv[6]), pitch = (float)std::atof(argv[7]);
    Rules opt;
    for (int i = 8; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dist" && i + 1 < argc) opt.dist = (float)std::atof(argv[++i]);
        else if (a == "--range" && i + 1 < argc) opt.range = (float)std::atof(argv[++i]);
        else if (a == "--lodpx" && i + 1 < argc) opt.lodpx = (float)std::atof(argv[++i]);
        else if (a == "--narrow") opt.wide = false;
    }
    opt.occRange = opt.targetRange = opt.range;
    Packs packs;
    if (!packs.open(packPath)) return 1;
    Scene sc;
    if (!loadScene(packs, sc, cam[0], cam[1], cam[0], cam[1], opt.dist + 150.0f)) {
        std::fprintf(stderr, "no ground tiles near %.1f %.1f in %s\n", cam[0], cam[1], packPath.c_str());
        return 1;
    }
    std::printf("tiles %zu, placements %u (%u without a model), models %zu, model winding agrees with normals %.0f%%\n", sc.tiles.size(),
                sc.placements, sc.missing, sc.models.size(), 100.0 * sc.normalsAgree);
    View v;
    v.client(cam, yaw, pitch, opt.dist, opt.wide);
    Target t;
    EyeResult r;
    const uint32_t inside = insideKey(sc, cam);
    renderEye(sc, v, opt, inside, t, true, &r, nullptr);
    writePicture(sc, t, outDir + "/occ_view.png");
    writeHidden(sc, v, t, r, outDir + "/occ_hidden.png");

    Tally bD, bS, iD, iS, dD, dS, dBlendD[3], dBlendS[3];
    uint32_t seenSmall = 0;
    for (size_t i = 0; i < sc.inst.size(); ++i) {
        if (!r.drawn[i]) continue;
        const Inst& in = sc.inst[i];
        const bool seen = r.pix[i] > 0;
        if (seen && r.pix[i] < 4) ++seenSmall;
        if (in.kind == kInterior) { iD.add(in, r.lod[i]); if (seen) iS.add(in, r.lod[i]); }
        else if (in.kind == kExterior) { bD.add(in, r.lod[i]); if (seen) bS.add(in, r.lod[i]); }
        else {
            dD.add(in, r.lod[i]); if (seen) dS.add(in, r.lod[i]);
            for (int b = 0; b < 3; ++b) { dBlendD[b].add(in, r.lod[i], b); if (seen) dBlendS[b].add(in, r.lod[i], b); }
        }
    }
    uint32_t tbD = 0, tbS = 0, ttD = 0, ttS = 0;
    for (size_t i = 0; i < sc.tb.size(); ++i) {
        if (!r.tbDrawn[i]) continue;
        const TileData& td = sc.tiles[sc.tb[i].tile];
        const TB& x = sc.tb[i];
        const float dx = x.c[0] - cam[0], dy = x.c[1] - cam[1], dz = x.c[2] - cam[2];
        const float dn = sqrtf(dx * dx + dy * dy + dz * dz) - x.rad;
        const int lod = dn < 40.0f ? 0 : dn < 140.0f ? 1 : 2;
        const uint32_t tris = reinterpret_cast<const W3dTerrainBatch*>(td.terr.data() + td.th->offBatches)[x.b].indexCount[lod] / 3;
        ++tbD; ttD += tris;
        if (r.tbPix[i]) { ++tbS; ttS += tris; }
    }
    std::string rep;
    char line[600];
    auto P = [&](const char* fmt, auto... a) { std::snprintf(line, sizeof line, fmt, a...); rep += line; rep += "\n"; };
    auto pct = [](uint32_t a, uint32_t b) { return b ? 100.0 * a / b : 0.0; };
    P("w3docc (6.54): %s, camera %.1f %.1f %.1f, yaw %.0f pitch %.0f deg, top screen %dx%d, draw distance %.0f, doodad range %.0f, lodpx %.2f",
      packPath.c_str(), cam[0], cam[1], cam[2], yaw, pitch, v.W, v.H, opt.dist, opt.range, opt.lodpx);
    P("pool: %s", packs.poolNote.c_str());
    if (packs.readFails) P("READ ERRORS: %u entries could not be read", packs.readFails);
    if (sc.placements && sc.missing * 2 > sc.placements) P("WARNING: most placements have no model - the numbers below are not meaningful");
    P("tiles %zu, placements %u (%u without a model); models: front side agrees with their normals for %.0f%% of sampled triangles; camera %s",
      sc.tiles.size(), sc.placements, sc.missing, 100.0 * sc.normalsAgree, inside != ~0u ? "inside a building (portal rules)" : "outdoors");
    P("");
    P("what the client draws from here vs what is seen (at least 1 pixel; draws = one per submesh, as a lone model is drawn)");
    P("%-22s %8s %8s %8s | %8s %8s %8s | hidden: %5s %5s", "", "inst", "tris", "draws", "inst", "tris", "draws", "tris", "draws");
    auto row = [&](const char* name, const Tally& d, const Tally& s) {
        if (!d.inst) { P("%-22s %8u", name, 0u); return; }
        P("%-22s %8u %8u %8u | %8u %8u %8u |        %4.0f%% %4.0f%%", name, d.inst, d.tris, d.draws, s.inst, s.tris, s.draws,
          100.0 - pct(s.tris, d.tris), 100.0 - pct(s.draws, d.draws));
    };
    row("outdoor WMO groups", bD, bS);
    row("indoor WMO groups", iD, iS);
    row("doodads", dD, dS);
    row("  opaque parts", dBlendD[0], dBlendS[0]);
    row("  alpha-keyed parts", dBlendD[1], dBlendS[1]);
    row("  blended parts", dBlendD[2], dBlendS[2]);
    P("ground batches (client LOD)   %u batches %u tris | seen %u batches %u tris | hidden %.0f%% of the batches", tbD, ttD, tbS, ttS, 100.0 - pct(tbS, tbD));
    P("(seen with fewer than 4 pixels: %u instances)", seenSmall);
    PvsFile pv;
    const std::string pvsPath = pvsPathFor(packPath);
    if (pv.load(pvsPath)) {
        CheckStat st;
        Target t2;
        const std::vector<FarBox> farB = farBoxes(packs, cam[0], cam[1], cam[0], cam[1]);
        checkView(sc, pv, cam, yaw, pitch, opt, st, t2, -1, -1, &farB);
        P("");
        if (st.noData) P("PVS (%s): no level for this camera here - the client draws as without it", pvsPath.c_str());
        else {
            P("PVS (%s): the client would draw %.0f of %.0f doodad draws (%.0f of %.0f tris), %.0f of %.0f ground batches", pvsPath.c_str(),
              st.keptDraws, st.drawnDraws, st.keptTris, st.drawnTris, st.keptBatches, st.drawnBatches);
            P("  WMO groups: %.0f of %.0f draws (%.0f of %.0f tris); far ground / far buildings %s", st.bldgKeptDraws, st.bldgDrawnDraws,
              st.bldgKeptTris, st.bldgDrawnTris, st.farHiddenViews ? (st.farMissViews ? "skipped - BUT SOMETHING PAST THE DISTANCE SHOWS" : "skipped") : "drawn");
            P("  MISSING here: %u doodads, %u ground batches, %u WMO groups%s", st.missingObjects, st.missingBatches, st.missingBldg,
              st.worstPixels ? "" : " - none");
            if (st.worstPixels) P("  worst: %s", st.worst.c_str());
        }
    } else {
        P("");
        P("(no %s: build one with w3docc --pvs to see what it would keep here)", pvsPath.c_str());
    }
    P("");
    P("not counted: units, water, particles, far ground / far buildings past the draw distance; merged doodad groups are counted per");
    P("doodad (the client draws them in fewer calls); alpha-keyed parts hide nothing here (their holes are not known).");
    P("pictures: occ_view.png (seen: WMO blue, doodads green, ground brown), occ_hidden.png (drawn but never seen: WMO red, doodads orange)");
    std::fputs(rep.c_str(), stdout);
    if (FILE* f = std::fopen((outDir + "/occ_report.txt").c_str(), "w")) { std::fputs(rep.c_str(), f); std::fclose(f); }
    return 0;
}
