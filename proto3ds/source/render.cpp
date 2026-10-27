// render.cpp — sky, terrain, doodads and water for all streamed tiles.
//
// Everything is drawn camera-relative: the CPU builds per-object modelView
// matrices in float32 from (object - camera), because PICA200 uniforms are
// float24 and absolute coordinates (~17000 yd) would visibly jitter.
#include "world.h"

#include <array>
#include <vector>
#include <math.h>
#include <string.h>

#include <algorithm>

#include "w3d_anim.h"

#include "model_shbin.h"
#include "skin_shbin.h"
#include "terrain_shbin.h"

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
};
Shader g_terrainSh, g_modelSh, g_skinSh;
C3D_AttrInfo g_terrainAttr, g_modelAttr, g_skinAttr;
std::vector<W3dMat34> g_pose;   // scratch: bone matrices of the creature being drawn
C3D_Mtx g_proj;
float g_near = 1.0f, g_far = 400.0f, g_skyRadius = 300.0f;
float g_fogX = 0.0f, g_fogY = 1.0f;   // visibility = clamp(g_fogY - dist * g_fogX, 0, 1)
// Terrain LOD switch distances (yards from the camera to a batch's bounding sphere).
constexpr float kLod1Dist = 40.0f, kLod2Dist = 140.0f;

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

// ------------------------------------------------------------------ frustum
struct Frustum {
    float cam[3];
    float n[6][3];
    float d[6];
    float maxDist;
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
    GpuTex* unit[2] = {nullptr, nullptr};
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

    // stage 0: rgb = lerp(tex0, tex1, vertex.a); stage 1: rgb *= vertex.rgb (baked light)
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE0, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFFFFFFFF);
    env = C3D_GetTexEnv(1);   // rgb *= baked light (vertex colour red)
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    fogStage(2);
    resetStages(3);
    setFogUniform(g_terrainSh, rs.fog, true);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_BACK_CCW);   // verified on hardware (stage 2)

    BindCache bc;
    for (Tile* t : streamerReadyTiles()) {
        const W3dTerrainHeader* h = t->th;
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, t->terr + h->offVertices, sizeof(W3dTerrainVertex), 3, 0x210);
        C3D_Mtx mv;
        camRelative(&mv, view, h->origin, cam.pos);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_terrainSh.uMV, &mv);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uScale, h->posScale[0], h->posScale[1], h->posScale[2], W3D_TERRAIN_UV_SCALE);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_terrainSh.uOffset, h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
        const uint16_t* idx = (const uint16_t*)(t->terr + h->offIndices);
        for (u32 i = 0; i < h->batchCount; ++i) {
            const W3dTerrainBatch& b = t->batches[i];
            float c[3], r2 = 0;
            for (int k = 0; k < 3; ++k) {
                c[k] = h->origin[k] + 0.5f * (b.bmin[k] + b.bmax[k]);
                float e = 0.5f * (b.bmax[k] - b.bmin[k]);
                r2 += e * e;
            }
            float rad = sqrtf(r2);
            if (!fr.sphere(c, rad)) continue;
            // LOD by distance from the camera to the batch's bounding sphere.
            float dx = c[0] - cam.pos[0], dy = c[1] - cam.pos[1], dz = c[2] - cam.pos[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz) - rad;
            int lod = d < kLod1Dist ? 0 : d < kLod2Dist ? 1 : 2;
            if (rs.forceLod >= 0) lod = rs.forceLod;
            bc.bind(0, t->ttex[b.tex0], fc);
            bc.bind(1, t->ttex[b.tex1], fc);
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

void collectModels(const Frustum& fr, const RenderSettings& rs, FrameCounters& fc) {
    for (Tile* t : streamerReadyTiles())
        for (const Instance& in : t->instances) {
            Model* m = in.model;
            if (!m->h || in.radius <= 0) continue;
            if (!rs.doodads && !m->building) continue;
            // Small doodads fade out long before the draw distance (bushes ~110 yd,
            // big trees reach the fog); this is what keeps draw calls in budget.
            // Buildings are kept to the draw distance.
            // Interior-only groups (rooms, Stormwind's halls and cellars; no portals yet) are drawn only
            // when the camera is near them: from outside they are hidden behind the exterior anyway.
            float maxD = m->interior ? in.radius + 25.0f
                       : m->building ? fr.maxDist + in.radius : 40.0f + rs.doodadRange * in.radius;
            float dx = in.pos[0] - fr.cam[0], dy = in.pos[1] - fr.cam[1], dz = in.pos[2] - fr.cam[2];
            if (dx * dx + dy * dy + dz * dz > maxD * maxD) continue;
            if (!fr.sphere(in.pos, in.radius)) continue;
            if (m->visible.empty()) g_activeModels.push_back(m);
            m->visible.push_back(&in);
            if (m->building) ++fc.buildings;
            else ++fc.visibleInstances;
        }
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
    fogStage(1);
    resetStages(2);
    setFogUniform(g_modelSh, rs.fog, true);

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
            if (isBlended != blendedPass) continue;
            setModelBlend(sm.blend, sm.flags & W3D_SUBF_TWO_SIDED, rs.modelsNoCull);
            bc.bind(0, sm.tex == W3D_NO_TEXTURE ? streamerWhiteTexture() : m->tex[sm.tex], fc);
            float unlit = (sm.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f;
            for (const Instance* in : m->visible) {
                C3D_Mtx mv;
                camRelative(&mv, view, in->pos, cam.pos);
                Mtx_RotateZ(&mv, in->rot[2], true);
                Mtx_RotateY(&mv, in->rot[1], true);
                Mtx_RotateX(&mv, in->rot[0], true);
                Mtx_Scale(&mv, in->scale, in->scale, in->scale);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_modelSh.uMV, &mv);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLight, in->lightModel[0], in->lightModel[1], in->lightModel[2], 0);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, g_modelSh.uLightParams, 0.55f, 0.6f, 0, unlit);
                C3D_DrawElements(GPU_TRIANGLES, sm.indexCount, C3D_UNSIGNED_SHORT, idx + sm.firstIndex);
                ++fc.drawCalls;
                fc.tris += sm.indexCount / 3;
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
    for (const Marker& m : markers) {
        const bool ring = m.shape == 1 && g_ringV;
        float c[3] = {m.pos[0], m.pos[1], m.pos[2] + m.height * 0.5f};
        if (!fr.sphere(c, ring ? m.width * 2.0f : m.height)) continue;
        if (boundShape != (ring ? 1 : 0)) {   // the ring has its own vertex buffer (x/y scaled by 2 / kMarkerQ)
            boundShape = ring ? 1 : 0;
            C3D_BufInfo* b2 = C3D_GetBufInfo();
            BufInfo_Init(b2);
            BufInfo_Add(b2, ring ? (void*)g_ringV : (void*)g_markerV, sizeof(W3dModelVertex), 3, 0x210);
            const float qq = ring ? 2.0f / kMarkerQ : 1.0f / kMarkerQ;
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
        const int nv = ring ? g_ringVerts : g_markerVerts;
        C3D_DrawArrays(GPU_TRIANGLES, 0, nv);
        ++fc.drawCalls;
        ++fc.markers;
        fc.tris += nv / 3;
    }
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

void drawCreatures(const Camera& cam, const C3D_Mtx& view, const Frustum& fr, const RenderSettings& rs,
                   const std::vector<CreatureInstance>& list, FrameCounters& fc) {
    if (list.empty()) return;
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
    fogStage(1);
    resetStages(2);
    setFogUniform(g_skinSh, rs.fog, true);
    BindCache bc;
    for (const CreatureInstance& ci : list) {
        const CreatureDisplay* d = ci.disp;
        if (!d || !d->ready()) continue;
        const uint8_t* blob = d->model->blob;
        const W3dCreatureHeader* h = d->model->h;
        const float s = ci.scale * (d->d.scale > 0.01f ? d->d.scale : 1.0f);
        float c[3] = {ci.pos[0], ci.pos[1], ci.pos[2] + 0.5f * (h->bmin[2] + h->bmax[2]) * s};
        if (!fr.sphere(c, h->radius * s)) continue;
        u64 t0 = svcGetSystemTick();
        if (g_pose.size() < h->boneCount) g_pose.resize(h->boneCount);
        w3d_anim_pose(blob, ci.seq, ci.timeMs, ci.loop ? 1 : 0, g_pose.data());
        fc.poseMs += ticksToMs(svcGetSystemTick() - t0);
        ++fc.creatures;

        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, blob + h->offVertices, sizeof(W3dSkinVertex), 4, 0x3210);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uScale, h->posScale[0], h->posScale[1], h->posScale[2], W3D_MODEL_UV_SCALE);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uOffset, h->posOffset[0], h->posOffset[1], h->posOffset[2], 0);
        C3D_Mtx mv;
        camRelative(&mv, view, ci.pos, cam.pos);
        Mtx_RotateZ(&mv, ci.yaw, true);
        Mtx_Scale(&mv, s, s, s);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_skinSh.uMV, &mv);
        float cy = cosf(-ci.yaw), sy = sinf(-ci.yaw);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uLight, kSun[0] * cy - kSun[1] * sy, kSun[0] * sy + kSun[1] * cy, kSun[2], 0);
        for (int w = 0; w < ci.wornCount; ++w) {   // item models: queued with their own matrix, drawn after the creatures
            const CreatureInstance::Worn& wi = ci.worn[w];
            if (!wi.model || !wi.model->h || wi.model->state != AssetState::Ready || !wi.model->texturesReady()) continue;
            if (wi.bone < 0 || (u32)wi.bone >= h->boneCount) continue;
            const float* M = g_pose[wi.bone].m;   // 3x4 row-major, bind pose -> animated pose
            C3D_Mtx bone;
            for (int r = 0; r < 3; ++r) { bone.r[r].x = M[r * 4]; bone.r[r].y = M[r * 4 + 1]; bone.r[r].z = M[r * 4 + 2]; bone.r[r].w = M[r * 4 + 3]; }
            bone.r[3].x = 0; bone.r[3].y = 0; bone.r[3].z = 0; bone.r[3].w = 1;
            WornDraw wd;
            Mtx_Multiply(&wd.mv, &mv, &bone);
            Mtx_Translate(&wd.mv, wi.pos[0], wi.pos[1], wi.pos[2], true);
            wd.model = wi.model;
            wd.light[0] = kSun[0] * cy - kSun[1] * sy; wd.light[1] = kSun[0] * sy + kSun[1] * cy; wd.light[2] = kSun[2];
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
                    const float* M = g_pose[p.bones[b] < h->boneCount ? p.bones[b] : 0].m;
                    for (int r = 0; r < 3; ++r) {
                        C3D_FVec& v = u[b * 3 + r];
                        v.x = M[r * 4]; v.y = M[r * 4 + 1]; v.z = M[r * 4 + 2]; v.w = M[r * 4 + 3];
                    }
                }
                curPal = m.palette;
                ++fc.boneUploads;
            }
            setModelBlend(m.blend, m.flags & W3D_SUBF_TWO_SIDED, rs.modelsNoCull);
            bc.bind(0, m.texSlot == W3D_NO_TEXTURE ? streamerWhiteTexture() : creatureTex(d, h, ct, m.texSlot), fc);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_skinSh.uLightParams, 0.55f, 0.6f, 0, (m.flags & W3D_SUBF_UNLIT) ? 1.0f : 0.0f);
            C3D_DrawElements(GPU_TRIANGLES, m.indexCount, C3D_UNSIGNED_SHORT, idx + m.firstIndex);
            ++fc.drawCalls;
            fc.tris += m.indexCount / 3;
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
        if (!h) continue;
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
            if (!fr.sphere(c, sqrtf(r2))) continue;
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
    g_pose.reserve(256);
    buildSky();
    buildMarker();
    buildRing();
    rendererSetProjection(250.0f);
}

void rendererShutdown() {
    shaderProgramFree(&g_terrainSh.prog);
    shaderProgramFree(&g_modelSh.prog);
    shaderProgramFree(&g_skinSh.prog);
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
    g_far = drawDist * 1.15f + 30.0f;
    g_skyRadius = g_far * 0.9f;
    buildProj();

    // Linear fog by true view distance, from 55% to 100% of the draw distance.
    const float start = drawDist * 0.55f, end = drawDist;
    g_fogX = 1.0f / (end - start);
    g_fogY = end / (end - start);
}

void rendererDraw(C3D_RenderTarget* target, const Camera& cam, const RenderSettings& rsIn, FrameCounters& fc,
                  const std::vector<Marker>* markers, const std::vector<CreatureInstance>* creatures) {
    (void)target;
    RenderSettings rs = rsIn;
    if (rs.underwater) rs.fog = true;   // the water is never clear: the fog switch only concerns the open air
    float f[3];
    cam.forward(f);
    C3D_Mtx view;
    Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(f[0], f[1], f[2]), FVec3_New(0, 0, 1), false);
    Frustum fr;
    fr.build(cam, g_near);
    // Under the surface (stage 5.26): the water's colour all around, sight ends at its fog distance (Light.dbc underwater
    // parameters): no sky, and nothing farther than that is drawn at all.
    const float keepX = g_fogX, keepY = g_fogY;
    if (rs.underwater) {
        const float end = std::max(rs.waterFogEnd, 6.0f);
        g_fogX = 1.0f / end;   // visibility = 1 - dist / end: from the eye
        g_fogY = 1.0f;
        g_fogColor = fogConstColor(rs.waterFog);
        fr.maxDist = std::min(fr.maxDist, end + 8.0f);
        fr.d[5] = fr.maxDist;
    } else {
        g_fogColor = fogConstColor(kHorizon);
    }

    if (!rs.underwater) drawSky(view, fc);
    drawTerrain(cam, view, fr, rs, fc);
    collectModels(fr, rs, fc);   // doodads (if enabled) and buildings
    drawModels(cam, view, rs, false, fc);
    if (markers) drawMarkers(cam, view, fr, rs, *markers, fc);
    if (creatures && rs.creatures) drawCreatures(cam, view, fr, rs, *creatures, fc);
    drawWater(cam, view, fr, rs, fc);
    drawModels(cam, view, rs, true, fc);
    // Drop per-frame instance pointers: tiles/models may be freed before the next frame.
    for (Model* m : g_activeModels) m->visible.clear();
    g_activeModels.clear();
    g_fogX = keepX;
    g_fogY = keepY;
}

u32 rendererClearColor(const RenderSettings& rs) {   // RGBA8 for C3D_RenderTargetClear
    if (!rs.underwater) return 0x8CB8E8FF;
    return ((u32)rs.waterFog[0] << 24) | ((u32)rs.waterFog[1] << 16) | ((u32)rs.waterFog[2] << 8) | 0xFF;
}

