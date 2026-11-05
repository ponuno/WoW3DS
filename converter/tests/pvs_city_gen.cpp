// 6.54: a synthetic walled town for the PVS (w3docc --pvs): written straight in the pack format (no game data, no converter).
//   pvs_city_gen <out.w3dp>
// 3 x 3 tiles (31..33 x 47..49, render-space origins as the converter's), ground with hills around a flat town in tile 32_48:
// an 8 x 8 grid of solid blocks (outdoor WMO groups, 24 x 24 x 18 yd) with 12-yd streets, every third block a hollow courtyard
// (four walls) with doodads inside it; lamps along the streets, trees (an opaque trunk + alpha-keyed crown cards) in the
// courtyards and the parks; a town wall around it all. Doodads in the courtyards and behind the blocks are what a PVS must hide;
// everything seen through a street must stay.
#include "w3d_format.h"
#include "w3d_ground.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr float TS = 533.33333f;
constexpr float ZP = 32.0f * TS;

struct Blob { uint32_t type, id; std::vector<uint8_t> data; };
std::vector<Blob> g_blobs;

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof x);
}
void align4(std::vector<uint8_t>& v) { while (v.size() & 3) v.push_back(0); }

bool g_peak = false;   // 6.54e: a 300-yd peak in a corner of the 3 x 3 tiles (a neighbour tile's far corner, ~1000 yd out)
float hill(float x, float y) {   // world height: flat town around the centre of tile 32_48, hills outside
    const float tx = ZP - 48.5f * TS, ty = ZP - 32.5f * TS;   // render-space centre of tile 32_48 (origin + TS/2)
    const float d = sqrtf((x - tx) * (x - tx) + (y - ty) * (y - ty));
    const float t = std::fmin(1.0f, std::fmax(0.0f, (d - 230.0f) / 120.0f));
    float z = t * (12.0f * sinf(x * 0.021f) * cosf(y * 0.017f) + 10.0f * sinf((x + y) * 0.011f) + 14.0f);
    if (g_peak) {
        const float px = x - (tx + 700.0f), py = y - (ty + 700.0f);
        z += 300.0f * expf(-(px * px + py * py) / (2.0f * 45.0f * 45.0f));
    }
    return z;
}

// ---- models: triangles in model space, one submesh per blend
struct Mesh {
    struct V { float p[3], n[3]; };
    std::vector<V> v;
    std::vector<uint16_t> idx[2];   // [0] opaque, [1] alpha-keyed
    void quad(int part, const float a[3], const float b[3], const float c[3], const float d[3]) {   // counter-clockwise seen from the front
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
        float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& c : n) c /= l;
        const uint16_t base = (uint16_t)v.size();
        for (const float* p : {a, b, c, d}) v.push_back({{p[0], p[1], p[2]}, {n[0], n[1], n[2]}});
        for (int k : {0, 1, 2, 0, 2, 3}) idx[part].push_back((uint16_t)(base + k));
    }
    void box(int part, float x0, float y0, float z0, float x1, float y1, float z1, bool bottom = false) {
        const float P[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        quad(part, P[0], P[1], P[5], P[4]);   // -y
        quad(part, P[1], P[2], P[6], P[5]);   // +x
        quad(part, P[2], P[3], P[7], P[6]);   // +y
        quad(part, P[3], P[0], P[4], P[7]);   // -x
        quad(part, P[4], P[5], P[6], P[7]);   // top
        if (bottom) quad(part, P[3], P[2], P[1], P[0]);
    }
    void card(float cx, float cy, float z0, float z1, float r, float ang) {   // a crown card: both sides
        const float dx = r * cosf(ang), dy = r * sinf(ang);
        const float a[3] = {cx - dx, cy - dy, z0}, b[3] = {cx + dx, cy + dy, z0}, c[3] = {cx + dx, cy + dy, z1}, d[3] = {cx - dx, cy - dy, z1};
        quad(1, a, b, c, d);
        quad(1, b, a, d, c);
    }
};

uint32_t addModel(uint32_t id, const Mesh& m, bool building, bool interior = false) {
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f}, rad = 0;
    for (const auto& v : m.v)
        for (int k = 0; k < 3; ++k) { mn[k] = std::fmin(mn[k], v.p[k]); mx[k] = std::fmax(mx[k], v.p[k]); }
    for (const auto& v : m.v) rad = std::fmax(rad, sqrtf(v.p[0] * v.p[0] + v.p[1] * v.p[1] + v.p[2] * v.p[2]));
    W3dModelHeader h{};
    float ext = 1;
    for (int k = 0; k < 3; ++k) ext = std::fmax(ext, std::fmax(fabsf(mn[k]), fabsf(mx[k])));
    for (int k = 0; k < 3; ++k) { h.posScale[k] = ext / 32000.0f; h.posOffset[k] = 0; h.bmin[k] = mn[k]; h.bmax[k] = mx[k]; }
    h.radius = rad;
    h.vertexCount = (uint32_t)m.v.size();
    h.indexCount = (uint32_t)(m.idx[0].size() + m.idx[1].size());
    h.submeshCount = (m.idx[0].empty() ? 0 : 1) + (m.idx[1].empty() ? 0 : 1);
    h.texCount = 0;
    h.uvScale = W3D_MODEL_UV_SCALE;
    std::vector<uint8_t> b(sizeof h);
    h.offTexIds = (uint32_t)b.size();
    h.offSubmeshes = (uint32_t)b.size();
    uint32_t first = 0;
    for (int part = 0; part < 2; ++part) {
        if (m.idx[part].empty()) continue;
        W3dSubmesh s{};
        s.tex = W3D_NO_TEXTURE;
        s.blend = part ? W3D_BLEND_ALPHAKEY : W3D_BLEND_OPAQUE;
        s.flags = (building ? W3D_SUBF_BUILDING : 0) | (interior ? W3D_SUBF_INTERIOR : 0);   // (6.56: interior-flagged halls)
        s.firstIndex = first;
        s.indexCount = (uint32_t)m.idx[part].size();
        first += s.indexCount;
        put(b, s);
    }
    align4(b);
    h.offVertices = (uint32_t)b.size();
    for (const auto& v : m.v) {
        W3dModelVertex q{};
        q.x = (int16_t)lrintf(v.p[0] / h.posScale[0]); q.y = (int16_t)lrintf(v.p[1] / h.posScale[1]); q.z = (int16_t)lrintf(v.p[2] / h.posScale[2]);
        q.nx = (int8_t)lrintf(v.n[0] * 127); q.ny = (int8_t)lrintf(v.n[1] * 127); q.nz = (int8_t)lrintf(v.n[2] * 127);
        put(b, q);
    }
    h.offIndices = (uint32_t)b.size();
    for (int part = 0; part < 2; ++part) for (uint16_t i : m.idx[part]) put(b, i);
    align4(b);
    std::memcpy(b.data(), &h, sizeof h);
    g_blobs.push_back({W3D_ENTRY_MODEL, id, std::move(b)});
    return id;
}

// ---- ground: 4 x 4 batches per tile, each a 17 x 17 grid (8.3 yd), LOD 1 / 2 every 2nd / 4th vertex
void addTerrain(int tx, int ty) {
    const float ox = ZP - (ty + 1) * TS, oy = ZP - (tx + 1) * TS;   // origin: render x follows tileY, y follows tileX (as the converter's)
    W3dTerrainHeader h{};
    h.tileX = (uint32_t)tx; h.tileY = (uint32_t)ty;
    h.origin[0] = ox; h.origin[1] = oy; h.origin[2] = 0;
    h.posScale[0] = h.posScale[1] = TS / 32000.0f; h.posScale[2] = 0.01f;
    h.gridSize = 9;
    std::vector<W3dTerrainVertex> verts;
    std::vector<uint16_t> idx;
    std::vector<W3dTerrainBatch> batches;
    for (int by = 0; by < 4; ++by)
        for (int bx = 0; bx < 4; ++bx) {
            const uint16_t base = (uint16_t)verts.size();
            W3dTerrainBatch b{};
            float zmin = 1e9f, zmax = -1e9f;
            for (int j = 0; j <= 16; ++j)
                for (int i = 0; i <= 16; ++i) {
                    const float lx = (bx * 16 + i) * TS / 64.0f, ly = (by * 16 + j) * TS / 64.0f;
                    const float z = hill(ox + lx, oy + ly);
                    zmin = std::fmin(zmin, z); zmax = std::fmax(zmax, z);
                    W3dTerrainVertex v{};
                    v.x = (int16_t)lrintf(lx / h.posScale[0]); v.y = (int16_t)lrintf(ly / h.posScale[1]); v.z = (int16_t)lrintf(z / h.posScale[2]);
                    v.r = 200;
                    verts.push_back(v);
                }
            for (int lod = 0; lod < 3; ++lod) {
                const int st = 1 << lod;
                b.firstIndex[lod] = (uint32_t)idx.size();
                for (int j = 0; j < 16; j += st)
                    for (int i = 0; i < 16; i += st) {
                        const uint16_t a = (uint16_t)(base + j * 17 + i), bb = (uint16_t)(a + st), c = (uint16_t)(a + st * 17 + st), d = (uint16_t)(a + st * 17);
                        for (uint16_t k : {a, bb, c, a, c, d}) idx.push_back(k);   // counter-clockwise from above
                    }
                b.indexCount[lod] = (uint32_t)idx.size() - b.firstIndex[lod];
            }
            b.bmin[0] = bx * TS / 4; b.bmin[1] = by * TS / 4; b.bmin[2] = zmin;
            b.bmax[0] = (bx + 1) * TS / 4; b.bmax[1] = (by + 1) * TS / 4; b.bmax[2] = zmax;
            batches.push_back(b);
        }
    h.vertexCount = (uint32_t)verts.size(); h.indexCount = (uint32_t)idx.size(); h.batchCount = (uint32_t)batches.size(); h.texCount = 0;
    std::vector<uint8_t> blob(sizeof h);
    h.offTexIds = (uint32_t)blob.size();
    h.offBatches = (uint32_t)blob.size();
    for (const auto& b : batches) put(blob, b);
    h.offVertices = (uint32_t)blob.size();
    for (const auto& v : verts) put(blob, v);
    h.offIndices = (uint32_t)blob.size();
    for (uint16_t i : idx) put(blob, i);
    align4(blob);
    h.offTex2 = 0;
    std::memcpy(blob.data(), &h, sizeof h);
    g_blobs.push_back({W3D_ENTRY_TERRAIN, w3d_tile_id((uint32_t)tx, (uint32_t)ty), std::move(blob)});
    // 6.54e: the exact heights (GRND, w3d_ground.h) of the same ground
    W3dGroundHeader gh{};
    gh.tileX = (uint32_t)tx; gh.tileY = (uint32_t)ty;
    gh.origin[0] = ox; gh.origin[1] = oy; gh.origin[2] = -50.0f;
    gh.zScale = 0.01f;
    gh.chunkCount = 256;
    std::vector<uint8_t> g(sizeof gh);
    std::memcpy(g.data(), &gh, sizeof gh);
    for (int cy = 0; cy < 16; ++cy)
        for (int cx = 0; cx < 16; ++cx) {
            W3dGroundChunk c{};
            c.flags = W3D_GROUND_CHUNK_PRESENT;
            for (int k = 0; k < 145; ++k) {
                const int row = k / 17, rem = k % 17;
                const bool centre = rem >= 9;
                const float gX = cy * 8 + row + (centre ? 0.5f : 0.0f), gY = cx * 8 + (centre ? rem - 9 : rem) + (centre ? 0.5f : 0.0f);
                const float x = ox + (1.0f - gX / 128.0f) * TS, y = oy + (1.0f - gY / 128.0f) * TS;
                c.h[k] = (uint16_t)std::fmax(0.0f, std::fmin(65535.0f, (hill(x, y) - gh.origin[2]) / gh.zScale));
            }
            put(g, c);
        }
    g_blobs.push_back({W3D_ENTRY_GROUND, w3d_tile_id((uint32_t)tx, (uint32_t)ty), std::move(g)});
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: pvs_city_gen <out.w3dp> [tall|dense]   (tall: a 120-yd town wall - nothing past it can be seen from inside;"
                  " dense: 15000 more doodads over the 3 x 3 tiles, for timing the builder as around a real city; tall peak: the walled town and a"
                  " 300-yd peak ~1000 yd out in a neighbour tile's far corner - the far ground's height per 133-yd block)");
        return 1;
    }
    const bool tall = argc > 2 && std::string(argv[2]) == "tall";
    const bool dense = argc > 2 && std::string(argv[2]) == "dense";
    g_peak = argc > 3 && std::string(argv[3]) == "peak";   // (tall peak: the walled town with a peak outside)
    for (int ty = 47; ty <= 49; ++ty)
        for (int tx = 31; tx <= 33; ++tx) addTerrain(tx, ty);
    // models
    Mesh block, court, wallX, wallY, lamp, crate, tree;
    block.box(0, -12, -12, 0, 12, 12, 18);
    court.box(0, -15, -15, 0, 15, -13, 14); court.box(0, -15, 13, 0, 15, 15, 14);
    court.box(0, -15, -13, 0, -13, 13, 14); court.box(0, 13, -13, 0, 15, 13, 14);
    wallX.box(0, -61.5f, -1.5f, -20, 61.5f, 1.5f, tall ? 120.0f : 16.0f);   // (from 20 yd under the ground: no gap over the hills' foot)
    wallY.box(0, -1.5f, -61.5f, -20, 1.5f, 61.5f, tall ? 120.0f : 16.0f);
    lamp.box(0, -0.2f, -0.2f, 0, 0.2f, 0.2f, 4); lamp.box(0, -0.6f, -0.6f, 4, 0.6f, 0.6f, 5);
    crate.box(0, -1, -1, 0, 1, 1, 2);
    tree.box(0, -0.5f, -0.5f, 0, 0.5f, 0.5f, 5);
    for (int k = 0; k < 4; ++k) tree.card(0, 0, 3, 11, 4.0f, k * 0.785f);
    addModel(0x10000001, block, true);
    addModel(0x10000002, court, true);
    addModel(0x10000005, block, true, true);   // 6.56: a block flagged interior (Stormwind's halls / canal walls): seen from afar too
    addModel(0x10000003, wallX, true);
    addModel(0x10000004, wallY, true);
    addModel(0x20000001, lamp, false);
    addModel(0x20000002, crate, false);
    addModel(0x20000003, tree, false);
    // placements of the town tile (32_48), its centre
    const float cx = ZP - 48.5f * TS, cy = ZP - 32.5f * TS;
    std::map<uint32_t, std::vector<W3dPlacement>> pl;
    auto place = [&](uint32_t model, float x, float y, float z, float yaw = 0, float scale = 1) {
        const int tx = (int)floorf(32.0f - y / TS), ty = (int)floorf(32.0f - x / TS);
        W3dPlacement p{};
        p.modelId = model; p.pos[0] = x; p.pos[1] = y; p.pos[2] = z; p.rot[2] = yaw; p.scale = scale;
        pl[w3d_tile_id((uint32_t)tx, (uint32_t)ty)].push_back(p);
    };
    const float pitch = 36.0f;   // block 24 + street 12
    int hidden = 0;
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) {
            const float x = cx + (i - 3.5f) * pitch, y = cy + (j - 3.5f) * pitch;
            if ((i + 2 * j) % 3 == 0) {   // a courtyard with crates and a tree inside
                place(0x10000002, x, y, 0);
                for (int k = 0; k < 4; ++k) { place(0x20000002, x - 8 + 5.3f * k, y - 6, 0); ++hidden; }
                place(0x20000003, x + 4, y + 5, 0); ++hidden;
            } else if ((i * 7 + j * 3) % 11 == 5) {   // a park: trees
                place(0x20000003, x - 5, y - 5, 0); place(0x20000003, x + 6, y + 4, 0);
            } else
                place((i * 5 + j * 3) % 7 == 2 ? 0x10000005 : 0x10000001, x, y, 0);
            place(0x20000001, x + 18, y + 18, 0);   // a lamp at the crossing
            place(0x20000002, x + 18, y + 5, 0);    // a crate in the street
        }
    // the town wall: 4 x 3 segments 120 yd long at 160 yd from the centre
    for (int s = -1; s <= 1; ++s) {
        place(0x10000003, cx + s * 120.0f, cy - 160, hill(cx + s * 120.0f, cy - 160));
        place(0x10000003, cx + s * 120.0f, cy + 160, hill(cx + s * 120.0f, cy + 160));
        place(0x10000004, cx - 160, cy + s * 120.0f, hill(cx - 160, cy + s * 120.0f));
        place(0x10000004, cx + 160, cy + s * 120.0f, hill(cx + 160, cy + s * 120.0f));
    }
    // trees on the hills outside
    for (int k = 0; k < 160; ++k) {
        const float a = k * 2.399963f, r = 200.0f + (k * 37 % 300);
        const float x = cx + r * cosf(a), y = cy + r * sinf(a);
        place(0x20000003, x, y, hill(x, y), a, 0.8f + (k % 5) * 0.1f);
    }
    if (dense)   // (6.54e)
        for (int k = 0; k < 15000; ++k) {
            const float x = cx + (float)((k * 7919) % 1500) - 750.0f + 0.37f * (k % 13), y = cy + (float)((k * 104729) % 1500) - 750.0f;
            if (fabsf(x - cx) < 165.0f && fabsf(y - cy) < 165.0f && k % 4) continue;   // (fewer inside the walls: the blocks)
            place(k % 3 ? 0x20000002 : 0x20000003, x, y, fabsf(x - cx) < 165.0f && fabsf(y - cy) < 165.0f ? 0.0f : hill(x, y), k * 0.1f);
        }
    for (auto& [tid, v] : pl) {
        std::vector<uint8_t> b;
        W3dPlacementHeader ph{};
        ph.count = (uint32_t)v.size();
        put(b, ph);
        for (const auto& p : v) put(b, p);
        g_blobs.push_back({W3D_ENTRY_PLACEMENT, tid, std::move(b)});
    }
    // the file
    std::vector<uint8_t> out(sizeof(W3dPackHeader));
    std::vector<W3dEntry> idx;
    for (const Blob& b : g_blobs) {
        while (out.size() % W3D_BLOB_ALIGN) out.push_back(0);
        W3dEntry e{b.type, b.id, (uint32_t)out.size(), (uint32_t)b.data.size()};
        out.insert(out.end(), b.data.begin(), b.data.end());
        idx.push_back(e);
    }
    while (out.size() % W3D_BLOB_ALIGN) out.push_back(0);
    W3dPackHeader hd{W3DP_MAGIC, W3DP_VERSION, (uint32_t)idx.size(), (uint32_t)out.size()};
    for (const W3dEntry& e : idx) put(out, e);
    std::memcpy(out.data(), &hd, sizeof hd);
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("pvs town: %zu entries, %zu KB; town centre %.1f %.1f; %d doodads inside courtyards\n", idx.size(), out.size() / 1024, cx, cy, hidden);
    return 0;
}
