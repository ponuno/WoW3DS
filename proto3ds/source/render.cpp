// render.cpp — sky, terrain, doodads and water for all streamed tiles.
//
// Everything is drawn camera-relative: the CPU builds per-object modelView
// matrices in float32 from (object - camera), because PICA200 uniforms are
// float24 and absolute coordinates (~17000 yd) would visibly jitter.
#include "w3d_farlod.h"
#include "w3d_farobj.h"
#include "world.h"

#include <array>
#include <vector>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>

#include "w3d_anim.h"

#include "model_shbin.h"
#include "skin_shbin.h"
#include "terrain_shbin.h"
#include "fx_shbin.h"
#include "fx.hpp"
#include "perflog.hpp"

namespace {

// Sky colours (RGB). Fog uses the horizon colour so distant terrain melts into the sky.
const uint8_t kZenith[3] = {70, 120, 200};
const uint8_t kHorizon[3] = {190, 210, 230};
inline u32 fogConstColor(const uint8_t c[3]) { return (u32)c[0] | ((u32)c[1] << 8) | ((u32)c[2] << 16) | 0xFF000000u; }
u32 g_fogColor = fogConstColor(kHorizon);   // this frame's fog: the horizon, or the water's colour under the surface

struct Shader {
    DVLB_s* dvlb = nullptr;
    shaderProgram_s prog;
    int uProj = -1, uMV = -1, uScale = -1, uOffset = -1, uUV = -1, uLight = -1, uLightParams = -1, uFog = -1, uBones = -1;
    int uMask = -1;
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
W3dTerrainVertex* g_skyV = nullptr;
uint16_t* g_skyI = nullptr;
int g_skyIndexCount = 0;

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
}

void buildSky() {
    const int rings = 7, segs = 16;
    const int nv = (rings + 1) * (segs + 1);
    g_skyV = (W3dTerrainVertex*)linearAlloc(nv * sizeof(W3dTerrainVertex));
    g_skyI = (uint16_t*)linearAlloc(rings * segs * 6 * sizeof(uint16_t));
    int k = 0;
    for (int r = 0; r <= rings; ++r) {
        // elevation from -10 deg (below horizon, hides the far-plane edge) to the zenith
        float el = (-10.0f + 100.0f * r / rings) * 3.14159265f / 180.0f;
        float t = std::clamp(sinf(el), 0.0f, 1.0f);
        t = sqrtf(t);
        for (int s = 0; s <= segs; ++s, ++k) {
            float az = 6.2831853f * s / segs;
            W3dTerrainVertex& v = g_skyV[k];
            v.x = (int16_t)(32000.0f * cosf(el) * cosf(az));
            v.y = (int16_t)(32000.0f * cosf(el) * sinf(az));
            v.z = (int16_t)(32000.0f * sinf(el));
            v.w = 0;
            v.r = (uint8_t)(kHorizon[0] + (kZenith[0] - kHorizon[0]) * t);
            v.g = (uint8_t)(kHorizon[1] + (kZenith[1] - kHorizon[1]) * t);
            v.b = (uint8_t)(kHorizon[2] + (kZenith[2] - kHorizon[2]) * t);
            v.a = 255;
            v.u = v.v = 0;
        }
    }
    int i = 0;
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segs; ++s) {
            uint16_t a = (uint16_t)(r * (segs + 1) + s), b = (uint16_t)(a + 1), c = (uint16_t)(a + segs + 1), d = (uint16_t)(c + 1);
            uint16_t q[6] = {a, b, c, c, b, d};
            memcpy(g_skyI + i, q, sizeof q);
            i += 6;
        }
    g_skyIndexCount = i;
    GSPGPU_FlushDataCache(g_skyV, nv * sizeof(W3dTerrainVertex));
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

struct BindCache {
    GpuTex* unit[3] = {nullptr, nullptr, nullptr};   // (6.32: was [2] - the terrain's third texture (5.47) wrote past it)
    void bind(int u, GpuTex* t, FrameCounters& fc) {
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
void fogStage(int stage) {
    C3D_TexEnv* env = C3D_GetTexEnv(stage);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_G);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, g_fogColor);
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
    C3D_BindProgram(&g_terrainSh.prog);
    C3D_SetAttrInfo(&g_terrainAttr);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_skyV, sizeof(W3dTerrainVertex), 3, 0x210);
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
    C3D_DrawElements(GPU_TRIANGLES, g_skyIndexCount, C3D_UNSIGNED_SHORT, g_skyI);
    ++fc.drawCalls;
}

void drawTerrain(const Camera& cam, const C3D_Mtx& view, const Frustum& fr, const RenderSettings& rs, FrameCounters& fc) {
    C3D_BindProgram(&g_terrainSh.prog);
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
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE0, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
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
        fogStage(next++);
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
                C3D_BufInfo* buf = C3D_GetBufInfo();
                BufInfo_Init(buf);
                BufInfo_Add(buf, d.verts, sizeof(W3dTerrainVertex), 3, 0x210);
                C3D_Mtx mv;
                camRelative(&mv, view, d.origin, cam.pos);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &mv);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, W3D_FAR_XY_SCALE, W3D_FAR_XY_SCALE, zs, W3D_FAR_UV_SCALE);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, 0, 0, -W3D_FAR_SINK, 0);
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
        const uint16_t* tex2 = w3d_terrain_tex2(h, t->terr);
        setTerrainEnv(tex2 ? 3 : 2);
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
            bc.bind(1, t->ttex[b.tex1], fc);
            if (tex2) bc.bind(2, t->ttex[tex2[i]], fc);
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
static inline void collectInstance(const Tile* t, Instance& in, const Frustum& fr, const RenderSettings& rs, FrameCounters& fc) {
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
    // Small doodads fade out long before the draw distance (bushes ~110 yd,
    // big trees reach the fog); this is what keeps draw calls in budget.
    // Buildings are kept to the draw distance.
    // Interior-only groups (rooms, Stormwind's halls and cellars; no portals yet) are drawn only
    // when the camera is near them: from outside they are hidden behind the exterior anyway.
    float maxD = m->interior ? in.radius + 25.0f
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
    in.lod = m->lods ? w3d_lod_for(d2, in.radius) : 0;   // stage 6.12: by the size on the screen
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
                if (ch.hasI) { maxD = std::max(maxD, ch.maxRi + 25.0f); r = std::max(r, ch.maxRi); }
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
    C3D_BindProgram(&g_modelSh.prog);
    C3D_SetAttrInfo(&g_modelAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uProj, &g_proj);
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
    fogStage(2);
    resetStages(3);
    setFogUniform(g_modelSh, rs.fog, true);

    if (!g_mvReady) buildMatrices(cam, view);
    BindCache bc;
    for (Model* m : g_activeModels) {
        const W3dModelHeader* h = m->h;
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, m->blob + h->offVertices, sizeof(W3dModelVertex), 3, 0x210);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uScale, h->posScale[0], h->posScale[1], h->posScale[2], w3d_model_uv_scale(h));
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uOffset, h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
        const uint16_t* idx = (const uint16_t*)(m->blob + h->offIndices);
        for (u32 s = 0; s < h->submeshCount; ++s) {
            const W3dSubmesh& sm = m->subs[s];
            bool isBlended = sm.blend == W3D_BLEND_ALPHA || sm.blend == W3D_BLEND_ADD;
            // stage 6.11: the opaque and cut-out parts of the instances that are fading are drawn alpha-blended, in the
            // transparent pass (depth is still written: a half-visible wall hides what is behind it)
            const bool fadeOnly = blendedPass && !isBlended && m->fadeCount;
            if (isBlended != blendedPass && !fadeOnly) continue;
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
            bc.bind(0, sm.tex == W3D_NO_TEXTURE ? streamerWhiteTexture() : m->tex[sm.tex], fc);
            const float unlit = (sm.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 0.55f, 0.6f, 0, unlit);   // (6.10: once a part)
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
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uMV, &mv);
                if (!lastLight || memcmp(lastLight, in->lightModel, sizeof in->lightModel) != 0) {   // (the same rotation: the same light)
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLight, in->lightModel[0], in->lightModel[1], in->lightModel[2], 0);
                    lastLight = in->lightModel;
                }
                uint32_t first = sm.firstIndex, count = sm.indexCount;
                if (in->lod && m->lods)   // stage 6.12: the lightest level there is, up to the one asked for
                    for (int k = in->lod; k >= 1; --k)
                        if (m->lods[s].indexCount[k - 1]) { first = m->lods[s].firstIndex[k - 1]; count = m->lods[s].indexCount[k - 1]; break; }
                C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, idx + first);
                ++fc.drawCalls;
                ++fc.lodDraws[in->lod < 3 ? in->lod : 2];
                fc.tris += count / 3;
            }
        }
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

void drawMarkers(const Camera& cam, const C3D_Mtx& view, const Frustum& fr, const RenderSettings& rs,
                 const std::vector<Marker>& markers, FrameCounters& fc) {
    if (markers.empty()) return;
    C3D_BindProgram(&g_modelSh.prog);
    C3D_SetAttrInfo(&g_modelAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uProj, &g_proj);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_markerV, sizeof(W3dModelVertex), 3, 0x210);
    const float q = 1.0f / kMarkerQ;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uScale, q, q, q, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uOffset, 0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 0.5f, 0.6f, 0, 0.35f);
    fogStage(1);
    resetStages(2);
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
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uScale, qq, qq, qq, 0);
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
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uMV, &mv);
        // light direction in marker space: rotate the sun by -yaw
        float cy = cosf(-m.yaw), sy = sinf(-m.yaw);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLight, sun[0] * cy - sun[1] * sy, sun[0] * sy + sun[1] * cy, sun[2], 0);
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
    C3D_BindProgram(&g_modelSh.prog);
    C3D_SetAttrInfo(&g_modelAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uProj, &g_proj);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    fogStage(1);
    resetStages(2);
    setFogUniform(g_modelSh, rs.fog, true);
    BindCache bc;
    for (int pass = 0; pass < 2; ++pass) {   // opaque, then blended
        for (const WornDraw& w : g_worn) {
            Model* m = w.model;
            const W3dModelHeader* h = m->h;
            C3D_BufInfo* buf = C3D_GetBufInfo();
            BufInfo_Init(buf);
            BufInfo_Add(buf, m->blob + h->offVertices, sizeof(W3dModelVertex), 3, 0x210);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uScale, h->posScale[0], h->posScale[1], h->posScale[2], w3d_model_uv_scale(h));
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uOffset, h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uMV, &w.mv);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLight, w.light[0], w.light[1], w.light[2], 0);
            const uint16_t* idx = (const uint16_t*)(m->blob + h->offIndices);
            for (u32 i = 0; i < h->submeshCount; ++i) {
                const W3dSubmesh& sm = m->subs[i];
                const bool blended = sm.blend == W3D_BLEND_ALPHA || sm.blend == W3D_BLEND_ADD;
                if (blended != (pass == 1)) continue;
                setModelBlend(sm.blend, sm.flags & W3D_SUBF_TWO_SIDED, rs.modelsNoCull);
                bc.bind(0, sm.tex == W3D_NO_TEXTURE ? streamerWhiteTexture() : m->tex[sm.tex], fc);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 0.55f, 0.6f, 0, (sm.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f);
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

static bool creatureVisible(const CreatureInstance& ci, const Frustum& fr, const W3dCreatureHeader** hOut, float* sOut) {
    const CreatureDisplay* d = ci.disp;
    if (!d || !d->ready()) return false;
    const W3dCreatureHeader* h = d->model->h;
    const float s = ci.scale * (d->d.scale > 0.01f ? d->d.scale : 1.0f);
    float c[3] = {ci.pos[0], ci.pos[1], ci.pos[2] + 0.5f * (h->bmin[2] + h->bmax[2]) * s};
    if (!fr.sphere(c, h->radius * s)) return false;
    *hOut = h;
    *sOut = s;
    return true;
}

// Stage 6.12: the far units (kCreatureFullAnimRange) share poses: one per model, sequence and loop flag a frame, at a common
// clock that steps 10 times a second - a herd far away moves in step, which nobody sees at that size.
static uint32_t g_nowMs6 = 0;   // (set by rendererPrepare)
void prepareCreatures(const Frustum& fr, const std::vector<CreatureInstance>& list, FrameCounters& fc) {
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
        if (!creatureVisible(ci, fr, &h, &s)) continue;
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
    const float sharedMs = (float)(g_nowMs6 / 100u * 100u);
    for (const Shared& x : shared) w3d_anim_pose(x.blob, x.seq, sharedMs, x.loop ? 1 : 0, g_posePool.data() + x.off);
    for (size_t i = 0; i < list.size(); ++i) {
        if (g_poseOff[i] < 0) continue;
        const CreatureInstance& ci = list[i];
        const W3dCreatureHeader* h = ci.disp->model->h;
        const float s = ci.scale * (ci.disp->d.scale > 0.01f ? ci.disp->d.scale : 1.0f);
        W3dMat34* P = g_posePool.data() + g_poseOff[i];
        if (!isFar[i]) w3d_anim_pose(ci.disp->model->blob, ci.seq, ci.timeMs, ci.loop ? 1 : 0, P);
        if (ci.guid) recordUnitPoints(ci, h, ci.disp->model->blob, s, P);
    }
    fc.poseMs += ticksToMs(svcGetSystemTick() - t0);
}

void drawCreatures(const Camera& cam, const C3D_Mtx& view, const Frustum& fr, const RenderSettings& rs,
                   const std::vector<CreatureInstance>& list, FrameCounters& fc) {
    if (list.empty()) return;
    const bool posed = g_posedList == &list && g_poseOff.size() == list.size();
    g_worn.clear();
    C3D_BindProgram(&g_skinSh.prog);
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
    C3D_TexEnv* tintEnv = C3D_GetTexEnv(1);
    C3D_TexEnvInit(tintEnv);
    C3D_TexEnvSrc(tintEnv, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(tintEnv, C3D_Both, GPU_MODULATE);
    C3D_TexEnvColor(tintEnv, 0xFFFFFFFFu);
    u32 tintNow = 0xFFFFFFFFu;
    bool tintAlphaConst = false;
    fogStage(2);
    resetStages(3);
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
            s = ci.scale * (d->d.scale > 0.01f ? d->d.scale : 1.0f);
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
            wd.model = wi.model;
            wd.light[0] = lightObj[0]; wd.light[1] = lightObj[1]; wd.light[2] = lightObj[2];
            g_worn.push_back(wd);
        }

        const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(blob + h->offSubmeshes);
        const W3dBonePalette* pal = (const W3dBonePalette*)(blob + h->offPalettes);
        const W3dCreatureTex* ct = (const W3dCreatureTex*)(blob + h->offTex);
        const uint16_t* idx = (const uint16_t*)(blob + h->offIndices);
        int curPal = -1;
        for (u32 i = 0; i < h->submeshCount; ++i) {
            const W3dSkinSubmesh& m = sm[i];
            if (d->d.geosetCount) {
                bool on = false;
                for (int k = 0; k < d->d.geosetCount && !on; ++k) on = d->d.geosets[k] == m.geoset;
                if (!on) continue;
            }
            if (m.palette != curPal && m.palette < h->paletteCount) {
                const W3dBonePalette& p = pal[m.palette];
                C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_skinSh.uBones, p.count * 3);
                for (int b = 0; b < p.count; ++b) {
                    const float* M = P[p.bones[b] < h->boneCount ? p.bones[b] : 0].m;
                    for (int r = 0; r < 3; ++r) {
                        C3D_FVec& v = u[b * 3 + r];
                        v.x = M[r * 4]; v.y = M[r * 4 + 1]; v.z = M[r * 4 + 2]; v.w = M[r * 4 + 3];
                    }
                }
                curPal = m.palette;
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
            const u32 want = fading ? (0x00FFFFFFu | (u32)(std::max(0.0f, ci.fade) * 255.0f + 0.5f) << 24) : 0xFFFFFFFFu;
            if (want != tintNow) { C3D_TexEnvColor(tintEnv, want); C3D_DirtyTexEnv(tintEnv); tintNow = want; }
            bc.bind(0, m.texSlot == W3D_NO_TEXTURE ? streamerWhiteTexture() : creatureTex(d, h, ct, m.texSlot), fc);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uLightParams, 0.55f, 0.6f, 0, (m.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f);
            uint32_t first = m.firstIndex, count = m.indexCount;
            const W3dSubLod* lods = d->model->lods;
            const int lod = posed ? g_unitLod[ci_i] : 0;
            if (lod && lods)   // stage 6.12: the lightest level there is, up to the one asked for
                for (int k = lod; k >= 1; --k)
                    if (lods[i].indexCount[k - 1]) { first = lods[i].firstIndex[k - 1]; count = lods[i].indexCount[k - 1]; break; }
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
    C3D_BindProgram(&g_terrainSh.prog);
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
    fogStage(1);
    resetStages(2);
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
                s.scale = ci.scale * (d->d.scale > 0.01f ? d->d.scale : 1.0f);
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

    C3D_BindProgram(&g_fxSh.prog);
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
    if (g_skyV) linearFree(g_skyV);
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
static void collectAll(const Frustum& fr, const RenderSettings& rs, FrameCounters& fc, const std::vector<SceneModel>* effects) {
    collectModels(fr, rs, fc);   // doodads (if enabled) and buildings
    g_effInst.clear();
    if (effects && rs.effects > 0) {
        g_effInst.reserve(effects->size());
        for (const SceneModel& sm : *effects) {
            Model* m = sm.model;
            if (!sm.emit || !m || !m->drawable || !m->h) continue;
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
    if (creatures && rs.creatures && !creatures->empty()) prepareCreatures(fr, *creatures, fc);
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
    if (rs.underwater) {
        const float end = std::max(rs.waterFogEnd, 6.0f);
        g_fogX = 1.0f / end;   // visibility = 1 - dist / end: from the eye
        g_fogY = 1.0f;
        g_fogColor = fogConstColor(rs.waterFog);
    } else {
        g_fogColor = fogConstColor(kHorizon);
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
    if (!rs.underwater) drawSky(view, fc);
    pass(FrameCounters::kSky);
    drawTerrain(cam, view, fr, rs, fc);
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
    drawWater(cam, view, fr, rs, fc);
    pass(FrameCounters::kWater);
    drawModels(cam, view, rs, true, fc);
    pass(FrameCounters::kAlpha);
    perf::mark("draw: particles");
    drawFx(cam, view, rs, fc, effects);
    pass(FrameCounters::kFx);
    // Drop per-frame instance pointers: tiles/models may be freed before the next frame (prepared: rendererEndFrame does,
    // after the last eye).
    if (!g_prepared) clearVisible();
    g_fogX = keepX;
    g_fogY = keepY;
}

void rendererDrawScene(const Camera& cam, float vfovDeg, float nearZ, float farZ, const std::vector<SceneModel>& models,
                       const std::vector<CreatureInstance>* creatures, FrameCounters& fc) {
    const C3D_Mtx keepProj = g_proj;
    const float keepNear = g_near, keepFar = g_far;
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
    const float s = ci.scale * (d->d.scale > 0.01f ? d->d.scale : 1.0f);
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

u32 rendererClearColor(const RenderSettings& rs) {   // RGBA8 for C3D_RenderTargetClear
    if (!rs.underwater) return 0x8CB8E8FF;
    return ((u32)rs.waterFog[0] << 24) | ((u32)rs.waterFog[1] << 16) | ((u32)rs.waterFog[2] << 8) | 0xFF;
}

