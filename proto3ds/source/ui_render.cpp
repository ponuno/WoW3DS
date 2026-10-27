#include "ui_render.hpp"

#include <string.h>

#include <algorithm>
#include <vector>

#include "ui_shbin.h"

namespace {

struct UiVertex {
    int16_t x, y;
    int16_t u, v;
    uint8_t r, g, b, a;
};   // 12 bytes

constexpr int kMaxQuads = 8000;              // per frame, both screens
constexpr uint32_t kPageBudget = 1600 * 1024; // icon and map pages resident at once (skin and fonts are extra)

struct Tex {
    C3D_Tex t;
    bool ok = false, failed = false;
    uint8_t kind = 0;
    int w = 0, h = 0;
    uint32_t bytes = 0, lastFrame = 0;
};

DVLB_s* g_dvlb = nullptr;
shaderProgram_s g_prog;
int g_uProj = -1, g_uScale = -1, g_uOffset = -1;
C3D_AttrInfo g_attr;
UiVertex* g_v = nullptr;
int g_used = 0;                 // quads written this frame
uint32_t g_frame = 1;
Tex g_font;                     // the built-in font
std::vector<Tex> g_tex;         // pack textures (index = id - kTexPack0)
UiTexStats g_stats;

inline uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

GPU_TEXCOLOR gpuFormat(uint8_t f) {
    switch (f) {
        case W3D_TEX_ETC1: return GPU_ETC1;
        case W3D_TEX_ETC1A4: return GPU_ETC1A4;
        case W3D_TEX_RGBA4: return GPU_RGBA4;
        case W3D_TEX_RGBA5551: return GPU_RGBA5551;
        case W3D_TEX_A8: return GPU_A8;
        default: return GPU_RGB565;
    }
}

void initBuiltinFont() {
    std::vector<uint8_t> a8;
    int w = 0, h = 0;
    ui::Assets::builtinFontA8(a8, &w, &h);
    if (!C3D_TexInit(&g_font.t, (u16)w, (u16)h, GPU_A8)) return;
    uint8_t* d = (uint8_t*)g_font.t.data;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) d[(size_t)((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)] = a8[(size_t)y * w + x];
    GSPGPU_FlushDataCache(g_font.t.data, (u32)(w * h));
    C3D_TexSetFilter(&g_font.t, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&g_font.t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g_font.ok = true;
    g_font.w = w;
    g_font.h = h;
    g_font.kind = W3D_UITEX_FONT;
}

void freeTex(Tex& t) {
    if (t.ok) {
        C3D_TexDelete(&t.t);
        g_stats.bytes -= t.bytes;
        --g_stats.resident;
    }
    t.ok = false;
}

// The texture of an id, uploaded on first use (nullptr: none / failed).
Tex* resolve(uint16_t id) {
    if (id == ui::kTexBuiltinFont) return g_font.ok ? &g_font : nullptr;
    const ui::Assets& a = ui::assets();
    if (id < ui::kTexPack0 || !a.loaded()) return nullptr;
    size_t i = id - ui::kTexPack0;
    if (i >= a.packTextures()) return nullptr;
    if (g_tex.size() < a.packTextures()) g_tex.resize(a.packTextures());
    Tex& t = g_tex[i];
    t.lastFrame = g_frame;
    if (t.ok) return &t;
    if (t.failed) return nullptr;
    std::vector<uint8_t> blob;
    int w = 0, h = 0;
    uint8_t fmt = 0, kind = 0;
    if (!a.texInfo(id, &w, &h, &fmt, &kind) || !a.readTexture(id, blob)) { t.failed = true; ++g_stats.failed; return nullptr; }
    W3dTexHeader th;
    memcpy(&th, blob.data(), sizeof th);
    if (!C3D_TexInit(&t.t, (u16)w, (u16)h, gpuFormat(fmt))) { t.failed = true; ++g_stats.failed; return nullptr; }
    const uint32_t n = std::min<uint32_t>(th.dataSize, t.t.size);
    memcpy(t.t.data, blob.data() + W3D_TEX_DATA_OFFSET, n);
    GSPGPU_FlushDataCache(t.t.data, n);
    const bool font = kind == W3D_UITEX_FONT;
    C3D_TexSetFilter(&t.t, font ? GPU_NEAREST : GPU_LINEAR, font ? GPU_NEAREST : GPU_LINEAR);
    C3D_TexSetWrap(&t.t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    t.ok = true;
    t.kind = kind;
    t.w = w;
    t.h = h;
    t.bytes = n;
    g_stats.bytes += n;
    ++g_stats.resident;
    ++g_stats.loads;
    return &t;
}

void setMode(uint8_t mode) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (mode == kHudFlat) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    } else if (mode == kHudAlpha) {   // font: colour from the vertex, coverage from the texture
        C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    }
}

}  // namespace

void uiRenderInit() {
    g_dvlb = DVLB_ParseFile((u32*)ui_shbin, ui_shbin_size);
    shaderProgramInit(&g_prog);
    shaderProgramSetVsh(&g_prog, &g_dvlb->DVLE[0]);
    g_uProj = shaderInstanceGetUniformLocation(g_prog.vertexShader, "projection");
    g_uScale = shaderInstanceGetUniformLocation(g_prog.vertexShader, "uvScale");
    g_uOffset = shaderInstanceGetUniformLocation(g_prog.vertexShader, "uvOffset");
    AttrInfo_Init(&g_attr);
    AttrInfo_AddLoader(&g_attr, 0, GPU_SHORT, 2);
    AttrInfo_AddLoader(&g_attr, 1, GPU_SHORT, 2);
    AttrInfo_AddLoader(&g_attr, 2, GPU_UNSIGNED_BYTE, 4);
    g_v = (UiVertex*)linearAlloc((size_t)kMaxQuads * 6 * sizeof(UiVertex));
    initBuiltinFont();
}

void uiRenderShutdown() {
    for (Tex& t : g_tex) freeTex(t);
    g_tex.clear();
    if (g_font.ok) C3D_TexDelete(&g_font.t);
    g_font.ok = false;
    if (g_v) linearFree(g_v);
    g_v = nullptr;
    shaderProgramFree(&g_prog);
    if (g_dvlb) DVLB_Free(g_dvlb);
    g_dvlb = nullptr;
}

void uiReloadTextures() {
    for (Tex& t : g_tex) freeTex(t);
    g_tex.clear();
}

const UiTexStats& uiTexStats() { return g_stats; }

void uiEndFrame() {
    ++g_frame;
    g_used = 0;
    if (g_stats.bytes <= kPageBudget) return;
    // least recently drawn icon / map pages first; never one drawn in the last two frames (the GPU may still read it)
    std::vector<Tex*> cand;
    for (Tex& t : g_tex)
        if (t.ok && (t.kind == W3D_UITEX_ICONS || t.kind == W3D_UITEX_MAP) && g_frame - t.lastFrame > 2) cand.push_back(&t);
    std::sort(cand.begin(), cand.end(), [](const Tex* a, const Tex* b) { return a->lastFrame < b->lastFrame; });
    for (Tex* t : cand) {
        if (g_stats.bytes <= kPageBudget) break;
        freeTex(*t);
        ++g_stats.evictions;
    }
}

void uiDraw(const HudList& list, int width) {
    if (!g_v || list.q.empty()) return;
    // vertices for everything drawable, batches by (mode, texture)
    struct Batch { int first, count; uint8_t mode; Tex* tex; };
    static std::vector<Batch> batches;
    batches.clear();
    const int start = g_used;
    for (const HudQuad& q : list.q) {
        if (g_used >= kMaxQuads) break;
        Tex* t = nullptr;
        if (q.mode != kHudFlat) {
            t = resolve(q.tex);
            if (!t) continue;   // missing picture: nothing rather than garbage
        }
        UiVertex* o = g_v + (size_t)g_used * 6;
        const uint8_t r = q.color & 0xFF, g = (q.color >> 8) & 0xFF, b = (q.color >> 16) & 0xFF, a = q.color >> 24;
        auto vert = [&](UiVertex& v, int x, int y, int u, int vv) { v = {(int16_t)x, (int16_t)y, (int16_t)u, (int16_t)vv, r, g, b, a}; };
        vert(o[0], q.x0, q.y0, q.u0, q.v0);
        vert(o[1], q.x1, q.y0, q.u1, q.v0);
        vert(o[2], q.x0, q.y1, q.u0, q.v1);
        vert(o[3], q.x1, q.y0, q.u1, q.v0);
        vert(o[4], q.x1, q.y1, q.u1, q.v1);
        vert(o[5], q.x0, q.y1, q.u0, q.v1);
        if (!batches.empty() && batches.back().mode == q.mode && (q.mode == kHudFlat || batches.back().tex == t)) {
            ++batches.back().count;
        } else {
            batches.push_back({g_used, 1, q.mode, t});
        }
        ++g_used;
    }
    if (g_used == start) return;
    GSPGPU_FlushDataCache(g_v + (size_t)start * 6, (u32)((g_used - start) * 6 * sizeof(UiVertex)));

    C3D_BindProgram(&g_prog);
    C3D_SetAttrInfo(&g_attr);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_v, sizeof(UiVertex), 3, 0x210);
    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0.0f, (float)width, 240.0f, 0.0f, 1.0f, -1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uProj, &proj);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    for (int i = 1; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    int mode = -1;
    Tex* bound = nullptr;
    for (const Batch& b : batches) {
        if (b.mode != mode) { setMode(b.mode); mode = b.mode; }
        if (b.tex && b.tex != bound) {
            C3D_TexBind(0, &b.tex->t);
            bound = b.tex;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uScale, 1.0f / (float)b.tex->w, -1.0f / (float)b.tex->h, 0, 0);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uOffset, 0, 1.0f, 0, 1.0f);
        }
        C3D_DrawArrays(GPU_TRIANGLES, b.first * 6, b.count * 6);
    }
    // back to what the 3D passes expect
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}
