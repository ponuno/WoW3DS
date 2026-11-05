// render.cpp — sky, terrain, doodads and water for all streamed tiles.
//
// Everything is drawn camera-relative: the CPU builds per-object modelView
// matrices in float32 from (object - camera), because PICA200 uniforms are
// float24 and absolute coordinates (~17000 yd) would visibly jitter.
#include "w3d_farlod.h"
#include "w3d_farobj.h"
#include "world.h"
#include "hlod.hpp"
#include "w3d_hlod.h"

#include <array>
#include <vector>
#include <math.h>
#include <string.h>

#include <algorithm>
#include "sky.hpp"
#include <map>
#include <unordered_map>

#include "w3d_anim.h"

#include "model_shbin.h"
#include "skin_shbin.h"
#include "vs_fold.hpp"
#include "terrain_shbin.h"
#include "fx_shbin.h"
#include "fx.hpp"
#include "perflog.hpp"
#include "static_batch.hpp"

namespace {

// Sky colours (RGB). Fog uses the horizon colour so distant terrain melts into the sky.
// 6.60: the sky's five colours (top, middle, two horizon bands, fog = the horizon) - these defaults until rendererSetSky says otherwise
// (the zone's and the server's time of day, sky.cpp); the dome is two vertex buffers so a new colouring never touches the one the GPU reads
enum { kSkyTop, kSkyMid, kSkyBand1, kSkyBand2, kSkyFog, kSkyN };
uint8_t g_skyCol[kSkyN][3] = {{70, 120, 200}, {110, 155, 215}, {150, 185, 225}, {175, 200, 228}, {190, 210, 230}};
inline u32 fogConstColor(const uint8_t c[3]) { return (u32)c[0] | ((u32)c[1] << 8) | ((u32)c[2] << 16) | 0xFF000000u; }
float g_skyBase[kSkyN][3] = {{70, 120, 200}, {110, 155, 215}, {150, 185, 225}, {175, 200, 228}, {190, 210, 230}};   // the zone's colours, before the spirit world's grading
float g_ghostFogShare = 0.55f;   // 6.80
float g_ghost = 0.0f;   // 6.62: 0 alive .. 1 ghost: the sky and the fog go grey
u32 g_fogColor = fogConstColor(g_skyCol[kSkyFog]);   // this frame's fog: the horizon, or the water's colour under the surface

struct Shader {
    DVLB_s* dvlb = nullptr;
    shaderProgram_s prog;
    int uProj = -1, uMV = -1, uScale = -1, uOffset = -1, uUV = -1, uLight = -1, uLightParams = -1, uFog = -1, uBones = -1;
    int uMask = -1;
    int uUvA = -1, uUvB = -1;   // 6.41: the model shader's texture transform rows
    int uMvp = -1;              // 6.70: the model shader's projection * modelView * T(offset) * S(scale)
};
Shader g_terrainSh, g_modelSh, g_skinSh, g_fxSh;
C3D_AttrInfo g_terrainAttr, g_modelAttr, g_skinAttr, g_fxAttr;
std::vector<W3dMat34> g_pose;   // scratch: bone matrices of the creature being drawn
C3D_Mtx g_proj;
float g_near = 1.0f, g_far = 400.0f, g_skyRadius = 300.0f;
// 6.34: the horizon - the ground (detailed of the loaded tiles, far of the others) and the far buildings' shells are drawn this
// far, past the draw distance (which stays the distance of the real buildings and doodads); the fog ends there
// (6.34b: a setting - off by default, it costs GPU time: rendererSetHorizon)
float g_horizonWant = 0.0f;   // 0 = off: the horizon is the draw distance (as before 6.34)
float g_horizon = 400.0f;
float g_fogX = 0.0f, g_fogY = 1.0f;   // visibility = clamp(g_fogY - dist * g_fogX, 0, 1)
// Terrain LOD switch distances: W3D_TERRAIN_LOD1_DIST / LOD2_DIST (w3d_terrain_lod.h, by region since 6.30).

// Object marker: an elongated diamond whose front tip shows the facing (model layout).
W3dModelVertex* g_markerV = nullptr;
int g_markerVerts = 0;
constexpr float kMarkerQ = 16384.0f;   // int16 units per marker unit

// Sky dome (terrain vertex layout, unit radius scaled by posScale).
W3dTerrainVertex* g_skyBuf[2] = {nullptr, nullptr};
int g_skyCur = 0;
uint16_t* g_skyI = nullptr;
int g_skyIndexCount = 0;
constexpr int kSkyRings = 9, kSkySegs = 16;
// 6.61: the sun and the moon: a bright disc inside a soft halo each, drawn added onto the dome; the vertices are rebuilt when the hour changes
// (three buffers in turn so the GPU never reads one being written)
constexpr int kCelSegs = 16, kCelVerts = 2 * 2 * kCelSegs * 3;   // (sun, moon) x (halo, core) x segments x 3
W3dTerrainVertex* g_celBuf[3] = {nullptr, nullptr, nullptr};
int g_celCur = 0, g_celCount = 0;
constexpr float kSkyElev[kSkyRings + 1] = {-10, 0, 4, 8, 15, 22, 33, 45, 65, 90};   // degrees of each ring

// 6.41: the model shader's texture transform: identity (a still texture), or the rows of one animated part
// 6.70: the model shader's uniforms are kept here and folded before a draw (vs_fold.hpp): the shader no longer scales, offsets,
// projects in two steps or scales the uv / normal per vertex. Every model-program draw calls modelVsFlush() first (drawFlush()).
struct ModelVs {
    C3D_Mtx proj, mv;
    float scale[4] = {1, 1, 1, 1}, off[3] = {0, 0, 0};
    float uvA[3] = {1, 0, 0}, uvB[3] = {0, 1, 0}, light[3] = {0, 0, 1};
    bool matDirty = true, uvDirty = true, lightDirty = true;
};
ModelVs g_mvs;
const C3D_Mtx* g_foldedLast = nullptr;   // 7.05: whose folded matrices the registers hold (mvsFolded; null: unknown)
bool g_progModel = false;   // the model program is the bound one (drawFlush folds its uniforms)
vsfold::M4 toM4(const C3D_Mtx& c) {
    vsfold::M4 r;
    for (int i = 0; i < 4; ++i) { r.m[i][0] = c.r[i].x; r.m[i][1] = c.r[i].y; r.m[i][2] = c.r[i].z; r.m[i][3] = c.r[i].w; }
    return r;
}
C3D_Mtx fromM4(const vsfold::M4& m) {
    C3D_Mtx c;
    for (int i = 0; i < 4; ++i) { c.r[i].x = m.m[i][0]; c.r[i].y = m.m[i][1]; c.r[i].z = m.m[i][2]; c.r[i].w = m.m[i][3]; }
    return c;
}
void bindProgram(Shader& sh) {
    C3D_BindProgram(&sh.prog);
    g_progModel = &sh == &g_modelSh;
    if (g_progModel) g_mvs.matDirty = g_mvs.uvDirty = g_mvs.lightDirty = true;   // (another program may have used the registers)
}
void mvsProj(const C3D_Mtx& m) { g_mvs.proj = m; g_mvs.matDirty = true; }
void mvsMV(const C3D_Mtx& m) { g_mvs.mv = m; g_mvs.matDirty = true; g_foldedLast = nullptr; }
void mvsScale(float x, float y, float z, float uv) {
    if (x != g_mvs.scale[0] || y != g_mvs.scale[1] || z != g_mvs.scale[2]) g_mvs.matDirty = true;
    if (uv != g_mvs.scale[3]) g_mvs.uvDirty = true;
    g_mvs.scale[0] = x; g_mvs.scale[1] = y; g_mvs.scale[2] = z; g_mvs.scale[3] = uv;
}
void mvsOffset(float x, float y, float z, float) {
    if (x != g_mvs.off[0] || y != g_mvs.off[1] || z != g_mvs.off[2]) g_mvs.matDirty = true;
    g_mvs.off[0] = x; g_mvs.off[1] = y; g_mvs.off[2] = z;
}
void mvsUvA(float a, float b, float c, float) { g_mvs.uvA[0] = a; g_mvs.uvA[1] = b; g_mvs.uvA[2] = c; g_mvs.uvDirty = true; }
void mvsUvB(float a, float b, float c, float) { g_mvs.uvB[0] = a; g_mvs.uvB[1] = b; g_mvs.uvB[2] = c; g_mvs.uvDirty = true; }
void mvsLight(float x, float y, float z, float) { g_mvs.light[0] = x; g_mvs.light[1] = y; g_mvs.light[2] = z; g_mvs.lightDirty = true; }
void modelVsFlush() {
    if (g_mvs.matDirty) {
        g_foldedLast = nullptr;
        const vsfold::M4 mvF = vsfold::foldScaleOffset(toM4(g_mvs.mv), g_mvs.scale, g_mvs.off);
        const C3D_Mtx mvC = fromM4(mvF), mvpC = fromM4(vsfold::mul(toM4(g_mvs.proj), mvF));
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uMvp, &mvpC);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uMV, &mvC);
        g_mvs.matDirty = false;
    }
    if (g_mvs.uvDirty) {
        float a[3], b[3];
        vsfold::foldUvRow(g_mvs.uvA, g_mvs.scale[3], a);
        vsfold::foldUvRow(g_mvs.uvB, g_mvs.scale[3], b);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uUvA, a[0], a[1], a[2], 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uUvB, b[0], b[1], b[2], 0);
        g_mvs.uvDirty = false;
    }
    if (g_mvs.lightDirty) {
        constexpr float k = 1.0f / 127.0f;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLight, g_mvs.light[0] * k, g_mvs.light[1] * k, g_mvs.light[2] * k, 0);
        g_mvs.lightDirty = false;
    }
}
inline void drawFlush() { if (g_progModel) modelVsFlush(); }
// 7.05: an instance's matrices folded once (drawModels: per instance, not per instance and part); the upload is skipped when
// the same instance's matrices are in the registers already (the parts of one model drawn one after another). mv is the
// unfolded one, kept for whoever folds next.
void mvsFolded(const C3D_Mtx& mvp, const C3D_Mtx& mvFolded, const C3D_Mtx& mv) {
    if (g_foldedLast == &mvp && !g_mvs.matDirty) return;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uMvp, &mvp);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uMV, &mvFolded);
    g_mvs.mv = mv;
    g_mvs.matDirty = false;
    g_foldedLast = &mvp;
}

void setUvIdentity() {
    mvsUvA(1, 0, 0, 0);
    mvsUvB(0, 1, 0, 0);
}

void initShader(Shader& s, const u8* bin, u32 size) {
    s.dvlb = DVLB_ParseFile((u32*)bin, size);
    shaderProgramInit(&s.prog);
    shaderProgramSetVsh(&s.prog, &s.dvlb->DVLE[0]);
    auto loc = [&](const char* n) { return shaderInstanceGetUniformLocation(s.prog.vertexShader, n); };
    s.uProj = loc("projection");
    s.uMV = loc("modelView");
    s.uScale = loc("posScale");
    s.uOffset = loc("posOffset");
    s.uUV = loc("uvOffset");
    s.uLight = loc("lightDir");
    s.uLightParams = loc("lightParams");
    s.uFog = loc("fogParams");
    s.uBones = loc("bones");
    s.uMask = loc("fogMask");
    s.uUvA = loc("uvRowA");
    s.uUvB = loc("uvRowB");
    s.uMvp = loc("mvp");
}

// The colour of the sky at an elevation (degrees): the fog at the horizon, band 2, band 1, the middle, the top at the zenith.
void skyColorAt(float el, uint8_t out[3]) {
    static const float stop[kSkyN] = {0, 8, 22, 45, 90};
    static const int idx[kSkyN] = {kSkyFog, kSkyBand2, kSkyBand1, kSkyMid, kSkyTop};
    if (el <= stop[0]) { memcpy(out, g_skyCol[idx[0]], 3); return; }
    for (int k = 0; k + 1 < kSkyN; ++k)
        if (el <= stop[k + 1]) {
            const float f = (el - stop[k]) / (stop[k + 1] - stop[k]);
            for (int c = 0; c < 3; ++c) out[c] = (uint8_t)(g_skyCol[idx[k]][c] + (g_skyCol[idx[k + 1]][c] - g_skyCol[idx[k]][c]) * f + 0.5f);
            return;
        }
    memcpy(out, g_skyCol[idx[kSkyN - 1]], 3);
}

void fillSky(W3dTerrainVertex* vb) {
    int k = 0;
    for (int r = 0; r <= kSkyRings; ++r) {
        // elevation from -10 deg (below the horizon, hides the far-plane edge) to the zenith
        const float elDeg = kSkyElev[r], el = elDeg * 3.14159265f / 180.0f;
        uint8_t c[3];
        skyColorAt(elDeg, c);
        for (int sg = 0; sg <= kSkySegs; ++sg, ++k) {
            const float az = 6.2831853f * sg / kSkySegs;
            W3dTerrainVertex& v = vb[k];
            v.x = (int16_t)(32000.0f * cosf(el) * cosf(az));
            v.y = (int16_t)(32000.0f * cosf(el) * sinf(az));
            v.z = (int16_t)(32000.0f * sinf(el));
            v.w = 0;
            v.r = c[0]; v.g = c[1]; v.b = c[2];
            v.a = 255;
            v.u = v.v = 0;
        }
    }
    GSPGPU_FlushDataCache(vb, (kSkyRings + 1) * (kSkySegs + 1) * sizeof(W3dTerrainVertex));
}

// A fan disc of angular radius 'rad' (degrees) around unit direction d on the dome (radius 31000): the centre colour c * cIn, the rim c * cOut.
int celDisc(W3dTerrainVertex* out, const float d[3], float radDeg, const float c[3], float cIn, float cOut) {
    float ux = d[1] * 1.0f - d[2] * 0.0f, uy = -d[0], uz = 0.0f;   // d x (0, 0, 1)
    float ul = sqrtf(ux * ux + uy * uy + uz * uz);
    if (ul < 1e-3f) { ux = 1; uy = 0; uz = 0; ul = 1; }
    ux /= ul; uy /= ul; uz /= ul;
    const float vx = d[1] * uz - d[2] * uy, vy = d[2] * ux - d[0] * uz, vz = d[0] * uy - d[1] * ux;
    const float R = 31000.0f, r = R * tanf(radDeg * 3.14159265f / 180.0f);
    auto vert = [&](W3dTerrainVertex& v, float px, float py, float pz, float k) {
        v.x = (int16_t)px; v.y = (int16_t)py; v.z = (int16_t)pz; v.w = 0;
        v.r = (uint8_t)(c[0] * k); v.g = (uint8_t)(c[1] * k); v.b = (uint8_t)(c[2] * k); v.a = 255;
        v.u = v.v = 0;
    };
    const float cx = d[0] * R, cy = d[1] * R, cz = d[2] * R;
    int n = 0;
    for (int i = 0; i < kCelSegs; ++i) {
        const float a0 = 6.2831853f * i / kCelSegs, a1 = 6.2831853f * (i + 1) / kCelSegs;
        vert(out[n++], cx, cy, cz, cIn);
        vert(out[n++], cx + r * (ux * cosf(a0) + vx * sinf(a0)), cy + r * (uy * cosf(a0) + vy * sinf(a0)), cz + r * (uz * cosf(a0) + vz * sinf(a0)), cOut);
        vert(out[n++], cx + r * (ux * cosf(a1) + vx * sinf(a1)), cy + r * (uy * cosf(a1) + vy * sinf(a1)), cz + r * (uz * cosf(a1) + vz * sinf(a1)), cOut);
    }
    return n;
}

void buildSky() {
    const int nv = (kSkyRings + 1) * (kSkySegs + 1);
    for (int b = 0; b < 2; ++b) g_skyBuf[b] = (W3dTerrainVertex*)linearAlloc(nv * sizeof(W3dTerrainVertex));
    g_skyI = (uint16_t*)linearAlloc(kSkyRings * kSkySegs * 6 * sizeof(uint16_t));
    for (int b = 0; b < 3; ++b) {
        g_celBuf[b] = (W3dTerrainVertex*)linearAlloc(kCelVerts * sizeof(W3dTerrainVertex));
        if (g_celBuf[b]) memset(g_celBuf[b], 0, kCelVerts * sizeof(W3dTerrainVertex));
    }
    fillSky(g_skyBuf[0]);
    g_skyCur = 0;
    int i = 0;
    for (int r = 0; r < kSkyRings; ++r)
        for (int sg = 0; sg < kSkySegs; ++sg) {
            uint16_t a = (uint16_t)(r * (kSkySegs + 1) + sg), b = (uint16_t)(a + 1), c = (uint16_t)(a + kSkySegs + 1), d = (uint16_t)(c + 1);
            uint16_t q[6] = {a, b, c, c, b, d};
            memcpy(g_skyI + i, q, sizeof q);
            i += 6;
        }
    g_skyIndexCount = i;
    GSPGPU_FlushDataCache(g_skyI, i * sizeof(uint16_t));
}

void buildMarker() {
    const float B[3] = {0, 0, 0}, T[3] = {0, 0, 1};
    const float F[3] = {0.9f, 0, 0.6f}, L[3] = {0, 0.45f, 0.6f}, K[3] = {-0.45f, 0, 0.6f}, R[3] = {0, -0.45f, 0.6f};
    const float* tris[8][3] = {{T, F, L}, {T, L, K}, {T, K, R}, {T, R, F}, {B, L, F}, {B, K, L}, {B, R, K}, {B, F, R}};
    g_markerVerts = 24;
    g_markerV = (W3dModelVertex*)linearAlloc(g_markerVerts * sizeof(W3dModelVertex));
    int k = 0;
    for (auto& t : tris) {
        float e1[3], e2[3], n[3];
        for (int i = 0; i < 3; ++i) { e1[i] = t[1][i] - t[0][i]; e2[i] = t[2][i] - t[0][i]; }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int v = 0; v < 3; ++v, ++k) {
            W3dModelVertex& o = g_markerV[k];
            o.x = (int16_t)(t[v][0] * kMarkerQ);
            o.y = (int16_t)(t[v][1] * kMarkerQ);
            o.z = (int16_t)(t[v][2] * kMarkerQ);
            o.w = 0;
            o.nx = (int8_t)(n[0] / l * 127);
            o.ny = (int8_t)(n[1] / l * 127);
            o.nz = (int8_t)(n[2] / l * 127);
            o.pad = 0;
            o.u = o.v = 0;
        }
    }
    GSPGPU_FlushDataCache(g_markerV, g_markerVerts * sizeof(W3dModelVertex));
}

// Flat targeting ring with an arrow (Forever style): an annulus 0.82..1.0 and an arrowhead pointing along +x.
W3dModelVertex* g_ringV = nullptr;
int g_ringVerts = 0;

void buildRing() {
    const int N = 28;
    std::vector<std::array<float, 2>> tri;   // 2D triangles
    auto add = [&](float ax, float ay, float bx, float by, float cx, float cy) {
        tri.push_back({ax, ay}); tri.push_back({bx, by}); tri.push_back({cx, cy});
    };
    const float ri = 0.82f, ro = 1.0f;
    for (int i = 0; i < N; ++i) {
        float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        add(ri * c0, ri * s0, ro * c0, ro * s0, ro * c1, ro * s1);
        add(ri * c0, ri * s0, ro * c1, ro * s1, ri * c1, ri * s1);
    }
    add(1.75f, 0, 1.1f, 0.38f, 1.1f, -0.38f);   // arrowhead outside the ring
    add(1.1f, 0.1f, 0.9f, 0.1f, 0.9f, -0.1f);   // stem joining it to the ring
    add(1.1f, 0.1f, 0.9f, -0.1f, 1.1f, -0.1f);
    g_ringVerts = (int)tri.size();
    g_ringV = (W3dModelVertex*)linearAlloc(g_ringVerts * sizeof(W3dModelVertex));
    for (int k = 0; k < g_ringVerts; ++k) {
        W3dModelVertex& o = g_ringV[k];
        o.x = (int16_t)(tri[k][0] * kMarkerQ * 0.5f);   // radius 1 -> 8192 (|2| fits int16 with the 0.5 factor)
        o.y = (int16_t)(tri[k][1] * kMarkerQ * 0.5f);
        o.z = 0;
        o.w = 0;
        o.nx = 0; o.ny = 0; o.nz = 127;
        o.pad = 0;
        o.u = o.v = 0;
    }
    GSPGPU_FlushDataCache(g_ringV, g_ringVerts * sizeof(W3dModelVertex));
}

// The selection circle (stage 5.44; shape of WoWee's selection_circle shader: a thin bright ring at r 0.92..1.0 and a faint disc
// inside it): [0, g_selRingVerts) the ring (shape 3), [g_selRingVerts, g_selVerts) the disc (shape 4, drawn dim).
W3dModelVertex* g_selV = nullptr;
int g_selVerts = 0, g_selRingVerts = 0;

void buildSelection() {
    const int N = 40;
    std::vector<std::array<float, 2>> tri;
    auto add = [&](float ax, float ay, float bx, float by, float cx, float cy) {
        tri.push_back({ax, ay}); tri.push_back({bx, by}); tri.push_back({cx, cy});
    };
    const float ri = 0.92f, ro = 1.0f;
    for (int i = 0; i < N; ++i) {
        float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        add(ri * c0, ri * s0, ro * c0, ro * s0, ro * c1, ro * s1);
        add(ri * c0, ri * s0, ro * c1, ro * s1, ri * c1, ri * s1);
    }
    g_selRingVerts = (int)tri.size();
    for (int i = 0; i < N; ++i) {   // the disc inside the ring
        float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        add(0, 0, ri * cosf(a0), ri * sinf(a0), ri * cosf(a1), ri * sinf(a1));
    }
    g_selVerts = (int)tri.size();
    g_selV = (W3dModelVertex*)linearAlloc(g_selVerts * sizeof(W3dModelVertex));
    for (int k = 0; k < g_selVerts; ++k) {
        W3dModelVertex& o = g_selV[k];
        o.x = (int16_t)(tri[k][0] * kMarkerQ * 0.5f);   // as the ring: radius 1 -> 8192, drawn with scale 2 / kMarkerQ
        o.y = (int16_t)(tri[k][1] * kMarkerQ * 0.5f);
        o.z = 0;
        o.w = 0;
        o.nx = 0; o.ny = 0; o.nz = 127;
        o.pad = 0;
        o.u = o.v = 0;
    }
    GSPGPU_FlushDataCache(g_selV, g_selVerts * sizeof(W3dModelVertex));
}

// ------------------------------------------------------------------ frustum
struct Frustum {
    float cam[3];
    float n[6][3];
    float d[6];
    float maxDist;
    float horizon;   // 6.34
    void build(const Camera& c, float nearZ) {
        memcpy(cam, c.pos, sizeof cam);
        float f[3];
        c.forward(f);
        float r[3] = {f[1], -f[0], 0};  // fwd x Z
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
        if (rl < 1e-4f) { r[0] = 1; r[1] = 0; rl = 1; }
        r[0] /= rl; r[1] /= rl;
        float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        const float hv = (30.0f + 3.0f) * 3.14159265f / 180.0f;               // vertical half-FOV + margin
        const float hh = atanf(tanf(30.0f * 3.14159265f / 180.0f) * 400.0f / 240.0f) + 3.0f * 3.14159265f / 180.0f;
        float sh = sinf(hh), ch = cosf(hh), sv = sinf(hv), cv = cosf(hv);
        for (int k = 0; k < 3; ++k) {
            n[0][k] = f[k] * sh + r[k] * ch;   // left
            n[1][k] = f[k] * sh - r[k] * ch;   // right
            n[2][k] = f[k] * sv + u[k] * cv;   // bottom
            n[3][k] = f[k] * sv - u[k] * cv;   // top
            n[4][k] = f[k];                    // near
            n[5][k] = -f[k];                   // far
        }
        d[0] = d[1] = d[2] = d[3] = 0;
        d[4] = -nearZ;
        d[5] = c.drawDist;
        maxDist = c.drawDist;
        horizon = std::max(c.drawDist, g_horizon);
    }
    // 6.34: the side planes and the horizon (the ground and the far shells)
    bool sphereFar(const float c[3], float rad) const {
        float p[3] = {c[0] - cam[0], c[1] - cam[1], c[2] - cam[2]};
        float lim = horizon + rad;
        if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] > lim * lim) return false;
        for (int i = 0; i < 5; ++i)
            if (n[i][0] * p[0] + n[i][1] * p[1] + n[i][2] * p[2] + d[i] < -rad) return false;
        return true;
    }
    bool sphere(const float c[3], float rad) const {
        float p[3] = {c[0] - cam[0], c[1] - cam[1], c[2] - cam[2]};
        float lim = maxDist + rad;
        if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] > lim * lim) return false;
        for (int i = 0; i < 6; ++i)
            if (n[i][0] * p[0] + n[i][1] * p[1] + n[i][2] * p[2] + d[i] < -rad) return false;
        return true;
    }
};

static uint32_t g_gtOff = 0;   // 6.46: RenderSettings::gpuTestOff of this frame (the GPU test)
struct BindCache {
    GpuTex* unit[3] = {nullptr, nullptr, nullptr};   // (6.32: was [2] - the terrain's third texture (5.47) wrote past it)
    void bind(int u, GpuTex* t, FrameCounters& fc) {
        if (g_gtOff & kGtTextures) t = streamerWhiteTexture();   // 6.46: the GPU test without texture fetches from memory
        if (unit[u] == t) return;
        C3D_TexBind(u, &t->tex);
        unit[u] = t;
        ++fc.texBinds;
    }
};

// Fog is computed per vertex (see terrain.v.pica): the hardware fog LUT is
// indexed by depth and drew near objects fully fogged on New 3DS. The shader
// puts fog visibility in the vertex colour's green channel; this TEV stage does
// rgb = lerp(fogColour, previous, visibility) and passes alpha through.
// 6.77: the spirit world as in the original - the world itself goes grey, not only the sky and the fog. Two stages turn the colour so
// far into its luminance (the PICA has no plain dot product with a constant: DOT3 is 4 * sum((a - .5)(b - .5)) and clamps, so the
// colour is first moved to 0.5 + c / 2 and the constant is 0.5 + w / 2 - the result is sum(c * w) >= 0, 7 bits of the colour kept);
// alpha passes. Only while the main world is drawn as a ghost (not the portraits, not the HUD). Returns the next free stage.
bool g_ghostPass = false;
int ghostStages(int stage) {
    if (!g_ghostPass) return stage;
    C3D_TexEnv* env = C3D_GetTexEnv(stage);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MULTIPLY_ADD);   // c * .5 + .5
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFF808080u);
    env = C3D_GetTexEnv(stage + 1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_DOT3_RGB);       // luminance .30 .59 .11 -> constant .65 .795 .555
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFF8ECBA6u);                // (ABGR: r 0xA6, g 0xCB, b 0x8E)
    return stage + 2;
}

// fog (and, as a ghost, the grey stages in front of it - the fog keeps the cold colour of the spirit world); returns the next free stage
int fogStage(int stage, bool grey = true) {
    if (grey) stage = ghostStages(stage);
    C3D_TexEnv* env = C3D_GetTexEnv(stage);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_G);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, g_fogColor);
    return stage + 1;
}

void resetStages(int from) {
    for (int i = from; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
}

void setFogUniform(const Shader& sh, bool on, bool world) {
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    if (on) C3D_FVUnifSet(GPU_VERTEX_SHADER, sh.uFog, g_fogX, g_fogY, world ? 1.0f : 0.0f, 0);
    else C3D_FVUnifSet(GPU_VERTEX_SHADER, sh.uFog, 0.0f, 1.0f, world ? 1.0f : 0.0f, 0);
}

void camRelative(C3D_Mtx* mv, const C3D_Mtx& view, const float pos[3], const float cam[3]) {
    *mv = view;
    Mtx_Translate(mv, pos[0] - cam[0], pos[1] - cam[1], pos[2] - cam[2], true);
}

// ------------------------------------------------------------------ passes
void drawSky(const C3D_Mtx& view, FrameCounters& fc) {
    bindProgram(g_terrainSh);
    C3D_SetAttrInfo(&g_terrainAttr);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_skyBuf[g_skyCur], sizeof(W3dTerrainVertex), 3, 0x210);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uProj, &g_proj);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &view);
    float s = g_skyRadius / 32000.0f;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, s, s, s, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, 0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uUV, 0, 0, 0, 0);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    resetStages(1);
    setFogUniform(g_terrainSh, false, false);   // raw vertex colour
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_CullFace(GPU_CULL_NONE);
    drawFlush();
    C3D_DrawElements(GPU_TRIANGLES, g_skyIndexCount, C3D_UNSIGNED_SHORT, g_skyI);
    ++fc.drawCalls;
}

// 6.61: a zone's sky model (LightSkybox: clouds, stars, a horizon) over the dome, scaled to it; static, unlit, no depth
Model* g_skyModel = nullptr;
void setModelBlend(uint8_t blend, bool twoSided, bool noCullAll);
void drawSkyModel(const C3D_Mtx& view, const RenderSettings& rs, FrameCounters& fc) {
    Model* m = g_skyModel;
    if (!m || !m->h || m->state != AssetState::Ready || !m->texturesReady() || !m->subs) return;
    const W3dModelHeader* h = m->h;
    float ext = 1.0f;
    for (int a = 0; a < 3; ++a) ext = std::max(ext, std::max(std::fabs(h->bmin[a]), std::fabs(h->bmax[a])));
    const float sc = 0.92f * g_skyRadius / ext;
    C3D_Mtx mv = view;
    Mtx_Scale(&mv, sc, sc, sc);
    bindProgram(g_modelSh);
    setUvIdentity();
    C3D_SetAttrInfo(&g_modelAttr);
    mvsProj(g_proj);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    resetStages(1);
    setFogUniform(g_modelSh, false, true);
    BindCache bc;
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, m->blob + h->offVertices, sizeof(W3dModelVertex), 3, 0x210);
    mvsScale(h->posScale[0], h->posScale[1], h->posScale[2], w3d_model_uv_scale(h));
    mvsOffset(h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
    mvsMV(mv);
    mvsLight(0, 0, 1, 0);
    const uint16_t* idx = (const uint16_t*)(m->blob + h->offIndices);
    for (int pass = 0; pass < 2; ++pass) {   // opaque, then blended
        for (u32 i = 0; i < h->submeshCount; ++i) {
            const W3dSubmesh& sm = m->subs[i];
            const bool blended = sm.blend == W3D_BLEND_ALPHA || sm.blend == W3D_BLEND_ADD;
            if (blended != (pass == 1)) continue;
            setModelBlend(sm.blend, true, rs.modelsNoCull);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);   // (the sky never touches the depth)
            bc.bind(0, sm.tex >= m->tex.size() ? streamerWhiteTexture() : m->tex[sm.tex], fc);   // 6.99: (also a model let go this frame: its list is empty)
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 1.0f, 1.0f, 0, 1.0f);   // unlit
            drawFlush();
            C3D_DrawElements(GPU_TRIANGLES, sm.indexCount, C3D_UNSIGNED_SHORT, idx + sm.firstIndex);
            ++fc.drawCalls;
            fc.tris += sm.indexCount / 3;
        }
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

// 6.61: the sun and the moon, added onto the sky (no depth: the ground and everything else is drawn over them)
void drawCelestial(const C3D_Mtx& view, FrameCounters& fc) {
    if (g_celCount <= 0 || !g_celBuf[g_celCur]) return;
    bindProgram(g_terrainSh);
    C3D_SetAttrInfo(&g_terrainAttr);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_celBuf[g_celCur], sizeof(W3dTerrainVertex), 3, 0x210);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uProj, &g_proj);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &view);
    const float s = g_skyRadius / 32000.0f;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, s, s, s, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, 0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uUV, 0, 0, 0, 0);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    resetStages(1);
    setFogUniform(g_terrainSh, false, false);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    C3D_CullFace(GPU_CULL_NONE);
    drawFlush();
    C3D_DrawArrays(GPU_TRIANGLES, 0, g_celCount);
    ++fc.drawCalls;
}

// 6.54d: the PVS says nothing past rs.pvsFarR can be seen from the camera's cell: a sphere wholly past it is skipped
static inline bool pvsFarSkip(const RenderSettings& rs, const float cam[3], const float c[3], float rad) {
    if (!rs.pvsOn || !rs.pvsFarHidden) return false;
    const float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
    const float lim = rs.pvsFarR + rad;
    return dx * dx + dy * dy + dz * dz >= lim * lim;
}

void drawTerrain(const Camera& cam, const C3D_Mtx& view, const Frustum& fr, const RenderSettings& rs, FrameCounters& fc) {
    bindProgram(g_terrainSh);
    C3D_SetAttrInfo(&g_terrainAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uProj, &g_proj);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uUV, 0, 0, 0, 0);

    // 2 layers: stage 0 rgb = lerp(tex0, tex1, vertex.a); stage 1 *= baked light (vertex red); stage 2 fog.
    // 3 layers (5.47): stage 1 rgb = lerp(prev, tex2, vertex.b = second overlay weight), light and fog move up one.
    int envLayers = 0;   // 0 = not set yet this frame
    auto setTerrainEnv = [&](int layers) {
        if (envLayers == layers) return;
        envLayers = layers;
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        if (layers == 1) {   // 6.46: the GPU test - the base layer alone (one texture fetch a pixel)
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        } else {
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE0, GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        }
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvColor(env, 0xFFFFFFFF);
        int next = 1;
        if (layers == 3) {
            env = C3D_GetTexEnv(next++);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE2, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_B);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        }
        env = C3D_GetTexEnv(next++);   // rgb *= baked light (vertex colour red)
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        next = fogStage(next);
        resetStages(next);
    };
    setFogUniform(g_terrainSh, rs.fog, true);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_BACK_CCW);   // verified on hardware (stage 2)

    BindCache bc;
    // 6.24: the far ground where a tile's detailed ground is not there (being read, or not loaded): instead of a hole into the
    // sky. Both texture units sample the colour atlas (vertex a = 0), r = baked light; 0.6 yd lower than the true heights so
    // that wherever it touches the detailed ground, the detailed one is in front.
    {
        static std::vector<FarDraw> far;
        GpuTex* atlas = nullptr;
        const uint16_t* fidx = nullptr;
        float zs = 0.25f;
        if (streamerFarDraws(cam.pos, fr.horizon, fr.maxDist, far, &atlas, &fidx, &zs)) {
            setTerrainEnv(2);
            bc.bind(0, atlas, fc);
            bc.bind(1, atlas, fc);
            for (const FarDraw& d : far) {
                if (!fr.sphereFar(d.center, d.radius)) continue;
                if (pvsFarSkip(rs, cam.pos, d.center, d.radius)) { ++fc.pvsFar; continue; }   // 6.54d
                C3D_BufInfo* buf = C3D_GetBufInfo();
                BufInfo_Init(buf);
                BufInfo_Add(buf, d.verts, sizeof(W3dTerrainVertex), 3, 0x210);
                C3D_Mtx mv;
                camRelative(&mv, view, d.origin, cam.pos);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &mv);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, W3D_FAR_XY_SCALE, W3D_FAR_XY_SCALE, zs, W3D_FAR_UV_SCALE);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, 0, 0, -W3D_FAR_SINK, 0);
                drawFlush();
                C3D_DrawElements(GPU_TRIANGLES, W3D_FAR_INDICES, C3D_UNSIGNED_SHORT, fidx);
                ++fc.drawCalls;
                fc.tris += W3D_FAR_INDICES / 3;
            }
        }
    }
    // 6.29: the far buildings - coarse shells of the large buildings whose tile is not loaded or that their tile does not show
    // yet, with the far ground's shader (palette on both units); drawn from both sides (a shell is not closed everywhere)
    {
        static std::vector<FarObjDraw> fo;
        GpuTex* pal = nullptr;
        float zs = 0.25f;
        if (streamerFarObjectDraws(cam.pos, fr.horizon, fr.maxDist, fo, &pal, &zs)) {
            setTerrainEnv(2);
            bc.bind(0, pal, fc);
            bc.bind(1, pal, fc);
            C3D_CullFace(GPU_CULL_NONE);
            const W3dTerrainVertex* boundV = nullptr;
            for (const FarObjDraw& d : fo) {
                if (!fr.sphereFar(d.center, d.radius)) continue;
                if (pvsFarSkip(rs, cam.pos, d.center, d.radius)) { ++fc.pvsFar; continue; }   // 6.54d
                if (d.verts != boundV) {
                    C3D_BufInfo* buf = C3D_GetBufInfo();
                    BufInfo_Init(buf);
                    BufInfo_Add(buf, d.verts, sizeof(W3dTerrainVertex), 3, 0x210);
                    C3D_Mtx mv;
                    camRelative(&mv, view, d.origin, cam.pos);
                    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &mv);
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, W3D_FAROBJ_XY_SCALE, W3D_FAROBJ_XY_SCALE, zs, W3D_FAR_UV_SCALE);
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, 0, 0, 0, 0);
                    boundV = d.verts;
                }
                drawFlush();
                C3D_DrawElements(GPU_TRIANGLES, d.count, C3D_UNSIGNED_SHORT, d.idx);
                ++fc.drawCalls;
                fc.tris += d.count / 3;
            }
            C3D_CullFace(GPU_CULL_BACK_CCW);
        }
    }
    // 6.57: the horizon - a far block's buildings and large doodads as one mesh (hlod.cpp decided which, before collectModels)
    if (!(g_gtOff & kGtBuildings)) {
        const std::vector<hlod::Draw>& hd = hlod::draws();
        GpuTex* pal = hlod::palette();
        if (!hd.empty() && pal) {
            setTerrainEnv(2);
            bc.bind(0, pal, fc);
            bc.bind(1, pal, fc);
            C3D_CullFace(GPU_CULL_NONE);
            for (const hlod::Draw& d : hd) {
                if (!fr.sphereFar(d.center, d.radius)) continue;
                C3D_BufInfo* buf = C3D_GetBufInfo();
                BufInfo_Init(buf);
                BufInfo_Add(buf, d.verts, sizeof(W3dTerrainVertex), 3, 0x210);
                C3D_Mtx mv;
                camRelative(&mv, view, d.origin, cam.pos);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &mv);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, W3D_HLOD_POS_UNIT, W3D_HLOD_POS_UNIT, W3D_HLOD_POS_UNIT, W3D_FAR_UV_SCALE);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, 0, 0, 0, 0);
                drawFlush();
                C3D_DrawElements(GPU_TRIANGLES, d.count, C3D_UNSIGNED_SHORT, d.idx);
                ++fc.drawCalls;
                fc.tris += d.count / 3;
            }
            C3D_CullFace(GPU_CULL_BACK_CCW);
        }
    }
    for (Tile* t : streamerReadyTiles()) {
        if (tileBeyond(t->tx, t->ty, cam.pos, fr.maxDist)) continue;   // 6.34c: its far ground mesh is drawn instead (above)
        const W3dTerrainHeader* h = t->th;
        const uint16_t* tex2 = (g_gtOff & kGtTerrainLayers) ? nullptr : w3d_terrain_tex2(h, t->terr);
        setTerrainEnv((g_gtOff & kGtTerrainLayers) ? 1 : tex2 ? 3 : 2);
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, t->terr + h->offVertices, sizeof(W3dTerrainVertex), 3, 0x210);
        C3D_Mtx mv;
        camRelative(&mv, view, h->origin, cam.pos);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &mv);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, h->posScale[0], h->posScale[1], h->posScale[2], W3D_TERRAIN_UV_SCALE);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
        const uint16_t* idx = (const uint16_t*)(t->terr + h->offIndices);
        // 6.30: the LOD of each region (all its batches alike: skirts are only on region borders)
        int8_t regLod[W3D_TERRAIN_REGIONS];
        const bool byRegion = t->batchRegion.size() == h->batchCount;
        for (int r = 0; r < W3D_TERRAIN_REGIONS; ++r)
            regLod[r] = byRegion && t->regions[r].used ? (int8_t)w3d_terrain_region_lod(&t->regions[r], h->origin, cam.pos) : 0;
        for (u32 i = 0; i < h->batchCount; ++i) {
            const W3dTerrainBatch& b = t->batches[i];
            float c[3], r2 = 0;
            for (int k = 0; k < 3; ++k) {
                c[k] = h->origin[k] + 0.5f * (b.bmin[k] + b.bmax[k]);
                float e = 0.5f * (b.bmax[k] - b.bmin[k]);
                r2 += e * e;
            }
            float rad = sqrtf(r2);
            if (!fr.sphereFar(c, rad)) continue;   // (6.34: the loaded tiles' ground to the horizon, LOD 2 out there)
            if (pvsFarSkip(rs, cam.pos, c, rad)) { ++fc.pvsFar; continue; }   // 6.54d: past what can be seen from here
            if (rs.pvsOn && t->pvsStamp == rs.pvsStamp && i < t->pvsTerr.size() && t->pvsTerr[i] != rs.pvsStamp) {   // 6.54: the PVS
                const float ex = c[0] - cam.pos[0], ey = c[1] - cam.pos[1], ez = c[2] - cam.pos[2];
                const float dc = sqrtf(ex * ex + ey * ey + ez * ez);
                if (dc - rad > rs.pvsNear && dc - rad < rs.pvsMaxDist) { ++fc.pvsTerr; continue; }   // (past its distance: the horizon, drawn)
            }
            int lod;
            if (byRegion) {
                lod = regLod[t->batchRegion[i]];   // (capped by the coarsest level all the region's batches keep their ground at, 6.13)
            } else {   // (no caps yet: the batch's own sphere)
                float dx = c[0] - cam.pos[0], dy = c[1] - cam.pos[1], dz = c[2] - cam.pos[2];
                float d = sqrtf(dx * dx + dy * dy + dz * dz) - rad;
                lod = d < W3D_TERRAIN_LOD1_DIST ? 0 : d < W3D_TERRAIN_LOD2_DIST ? 1 : 2;
            }
            if (rs.forceLod >= 0) lod = rs.forceLod;
            if (i < t->batchMaxLod.size() && lod > t->batchMaxLod[i]) lod = t->batchMaxLod[i];   // stage 6.13: no ground lost at holes
            bc.bind(0, t->ttex[b.tex0], fc);
            if (!(g_gtOff & kGtTerrainLayers)) bc.bind(1, t->ttex[b.tex1], fc);
            if (tex2) bc.bind(2, t->ttex[tex2[i]], fc);
            drawFlush();
            C3D_DrawElements(GPU_TRIANGLES, b.indexCount[lod], C3D_UNSIGNED_SHORT, idx + b.firstIndex[lod]);
            ++fc.drawCalls;
            ++fc.visibleBatches;
            ++fc.lodBatches[lod];
            fc.tris += b.indexCount[lod] / 3;
        }
    }
}

void setModelBlend(uint8_t blend, bool twoSided, bool noCullAll) {
    // Foliage (alpha-keyed / blended) is often single-sided without the flag; never cull it.
    bool noCull = noCullAll || twoSided || blend != W3D_BLEND_OPAQUE;
    C3D_CullFace(noCull ? GPU_CULL_NONE : GPU_CULL_BACK_CCW);
    switch (blend) {
        case W3D_BLEND_ALPHAKEY:
            C3D_AlphaTest(true, GPU_GREATER, 0x80);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
            break;
        case W3D_BLEND_ALPHA:
            C3D_AlphaTest(true, GPU_GREATER, 0x08);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
                           GPU_ONE_MINUS_SRC_ALPHA);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
            break;
        case W3D_BLEND_ADD:
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
            break;
        default:
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    }
}

std::vector<Model*> g_activeModels;   // models with visible instances this frame

// Stage 6.11: fade-in. 'born' of an instance = the latest of: its model became drawable, its tile's ground did, its building
// opened. Wrap-safe (ms clock of the streamer).
static inline uint32_t laterMs(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0 ? a : b; }
static uint32_t g_nowMs = 0;
static bool g_fadeOn = true;

// One instance: drawn this frame?
// 6.42: which interior groups of the WMOs are seen (wmo_portals.hpp: WoWee's rules). The tile's building groups become a portal graph
// once every one of them is in (a model still loading would look like a missing room). Each frame the first interior group asks, the
// walk decides whether the camera is surely inside a building (then that building's interior groups are drawn only when reached, up to
// kPortalDist yards) or not (nothing is culled: the old rule, an interior group within its radius + 25 yards). Packs without portal
// tables (older converters) always keep the old rule.
static constexpr float kPortalDist = 70.0f;
static uint32_t g_portalFrame = 0;
static bool g_haveViewer = false;
static float g_viewer[3] = {0, 0, 0};
static bool portalSphere(void* ctx, const float c[3], float r) { return ((const Frustum*)ctx)->sphere(c, r); }
static bool portalsReady(Tile* t, const Frustum& fr) {
    if (t->pstate == 2) return false;
    if (t->pstate == 1 && t->pinstances != t->instances.size()) t->pstate = 0;   // (the tile's instances changed: again)
    if (t->pstate == 0) {
        bool any = false;
        for (const Instance& in : t->instances) {
            const Model* m = in.model;
            if (!m->building) continue;
            if (m->state != AssetState::Ready) return false;   // a group still loading: the graph waits
            if (m->h && m->portals) any = true;
        }
        if (!any) { t->pstate = 2; return false; }
        t->pnodes.clear();
        for (uint32_t i = 0; i < t->instances.size(); ++i) {
            Instance& in = t->instances[i];
            in.pnode = -1;
            const Model* m = in.model;
            if (!m->building || !m->h) continue;
            wmoportal::Node n;
            n.modelId = m->id;
            if (m->portals) { n.portals = w3d_portals_of(m->portals); n.nPortals = m->portals->count; n.hasTable = true; }
            n.interior = m->interior;
            memcpy(n.pos, in.pos, sizeof n.pos);
            memcpy(n.rot, in.rot, sizeof n.rot);
            memcpy(n.bmin, m->h->bmin, sizeof n.bmin);
            memcpy(n.bmax, m->h->bmax, sizeof n.bmax);
            n.radius = m->h->radius * in.scale;
            n.inst = i;
            in.pnode = (int32_t)t->pnodes.size();
            t->pnodes.push_back(std::move(n));
        }
        wmoportal::buildGraph(t->pnodes);
        t->pinstances = (uint32_t)t->instances.size();
        t->pstate = 1;
        t->pframe = 0;
    }
    if (t->pframe != g_portalFrame) {
        t->pframe = g_portalFrame;
        t->pcomp = wmoportal::reach(t->pnodes, fr.cam, g_haveViewer ? g_viewer : nullptr, portalSphere, (void*)&fr, g_portalFrame);
        // 6.56: the other buildings from outside - their interior groups seen through an opening (Stormwind's canal walls and halls
        // are "interior" groups: drawn only within radius + 25 before, holes in the town at a distance)
        wmoportal::reachOutside(t->pnodes, fr.cam, t->pcomp, fr.maxDist, portalSphere, (void*)&fr, g_portalFrame);
    }
    return true;
}

// 6.44: small static doodads merged per cell (static_batch.hpp). Plan: once every model of the tile is in, the eligible instances are
// grouped by 32-yd cell, size class and owning building (two or more members). Decision, once a frame per group (the first member that
// is collected asks): merged when the whole group lies inside the doodads' solid distance (no member at the dissolving edge), every member
// has finished fading in and its building is shown; then its batches are drawn and its members skipped. A group's buffers are built the
// first time it would be merged (one group a frame, kStaticCapBytes of linear memory over all tiles); until then, and on every "no",
// its members are drawn one by one as before.
static constexpr uint32_t kStaticCapBytes = 8u << 20;
// (6.44b: a tile and indices, not pointers into its vectors - a group built later in the same frame grows Tile::sbatches and moved the batches
// queued before it: the GPU then read freed memory as vertex addresses and counts and hung on the command list)
struct ActiveBatch { Tile* t; uint32_t batch, group; uint8_t lod; };
static std::vector<ActiveBatch> g_activeBatches;
static bool g_batchesSorted = false;
static int g_staticBuildsLeft = 0;
static uint32_t g_staticBytes = 0;

static bool staticPlan(Tile* t) {
    if (t->sstate == 1 && t->sinstances == t->instances.size()) return true;
    for (const Instance& in : t->instances)
        if (in.model->state != AssetState::Ready) return false;   // (a model still loading: plan later)
    if (t->sstate == 1) {   // (the instances changed: the old buffers go with the tile; a fresh plan)
        for (Instance& in : t->instances) in.sgroup = -1;
    }
    for (Instance& in : t->instances) in.sgroup = -1;
    t->sgroups.clear();
    struct Key {
        int cx, cy, b;
        uint16_t owner;
        bool operator<(const Key& o) const {
            return cx != o.cx ? cx < o.cx : cy != o.cy ? cy < o.cy : b != o.b ? b < o.b : owner < o.owner;
        }
    };
    std::map<Key, uint32_t> byKey;
    std::vector<Tile::StaticGroup> groups;
    for (uint32_t i = 0; i < t->instances.size(); ++i) {
        const Instance& in = t->instances[i];
        const Model* m = in.model;
        if (!m->h || m->building || m->fx || m->meshHidden || m->fxAnim.a || m->uva || in.bldg || !(in.scale > 0)) continue;
        const float r = m->h->radius * in.scale;
        const int b = sbatch::bucketOf(r);
        if (b < 0 || !sbatch::eligibleModel(m->blob, m->size)) continue;
        const Key k{(int)floorf(in.pos[0] / sbatch::kCell), (int)floorf(in.pos[1] / sbatch::kCell), b, in.owner};
        auto it = byKey.find(k);
        if (it == byKey.end()) {
            it = byKey.emplace(k, (uint32_t)groups.size()).first;
            groups.emplace_back();
            groups.back().owner = in.owner;
        }
        groups[it->second].members.push_back(i);
    }
    for (Tile::StaticGroup& g : groups) {
        if (g.members.size() < 2) continue;   // (a group of one is no gain)
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        g.minR = 1e30f;
        g.maxR = 0;
        for (uint32_t i : g.members) {
            const Instance& in = t->instances[i];
            const float r = in.model->h->radius * in.scale;
            g.minR = std::min(g.minR, r);
            g.maxR = std::max(g.maxR, r);
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], in.pos[k] - r); hi[k] = std::max(hi[k], in.pos[k] + r); }
        }
        for (int k = 0; k < 3; ++k) g.center[k] = 0.5f * (lo[k] + hi[k]);
        g.radius = 0;
        for (uint32_t i : g.members) {
            const Instance& in = t->instances[i];
            const float dx = in.pos[0] - g.center[0], dy = in.pos[1] - g.center[1], dz = in.pos[2] - g.center[2];
            g.radius = std::max(g.radius, sqrtf(dx * dx + dy * dy + dz * dz) + in.model->h->radius * in.scale);
        }
        t->sgroups.push_back(std::move(g));
    }
    for (uint32_t gi = 0; gi < t->sgroups.size(); ++gi)
        for (uint32_t i : t->sgroups[gi].members) t->instances[i].sgroup = (int32_t)gi;
    t->sinstances = (uint32_t)t->instances.size();
    t->sstate = 1;
    return true;
}

static bool staticBuild(Tile* t, Tile::StaticGroup& g) {
    std::vector<sbatch::MemberIn> mem;
    std::vector<std::vector<uintptr_t>> keys(g.members.size());
    uint32_t born = 0;
    for (size_t k = 0; k < g.members.size(); ++k) {
        const Instance& in = t->instances[g.members[k]];
        const Model* m = in.model;
        if (!m->drawable) return false;   // (its textures are not in yet: later)
        born = laterMs(born, m->drawableMs);
        keys[k].resize(m->h->submeshCount);
        for (uint32_t s = 0; s < m->h->submeshCount; ++s)
            keys[k][s] = (uintptr_t)(m->subs[s].tex == W3D_NO_TEXTURE || m->subs[s].tex >= m->tex.size() ? streamerWhiteTexture()
                                                                                                            : m->tex[m->subs[s].tex]);
        sbatch::MemberIn mi;
        mi.blob = m->blob;
        memcpy(mi.pos, in.pos, sizeof mi.pos);
        memcpy(mi.rot, in.rot, sizeof mi.rot);
        mi.scale = in.scale;
        mi.texKeys = keys[k].data();
        mem.push_back(mi);
    }
    std::vector<sbatch::BatchOut> out = sbatch::build(mem, g.center);
    const uint32_t first = (uint32_t)t->sbatches.size();
    for (sbatch::BatchOut& o : out) {
        Tile::StaticBatch b;
        b.tex = (GpuTex*)o.tex;
        b.blend = o.blend;
        b.flags = o.flags;
        const size_t vb = o.v.size() * sizeof(W3dModelVertex), ib = o.idx.size() * sizeof(uint16_t);
        b.v = (W3dModelVertex*)linearMemAlign(vb, 16);
        b.idx = (uint16_t*)linearAlloc(ib);
        if (!b.v || !b.idx) {   // out of linear memory: this group stays single (what was made of it is freed with the tile)
            if (b.v) linearFree(b.v);
            if (b.idx) linearFree(b.idx);
            g.state = 2;
            g.firstBatch = first;
            g.batchCount = 0;
            return false;
        }
        b.vBytes = (uint32_t)vb;   // 6.52
        b.iBytes = (uint32_t)ib;
        memcpy(b.v, o.v.data(), vb);
        memcpy(b.idx, o.idx.data(), ib);
        GSPGPU_FlushDataCache(b.v, vb);
        GSPGPU_FlushDataCache(b.idx, ib);
        memcpy(b.first, o.first, sizeof b.first);
        memcpy(b.count, o.count, sizeof b.count);
        memcpy(b.scale, o.scale, sizeof b.scale);
        memcpy(b.offset, o.offset, sizeof b.offset);
        b.uvScale = o.uvScale;
        t->sbatches.push_back(b);
        t->sbytes += (uint32_t)(vb + ib);
        g_staticBytes += (uint32_t)(vb + ib);
    }
    g.firstBatch = first;
    g.batchCount = (uint32_t)(t->sbatches.size() - first);
    g.bornMs = born;
    g.state = 1;
    return true;
}

// Is the group drawn merged this frame? (its batches are queued when it is, and in view)
static void staticDecide(Tile* t, Tile::StaticGroup& g, const Frustum& fr, const RenderSettings& rs, FrameCounters& fc) {
    g.frame = g_portalFrame;
    g.merged = false;
    if (g.state == 2) return;
    const Tile::Bldg* og = g.owner && g.owner <= t->bldgs.size() ? &t->bldgs[g.owner - 1] : nullptr;
    if (og && !og->open) return;
    const float dx = g.center[0] - fr.cam[0], dy = g.center[1] - fr.cam[1], dz = g.center[2] - fr.cam[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    const float solid = 0.75f * (40.0f + rs.doodadRange * g.minR);   // (collectInstance: a doodad dissolves past 75% of its distance)
    if (dist + g.radius > solid) return;
    if (g.state == 0) {
        if (g_staticBuildsLeft <= 0 || g_staticBytes >= kStaticCapBytes) return;
        --g_staticBuildsLeft;
        if (!staticBuild(t, g)) return;
        ++fc.sbBuilt;
    }
    if (g_fadeOn) {   // (every member done fading in, its building too)
        uint32_t born = laterMs(g.bornMs, t->readyMs);
        if (og) born = laterMs(born, og->openMs);
        if (g_nowMs - born < kFadeMs) return;
    }
    g.merged = true;
    if (!fr.sphere(g.center, g.radius)) return;   // every member is out of view
    const float dn = std::max(0.0f, dist - g.radius);
    const uint8_t lod = rs.forceLod >= 0 ? (uint8_t)std::min(rs.forceLod, 2) : w3d_lod_for(dn * dn, g.maxR);
    const uint32_t gi = (uint32_t)(&g - t->sgroups.data());
    for (uint32_t k = 0; k < g.batchCount; ++k) g_activeBatches.push_back({t, g.firstBatch + k, gi, lod});
    ++fc.sbGroups;
    fc.sbMembers += (u32)g.members.size();
}

static inline void collectInstance(Tile* t, Instance& in, const Frustum& fr, const RenderSettings& rs, FrameCounters& fc) {
    Model* m = in.model;
    if (!m->drawable || !m->h) return;   // stage 5.30: each model as soon as it and its textures are in
    if (m->meshHidden) return;           // 6.17: an emitter volume (its particles come from the tile's instances, not from here)
    const Tile::Bldg* bg = in.bldg && in.bldg <= t->bldgs.size() && m->building && !m->interior ? &t->bldgs[in.bldg - 1] : nullptr;
    if (bg && !bg->open) return;         // stage 6.11: a building is shown whole (or when a lagging group is given up on)
    // 6.33: a WMO's doodad only with its building (a building's own group that turned out a doodad-like model counts too)
    const Tile::Bldg* og = in.owner && in.owner <= t->bldgs.size() ? &t->bldgs[in.owner - 1] : nullptr;
    if (og && !og->open) return;
    if (in.radius <= 0) in.radius = m->h->radius * in.scale;
    if (in.radius <= 0) return;
    if (!rs.doodads && !m->building) return;
    if ((g_gtOff & kGtBuildings) && m->building) return;   // 6.46: the GPU test
    // 6.57: its block is far: the horizon's mesh stands for it
    if (rs.hlodStamp && in.hblock >= 0 && t->hlodFar[in.hblock] == rs.hlodStamp) { ++fc.hlodSkip; return; }
    // Interior-only groups (rooms, Stormwind's halls and cellars) are drawn only when the camera is near them (radius + 25), or:
    // 6.42: in the building the camera is inside, when the portal walk reached them (to kPortalDist); 6.56: from outside, when the
    // walk from the building's outdoor groups through its openings reached them (to the draw distance, as the game).
    bool portalGated = false, outsideSeen = false, linked = false;
    if (m->interior && m->portals && portalsReady(t, fr) && in.pnode >= 0) {
        const wmoportal::Node& pn = t->pnodes[in.pnode];
        if (t->pcomp != wmoportal::kNone && pn.comp == t->pcomp) {   // the camera is inside this building: only the rooms reached
            if (pn.stamp != g_portalFrame) return;
            portalGated = true;
        } else outsideSeen = pn.ostamp == g_portalFrame;
        linked = pn.outdoorLinked;
    }
    // 6.57f: an interior group WITHOUT portals in a tile whose buildings have portal tables has no opening to be seen through: only
    // from inside its own box (Stormwind's stand-in cathedral spires: no portals and a radius that made "near" half the town)
    // (6.57g: not through the tile's portal graph - the spires stand in a tile without any building with portals: its graph never exists)
    if (m->interior && !m->portals && g_packHasPortals) {
        const float d[3] = {fr.cam[0] - in.pos[0], fr.cam[1] - in.pos[1], fr.cam[2] - in.pos[2]};
        float l[3];
        wmoportal::unrotate(in.rot, d, l);
        for (int k = 0; k < 3; ++k)
            if (l[k] < m->h->bmin[k] - 1.0f || l[k] > m->h->bmax[k] + 1.0f) return;
    }
    if (rs.dbgBldg && m->building) {   // 6.57e: the test of pad.txt 'dbgbldg'
        const int k = rs.dbgBldg;
        if ((k == 1 && m->interior && !portalGated) || (k == 2 && !m->interior) || (k == 3 && m->interior && m->portals && !portalGated) ||
            (k == 4 && m->interior && !m->portals))
            return;
    }
    // 6.54: the PVS - a doodad (6.54d: or WMO group) of a covered tile that cannot be seen from the camera's cell (nearer than pvsNear +
    // its radius: drawn anyway)
    // (6.56: not an interior group seen from outside unless the file (v4) knows them: the walk / the old rule decide those)
    // (6.56d: far only with portals - an unreachable interior group is inside-only scenery, e.g. Stormwind's stand-in cathedral spires)
    // (6.57c: and only one whose building has an outdoor group - the portals of the stand-in cathedral lead only among its own)
    const bool farInterior = m->interior && !portalGated && (outsideSeen || (rs.interiorFar && m->portals && linked));
    if (rs.pvsOn && t->pvsStamp == rs.pvsStamp && in.pvsStamp != rs.pvsStamp && !(farInterior && !rs.pvsInteriors)) {
        const float dx = in.pos[0] - fr.cam[0], dy = in.pos[1] - fr.cam[1], dz = in.pos[2] - fr.cam[2];
        const float lim = rs.pvsNear + in.radius;
        if (dx * dx + dy * dy + dz * dz > lim * lim) { ++(m->building ? fc.pvsBldg : fc.pvsDood); return; }
    }
    if (in.sgroup >= 0 && rs.staticBatch && in.sgroup < (int32_t)t->sgroups.size()) {   // 6.44: merged with its cell's doodads?
        Tile::StaticGroup& g = t->sgroups[in.sgroup];
        if (g.frame != g_portalFrame) staticDecide(t, g, fr, rs, fc);
        if (g.merged) return;
    }
    // Small doodads fade out long before the draw distance (bushes ~110 yd,
    // big trees reach the fog); this is what keeps draw calls in budget.
    // Buildings are kept to the draw distance.
    float maxD = m->interior ? (farInterior ? fr.maxDist + in.radius : in.radius + (portalGated ? kPortalDist : 25.0f))
               : m->building ? fr.maxDist + in.radius : 40.0f + rs.doodadRange * in.radius;
    float dx = in.pos[0] - fr.cam[0], dy = in.pos[1] - fr.cam[1], dz = in.pos[2] - fr.cam[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > maxD * maxD) return;
    if (!fr.sphere(in.pos, in.radius)) return;
    float fade = 1.0f;
    if (g_fadeOn && !m->interior && !m->fx && !m->fxAnim.a) {
        uint32_t born = laterMs(m->drawableMs, t->readyMs);
        if (bg) born = laterMs(born, bg->openMs);
        if (og) born = laterMs(born, og->openMs);   // (6.33: fades in with its building)
        const uint32_t age = g_nowMs - born;
        if (age < kFadeMs) fade = (float)age * (1.0f / (float)kFadeMs);
        if (!m->building) {   // a small doodad dissolves over the last 25% of its distance instead of popping out
            const float lim = maxD * 0.75f;
            if (d2 > lim * lim) fade = std::min(fade, (maxD - sqrtf(d2)) / (maxD - lim));
        }
        if (fade < 0.03f) return;
        if (fade >= 0.97f) fade = 1.0f;
    }
    in.fade = fade;
    if (fade < 1.0f) { ++m->fadeCount; ++fc.fading; }
    if (m->lods && rs.forceLod >= 0)      // 6.47: the GPU test's "lightest LOD everywhere" (it reached only terrain and merged doodads)
        in.lod = (uint8_t)std::min(rs.forceLod, 2);
    else if (m->lods && m->lodErr[0] > 0) {   // 6.47: a building group's levels by its error's size on the screen
        const float dn = std::max(0.0f, sqrtf(d2) - in.radius);
        in.lod = w3d_lod_for_error(dn, m->lodErr, in.scale > 0 ? in.scale : 1.0f, rs);
        ++fc.bldgLod[in.lod];
    } else
        in.lod = !m->lods ? 0 : m->foliage ? w3d_lod_for_foliage(d2, in.radius) : w3d_lod_for(d2, in.radius);   // stage 6.12: by the size on the screen (6.84: foliage nearer)
    if (m->visible.empty()) g_activeModels.push_back(m);
    m->visible.push_back(&in);
    if (m->building) ++fc.buildings;
    else ++fc.visibleInstances;
}

// Stage 6.10: a chunk's largest radii, once every model in it is in (Tile::Chunk)
static void chunkRadii(Tile* t, Tile::Chunk& ch) {
    Tile::Chunk c = ch;
    c.maxRb = c.maxRd = c.maxRi = 0;
    c.hasB = c.hasD = c.hasI = false;
    for (uint32_t i = ch.begin; i < ch.end; ++i) {
        Instance& in = t->instances[i];
        const Model* m = in.model;
        if (m->state != AssetState::Ready) return;   // not yet: tested one by one meanwhile
        if (!m->h) continue;                          // (failed: never drawn)
        if (in.radius <= 0) in.radius = m->h->radius * in.scale;
        if (in.radius <= 0) continue;
        if (m->interior) { c.hasI = true; c.maxRi = std::max(c.maxRi, in.radius); }
        else if (m->building) { c.hasB = true; c.maxRb = std::max(c.maxRb, in.radius); }
        else { c.hasD = true; c.maxRd = std::max(c.maxRd, in.radius); }
    }
    c.ready = true;
    ch = c;
}

void collectModels(const Frustum& fr, const RenderSettings& rs, FrameCounters& fc) {
    ++g_portalFrame;
    g_activeBatches.clear();   // 6.44
    g_batchesSorted = false;
    g_staticBuildsLeft = 1;
    g_staticBytes = 0;
    for (Tile* t : streamerReadyTiles()) g_staticBytes += t->sbytes;
    if (rs.staticBatch)
        for (Tile* t : streamerReadyTiles()) staticPlan(t);
    for (Tile* t : streamerReadyTiles()) {
        if (t->chunks.empty()) {   // (no grid: a tile built elsewhere)
            for (Instance& in : t->instances) collectInstance(t, in, fr, rs, fc);
            continue;
        }
        for (Tile::Chunk& ch : t->chunks) {
            if (!ch.ready) chunkRadii(t, ch);
            if (ch.ready) {
                // the farthest any of its instances is drawn, and the sphere around all of them
                const bool doods = ch.hasD && rs.doodads;
                if (!ch.hasB && !ch.hasI && !doods) { ++fc.chunksCulled; continue; }
                float maxD = 0, r = 0;
                if (ch.hasB) { maxD = fr.maxDist + ch.maxRb; r = ch.maxRb; }
                // (6.56: an interior group seen through an opening from outside is drawn to the draw distance; each decides itself)
                if (ch.hasI) { maxD = std::max(maxD, fr.maxDist + ch.maxRi); r = std::max(r, ch.maxRi); }
                if (doods) { maxD = std::max(maxD, 40.0f + rs.doodadRange * ch.maxRd); r = std::max(r, ch.maxRd); }
                const float dx = ch.center[0] - fr.cam[0], dy = ch.center[1] - fr.cam[1], dz = ch.center[2] - fr.cam[2];
                const float lim = maxD + ch.half;
                if (dx * dx + dy * dy + dz * dz > lim * lim) { ++fc.chunksCulled; continue; }
                if (!fr.sphere(ch.center, ch.half + r)) { ++fc.chunksCulled; continue; }
            }
            ++fc.chunksTested;
            for (uint32_t i = ch.begin; i < ch.end; ++i) collectInstance(t, t->instances[i], fr, rs, fc);
        }
    }
}

// Stage 6.10: every visible instance's model-view matrix once a frame (they were built again for every part of a model,
// and in both passes): Model::mvBase indexes g_instMv in the order of Model::visible.
static std::vector<C3D_Mtx> g_instMv;
static bool g_mvReady = false;
static void buildMatrices(const Camera& cam, const C3D_Mtx& view) {
    // 6.44: models in the order of their first part's texture - neighbours that share one skip the bind (BindCache)
    std::sort(g_activeModels.begin(), g_activeModels.end(), [](const Model* a, const Model* b) {
        const GpuTex* ta = a->h && a->h->submeshCount && a->subs[0].tex < a->tex.size() ? a->tex[a->subs[0].tex] : nullptr;
        const GpuTex* tb = b->h && b->h->submeshCount && b->subs[0].tex < b->tex.size() ? b->tex[b->subs[0].tex] : nullptr;
        return ta != tb ? ta < tb : a < b;
    });
    size_t n = 0;
    for (Model* m : g_activeModels) n += m->visible.size();
    g_instMv.resize(n);
    uint32_t k = 0;
    for (Model* m : g_activeModels) {
        m->mvBase = k;
        for (const Instance* in : m->visible) {
            C3D_Mtx& mv = g_instMv[k++];
            camRelative(&mv, view, in->pos, cam.pos);
            if (in->rot[2] != 0) Mtx_RotateZ(&mv, in->rot[2], true);   // (a turn by 0 changes nothing)
            if (in->rot[1] != 0) Mtx_RotateY(&mv, in->rot[1], true);
            if (in->rot[0] != 0) Mtx_RotateX(&mv, in->rot[0], true);
            if (in->scale != 1.0f) Mtx_Scale(&mv, in->scale, in->scale, in->scale);
        }
    }
    g_mvReady = true;
}

void drawModels(const Camera& cam, const C3D_Mtx& view, const RenderSettings& rs, bool blendedPass, FrameCounters& fc) {
    bindProgram(g_modelSh);
    setUvIdentity();   // 6.41: the uniform registers are shared by the shaders: never left to chance
    C3D_SetAttrInfo(&g_modelAttr);
    mvsProj(g_proj);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    // rgb = texture * light (vertex colour red), alpha = texture alpha
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    // 5.35j: stage 1 multiplies by a constant - white, except the parts of effect models that fade / are tinted
    C3D_TexEnv* tintEnv = C3D_GetTexEnv(1);
    C3D_TexEnvInit(tintEnv);
    C3D_TexEnvSrc(tintEnv, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(tintEnv, C3D_Both, GPU_MODULATE);
    C3D_TexEnvColor(tintEnv, 0xFFFFFFFFu);
    u32 tintNow = 0xFFFFFFFFu;     // stage 6.11: the constant of stage 1 now (white; a fading instance's opacity; an effect's tint)
    bool tintAlphaConst = false;   // stage 6.11: stage 1 takes its alpha from the constant alone (opaque parts of fading instances)
    resetStages(fogStage(2));
    setFogUniform(g_modelSh, rs.fog, true);

    if (!g_mvReady) buildMatrices(cam, view);
    BindCache bc;
    bool uvMoved = false;   // 6.41: the shader holds a part's texture transform now
    for (Model* m : g_activeModels) {
        const W3dModelHeader* h = m->h;
        // 6.52: the GPU test's phase "vertices in VRAM" draws from a copy of the blob there once it is made (null otherwise)
        const uint8_t* vcopy = streamerGtVramCopy(m->blob, m->size);
        const uint8_t* base = vcopy ? vcopy : m->blob;
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, base + h->offVertices, sizeof(W3dModelVertex), 3, 0x210);
        mvsScale(h->posScale[0], h->posScale[1], h->posScale[2], w3d_model_uv_scale(h));
        mvsOffset(h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
        const uint16_t* idx = (const uint16_t*)(base + h->offIndices);
        // 7.05: the instances' matrices folded once for all the parts (a part riding a bone - fxAnim - still folds its own)
        static std::vector<C3D_Mtx> foldMv, foldMvp;
        const bool folded = !m->fxAnim.a;
        if (folded) {
            const C3D_Mtx* mvs0 = g_instMv.data() + m->mvBase;
            const size_t nv = m->visible.size();
            if (foldMv.size() < nv) { foldMv.resize(nv); foldMvp.resize(nv); }
            const vsfold::M4 P = toM4(g_mvs.proj);
            for (size_t vi = 0; vi < nv; ++vi) {
                const vsfold::M4 f = vsfold::foldScaleOffset(toM4(mvs0[vi]), g_mvs.scale, g_mvs.off);
                foldMv[vi] = fromM4(f);
                foldMvp[vi] = fromM4(vsfold::mul(P, f));
            }
        }
        g_foldedLast = nullptr;   // (the arrays are reused by every model: the same address is another model's matrix now)
        for (u32 s = 0; s < h->submeshCount; ++s) {
            const W3dSubmesh& sm = m->subs[s];
            bool isBlended = sm.blend == W3D_BLEND_ALPHA || sm.blend == W3D_BLEND_ADD;
            // stage 6.11: the opaque and cut-out parts of the instances that are fading are drawn alpha-blended, in the
            // transparent pass (depth is still written: a half-visible wall hides what is behind it)
            const bool fadeOnly = blendedPass && !isBlended && m->fadeCount;
            if (isBlended != blendedPass && !fadeOnly) continue;
            if ((g_gtOff & kGtAlphaKey) && sm.blend == W3D_BLEND_ALPHAKEY && !m->building) continue;   // 6.51: the GPU test
            // (5.31: drawing buildings from both sides did not close the holes in Stormwind's walls - they were cracks
            // from the simplification, fixed in the converter - so buildings are culled again: the GPU time matters.)
            setModelBlend(sm.blend, sm.flags & W3D_SUBF_TWO_SIDED, rs.modelsNoCull);
            if (fadeOnly) {
                C3D_AlphaTest(sm.blend == W3D_BLEND_ALPHAKEY, GPU_GREATER, 0x08);
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
            }
            const bool wantConst = fadeOnly && sm.blend == W3D_BLEND_OPAQUE;   // (an opaque part ignores its texture's alpha)
            if (wantConst != tintAlphaConst) {
                tintAlphaConst = wantConst;
                if (wantConst) {
                    C3D_TexEnvSrc(tintEnv, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
                    C3D_TexEnvFunc(tintEnv, C3D_RGB, GPU_MODULATE);
                    C3D_TexEnvSrc(tintEnv, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
                    C3D_TexEnvFunc(tintEnv, C3D_Alpha, GPU_REPLACE);
                } else {
                    C3D_TexEnvSrc(tintEnv, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
                    C3D_TexEnvFunc(tintEnv, C3D_Both, GPU_MODULATE);
                }
                // (citro3d sends a stage to the GPU only when it is marked dirty - C3D_GetTexEnv / C3D_DirtyTexEnv; changes through a
                // kept pointer were silently lost: 6.11/6.12 fades drew fully opaque - perf log 6m: r_fading ~28, nothing seen)
                C3D_DirtyTexEnv(tintEnv);
            }
            bc.bind(0, sm.tex >= m->tex.size() ? streamerWhiteTexture() : m->tex[sm.tex], fc);   // 6.99: (also a model let go this frame: its list is empty)
            const float unlit = (sm.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 0.55f, 0.6f, 0, unlit);   // (6.10: once a part)
            if (const W3dUvAnim* ua = m->uva ? w3d_uv_of(m->blob, m->uva, s) : nullptr) {   // 6.41: a moving texture (lava, falls, portals, runes)
                float r0[4], r1[4];
                w3d_uv_rows(m->blob, ua, g_nowMs, r0, r1);
                mvsUvA(r0[0], r0[1], r0[2], 0);
                mvsUvB(r1[0], r1[1], r1[2], 0);
                uvMoved = true;
            } else if (uvMoved) {
                setUvIdentity();
                uvMoved = false;
            }
            const float* lastLight = nullptr;
            const C3D_Mtx* mvs = g_instMv.data() + m->mvBase;
            for (u32 vi = 0; vi < m->visible.size(); ++vi) {
                const Instance* in = m->visible[vi];
                if (in->fade < 1.0f ? !blendedPass : fadeOnly) continue;   // (fading ones are drawn in the transparent pass only)
                C3D_Mtx mv = mvs[vi];
                if (m->fxAnim.a && s < m->fxAnim.a->subCount) {   // stage 5.35: the part rides its bone (model space)
                    float a[12];
                    w3d_fx_anim_matrix(&m->fxAnim, m->fxAnim.subBone[s], in->animMs, a);
                    C3D_Mtx bm, out;
                    Mtx_Identity(&bm);
                    for (int r = 0; r < 3; ++r) { bm.r[r].x = a[r * 4]; bm.r[r].y = a[r * 4 + 1]; bm.r[r].z = a[r * 4 + 2]; bm.r[r].w = a[r * 4 + 3]; }
                    Mtx_Multiply(&out, &mv, &bm);
                    mv = out;
                }
                if (m->fxAnim.tint && s < m->fxAnim.a->subCount) {   // 5.35j: the part's colour and opacity now
                    float c[4];
                    w3d_fx_anim_tint(&m->fxAnim, (int)s, in->animMs, c);
                    if (sm.blend == W3D_BLEND_ADD) for (int k = 0; k < 3; ++k) c[k] *= c[3];   // additive: fades by darkening
                    if (sm.blend == W3D_BLEND_ADD && c[0] + c[1] + c[2] < 0.01f) continue;   // invisible now
                    if (sm.blend == W3D_BLEND_ALPHA && c[3] < 0.01f) continue;
                    const u32 rgba = (u32)(c[0] * 255.0f + 0.5f) | (u32)(c[1] * 255.0f + 0.5f) << 8 | (u32)(c[2] * 255.0f + 0.5f) << 16 |
                                     (u32)(c[3] * 255.0f + 0.5f) << 24;
                    if (rgba != tintNow) { C3D_TexEnvColor(tintEnv, rgba); C3D_DirtyTexEnv(tintEnv); tintNow = rgba; }
                } else {
                    // stage 6.11: a fading instance - opacity in the constant's alpha (the blend multiplies the colour by it)
                    const u32 want = in->fade < 1.0f ? (0x00FFFFFFu | (u32)(in->fade * 255.0f + 0.5f) << 24) : 0xFFFFFFFFu;
                    if (want != tintNow) { C3D_TexEnvColor(tintEnv, want); C3D_DirtyTexEnv(tintEnv); tintNow = want; }
                }
                if (folded) mvsFolded(foldMvp[vi], foldMv[vi], mvs[vi]);
                else mvsMV(mv);
                if (!lastLight || memcmp(lastLight, in->lightModel, sizeof in->lightModel) != 0) {   // (the same rotation: the same light)
                    mvsLight(in->lightModel[0], in->lightModel[1], in->lightModel[2], 0);
                    lastLight = in->lightModel;
                }
                uint32_t first = sm.firstIndex, count = sm.indexCount;
                if (in->lod && m->lods)   // stage 6.12: the lightest level there is, up to the one asked for
                    for (int k = in->lod; k >= 1; --k)
                        if (m->lods[s].indexCount[k - 1]) { first = m->lods[s].firstIndex[k - 1]; count = m->lods[s].indexCount[k - 1]; break; }
                drawFlush();
                C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, idx + first);
                ++fc.drawCalls;
                ++fc.lodDraws[in->lod < 3 ? in->lod : 2];
                fc.tris += count / 3;
                if (vcopy) fc.vramTris += count / 3;   // 6.52
                if (!m->building) {   // 6.51: the doodads by blend and size
                    const int bi = sm.blend == W3D_BLEND_OPAQUE ? 0 : sm.blend == W3D_BLEND_ALPHAKEY ? 1 : 2;
                    const int ci = in->radius < 1.5f ? 0 : in->radius < 4.0f ? 1 : in->radius < 12.0f ? 2 : 3;
                    fc.dTris[bi][ci] += count / 3;
                    ++fc.dDraws[bi][ci];
                }
            }
        }
    }
    // 6.44: the merged small doodads (opaque / cut-out parts only), sorted by texture
    if (!blendedPass && !g_activeBatches.empty()) {
        if (!g_batchesSorted) {
            std::sort(g_activeBatches.begin(), g_activeBatches.end(), [](const ActiveBatch& a, const ActiveBatch& b) {
                const Tile::StaticBatch& x = a.t->sbatches[a.batch];
                const Tile::StaticBatch& y = b.t->sbatches[b.batch];
                return x.tex != y.tex ? x.tex < y.tex : x.flags < y.flags;
            });
            g_batchesSorted = true;
        }
        if (tintNow != 0xFFFFFFFFu || tintAlphaConst) {
            C3D_TexEnvSrc(tintEnv, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(tintEnv, C3D_Both, GPU_MODULATE);
            C3D_TexEnvColor(tintEnv, 0xFFFFFFFFu);
            C3D_DirtyTexEnv(tintEnv);
            tintNow = 0xFFFFFFFFu;
            tintAlphaConst = false;
        }
        if (uvMoved) { setUvIdentity(); uvMoved = false; }
        mvsLight(0.30f, 0.40f, 0.866f, 0);   // (world space: the batches are not turned)
        const Tile::StaticGroup* lastG = nullptr;
        float lastUnlit = -1;
        for (const ActiveBatch& ab : g_activeBatches) {
            if (ab.batch >= ab.t->sbatches.size() || ab.group >= ab.t->sgroups.size()) continue;   // (never: a guard)
            const Tile::StaticBatch& b = ab.t->sbatches[ab.batch];
            const Tile::StaticGroup* g = &ab.t->sgroups[ab.group];
            const int lod = ab.lod < 3 ? ab.lod : 2;
            if (!b.count[lod] || !b.v || !b.idx) continue;
            if ((g_gtOff & kGtAlphaKey) && b.blend == W3D_BLEND_ALPHAKEY) continue;   // 6.51: the GPU test
            if (g != lastG) {
                C3D_Mtx mv;
                camRelative(&mv, view, g->center, cam.pos);
                mvsMV(mv);
                lastG = g;
            }
            // 6.52: the GPU test's copies in VRAM (both buffers, or neither)
            const uint8_t* cv = streamerGtVramCopy(b.v, b.vBytes);
            const uint8_t* ci = streamerGtVramCopy(b.idx, b.iBytes);
            if (!cv) ci = nullptr;
            const W3dModelVertex* bv = ci ? (const W3dModelVertex*)cv : b.v;
            const uint16_t* bi = ci ? (const uint16_t*)ci : b.idx;
            C3D_BufInfo* buf = C3D_GetBufInfo();
            BufInfo_Init(buf);
            BufInfo_Add(buf, bv, sizeof(W3dModelVertex), 3, 0x210);
            mvsScale(b.scale[0], b.scale[1], b.scale[2], b.uvScale);
            mvsOffset(b.offset[0], b.offset[1], b.offset[2], 0);
            setModelBlend(b.blend, b.flags & W3D_SUBF_TWO_SIDED, rs.modelsNoCull);
            const float unlit = (b.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f;
            if (unlit != lastUnlit) {
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 0.55f, 0.6f, 0, unlit);
                lastUnlit = unlit;
            }
            bc.bind(0, b.tex ? b.tex : streamerWhiteTexture(), fc);
            drawFlush();
            C3D_DrawElements(GPU_TRIANGLES, b.count[lod], C3D_UNSIGNED_SHORT, bi + b.first[lod]);
            if (ci) fc.vramTris += b.count[lod] / 3;   // 6.52
            ++fc.drawCalls;
            ++fc.sbDraws;
            ++fc.lodDraws[lod];
            fc.tris += b.count[lod] / 3;
            {   // 6.51
                const int bi = b.blend == W3D_BLEND_OPAQUE ? 0 : b.blend == W3D_BLEND_ALPHAKEY ? 1 : 2;
                fc.sbTris[bi] += b.count[lod] / 3;
                ++fc.sbDrawsBl[bi];
            }
        }
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

void drawMarkers(const Camera& cam, const C3D_Mtx& view, const Frustum& fr, const RenderSettings& rs,
                 const std::vector<Marker>& markers, FrameCounters& fc) {
    if (markers.empty()) return;
    bindProgram(g_modelSh);
    setUvIdentity();   // 6.41: the uniform registers are shared by the shaders: never left to chance
    C3D_SetAttrInfo(&g_modelAttr);
    mvsProj(g_proj);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_markerV, sizeof(W3dModelVertex), 3, 0x210);
    const float q = 1.0f / kMarkerQ;
    mvsScale(q, q, q, 0);
    mvsOffset(0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 0.5f, 0.6f, 0, 0.35f);
    resetStages(fogStage(1));
    setFogUniform(g_modelSh, rs.fog, true);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    const float sun[3] = {0.42f, 0.30f, 0.86f};   // world-space direction to the sun
    int boundShape = 0;
    bool blending = false;   // the selection disc is alpha-blended, everything else replaces
    for (const Marker& m : markers) {
        const bool sel = (m.shape == 3 || m.shape == 4) && g_selV;
        const bool ring = !sel && (m.shape == 1 || m.shape == 2) && g_ringV;
        float c[3] = {m.pos[0], m.pos[1], m.pos[2] + m.height * 0.5f};
        if (!fr.sphere(c, ring || sel ? m.width * 2.0f : m.height)) continue;
        const int wantShape = sel ? 2 : ring ? 1 : 0;
        if (boundShape != wantShape) {   // the ring and the selection circle have their own vertex buffers (x/y scaled by 2 / kMarkerQ)
            boundShape = wantShape;
            C3D_BufInfo* b2 = C3D_GetBufInfo();
            BufInfo_Init(b2);
            BufInfo_Add(b2, sel ? (void*)g_selV : ring ? (void*)g_ringV : (void*)g_markerV, sizeof(W3dModelVertex), 3, 0x210);
            const float qq = ring || sel ? 2.0f / kMarkerQ : 1.0f / kMarkerQ;
            mvsScale(qq, qq, qq, 0);
        }
        // stage 0: rgb = marker colour * light (vertex red), alpha = 1
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvColor(env, m.color);
        C3D_Mtx mv;
        camRelative(&mv, view, m.pos, cam.pos);
        Mtx_RotateZ(&mv, m.yaw, true);
        Mtx_Scale(&mv, m.width, m.width, m.height);
        mvsMV(mv);
        // light direction in marker space: rotate the sun by -yaw
        float cy = cosf(-m.yaw), sy = sinf(-m.yaw);
        mvsLight(sun[0] * cy - sun[1] * sy, sun[0] * sy + sun[1] * cy, sun[2], 0);
        const bool wantBlend = sel && m.shape == 4;
        if (wantBlend != blending) {
            blending = wantBlend;
            if (blending) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
            else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        }
        int first = 0;
        int nv = !ring ? g_markerVerts : m.shape == 2 ? 28 * 6 : g_ringVerts;   // shape 2: the ring alone
        if (sel) {   // shape 3: the thin ring, shape 4: the disc inside it
            first = m.shape == 3 ? 0 : g_selRingVerts;
            nv = m.shape == 3 ? g_selRingVerts : g_selVerts - g_selRingVerts;
        }
        drawFlush();
        C3D_DrawArrays(GPU_TRIANGLES, first, nv);
        ++fc.drawCalls;
        ++fc.markers;
        fc.tris += nv / 3;
    }
    if (blending) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

// Animated creatures: the pose is evaluated on the CPU (w3d_anim.h, a few hundred
// matrix products per creature) and each bone palette (<= 24 bones, 72 uniforms) is
// uploaded before the submeshes that use it; the vertex shader blends 2 bones.
const float kSun[3] = {0.42f, 0.30f, 0.86f};   // world-space direction to the sun (as markers)

GpuTex* creatureTex(const CreatureDisplay* d, const W3dCreatureHeader* h, const W3dCreatureTex* ct, uint16_t slot) {
    if (slot >= h->texCount) return streamerWhiteTexture();
    uint8_t kind = ct[slot].kind;
    GpuTex* t = nullptr;
    if (kind == W3D_CTEX_FIXED) t = d->model->slotTex[slot];
    else if (kind >= W3D_CTEX_SKIN1 && kind <= W3D_CTEX_CAPE) t = d->tex[kind - 1];
    return t ? t : streamerWhiteTexture();
}

// Worn item models (stage 5.19): static models drawn with the doodad shader after the creatures; each rides a bone
// of its unit: mv * bone matrix * T(attachment point).
struct WornDraw {
    C3D_Mtx mv;
    Model* model;
    float light[3];
};
std::vector<WornDraw> g_worn;

void drawWorn(const RenderSettings& rs, FrameCounters& fc) {
    if (g_worn.empty()) return;
    bindProgram(g_modelSh);
    setUvIdentity();   // 6.41: the uniform registers are shared by the shaders: never left to chance
    C3D_SetAttrInfo(&g_modelAttr);
    mvsProj(g_proj);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    resetStages(fogStage(1));
    setFogUniform(g_modelSh, rs.fog, true);
    BindCache bc;
    for (int pass = 0; pass < 2; ++pass) {   // opaque, then blended
        for (const WornDraw& w : g_worn) {
            Model* m = w.model;
            const W3dModelHeader* h = m->h;
            C3D_BufInfo* buf = C3D_GetBufInfo();
            BufInfo_Init(buf);
            BufInfo_Add(buf, m->blob + h->offVertices, sizeof(W3dModelVertex), 3, 0x210);
            mvsScale(h->posScale[0], h->posScale[1], h->posScale[2], w3d_model_uv_scale(h));
            mvsOffset(h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
            mvsMV(w.mv);
            mvsLight(w.light[0], w.light[1], w.light[2], 0);
            const uint16_t* idx = (const uint16_t*)(m->blob + h->offIndices);
            for (u32 i = 0; i < h->submeshCount; ++i) {
                const W3dSubmesh& sm = m->subs[i];
                const bool blended = sm.blend == W3D_BLEND_ALPHA || sm.blend == W3D_BLEND_ADD;
                if (blended != (pass == 1)) continue;
                setModelBlend(sm.blend, sm.flags & W3D_SUBF_TWO_SIDED, rs.modelsNoCull);
                bc.bind(0, sm.tex >= m->tex.size() ? streamerWhiteTexture() : m->tex[sm.tex], fc);   // 6.99: (also a model let go this frame: its list is empty)
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 0.55f, 0.6f, 0, (sm.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f);
                drawFlush();
                C3D_DrawElements(GPU_TRIANGLES, sm.indexCount, C3D_UNSIGNED_SHORT, idx + sm.firstIndex);
                ++fc.drawCalls;
                fc.tris += sm.indexCount / 3;
            }
        }
    }
    g_worn.clear();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

// Stage 5.33: the points spell effects hang on, per unit drawn: from its attachments (hands 1 / 2, helm 11, shoulders
// 5 / 6 -> chest) through the animated pose; feet = the unit's position.
struct UnitPoints { float p[6][3]; uint8_t has = 0; u32 frame = 0; };
std::unordered_map<uint64_t, UnitPoints> g_unitPts;
u32 g_ptsFrame = 0;

void recordUnitPoints(const CreatureInstance& ci, const W3dCreatureHeader* h, const uint8_t* blob, float s, const W3dMat34* pose) {
    UnitPoints& u = g_unitPts[ci.guid];
    u.frame = g_ptsFrame;
    u.has = 1u << W3D_SLOT_BASE;
    memcpy(u.p[W3D_SLOT_BASE], ci.pos, sizeof ci.pos);
    if (!h->attachCount || !h->offAttach) return;
    const W3dAttach* at = (const W3dAttach*)(blob + h->offAttach);
    const float cy = cosf(ci.yaw), sy = sinf(ci.yaw);
    float sh[2][3];
    int shoulders = 0;
    for (u32 k = 0; k < h->attachCount; ++k) {
        const W3dAttach& a = at[k];
        int slot = a.id == W3D_ATT_RHAND ? W3D_SLOT_RHAND : a.id == W3D_ATT_LHAND ? W3D_SLOT_LHAND : a.id == W3D_ATT_HELM ? W3D_SLOT_HEAD : -1;
        const bool shoulder = a.id == W3D_ATT_RSHOULDER || a.id == W3D_ATT_LSHOULDER;
        if (slot < 0 && !shoulder) continue;
        if (a.bone < 0 || (u32)a.bone >= h->boneCount) continue;
        const float* M = pose[a.bone].m;
        float m[3];
        for (int r = 0; r < 3; ++r) m[r] = (M[r * 4] * a.pos[0] + M[r * 4 + 1] * a.pos[1] + M[r * 4 + 2] * a.pos[2] + M[r * 4 + 3]) * s;
        const float w[3] = {ci.pos[0] + cy * m[0] - sy * m[1], ci.pos[1] + sy * m[0] + cy * m[1], ci.pos[2] + m[2]};
        if (shoulder) { if (shoulders < 2) memcpy(sh[shoulders++], w, sizeof w); continue; }
        memcpy(u.p[slot], w, sizeof w);
        u.has |= (uint8_t)(1u << slot);
    }
    if (shoulders == 2) {   // the chest: between the shoulders, a little lower
        for (int k = 0; k < 3; ++k) u.p[W3D_SLOT_CHEST][k] = 0.5f * (sh[0][k] + sh[1][k]);
        u.p[W3D_SLOT_CHEST][2] -= 0.25f * (u.p[W3D_SLOT_CHEST][2] - ci.pos[2]);
        u.has |= 1u << W3D_SLOT_CHEST;
    }
}

// Stage 6.4: the units' poses computed by rendererPrepare (before the wait for the GPU), per index of the list it was given;
// -1 = outside the frustum / not ready. drawCreatures uses them for that same list (both eyes), else poses as before.
std::vector<W3dMat34> g_posePool;
std::vector<int> g_poseOff;
std::vector<uint8_t> g_unitLod;   // stage 6.12: the mesh level of each unit of g_posedList
const std::vector<CreatureInstance>* g_posedList = nullptr;

static bool creatureVisible(const CreatureInstance& ci, const Frustum& fr, const W3dCreatureHeader** hOut, float* sOut,
                            const uint8_t* readySnap = nullptr, size_t idx = 0) {
    const CreatureDisplay* d = ci.disp;
    if (!d || !(readySnap ? readySnap[idx] != 0 : d->ready())) return false;   // (7.05: the pose thread reads the main thread's snapshot)
    const W3dCreatureHeader* h = d->model->h;
    const float s = w3d_unit_draw_scale(ci.scale, &d->d);
    float c[3] = {ci.pos[0], ci.pos[1], ci.pos[2] + 0.5f * (h->bmin[2] + h->bmax[2]) * s};
    if (!fr.sphere(c, h->radius * s)) return false;
    *hOut = h;
    *sOut = s;
    return true;
}

// Stage 6.12: the far units (kCreatureFullAnimRange) share poses: one per model, sequence and loop flag a frame, at a common
// clock that steps 10 times a second - a herd far away moves in step, which nobody sees at that size.
static uint32_t g_nowMs6 = 0;   // (set by rendererPrepare)
// 7.05: readySnap (the pose thread): which units were ready when the job was given (the main thread may make others ready
// meanwhile); recordPoints false: the unit points (a map the main thread reads) are recorded after the join, on the main thread.
void prepareCreatures(const Frustum& fr, const std::vector<CreatureInstance>& list, FrameCounters& fc, const uint8_t* readySnap = nullptr,
                      bool recordPoints = true, uint32_t clockMs = ~0u) {
    const u64 t0 = svcGetSystemTick();
    g_posedList = &list;
    g_poseOff.assign(list.size(), -1);
    g_unitLod.assign(list.size(), 0);
    struct Shared { const uint8_t* blob; int seq; bool loop; int off; };
    static std::vector<Shared> shared;
    shared.clear();
    std::vector<uint8_t> isFar(list.size(), 0);
    size_t total = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const W3dCreatureHeader* h;
        float s;
        const CreatureInstance& ci = list[i];
        if (!creatureVisible(ci, fr, &h, &s, readySnap, i)) continue;
        const float dx = ci.pos[0] - fr.cam[0], dy = ci.pos[1] - fr.cam[1], dz = ci.pos[2] - fr.cam[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        g_unitLod[i] = ci.disp->model->lods ? w3d_lod_for(d2, h->radius * s) : 0;
        if (d2 > kCreatureFullAnimRange * kCreatureFullAnimRange && ci.seq >= 0) {
            isFar[i] = 1;
            ++fc.farUnits;
            const uint8_t* blob = ci.disp->model->blob;
            int found = -1;
            for (const Shared& x : shared)
                if (x.blob == blob && x.seq == ci.seq && x.loop == ci.loop) { found = x.off; break; }
            if (found >= 0) { g_poseOff[i] = found; ++fc.sharedPoses; continue; }
            shared.push_back({blob, ci.seq, ci.loop, (int)total});
        }
        g_poseOff[i] = (int)total;
        total += h->boneCount;
    }
    if (g_posePool.size() < total) g_posePool.resize(total);
    const float sharedMs = (float)((clockMs != ~0u ? clockMs : g_nowMs6) / 100u * 100u);
    for (const Shared& x : shared) w3d_anim_pose(x.blob, x.seq, sharedMs, x.loop ? 1 : 0, g_posePool.data() + x.off);
    // 6.99: the units between the near ring and the far ones are posed less often - every 2nd frame from 22 yd, every 3rd from
    // 45 yd (the frames staggered by the unit, so the work spreads); between those frames the last pose is drawn again. A camp
    // of 25 Turtle WoW refugees (88-162 bones each) cost ~3 ms of posing a frame. A new sequence or model is posed at once.
    struct PoseMemo { const uint8_t* blob = nullptr; int seq = -2; bool loop = false; uint32_t frame = 0, used = 0; std::vector<W3dMat34> m; };
    static std::unordered_map<uint64_t, PoseMemo> memo;
    static uint32_t poseFrame = 0;
    ++poseFrame;
    for (size_t i = 0; i < list.size(); ++i) {
        if (g_poseOff[i] < 0) continue;
        const CreatureInstance& ci = list[i];
        const W3dCreatureHeader* h = ci.disp->model->h;
        const float s = w3d_unit_draw_scale(ci.scale, &ci.disp->d);
        W3dMat34* P = g_posePool.data() + g_poseOff[i];
        if (!isFar[i]) {
            const float dx = ci.pos[0] - fr.cam[0], dy = ci.pos[1] - fr.cam[1], dz = ci.pos[2] - fr.cam[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            const uint32_t every = !ci.guid || d2 < 22.0f * 22.0f ? 1u : d2 < 45.0f * 45.0f ? 2u : 3u;
            const uint8_t* blob = ci.disp->model->blob;
            if (every == 1) {
                w3d_anim_pose(blob, ci.seq, ci.timeMs, ci.loop ? 1 : 0, P);
            } else {
                PoseMemo& pm = memo[ci.guid];
                const bool same = pm.blob == blob && pm.seq == ci.seq && pm.loop == ci.loop && pm.m.size() == h->boneCount;
                const bool due = !same || poseFrame - pm.frame >= every || ((poseFrame + (uint32_t)(ci.guid * 2654435761u)) % every) == 0;
                if (due) {
                    w3d_anim_pose(blob, ci.seq, ci.timeMs, ci.loop ? 1 : 0, P);
                    pm.blob = blob; pm.seq = ci.seq; pm.loop = ci.loop; pm.frame = poseFrame;
                    pm.m.assign(P, P + h->boneCount);
                    ++fc.posesFresh;
                } else {
                    memcpy(P, pm.m.data(), h->boneCount * sizeof(W3dMat34));
                    ++fc.posesReused;
                }
                pm.used = poseFrame;
            }
        }
        if (ci.guid && recordPoints) recordUnitPoints(ci, h, ci.disp->model->blob, s, P);
    }
    if ((poseFrame & 255) == 0)   // units gone for a while: their memo goes
        for (auto it = memo.begin(); it != memo.end();) it = poseFrame - it->second.used > 120 ? memo.erase(it) : std::next(it);
    fc.poseMs += ticksToMs(svcGetSystemTick() - t0);
}

// 7.05: the units' poses on core 2 (New 3DS). The main thread hands the job over as soon as the frame's unit list and camera
// are final (main.cpp, after the game phase) and goes on with the HUD, the touch screen, the streamer and the visibility pass;
// rendererPrepare joins it. The job writes only the pose globals (g_posePool / g_poseOff / g_unitLod / g_posedList) and its
// own counters, which nobody reads before the join; the unit points (a map the spells read) are recorded after it, here.
// Without core 2 (Old 3DS) or when the thread cannot start, rendererPrepare poses on the main thread as before.
namespace {
struct PoseJob {
    const std::vector<CreatureInstance>* list = nullptr;
    Frustum fr;
    std::vector<uint8_t> ready;
    FrameCounters fc;
    uint32_t nowMs = 0;
    bool pending = false;
};
PoseJob g_job;
Thread g_poseThread = nullptr;
LightEvent g_poseGo, g_poseDone;
volatile bool g_poseQuit = false;
bool g_poseTried = false;

void poseMain(void*) {
    for (;;) {
        LightEvent_Wait(&g_poseGo);
        if (g_poseQuit) break;
        g_job.fc = FrameCounters();
        prepareCreatures(g_job.fr, *g_job.list, g_job.fc, g_job.ready.data(), false, g_job.nowMs);
        LightEvent_Signal(&g_poseDone);
    }
}

void poseWait() {
    if (!g_job.pending) return;
    LightEvent_Wait(&g_poseDone);
    g_job.pending = false;
}
}  // namespace

bool posesStartImpl(const Frustum& fr, const RenderSettings& rs, const std::vector<CreatureInstance>* list) {
    poseWait();   // (a job nobody joined: the login screen came in between)
    if (!g_poseTried) {
        g_poseTried = true;
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        LightEvent_Init(&g_poseGo, RESET_ONESHOT);
        LightEvent_Init(&g_poseDone, RESET_ONESHOT);
        if (n3ds) g_poseThread = threadCreate(poseMain, nullptr, 32 * 1024, prio, 2, false);   // (the same priority: core 2 is its own)
        perf::event("unit poses: %s", g_poseThread ? "on core 2 (7.05)" : "on the main thread (no core 2)");
    }
    if (!g_poseThread || !list || list->empty() || !rs.creatures) return false;
    g_job.list = list;
    g_job.fr = fr;
    g_job.ready.resize(list->size());
    for (size_t i = 0; i < list->size(); ++i) g_job.ready[i] = (*list)[i].disp && (*list)[i].disp->ready() ? 1 : 0;
    g_job.nowMs = rs.nowMs;
    g_job.pending = true;
    LightEvent_Signal(&g_poseGo);
    return true;
}

// The join (rendererPrepare): true when the job posed this very list; its counters go to fc, the unit points are recorded.
static bool posesJoin(const std::vector<CreatureInstance>* list, FrameCounters& fc) {
    if (!g_job.pending) return false;
    const u64 w0 = svcGetSystemTick();
    poseWait();
    fc.poseWaitMs += ticksToMs(svcGetSystemTick() - w0);
    if (g_job.list != list || g_poseOff.size() != list->size()) return false;
    fc.farUnits += g_job.fc.farUnits;
    fc.sharedPoses += g_job.fc.sharedPoses;
    fc.posesFresh += g_job.fc.posesFresh;
    fc.posesReused += g_job.fc.posesReused;
    fc.poseMs += g_job.fc.poseMs;
    for (size_t i = 0; i < list->size(); ++i) {
        const CreatureInstance& ci = (*list)[i];
        if (g_poseOff[i] < 0 || !ci.guid) continue;
        const W3dCreatureHeader* h = ci.disp->model->h;
        recordUnitPoints(ci, h, ci.disp->model->blob, w3d_unit_draw_scale(ci.scale, &ci.disp->d), g_posePool.data() + g_poseOff[i]);
    }
    ++fc.posesThreaded;
    return true;
}

void posesShutdownImpl() {
    if (!g_poseThread) return;
    poseWait();
    g_poseQuit = true;
    LightEvent_Signal(&g_poseGo);
    threadJoin(g_poseThread, U64_MAX);
    threadFree(g_poseThread);
    g_poseThread = nullptr;
}

void drawCreatures(const Camera& cam, const C3D_Mtx& view, const Frustum& fr, const RenderSettings& rs,
                   const std::vector<CreatureInstance>& list, FrameCounters& fc) {
    if (list.empty()) return;
    const bool posed = g_posedList == &list && g_poseOff.size() == list.size();
    g_worn.clear();
    bindProgram(g_skinSh);
    C3D_SetAttrInfo(&g_skinAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_skinSh.uProj, &g_proj);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    // stage 6.12: stage 1 = the unit's opacity while it appears (constant alpha; white = nothing changes), fog after it
    const int tintStage = ghostStages(1);   // 6.77: as a ghost the grey comes before the unit's tint (ghosts, spirit healers stay blue)
    C3D_TexEnv* tintEnv = C3D_GetTexEnv(tintStage);
    C3D_TexEnvInit(tintEnv);
    C3D_TexEnvSrc(tintEnv, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(tintEnv, C3D_Both, GPU_MODULATE);
    C3D_TexEnvColor(tintEnv, 0xFFFFFFFFu);
    u32 tintNow = 0xFFFFFFFFu;
    bool tintAlphaConst = false;
    resetStages(fogStage(tintStage + 1, false));
    setFogUniform(g_skinSh, rs.fog, true);
    BindCache bc;
    for (size_t ci_i = 0; ci_i < list.size(); ++ci_i) {
        const CreatureInstance& ci = list[ci_i];
        const CreatureDisplay* d = ci.disp;
        const W3dCreatureHeader* h;
        float s;
        const W3dMat34* P;
        if (posed) {   // stage 6.4: culled and posed before the wait
            if (g_poseOff[ci_i] < 0) continue;
            h = d->model->h;
            s = w3d_unit_draw_scale(ci.scale, &d->d);
            P = g_posePool.data() + g_poseOff[ci_i];
        } else {
            if (!creatureVisible(ci, fr, &h, &s)) continue;
            u64 t0 = svcGetSystemTick();
            if (g_pose.size() < h->boneCount) g_pose.resize(h->boneCount);
            w3d_anim_pose(d->model->blob, ci.seq, ci.timeMs, ci.loop ? 1 : 0, g_pose.data());
            if (ci.guid) recordUnitPoints(ci, h, d->model->blob, s, g_pose.data());
            fc.poseMs += ticksToMs(svcGetSystemTick() - t0);
            P = g_pose.data();
        }
        if (d->model->meshHidden) continue;   // 6.17: an emitter volume - its particles only (fxSimulate takes them from the list)
        const uint8_t* blob = d->model->blob;
        ++fc.creatures;
        if (ci.fade < 1.0f) ++fc.fading;   // (6.12: counted with the objects for the HUD / log)

        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, blob + h->offVertices, sizeof(W3dSkinVertex), 4, 0x3210);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uScale, h->posScale[0], h->posScale[1], h->posScale[2], W3D_MODEL_UV_SCALE);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uOffset, h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
        C3D_Mtx mv;
        camRelative(&mv, view, ci.pos, cam.pos);
        float cy = cosf(-ci.yaw), sy = sinf(-ci.yaw);
        float lightObj[3] = {kSun[0] * cy - kSun[1] * sy, kSun[0] * sy + kSun[1] * cy, kSun[2]};   // the sun in the object's frame
        if (ci.tilted) {   // stage 5.41: an object standing at an angle (GAMEOBJECT_ROTATION), the full rotation instead of the yaw
            const float* R = ci.rot;
            C3D_Mtx rm, tmp;
            Mtx_Identity(&rm);
            for (int r = 0; r < 3; ++r) { rm.r[r].x = R[r * 3]; rm.r[r].y = R[r * 3 + 1]; rm.r[r].z = R[r * 3 + 2]; }
            Mtx_Multiply(&tmp, &mv, &rm);
            mv = tmp;
            lightObj[0] = R[0] * kSun[0] + R[3] * kSun[1] + R[6] * kSun[2];   // R^T * sun
            lightObj[1] = R[1] * kSun[0] + R[4] * kSun[1] + R[7] * kSun[2];
            lightObj[2] = R[2] * kSun[0] + R[5] * kSun[1] + R[8] * kSun[2];
        } else {
            Mtx_RotateZ(&mv, ci.yaw, true);
        }
        Mtx_Scale(&mv, s, s, s);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_skinSh.uMV, &mv);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uLight, lightObj[0], lightObj[1], lightObj[2], 0);
        for (int w = 0; w < ci.wornCount && ci.fade >= 0.6f; ++w) {   // item models: queued with their own matrix, drawn after the creatures
            // (6.12: a unit that is still appearing gets its helmet and weapons when it is more than half there)
            const CreatureInstance::Worn& wi = ci.worn[w];
            if (!wi.model || !wi.model->h || wi.model->state != AssetState::Ready || !wi.model->texturesReady()) continue;
            if (wi.bone < 0 || (u32)wi.bone >= h->boneCount) continue;
            const float* M = P[wi.bone].m;   // 3x4 row-major, bind pose -> animated pose
            C3D_Mtx bone;
            for (int r = 0; r < 3; ++r) { bone.r[r].x = M[r * 4]; bone.r[r].y = M[r * 4 + 1]; bone.r[r].z = M[r * 4 + 2]; bone.r[r].w = M[r * 4 + 3]; }
            bone.r[3].x = 0; bone.r[3].y = 0; bone.r[3].z = 0; bone.r[3].w = 1;
            WornDraw wd;
            Mtx_Multiply(&wd.mv, &mv, &bone);
            Mtx_Translate(&wd.mv, wi.pos[0], wi.pos[1], wi.pos[2], true);
            // 6.92: a two-hander, bow or gun on the back (attachment 12) takes WoWee's turn (a shield there does not: backFix is off)
            if (wi.backFix) {
                float f[12];
                w3d_back_weapon_xform(f);
                C3D_Mtx fx;
                for (int r = 0; r < 3; ++r) { fx.r[r].x = f[r * 4]; fx.r[r].y = f[r * 4 + 1]; fx.r[r].z = f[r * 4 + 2]; fx.r[r].w = f[r * 4 + 3]; }
                fx.r[3].x = 0; fx.r[3].y = 0; fx.r[3].z = 0; fx.r[3].w = 1;
                C3D_Mtx t;
                Mtx_Multiply(&t, &wd.mv, &fx);
                wd.mv = t;
            }
            wd.model = wi.model;
            wd.light[0] = lightObj[0]; wd.light[1] = lightObj[1]; wd.light[2] = lightObj[2];
            g_worn.push_back(wd);
        }

        const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(blob + h->offSubmeshes);
        const W3dBonePalette* pal = (const W3dBonePalette*)(blob + h->offPalettes);
        const W3dCreatureTex* ct = (const W3dCreatureTex*)(blob + h->offTex);
        const uint16_t* idx = (const uint16_t*)(blob + h->offIndices);
        int curPal = -1;
        // 7.0: at LOD 2 a blob with a far skeleton draws every piece with that one palette (its own vertex copies there)
        const int unitLod = posed ? g_unitLod[ci_i] : 0;
        const W3dSubLod* subLods = d->model->lods;
        const bool farSkel = unitLod >= W3D_MODEL_LODS && subLods && h->farPalette && h->farPalette <= h->paletteCount;
        for (u32 i = 0; i < h->submeshCount; ++i) {
            const W3dSkinSubmesh& m = sm[i];
            if (d->d.geosetCount) {
                bool on = false;
                for (int k = 0; k < d->d.geosetCount && !on; ++k) on = d->d.geosets[k] == m.geoset;
                if (!on) continue;
            }
            // 7.03: a player's piece with no texture to wear (the model's own art is not a type the converter knows) or a cloak
            // with no cloak worn was drawn in the white stand-in texture - Turtle WoW's Goblin showed a pale ragged flap sticking
            // out of its side. Such pieces are skipped (only for players: a monster's untextured part stays as it was).
            if (d->look) {
                if (m.texSlot == W3D_NO_TEXTURE) continue;
                if (m.texSlot < h->texCount && ct[m.texSlot].kind == W3D_CTEX_CAPE && !d->tex[W3D_CTEX_CAPE - 1]) continue;
            }
            const bool farHere = farSkel && subLods[i].indexCount[W3D_MODEL_LODS - 1];
            const int wantPal = farHere ? h->farPalette - 1 : m.palette;
            if (wantPal != curPal && wantPal < (int)h->paletteCount) {
                const W3dBonePalette& p = pal[wantPal];
                // 6.35: never more than the shader's bones[] (W3D_PALETTE_BONES * 3 registers) - past it citro3d's
                // uniform array ends and libctru's allocators and the GPU command buffer pointer follow in memory
                const int nb = p.count < W3D_PALETTE_BONES ? p.count : W3D_PALETTE_BONES;
                C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_skinSh.uBones, nb * 3);
                for (int b = 0; b < nb; ++b) {
                    const float* M = P[p.bones[b] < h->boneCount ? p.bones[b] : 0].m;
                    for (int r = 0; r < 3; ++r) {
                        C3D_FVec& v = u[b * 3 + r];
                        v.x = M[r * 4]; v.y = M[r * 4 + 1]; v.z = M[r * 4 + 2]; v.w = M[r * 4 + 3];
                    }
                }
                curPal = wantPal;
                ++fc.boneUploads;
            }
            setModelBlend(m.blend, m.flags & W3D_SUBF_TWO_SIDED, rs.modelsNoCull);
            // stage 6.12: a unit that is appearing / at the far edge is alpha-blended (depth still written)
            const bool fading = ci.fade < 1.0f;
            if (fading && m.blend != W3D_BLEND_ALPHA && m.blend != W3D_BLEND_ADD) {
                C3D_AlphaTest(m.blend == W3D_BLEND_ALPHAKEY, GPU_GREATER, 0x08);
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
            }
            const bool wantConst = fading && m.blend == W3D_BLEND_OPAQUE;   // (an opaque part ignores its texture's alpha)
            if (wantConst != tintAlphaConst) {
                tintAlphaConst = wantConst;
                if (wantConst) {
                    C3D_TexEnvSrc(tintEnv, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
                    C3D_TexEnvFunc(tintEnv, C3D_RGB, GPU_MODULATE);
                    C3D_TexEnvSrc(tintEnv, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
                    C3D_TexEnvFunc(tintEnv, C3D_Alpha, GPU_REPLACE);
                } else {
                    C3D_TexEnvSrc(tintEnv, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
                    C3D_TexEnvFunc(tintEnv, C3D_Both, GPU_MODULATE);
                }
                // (citro3d sends a stage to the GPU only when it is marked dirty - C3D_GetTexEnv / C3D_DirtyTexEnv; changes through a
                // kept pointer were silently lost: 6.11/6.12 fades drew fully opaque - perf log 6m: r_fading ~28, nothing seen)
                C3D_DirtyTexEnv(tintEnv);
            }
            const u32 want = fading ? ((ci.tint & 0x00FFFFFFu) | (u32)(std::max(0.0f, ci.fade) * 255.0f + 0.5f) << 24) : 0xFFFFFFFFu;   // (6.62: ci.tint colours it: a ghost's blue)
            if (want != tintNow) { C3D_TexEnvColor(tintEnv, want); C3D_DirtyTexEnv(tintEnv); tintNow = want; }
            bc.bind(0, m.texSlot == W3D_NO_TEXTURE ? streamerWhiteTexture() : creatureTex(d, h, ct, m.texSlot), fc);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uLightParams, 0.55f, 0.6f, 0, (m.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f);
            uint32_t first = m.firstIndex, count = m.indexCount;
            const W3dSubLod* lods = subLods;
            const int lod = unitLod;
            if (farHere) {   // 7.0: the far skeleton's own copy of the piece (never a level with the piece's palette)
                first = lods[i].firstIndex[W3D_MODEL_LODS - 1];
                count = lods[i].indexCount[W3D_MODEL_LODS - 1];
            } else if (lod && lods) {   // stage 6.12: the lightest level there is, up to the one asked for
                // 7.0: a blob with a far skeleton has LOD-2 copies bound to the far palette - never with this piece's own
                for (int k = std::min(lod, h->farPalette ? W3D_MODEL_LODS - 1 : W3D_MODEL_LODS); k >= 1; --k)
                    if (lods[i].indexCount[k - 1]) { first = lods[i].firstIndex[k - 1]; count = lods[i].indexCount[k - 1]; break; }
            }
            drawFlush();
            C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, idx + first);
            ++fc.drawCalls;
            ++fc.lodDraws[lod];
            fc.tris += count / 3;
        }
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    drawWorn(rs, fc);
}

void drawWater(const Camera& cam, const C3D_Mtx& view, const Frustum& fr, const RenderSettings& rs, FrameCounters& fc) {
    bindProgram(g_terrainSh);
    C3D_SetAttrInfo(&g_terrainAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uProj, &g_proj);
    const float t = rs.timeSec;
    // stage 0: rgb = texture * vertex colour (the Light.dbc colour of the liquid, shallow -> deep), alpha = vertex
    // alpha (opacity by depth); then fog. The game's animated textures move by themselves; the old still procedural
    // texture (packs before stage 5.26) scrolls instead.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    resetStages(fogStage(1));
    setFogUniform(g_terrainSh, rs.fog, true);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);   // visible from below when swimming
    const u32 timeMs = (u32)(t * 1000.0f);

    BindCache bc;
    for (Tile* tile : streamerReadyTiles()) {
        const W3dWaterHeader* h = tile->wh;
        if (!h || !tile->waterReady) continue;
        if (tileBeyond(tile->tx, tile->ty, cam.pos, fr.maxDist)) continue;   // 6.34c: a tile drawn as far ground has no water drawn
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, tile->water + h->offVertices, sizeof(W3dTerrainVertex), 3, 0x210);
        C3D_Mtx mv;
        camRelative(&mv, view, h->origin, cam.pos);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &mv);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, h->posScale[0], h->posScale[1], h->posScale[2], W3D_TERRAIN_UV_SCALE);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
        const uint16_t* idx = (const uint16_t*)(tile->water + h->offIndices);
        int scrolling = -1;
        for (u32 i = 0; i < h->batchCount; ++i) {
            const W3dWaterBatch& b = tile->wbatches[i];
            float c[3], r2 = 0;
            for (int k = 0; k < 3; ++k) {
                c[k] = h->origin[k] + 0.5f * (b.bmin[k] + b.bmax[k]);
                float e = 0.5f * (b.bmax[k] - b.bmin[k]);
                r2 += e * e;
            }
            if (!fr.sphereFar(c, sqrtf(r2))) continue;   // (6.34: the loaded tiles' water to the horizon, as their ground)
            u32 tex = b.tex;
            const bool animated = b.frames > 1;
            if (animated) {   // fewer frames than the original 30 (--water-frames) are shown longer each
                const u32 frameMs = W3D_WATER_FRAME_MS * 30u / b.frames;
                tex += (timeMs / frameMs) % b.frames;
            }
            if (tex >= tile->wtex.size()) tex = b.tex;
            if (scrolling != (animated ? 0 : 1)) {
                scrolling = animated ? 0 : 1;
                if (animated) C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uUV, 0, 0, 0, 0);
                else C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uUV, fmodf(t * 0.05f, 64.0f), fmodf(t * 0.03f, 64.0f), 0, 0);
            }
            bc.bind(0, tile->wtex[tex], fc);
            drawFlush();
            C3D_DrawElements(GPU_TRIANGLES, b.indexCount, C3D_UNSIGNED_SHORT, idx + b.firstIndex);
            ++fc.drawCalls;
            ++fc.waterBatches;
            fc.tris += b.indexCount / 3;
        }
    }
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}


// ------------------------------------------------------------------ effects (stage 5.32)
// Particles and lamp glows: fx.cpp simulates and hands back camera-facing quads in view space; here they go into a
// vertex buffer (one per draw: a frame can be drawn twice, for the two eyes, so a ring of 4) and are drawn alpha-blended
// ones back to front, then additive ones by texture, depth-tested, no depth writes.
struct FxVertex { float x, y, z, u, v; uint8_t r, g, b, a; };   // 24 bytes
constexpr int kFxMaxQuads = 720, kFxBuffers = 4;
FxVertex* g_fxVerts[kFxBuffers] = {};
uint16_t* g_fxIdx = nullptr;
int g_fxRing = 0;
C3D_Tex g_fxDot;                 // the built-in soft dot (glows, particles without a texture of their own)
bool g_fxDotOk = false;
std::vector<fx::Source> g_fxSources;
std::vector<fx::Quad> g_fxQuads;
float g_fxLastTime = -1.0f;

uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

void buildFx() {
    for (int i = 0; i < kFxBuffers; ++i) g_fxVerts[i] = (FxVertex*)linearAlloc(sizeof(FxVertex) * 4 * kFxMaxQuads);
    g_fxIdx = (uint16_t*)linearAlloc(sizeof(uint16_t) * 6 * kFxMaxQuads);
    if (g_fxIdx) {
        for (int q = 0; q < kFxMaxQuads; ++q) {
            uint16_t* d = g_fxIdx + q * 6;
            const uint16_t b = (uint16_t)(q * 4);
            d[0] = b; d[1] = (uint16_t)(b + 1); d[2] = (uint16_t)(b + 2); d[3] = b; d[4] = (uint16_t)(b + 2); d[5] = (uint16_t)(b + 3);
        }
        GSPGPU_FlushDataCache(g_fxIdx, sizeof(uint16_t) * 6 * kFxMaxQuads);
    }
    // 32x32 white with a soft round alpha (a bright core, a long falloff), PICA tiled (8x8 tiles, Morton inside)
    const int N = 32;
    if (C3D_TexInit(&g_fxDot, N, N, GPU_RGBA8)) {
        uint32_t* px = (uint32_t*)g_fxDot.data;
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const float dx = (x + 0.5f) / N * 2.0f - 1.0f, dy = (y + 0.5f) / N * 2.0f - 1.0f;
                const float r = sqrtf(dx * dx + dy * dy);
                float a = r >= 1.0f ? 0.0f : (1.0f - r) * (1.0f - r);
                a = a * 0.75f + (r < 0.25f ? (0.25f - r) * 1.0f : 0.0f);
                const uint32_t A = (uint32_t)std::min(255.0f, a * 255.0f + 0.5f);
                const uint32_t tile = (uint32_t)((y / 8) * (N / 8) + x / 8);
                px[tile * 64 + morton8((uint32_t)x & 7, (uint32_t)y & 7)] = 0xFFFFFF00u | A;
            }
        C3D_TexFlush(&g_fxDot);
        C3D_TexSetFilter(&g_fxDot, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&g_fxDot, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_fxDotOk = true;
    }
}

// The particles' simulation of a frame (once: the second eye of a 3D frame reuses the quads; stage 6.4: rendererPrepare
// runs it before the wait for the GPU).
static const std::vector<CreatureInstance>* g_fxCreatures = nullptr;   // stage 6.13: this frame's units (rendererPrepare)
static void fxSimulate(const Camera& cam, const C3D_Mtx& view, const RenderSettings& rs, const std::vector<SceneModel>* effects) {
    if (rs.effects <= 0 || !g_fxIdx || !g_fxDotOk || rs.timeSec == g_fxLastTime) return;
    fx::System& sys = fx::global();
    sys.setBudget(fx::budgetFor(rs.effects));
    float rows[3][3];
    for (int i = 0; i < 3; ++i) { rows[i][0] = view.r[i].x; rows[i][1] = view.r[i].y; rows[i][2] = view.r[i].z; }
    {
        const float dt = g_fxLastTime < 0 ? 0.0f : rs.timeSec - g_fxLastTime;
        g_fxLastTime = rs.timeSec;
        const fx::Budget& b = sys.budget();
        const float range = std::max(b.emitRange, b.glowRange);
        g_fxSources.clear();
        for (Tile* t : streamerReadyTiles()) {
            // the tile's square: no instance of it can be in range if the square is not
            const float cx = (32.0f - t->ty - 0.5f) * kTileSize, cy = (32.0f - t->tx - 0.5f) * kTileSize;
            const float ox = std::max(fabsf(cam.pos[0] - cx) - kTileSize * 0.5f, 0.0f);
            const float oy = std::max(fabsf(cam.pos[1] - cy) - kTileSize * 0.5f, 0.0f);
            if (ox * ox + oy * oy > range * range) continue;
            for (const Instance& in : t->instances) {
                const Model* m = in.model;
                if (!m->fx || !m->drawable) continue;
                if (in.owner && in.owner <= t->bldgs.size() && !t->bldgs[in.owner - 1].open) continue;   // 6.33: lamps' flames with their building
                const float dx = in.pos[0] - cam.pos[0], dy = in.pos[1] - cam.pos[1], dz = in.pos[2] - cam.pos[2];
                if (dx * dx + dy * dy + dz * dz > range * range) continue;
                fx::Source s;
                s.key = &in;
                s.owner = m;
                s.blob = m->blob;
                s.h = m->fx;
                s.tex = (const void* const*)(m->tex.data() + m->fxTexBase);
                memcpy(s.pos, in.pos, sizeof s.pos);
                memcpy(s.rot, in.rot, sizeof s.rot);
                s.scale = in.scale;
                g_fxSources.push_back(s);
            }
        }
        // stage 6.13: creatures and game objects with emitters (braziers, forges): their fire from the unit's position and turn
        if (g_fxCreatures)
            for (const CreatureInstance& ci : *g_fxCreatures) {
                const CreatureDisplay* d = ci.disp;
                const CreatureModel* m = d ? d->model : nullptr;
                if (!ci.guid || !m || !m->fx || !m->h || !m->fxReady() || ci.fade < 0.5f) continue;
                const float dx = ci.pos[0] - cam.pos[0], dy = ci.pos[1] - cam.pos[1], dz = ci.pos[2] - cam.pos[2];
                if (dx * dx + dy * dy + dz * dz > range * range) continue;
                fx::Source s;
                s.key = (const void*)(uintptr_t)(0x80000000u | (uint32_t)(ci.guid & 0x7FFFFFFFu));   // (never a heap address)
                s.owner = m;
                s.blob = m->blob;
                s.h = m->fx;
                s.tex = (const void* const*)m->fxTex.data();
                memcpy(s.pos, ci.pos, sizeof s.pos);
                s.rot[0] = s.rot[1] = 0;
                s.rot[2] = ci.yaw;
                s.scale = w3d_unit_draw_scale(ci.scale, &d->d);
                g_fxSources.push_back(s);
            }
        if (effects)   // spell effects (stage 5.33): first served, the pool's hard cap still holds
            for (const SceneModel& sm : *effects) {
                const Model* m = sm.model;
                if (!sm.emit || !m || !m->fx || !m->drawable) continue;
                fx::Source s;
                s.key = (const void*)(uintptr_t)sm.fxKey;   // small numbers: never an Instance's address
                s.owner = m;
                s.blob = m->blob;
                s.h = m->fx;
                s.tex = (const void* const*)(m->tex.data() + m->fxTexBase);
                memcpy(s.pos, sm.pos, sizeof s.pos);
                memcpy(s.rot, sm.rot, sizeof s.rot);
                s.scale = sm.scale;
                s.priority = true;
                s.anim = m->fxAnim;
                s.animMs = sm.fxAgeMs;
                g_fxSources.push_back(s);
            }
        sys.frame(dt, rs.timeSec, cam.pos, rows, g_fxSources, g_fxQuads);
        if ((int)g_fxQuads.size() > kFxMaxQuads) g_fxQuads.resize(kFxMaxQuads);
        // alpha-blended back to front, then the additive ones by texture
        std::stable_sort(g_fxQuads.begin(), g_fxQuads.end(), [](const fx::Quad& a, const fx::Quad& b) {
            if (a.blend != b.blend) return a.blend == W3D_BLEND_ALPHA;
            if (a.blend == W3D_BLEND_ALPHA) return a.c[2] < b.c[2];
            return a.tex < b.tex;
        });
    }
}

void drawFx(const Camera& cam, const C3D_Mtx& view, const RenderSettings& rs, FrameCounters& fc,
            const std::vector<SceneModel>* effects) {
    if (rs.effects <= 0 || !g_fxIdx || !g_fxDotOk) {
        if (fx::global().alive()) fx::global().clear();
        return;
    }
    const u64 t0 = svcGetSystemTick();
    fxSimulate(cam, view, rs, effects);   // (stage 6.4: normally done already by rendererPrepare - then a no-op)
    fx::System& sys = fx::global();
    const fx::Stats& st = sys.stats();
    fc.fxParticles = st.alive;
    fc.fxGlows = st.glows;
    fc.fxSources = st.sources;
    fc.fxQuads = (u32)g_fxQuads.size();
    if (g_fxQuads.empty()) { fc.fxMs += ticksToMs(svcGetSystemTick() - t0); return; }

    FxVertex* vb = g_fxVerts[g_fxRing];
    g_fxRing = (g_fxRing + 1) % kFxBuffers;
    if (!vb) return;
    for (size_t i = 0; i < g_fxQuads.size(); ++i) {
        const fx::Quad& q = g_fxQuads[i];
        const uint8_t r = (uint8_t)(q.color & 255), g = (uint8_t)((q.color >> 8) & 255), b = (uint8_t)((q.color >> 16) & 255),
                      a = (uint8_t)(q.color >> 24);
        FxVertex* v = vb + i * 4;
        v[0] = {q.c[0] - q.half, q.c[1] - q.half, q.c[2], q.u0, q.v0, r, g, b, a};
        v[1] = {q.c[0] + q.half, q.c[1] - q.half, q.c[2], q.u1, q.v0, r, g, b, a};
        v[2] = {q.c[0] + q.half, q.c[1] + q.half, q.c[2], q.u1, q.v1, r, g, b, a};
        v[3] = {q.c[0] - q.half, q.c[1] + q.half, q.c[2], q.u0, q.v1, r, g, b, a};
    }
    GSPGPU_FlushDataCache(vb, sizeof(FxVertex) * 4 * g_fxQuads.size());

    bindProgram(g_fxSh);
    C3D_SetAttrInfo(&g_fxAttr);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, vb, sizeof(FxVertex), 3, 0x210);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_fxSh.uProj, &g_proj);
    if (rs.fog) C3D_FVUnifSet(GPU_VERTEX_SHADER, g_fxSh.uFog, g_fogX, g_fogY, 0, 0);
    else C3D_FVUnifSet(GPU_VERTEX_SHADER, g_fxSh.uFog, 0.0f, 1.0f, 0, 0);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    resetStages(1);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_AlphaTest(true, GPU_GREATER, 2);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    size_t run = 0;
    int boundBlend = -1;
    const void* boundTex = (const void*)1;
    for (size_t i = 0; i <= g_fxQuads.size(); ++i) {
        const bool end = i == g_fxQuads.size();
        if (!end && i > run && g_fxQuads[i].tex == g_fxQuads[run].tex && g_fxQuads[i].blend == g_fxQuads[run].blend) continue;
        if (i > run) {
            const fx::Quad& q = g_fxQuads[run];
            if (q.blend != boundBlend) {
                boundBlend = q.blend;
                if (q.blend == W3D_BLEND_ADD) {
                    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_fxSh.uMask, 1, 1, 1, 0);
                } else {
                    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_fxSh.uMask, 0, 0, 0, 1);
                }
            }
            if (q.tex != boundTex) {
                boundTex = q.tex;
                // 6.16: a particle texture that is not there (out of memory and waiting for a retry, not in the pack, a failed
                // read) is the white stand-in - opaque, no alpha: smoke drew as big grey squares (Orgrimmar). The soft dot then.
                const GpuTex* gt = (const GpuTex*)q.tex;
                const bool standIn = gt && (gt == streamerWhiteTexture() || gt->tex.data == streamerWhiteTexture()->tex.data);
                C3D_TexBind(0, gt && !standIn ? &((GpuTex*)q.tex)->tex : &g_fxDot);
                ++fc.texBinds;
            }
            drawFlush();
            C3D_DrawElements(GPU_TRIANGLES, (int)(i - run) * 6, C3D_UNSIGNED_SHORT, g_fxIdx + run * 6);
            ++fc.drawCalls;
            fc.tris += (u32)(i - run) * 2;
        }
        run = i;
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    fc.fxMs += ticksToMs(svcGetSystemTick() - t0);
}

}  // namespace

// ================================================================== API
void Camera::forward(float out[3]) const {
    out[0] = cosf(pitch) * cosf(yaw);
    out[1] = cosf(pitch) * sinf(yaw);
    out[2] = sinf(pitch);
}

void rendererInit() {
    initShader(g_terrainSh, terrain_shbin, terrain_shbin_size);
    initShader(g_modelSh, model_shbin, model_shbin_size);
    initShader(g_skinSh, skin_shbin, skin_shbin_size);
    initShader(g_fxSh, fx_shbin, fx_shbin_size);
    AttrInfo_Init(&g_terrainAttr);
    AttrInfo_AddLoader(&g_terrainAttr, 0, GPU_SHORT, 4);
    AttrInfo_AddLoader(&g_terrainAttr, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&g_terrainAttr, 2, GPU_SHORT, 2);
    AttrInfo_Init(&g_modelAttr);
    AttrInfo_AddLoader(&g_modelAttr, 0, GPU_SHORT, 4);
    AttrInfo_AddLoader(&g_modelAttr, 1, GPU_BYTE, 4);
    AttrInfo_AddLoader(&g_modelAttr, 2, GPU_SHORT, 2);
    AttrInfo_Init(&g_skinAttr);   // W3dSkinVertex (20 bytes)
    AttrInfo_AddLoader(&g_skinAttr, 0, GPU_SHORT, 4);
    AttrInfo_AddLoader(&g_skinAttr, 1, GPU_BYTE, 4);
    AttrInfo_AddLoader(&g_skinAttr, 2, GPU_SHORT, 2);
    AttrInfo_AddLoader(&g_skinAttr, 3, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_Init(&g_fxAttr);    // FxVertex (24 bytes)
    AttrInfo_AddLoader(&g_fxAttr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&g_fxAttr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&g_fxAttr, 2, GPU_UNSIGNED_BYTE, 4);
    g_pose.reserve(256);
    buildFx();
    buildSky();
    buildMarker();
    buildRing();
    buildSelection();
    rendererSetProjection(250.0f);
}

void rendererShutdown() {
    rendererPosesShutdown();   // 7.05
    shaderProgramFree(&g_terrainSh.prog);
    shaderProgramFree(&g_modelSh.prog);
    shaderProgramFree(&g_skinSh.prog);
    shaderProgramFree(&g_fxSh.prog);
    DVLB_Free(g_fxSh.dvlb);
    for (FxVertex*& v : g_fxVerts) { if (v) linearFree(v); v = nullptr; }
    if (g_fxIdx) linearFree(g_fxIdx);
    g_fxIdx = nullptr;
    if (g_fxDotOk) C3D_TexDelete(&g_fxDot);
    g_fxDotOk = false;
    fx::global().clear();
    DVLB_Free(g_skinSh.dvlb);
    DVLB_Free(g_terrainSh.dvlb);
    DVLB_Free(g_modelSh.dvlb);
    for (int b = 0; b < 2; ++b) if (g_skyBuf[b]) { linearFree(g_skyBuf[b]); g_skyBuf[b] = nullptr; }
    for (int b = 0; b < 3; ++b) if (g_celBuf[b]) { linearFree(g_celBuf[b]); g_celBuf[b] = nullptr; }
    if (g_skyI) linearFree(g_skyI);
    if (g_markerV) linearFree(g_markerV);
}

static float g_iod = 0;
static void buildProj() {
    if (g_iod == 0) Mtx_PerspTilt(&g_proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, g_near, g_far, false);
    else Mtx_PerspStereoTilt(&g_proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, g_near, g_far, g_iod, 5.0f, false);
}

void rendererSetEye(float iod) {
    g_iod = iod;
    buildProj();
}

void rendererSetProjection(float drawDist) {
    // A near plane of 1 yd keeps depth precision usable with 24-bit depth;
    // the far plane follows the draw distance so fog and clipping agree.
    g_near = 1.0f;
    // 6.34: the far plane at the horizon (24-bit depth: ~0.1 yd steps at 1200 yd with the 1 yd near plane)
    const bool horizonOn = g_horizonWant > drawDist;
    g_horizon = horizonOn ? g_horizonWant : drawDist;
    g_far = horizonOn ? g_horizon * 1.05f + 30.0f : drawDist * 1.15f + 30.0f;
    g_skyRadius = g_far * 0.9f;
    buildProj();

    // Linear fog by true view distance: from 55% to 100% of the draw distance; with the horizon (6.34) from 45% of it to the
    // horizon (the ground and the shells beyond the draw distance melt into the sky there)
    const float start = drawDist * (horizonOn ? 0.45f : 0.55f), end = g_horizon;
    g_fogX = 1.0f / (end - start);
    g_fogY = end / (end - start);
}

void rendererSetHorizon(float yards) { g_horizonWant = yards; }
void rendererSetViewer(const float* p) {   // 6.42: the character's position for the portal walk
    g_haveViewer = p != nullptr;
    if (p) memcpy(g_viewer, p, sizeof g_viewer);
}

bool rendererUnitPoint(uint64_t guid, int slot, float out[3]) {
    if (slot < 0 || slot > 5) return false;
    auto it = g_unitPts.find(guid);
    if (it == g_unitPts.end() || g_ptsFrame - it->second.frame > 2 || !(it->second.has & (1u << slot))) return false;
    memcpy(out, it->second.p[slot], sizeof it->second.p[slot]);
    return true;
}

// ------------------------------------------------------------------ stage 6.3: early GPU kick
// citro3d builds the whole frame's command list and hands it to the GPU in C3D_FrameEnd: the GPU idles while the CPU builds
// it and the CPU idles in the next C3D_FrameBegin while the GPU draws it (perf log, 400x240 Stormwind: CPU 12.4 ms, GPU
// 12.2 ms, frame 23 ms). A kick splits the list (C3D_FrameSplit: the commands so far become an entry of citro3d's GX queue)
// and starts the queue, which C3D_FrameBegin stopped - the GPU draws the terrain / opaque models while the CPU goes on.
// The queue is the first member of citro3d's context (__C3D_Context.gxQueue, citro3d source/internal.h); checked before use.
extern "C" {
extern gxCmdQueue_s __C3D_Context;
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
}
static gxCmdQueue_s* c3dQueue() {
    static int ok = -1;   // the layout check, once
    gxCmdQueue_s* q = &__C3D_Context;
    if (ok < 0) ok = q->entries && q->maxEntries >= 16 && q->maxEntries <= 1024 && q->callback ? 1 : 0;
    return ok ? q : nullptr;
}
static void gpuKick(FrameCounters& fc) {
    gxCmdQueue_s* q = c3dQueue();
    if (!q) return;
    perf::mark("draw: GPU kick");
    const u64 t0 = svcGetSystemTick();
    // Everything the CPU wrote that the GPU reads out of the cache first, as C3D_FrameEnd does. Then the split WITH
    // GX_CMDLIST_FLUSH: the split itself writes the list's last words (framebuffer flush, GPUREG_FINALIZE) after the flush
    // above, and once the queue runs the entry is submitted at once - without the flag the GPU could read stale words for
    // the list's end and run past it (stage 6.7: the GPU hung in the first kicked list, "5 commands, 2 sent, 1 done,
    // waiting on command list", a faked done did not help; C3D_FrameEnd splits before its flush, which is why it never did).
    GSPGPU_FlushDataCache((void*)__ctru_linear_heap, __ctru_linear_heap_size);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    gxCmdQueueRun(q);   // (a no-op while it runs: the next entries follow by themselves)
    if (!fc.kicks) fc.firstKickTick = t0;
    ++fc.kicks;
    fc.kickMs += ticksToMs(svcGetSystemTick() - t0);
}
gxCmdQueue_s* rendererGxQueue() { return c3dQueue(); }

void rendererBeforeFrameEnd(FrameCounters& fc) {
    if (!fc.kicks) return;
    GSPGPU_FlushDataCache((void*)__ctru_linear_heap, __ctru_linear_heap_size);
    fc.kickLeadMs = ticksToMs(svcGetSystemTick() - fc.firstKickTick);
}

// The view frustum of a frame (under water: sight ends at the water's fog distance).
static void frameFrustum(const Camera& cam, const RenderSettings& rs, Frustum& fr) {
    fr.build(cam, g_near);
    if (rs.underwater) {
        fr.maxDist = std::min(fr.maxDist, std::max(rs.waterFogEnd, 6.0f) + 8.0f);
        fr.d[5] = fr.maxDist;
        fr.horizon = fr.maxDist;
    }
}

// Visible doodads / buildings / spell-effect meshes into each model's 'visible' list (g_activeModels).
static std::vector<Instance> g_effInst;   // spell effects' meshes (stage 5.33), drawn with the doodads
// 6.59: the sun in a turned model's own space (as the streamer gives the tiles' instances: lightToModel)
// 7.05: the pose thread's public face (frameFrustum is defined just above)
bool rendererPosesStart(const Camera& cam, const RenderSettings& rs, const std::vector<CreatureInstance>* list) {
    Frustum fr;
    frameFrustum(cam, rs, fr);
    return posesStartImpl(fr, rs, list);
}
void rendererPosesShutdown() { posesShutdownImpl(); }

static void sunInModel(const float rot[3], float out[3]) {
    float v[3] = {0.30f, 0.40f, 0.866f};   // (the tiles' sun: streamer.cpp lightToModel)
    float c, s, tmp;
    c = cosf(-rot[2]); s = sinf(-rot[2]);
    tmp = c * v[0] - s * v[1]; v[1] = s * v[0] + c * v[1]; v[0] = tmp;
    c = cosf(-rot[1]); s = sinf(-rot[1]);
    tmp = c * v[0] + s * v[2]; v[2] = -s * v[0] + c * v[2]; v[0] = tmp;
    c = cosf(-rot[0]); s = sinf(-rot[0]);
    tmp = c * v[1] - s * v[2]; v[2] = s * v[1] + c * v[2]; v[1] = tmp;
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
}

static void collectAll(const Frustum& fr, const RenderSettings& rs, FrameCounters& fc, const std::vector<SceneModel>* effects) {
    collectModels(fr, rs, fc);   // doodads (if enabled) and buildings
    g_effInst.clear();
    if (effects) {
        g_effInst.reserve(effects->size());
        for (const SceneModel& sm : *effects) {
            Model* m = sm.model;
            if (!sm.emit || !m || !m->drawable || !m->h) continue;
            if (sm.building) {   // 6.59: a game object that is a building (its group or a piece of its furniture)
                if ((g_gtOff & kGtBuildings) && m->building) continue;
                if (!rs.doodads && !m->building) continue;
                Instance in{};
                in.model = m;
                memcpy(in.pos, sm.pos, sizeof in.pos);
                memcpy(in.rot, sm.rot, sizeof in.rot);
                in.scale = sm.scale;
                in.radius = m->h->radius * sm.scale;
                sunInModel(in.rot, in.lightModel);
                g_effInst.push_back(in);
                ++fc.objWmoParts;
                continue;
            }
            if (rs.effects <= 0) continue;
            Instance in{};
            in.model = m;
            memcpy(in.pos, sm.pos, sizeof in.pos);
            memcpy(in.rot, sm.rot, sizeof in.rot);
            in.scale = sm.scale;
            in.animMs = sm.fxAgeMs;
            in.radius = m->h->radius * sm.scale * (m->fxAnim.a ? 1.5f : 1.0f);   // (animated parts swing out)
            in.lightModel[0] = kSun[0]; in.lightModel[1] = kSun[1]; in.lightModel[2] = kSun[2];   // effects are mostly unlit
            g_effInst.push_back(in);
        }
        for (const Instance& in : g_effInst) {
            if (!fr.sphere(in.pos, std::max(in.radius, 0.5f))) continue;
            if (in.model->visible.empty()) g_activeModels.push_back(in.model);
            in.model->visible.push_back(&in);
        }
    }
}
static void clearVisible() {   // per-frame instance pointers: tiles / models may be freed before the next frame
    for (Model* m : g_activeModels) { m->visible.clear(); m->fadeCount = 0; }
    g_activeModels.clear();
    g_activeBatches.clear();   // 6.44: points into tiles that may be freed before the next frame (and the login scene draws none)
    g_mvReady = false;
}

// A new frame (not the second eye): unit points age; old ones go now and then.
static float g_ptsLastTime = -1.0f;
static void agePoints(const RenderSettings& rs) {
    if (rs.timeSec == g_ptsLastTime) return;
    g_ptsLastTime = rs.timeSec;
    ++g_ptsFrame;
    if ((g_ptsFrame & 127) == 0)
        for (auto it = g_unitPts.begin(); it != g_unitPts.end();) it = g_ptsFrame - it->second.frame > 60 ? g_unitPts.erase(it) : std::next(it);
}

// Stage 6.4: the visibility pass before C3D_FrameBegin - it runs while the GPU still draws the previous frame instead of
// after the wait (perf log: collect 2.2-3.9 ms a frame, CPU and GPU taking turns). rendererDraw then only builds commands;
// with two eyes both use the same lists (they were collected twice before, with the same frustum).
static bool g_prepared = false;
void rendererPrepare(const Camera& cam, const RenderSettings& rsIn, FrameCounters& fc, const std::vector<SceneModel>* effects,
                     const std::vector<CreatureInstance>* creatures) {
    g_gtOff = rsIn.gpuTestOff;   // 6.46
    clearVisible();
    RenderSettings rs = rsIn;
    if (rs.underwater) rs.fog = true;
    g_nowMs = rs.nowMs;     // stage 6.11
    g_nowMs6 = rs.nowMs;    // stage 6.12
    g_fxCreatures = creatures && rs.creatures ? creatures : nullptr;   // stage 6.13
    g_fadeOn = rs.fadeIn;
    Frustum fr;
    frameFrustum(cam, rs, fr);
    const u64 t0 = svcGetSystemTick();
    perf::mark("prepare: visibility");
    collectAll(fr, rs, fc, effects);
    const u64 t1 = svcGetSystemTick();
    perf::mark("prepare: unit poses");
    fc.passMs[FrameCounters::kCollect] += ticksToMs(t1 - t0);
    // stage 6.4 (second step): the units' poses and the particles' simulation too
    agePoints(rs);
    if (creatures && rs.creatures && !creatures->empty() && !posesJoin(creatures, fc)) prepareCreatures(fr, *creatures, fc);   // 7.05
    float f[3];
    cam.forward(f);
    C3D_Mtx view;
    Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(f[0], f[1], f[2]), FVec3_New(0, 0, 1), false);
    const u64 tm = svcGetSystemTick();
    buildMatrices(cam, view);   // stage 6.10: before the GPU wait too
    fc.matrixMs += ticksToMs(svcGetSystemTick() - tm);
    const u64 t2 = svcGetSystemTick();
    perf::mark("prepare: particles");
    fxSimulate(cam, view, rs, effects);
    fc.fxMs += ticksToMs(svcGetSystemTick() - t2);
    fc.prepareMs += ticksToMs(svcGetSystemTick() - t1);
    g_prepared = true;
}
void rendererEndFrame() {
    clearVisible();
    g_fxCreatures = nullptr;
    g_prepared = false;
    g_posedList = nullptr;
}

void rendererDraw(C3D_RenderTarget* target, const Camera& cam, const RenderSettings& rsIn, FrameCounters& fc,
                  const std::vector<Marker>* markers, const std::vector<CreatureInstance>* creatures,
                  const std::vector<SceneModel>* effects) {
    (void)target;
    RenderSettings rs = rsIn;
    g_gtOff = rs.gpuTestOff;   // 6.46
    g_ghostPass = g_ghost > 0.5f;   // 6.77
    if (rs.underwater) rs.fog = true;   // the water is never clear: the fog switch only concerns the open air
    float f[3];
    cam.forward(f);
    C3D_Mtx view;
    Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(f[0], f[1], f[2]), FVec3_New(0, 0, 1), false);
    Frustum fr;
    frameFrustum(cam, rs, fr);
    // Under the surface (stage 5.26): the water's colour all around, sight ends at its fog distance (Light.dbc underwater
    // parameters): no sky, and nothing farther than that is drawn at all.
    const float keepX = g_fogX, keepY = g_fogY;
    if (g_ghostPass && !rs.underwater && keepX > 1e-6f) {   // 6.80: a ghost sees less: the fog comes in (start and end both pulled in), the far slabs melt into the haze
        const float e = keepY / keepX, s0 = e - 1.0f / keepX;
        const float e2 = std::max(e * g_ghostFogShare, 40.0f), s2 = std::max(s0 * g_ghostFogShare * 0.6f, 5.0f);
        g_fogX = 1.0f / std::max(e2 - s2, 1.0f);
        g_fogY = e2 * g_fogX;
    }
    if (rs.underwater) {
        const float end = std::max(rs.waterFogEnd, 6.0f);
        g_fogX = 1.0f / end;   // visibility = 1 - dist / end: from the eye
        g_fogY = 1.0f;
        g_fogColor = fogConstColor(rs.waterFog);
    } else {
        g_fogColor = fogConstColor(g_skyCol[kSkyFog]);
    }

    // stage 6 (perf log): each pass's CPU time, draws and triangles
    ++fc.eyes;
    u64 pt = svcGetSystemTick();
    u32 pd = fc.drawCalls, ptr = fc.tris;
    auto pass = [&](int k) {
        const u64 now = svcGetSystemTick();
        fc.passMs[k] += ticksToMs(now - pt);
        fc.passDraws[k] += fc.drawCalls - pd;
        fc.passTris[k] += fc.tris - ptr;
        pt = now; pd = fc.drawCalls; ptr = fc.tris;
    };
    perf::mark("draw: sky / terrain");
    if (!rs.underwater) { drawSky(view, fc); drawCelestial(view, fc); drawSkyModel(view, rs, fc); }
    pass(FrameCounters::kSky);
    if (!(g_gtOff & kGtTerrain)) drawTerrain(cam, view, fr, rs, fc);
    pass(FrameCounters::kTerrain);
    if (rs.gpuKick) gpuKick(fc);   // stage 6.3: the GPU draws the sky and the ground while the models are sorted out
    agePoints(rs);
    if (!g_prepared) collectAll(fr, rs, fc, effects);   // (stage 6.4: normally done by rendererPrepare)
    pass(FrameCounters::kCollect);
    perf::mark("draw: opaque models");
    drawModels(cam, view, rs, false, fc);
    pass(FrameCounters::kOpaque);
    if (rs.gpuKick) gpuKick(fc);   // ... and the opaque models while units, water, the transparent and the HUD are built
    if (markers) drawMarkers(cam, view, fr, rs, *markers, fc);
    pass(FrameCounters::kMarkers);
    perf::mark("draw: units");
    if (creatures && rs.creatures) drawCreatures(cam, view, fr, rs, *creatures, fc);
    pass(FrameCounters::kCreatures);
    perf::mark("draw: water / transparent");
    const bool alphaOn = !(g_gtOff & kGtAlpha);   // 6.46: the GPU test - no water, no transparent pass, no particles
    if (alphaOn) drawWater(cam, view, fr, rs, fc);
    pass(FrameCounters::kWater);
    if (alphaOn) drawModels(cam, view, rs, true, fc);
    pass(FrameCounters::kAlpha);
    perf::mark("draw: particles");
    if (alphaOn) drawFx(cam, view, rs, fc, effects);
    pass(FrameCounters::kFx);
    // Drop per-frame instance pointers: tiles/models may be freed before the next frame (prepared: rendererEndFrame does,
    // after the last eye).
    if (!g_prepared) clearVisible();
    g_fogX = keepX;
    g_fogY = keepY;
    g_ghostPass = false;
}

void rendererDrawScene(const Camera& cam, float vfovDeg, float nearZ, float farZ, const std::vector<SceneModel>& models,
                       const std::vector<CreatureInstance>* creatures, FrameCounters& fc) {
    poseWait();   // 7.05: a pose job nobody joined (the login screen came) is over before anything is drawn
    const C3D_Mtx keepProj = g_proj;
    const float keepNear = g_near, keepFar = g_far;
    const uint32_t keepNowMs = g_nowMs;
    g_nowMs = streamerNowMs();   // 6.41: rendererPrepare does not run behind the login screens: the portal's textures move on this clock
    g_near = std::max(nearZ, 0.1f);
    g_far = std::max(farZ, g_near + 1.0f);
    if (g_iod == 0) Mtx_PerspTilt(&g_proj, C3D_AngleFromDegrees(vfovDeg), C3D_AspectRatioTop, g_near, g_far, false);
    else Mtx_PerspStereoTilt(&g_proj, C3D_AngleFromDegrees(vfovDeg), C3D_AspectRatioTop, g_near, g_far, g_iod, 3.0f, false);
    float f[3];
    cam.forward(f);
    C3D_Mtx view;
    Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(f[0], f[1], f[2]), FVec3_New(0, 0, 1), false);
    Camera wide = cam;
    wide.drawDist = g_far;
    Frustum fr;
    fr.build(wide, g_near);
    RenderSettings rs;
    rs.fog = false;
    static std::vector<Instance> inst;
    inst.clear();
    inst.reserve(models.size());
    for (const SceneModel& sm : models) {
        Model* m = sm.model;
        if (!m || !m->h || m->state != AssetState::Ready || !m->texturesReady()) continue;
        Instance in{};
        in.model = m;
        memcpy(in.pos, sm.pos, sizeof in.pos);
        memcpy(in.rot, sm.rot, sizeof in.rot);
        in.scale = sm.scale;
        in.radius = 1e6f;
        in.lightModel[0] = kSun[0]; in.lightModel[1] = kSun[1]; in.lightModel[2] = kSun[2];
        inst.push_back(in);
    }
    for (const Instance& in : inst) {
        if (in.model->visible.empty()) g_activeModels.push_back(in.model);
        in.model->visible.push_back(&in);
    }
    drawModels(cam, view, rs, false, fc);
    if (creatures) drawCreatures(cam, view, fr, rs, *creatures, fc);
    drawModels(cam, view, rs, true, fc);
    clearVisible();
    g_nowMs = keepNowMs;
    g_proj = keepProj;
    g_near = keepNear;
    g_far = keepFar;
}

bool rendererDrawPortrait(const CreatureInstance& ciIn, const float corr[4], FrameCounters& fc, bool body, float turn) {
    const CreatureDisplay* d = ciIn.disp;
    if (!d || !d->ready()) return false;
    const uint8_t* blob = d->model->blob;
    const W3dCreatureHeader* h = d->model->h;
    CreatureInstance ci = ciIn;   // at the origin, facing +x
    ci.pos[0] = ci.pos[1] = ci.pos[2] = 0;
    ci.yaw = 0;
    ci.guid = 0;                  // (no unit points recorded)
    const float s = w3d_unit_draw_scale(ci.scale, &d->d);
    // the head: the helmet attachment through the pose, else the front top of the bounding box (beasts' heads are in front)
    if (g_pose.size() < h->boneCount) g_pose.resize(h->boneCount);
    w3d_anim_pose(blob, ci.seq, ci.timeMs, ci.loop ? 1 : 0, g_pose.data());
    float head[3] = {0.65f * h->bmax[0] * s, 0, 0.78f * h->bmax[2] * s};
    if (h->attachCount && h->offAttach) {
        const W3dAttach* at = (const W3dAttach*)(blob + h->offAttach);
        for (u32 k = 0; k < h->attachCount; ++k) {
            const W3dAttach& a = at[k];
            if (a.id != W3D_ATT_HELM || a.bone < 0 || (u32)a.bone >= h->boneCount) continue;
            const float* M = g_pose[a.bone].m;
            for (int r = 0; r < 3; ++r) head[r] = (M[r * 4] * a.pos[0] + M[r * 4 + 1] * a.pos[1] + M[r * 4 + 2] * a.pos[2] + M[r * 4 + 3]) * s;
            break;
        }
    }
    // framing: about a sixth of the unit's height above and below the head's centre (head and shoulders of a person);
    // the character window (UI stage 14, body): the whole figure, turned by 'turn' from the front
    const float height = std::max(0.3f, (h->bmax[2] - h->bmin[2]) * s);
    float half = std::clamp(0.16f * height, 0.12f, 4.0f);
    float focus[3] = {head[0], head[1], head[2]};
    if (body) {
        half = 0.56f * height;
        focus[0] = 0.5f * (h->bmin[0] + h->bmax[0]) * s;
        focus[1] = 0.5f * (h->bmin[1] + h->bmax[1]) * s;
        focus[2] = (h->bmin[2] + 0.5f * (h->bmax[2] - h->bmin[2])) * s;
    }
    const float fov = body ? 30.0f : 40.0f, dist = half / tanf(fov * 0.5f * 3.14159265f / 180.0f);
    const float side = body ? 0.25f + turn : 0.35f;     // a little from the unit's left (the original portraits look slightly sideways)
    Camera cam;
    cam.pos[0] = focus[0] + cosf(side) * dist;
    cam.pos[1] = focus[1] + sinf(side) * dist;
    cam.pos[2] = focus[2] + (body ? 0.04f : 0.12f) * dist;
    const float dx = focus[0] - cam.pos[0], dy = focus[1] - cam.pos[1], dz = focus[2] - (body ? 0.0f : 0.05f * half) - cam.pos[2];
    cam.yaw = atan2f(dy, dx);
    cam.pitch = atan2f(dz, sqrtf(dx * dx + dy * dy));
    cam.drawDist = dist + 4.0f * height + 10.0f;
    const C3D_Mtx keepProj = g_proj;
    const float keepNear = g_near, keepFar = g_far;
    g_near = std::max(0.05f, dist * 0.25f);
    g_far = cam.drawDist + 5.0f;
    C3D_Mtx p, c;
    Mtx_Persp(&p, C3D_AngleFromDegrees(fov), 1.0f, g_near, g_far, false);
    Mtx_Identity(&c);   // the orientation fix: clip x' = corr0 x + corr1 y, y' = corr2 x + corr3 y
    c.r[0].x = corr[0]; c.r[0].y = corr[1];
    c.r[1].x = corr[2]; c.r[1].y = corr[3];
    Mtx_Multiply(&g_proj, &c, &p);
    float f[3];
    cam.forward(f);
    C3D_Mtx view;
    Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(f[0], f[1], f[2]), FVec3_New(0, 0, 1), false);
    Frustum fr;
    fr.build(cam, g_near);
    RenderSettings rs;
    rs.fog = false;
    rs.modelsNoCull = true;       // (the fix may mirror the picture: both faces, the depth test sorts them out)
    std::vector<CreatureInstance> one(1, ci);
    const u32 before = fc.creatures;
    drawCreatures(cam, view, fr, rs, one, fc);
    g_proj = keepProj;
    g_near = keepNear;
    g_far = keepFar;
    return fc.creatures != before;
}

// 6.61: the zone's sky model (streamerAcquireItemModel; the caller keeps it acquired and releases it); null: none
void rendererSetSkyModel(Model* m) { g_skyModel = m; }

// 6.61: the sun and the moon for this hour of the server's day (sky.cpp positions); strengths 0 = below the horizon, not drawn
void rendererSetCelestial(const float sunDir[3], float sunStrength, const float moonDir[3], float moonStrength) {
    if (!g_celBuf[0]) return;
    const int next = (g_celCur + 1) % 3;
    W3dTerrainVertex* out = g_celBuf[next];
    int n = 0;
    if (sunStrength > 0.01f) {
        const float warm[3] = {255, 244, 205}, halo[3] = {255, 190, 110};
        n += celDisc(out + n, sunDir, 13.0f, halo, 0.55f * sunStrength, 0.0f);
        n += celDisc(out + n, sunDir, 3.6f, warm, sunStrength, sunStrength);
    }
    if (moonStrength > 0.01f) {
        const float pale[3] = {225, 232, 255}, halo[3] = {120, 140, 200};
        n += celDisc(out + n, moonDir, 7.0f, halo, 0.45f * moonStrength, 0.0f);
        n += celDisc(out + n, moonDir, 2.8f, pale, moonStrength, moonStrength);
    }
    GSPGPU_FlushDataCache(out, (size_t)n * sizeof(W3dTerrainVertex));
    g_celCount = n;
    g_celCur = next;
}

// 6.60: the sky for this spot and hour (sky.cpp); 0..255 each. The dome is coloured again when a colour moved by a step or more.
// (the zone's colours, graded for the spirit world, into the dome)
static void applySkyColours() {
    uint8_t next[kSkyN][3];
    bool changed = false;
    for (int k = 0; k < kSkyN; ++k) {
        sky::Rgb in{g_skyBase[k][0], g_skyBase[k][1], g_skyBase[k][2]};
        if (g_ghost > 0.001f) in = sky::ghostGrade(in, g_ghost);
        const float v[3] = {in.r, in.g, in.b};
        for (int c = 0; c < 3; ++c) {
            next[k][c] = (uint8_t)std::clamp((int)(v[c] + 0.5f), 0, 255);
            if (std::abs((int)next[k][c] - (int)g_skyCol[k][c]) >= 1) changed = true;
        }
    }
    if (!changed || !g_skyBuf[0]) return;
    memcpy(g_skyCol, next, sizeof g_skyCol);
    const int spare = 1 - g_skyCur;
    fillSky(g_skyBuf[spare]);   // (the buffer the GPU is not reading; it is the current one from the next frame on)
    g_skyCur = spare;
}

void rendererSetSky(const float top[3], const float mid[3], const float band1[3], const float band2[3], const float fog[3]) {
    const float* in[kSkyN] = {top, mid, band1, band2, fog};
    for (int k = 0; k < kSkyN; ++k)
        for (int c = 0; c < 3; ++c) g_skyBase[k][c] = in[k][c];
    applySkyColours();
}

// 6.62: the spirit world: 0 = alive, 1 = a ghost (the sky and the fog go grey, darker and cold); 0.4 for the body before the spirit is released
void rendererSetGhostFog(float share) { g_ghostFogShare = std::clamp(share, 0.25f, 1.0f); }

void rendererSetGhost(float amount) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    if (std::fabs(amount - g_ghost) < 0.004f) return;
    g_ghost = amount;
    applySkyColours();
}

u32 rendererClearColor(const RenderSettings& rs) {   // RGBA8 for C3D_RenderTargetClear
    if (!rs.underwater) return ((u32)g_skyCol[kSkyMid][0] << 24) | ((u32)g_skyCol[kSkyMid][1] << 16) | ((u32)g_skyCol[kSkyMid][2] << 8) | 0xFF;
    return ((u32)rs.waterFog[0] << 24) | ((u32)rs.waterFog[1] << 16) | ((u32)rs.waterFog[2] << 8) | 0xFF;
}

