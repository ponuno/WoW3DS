#include "ui_render.hpp"

#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <string>
#include <vector>

#include "ui_shbin.h"

namespace {

struct UiVertex {
    int16_t x, y;
    int16_t u, v;
    uint8_t r, g, b, a;
};   // 12 bytes

constexpr int kMaxQuads = 8000;              // per frame, both screens
// Icon and map pages resident at once (skin and fonts are extra). 5.35i: the touch screen is drawn only when it changes
// (~4 times a second), so its pages looked unused between two of its draws and were evicted and read back from the SD
// card at every redraw (the spell book, the bags and the map show icons of many pages): a stall several times a second.
// Now a page goes only when it was not drawn for kPageIdleMs, or the hard limit is passed.
constexpr uint32_t kPageBudget = 4 * 1024 * 1024, kPageHardLimit = 8 * 1024 * 1024;
constexpr uint64_t kPageIdleMs = 4000;

struct Tex {
    C3D_Tex t;
    bool ok = false, failed = false;
    bool pending = false;           // stage 6.2: an icon / map page being read by the loader thread
    uint8_t kind = 0;
    int w = 0, h = 0;
    uint32_t bytes = 0, lastFrame = 0;
    uint64_t lastMs = 0;
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
Tex g_ext[ui::kExternalCount];  // UI stage 8: run-time textures (portraits), not owned here
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

// Makes the GPU texture of a pack texture from its TEX blob (linear memory: a memcpy, no GPU wait).
bool makeTex(Tex& t, uint16_t id, const std::vector<uint8_t>& blob) {
    const ui::Assets& a = ui::assets();
    int w = 0, h = 0;
    uint8_t fmt = 0, kind = 0;
    if (blob.size() < W3D_TEX_DATA_OFFSET || !a.texInfo(id, &w, &h, &fmt, &kind)) { t.failed = true; ++g_stats.failed; return false; }
    W3dTexHeader th;
    memcpy(&th, blob.data(), sizeof th);
    if (!C3D_TexInit(&t.t, (u16)w, (u16)h, gpuFormat(fmt))) { t.failed = true; ++g_stats.failed; return false; }
    const uint32_t n = std::min<uint32_t>({th.dataSize, t.t.size, (uint32_t)(blob.size() - W3D_TEX_DATA_OFFSET)});
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
    g_stats.uploadBytes += n;
    ++g_stats.resident;
    ++g_stats.loads;
    return true;
}

// ------------------------------------------------------------------ stage 6.2: the page loader thread
// Icon and map pages were read from the SD card on the main thread when first drawn: 110-390 ms frozen when a tab with
// icons opened (perf log, every world hitch of the second run). Now they are read by a thread; the page shows a frame or
// two later (its quads are skipped meanwhile) and uiPagesArrived() tells the main loop to redraw the touch screen.
// ui::Assets::readTexture opens its own FILE: the thread shares nothing with the main thread but the queues.
struct Loaded { uint16_t id; uint32_t gen; bool ok; double ms; std::vector<uint8_t> blob; };
Thread g_loader = nullptr;
LightLock g_lLock;
LightEvent g_lWake;
std::atomic<bool> g_lQuit{false};
std::deque<std::pair<uint16_t, uint32_t>> g_lReq;   // (id, generation), guarded
std::vector<Loaded> g_lDone;                          // guarded
std::deque<std::pair<std::string, std::string>> g_lWrites;   // stage 6.5: (path, contents) to write, guarded

bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}
uint32_t g_gen = 1;                                   // bumped when the textures are dropped (uiReloadTextures)
bool g_arrived = false;

void loaderMain(void*) {
    while (!g_lQuit) {
        LightEvent_WaitTimeout(&g_lWake, 200 * 1000000LL);
        for (;;) {   // files to write first (small, the last of a path wins: written in order)
            LightLock_Lock(&g_lLock);
            if (g_lWrites.empty()) { LightLock_Unlock(&g_lLock); break; }
            auto w = std::move(g_lWrites.front());
            g_lWrites.pop_front();
            LightLock_Unlock(&g_lLock);
            writeFile(w.first, w.second);
        }
        for (;;) {
            LightLock_Lock(&g_lLock);
            if (g_lReq.empty() || g_lQuit) { LightLock_Unlock(&g_lLock); break; }
            const auto r = g_lReq.front();
            g_lReq.pop_front();
            LightLock_Unlock(&g_lLock);
            Loaded l{r.first, r.second, false, 0.0, {}};
            const u64 t0 = svcGetSystemTick();
            l.ok = ui::assets().readTexture(r.first, l.blob);
            l.ms = (double)(svcGetSystemTick() - t0) * 1000.0 / (double)SYSCLOCK_ARM11;
            LightLock_Lock(&g_lLock);
            g_lDone.push_back(std::move(l));
            LightLock_Unlock(&g_lLock);
        }
    }
}

void loaderStart() {
    LightLock_Init(&g_lLock);
    LightEvent_Init(&g_lWake, RESET_ONESHOT);
    g_lQuit = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    const int p = prio + 1 > 0x3F ? 0x3F : prio + 1;   // below the main thread
    if (n3ds) g_loader = threadCreate(loaderMain, nullptr, 16 * 1024, p, 2, false);
    if (!g_loader) g_loader = threadCreate(loaderMain, nullptr, 16 * 1024, p, 1, false);
    if (!g_loader) g_loader = threadCreate(loaderMain, nullptr, 16 * 1024, p, 0, false);
}

void loaderStop() {
    if (!g_loader) return;
    g_lQuit = true;
    LightEvent_Signal(&g_lWake);
    threadJoin(g_loader, U64_MAX);
    threadFree(g_loader);
    g_loader = nullptr;
    for (auto& w : g_lWrites) writeFile(w.first, w.second);   // what was still waiting (quitting right after a change)
    g_lWrites.clear();
    g_lReq.clear();
    g_lDone.clear();
}

// The pages the thread has read: made into textures here, on the main thread (between frames).
void loaderCollect() {
    if (!g_loader) return;
    static std::vector<Loaded> done;
    LightLock_Lock(&g_lLock);
    done.swap(g_lDone);
    LightLock_Unlock(&g_lLock);
    for (Loaded& l : done) {
        g_stats.loadMs += l.ms;   // (the thread's time)
        if (l.gen != g_gen) continue;   // dropped meanwhile (uiReloadTextures)
        const size_t i = l.id - ui::kTexPack0;
        if (i >= g_tex.size()) continue;
        Tex& t = g_tex[i];
        t.pending = false;
        if (t.ok) continue;
        if (!l.ok) { t.failed = true; ++g_stats.failed; continue; }
        if (makeTex(t, l.id, l.blob)) g_arrived = true;
    }
    done.clear();
    g_stats.queued = (uint32_t)std::count_if(g_tex.begin(), g_tex.end(), [](const Tex& t) { return t.pending; });
}

// The texture of an id, uploaded on first use (nullptr: none / failed / an icon or map page still being read).
Tex* resolve(uint16_t id) {
    if (id == ui::kTexBuiltinFont) return g_font.ok ? &g_font : nullptr;
    if (ui::isExternalTex(id)) { Tex& e = g_ext[id - ui::kTexExternal0]; return e.ok ? &e : nullptr; }
    const ui::Assets& a = ui::assets();
    if (id < ui::kTexPack0 || !a.loaded()) return nullptr;
    size_t i = id - ui::kTexPack0;
    if (i >= a.packTextures()) return nullptr;
    if (g_tex.size() < a.packTextures()) g_tex.resize(a.packTextures());
    Tex& t = g_tex[i];
    t.lastFrame = g_frame;
    t.lastMs = osGetTime();
    if (t.ok) return &t;
    if (t.failed || t.pending) return nullptr;
    int w = 0, h = 0;
    uint8_t fmt = 0, kind = 0;
    if (!a.texInfo(id, &w, &h, &fmt, &kind)) { t.failed = true; ++g_stats.failed; return nullptr; }
    if (g_loader && (kind == W3D_UITEX_ICONS || kind == W3D_UITEX_MAP)) {   // pages: by the loader thread
        t.pending = true;
        LightLock_Lock(&g_lLock);
        g_lReq.emplace_back(id, g_gen);
        LightLock_Unlock(&g_lLock);
        LightEvent_Signal(&g_lWake);
        ++g_stats.asyncLoads;
        return nullptr;
    }
    // the skin and the fonts (wanted at once, read at the start): here
    const u64 l0 = svcGetSystemTick();
    std::vector<uint8_t> blob;
    const bool read = a.readTexture(id, blob);
    const double ms = (double)(svcGetSystemTick() - l0) * 1000.0 / (double)SYSCLOCK_ARM11;
    g_stats.loadMs += ms;
    g_stats.syncLoadMs += ms;
    ++g_stats.syncLoads;
    if (!read) { t.failed = true; ++g_stats.failed; return nullptr; }
    if (!makeTex(t, id, blob)) return nullptr;
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
    if (g_v) g_stats.vertexBytes = (uint32_t)((size_t)kMaxQuads * 6 * sizeof(UiVertex));
    initBuiltinFont();
    if (g_font.ok) g_stats.fontBytes = g_font.t.size;
    loaderStart();
}

void uiRenderShutdown() {
    loaderStop();
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
    if (g_loader) {   // reads in flight belong to the old textures
        LightLock_Lock(&g_lLock);
        g_lReq.clear();
        LightLock_Unlock(&g_lLock);
    }
    ++g_gen;
    for (Tex& t : g_tex) freeTex(t);
    g_tex.clear();
}

void uiWriteFileAsync(const char* path, const std::string& data) {
    if (!g_loader) { writeFile(path, data); return; }
    LightLock_Lock(&g_lLock);
    g_lWrites.emplace_back(path, data);
    LightLock_Unlock(&g_lLock);
    LightEvent_Signal(&g_lWake);
}

bool uiPagesArrived() {
    const bool a = g_arrived;
    g_arrived = false;
    return a;
}

const UiTexStats& uiTexStats() { return g_stats; }

void uiSetExternalTexture(int n, C3D_Tex* tex, int w, int h) {
    if (n < 0 || n >= (int)ui::kExternalCount) return;
    Tex& e = g_ext[n];
    e = Tex();
    if (!tex) return;
    e.t = *tex;   // the descriptor (the owner keeps the memory)
    e.ok = true;
    e.w = w;
    e.h = h;
    e.kind = 0xFF;   // never evicted
}

void uiDrawOrientationTest() {
    if (!g_v || g_used + 2 > kMaxQuads) return;
    // two quads straight in clip space (an orthographic projection that maps -1..1 to itself, not tilted)
    UiVertex* o = g_v + (size_t)g_used * 6;
    auto quad = [&](UiVertex* q, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g) {
        q[0] = {(int16_t)x0, (int16_t)y0, 0, 0, r, g, 0, 0};
        q[1] = {(int16_t)x1, (int16_t)y0, 0, 0, r, g, 0, 0};
        q[2] = {(int16_t)x0, (int16_t)y1, 0, 0, r, g, 0, 0};
        q[3] = {(int16_t)x1, (int16_t)y0, 0, 0, r, g, 0, 0};
        q[4] = {(int16_t)x1, (int16_t)y1, 0, 0, r, g, 0, 0};
        q[5] = {(int16_t)x0, (int16_t)y1, 0, 0, r, g, 0, 0};
    };
    // levels that tell the two apart in whatever byte order the colour buffer has: 200 = top, 100 = left
    quad(o, -1, 0, 1, 1, 200, 0);       // top half (y > 0): red 200
    quad(o + 6, -1, -1, 0, 1, 0, 100);  // left half (x < 0): green 100, added to the red
    const int first = g_used;
    g_used += 2;
    GSPGPU_FlushDataCache(o, 12 * sizeof(UiVertex));
    C3D_BindProgram(&g_prog);
    C3D_SetAttrInfo(&g_attr);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_v, sizeof(UiVertex), 3, 0x210);
    C3D_Mtx proj;
    Mtx_Ortho(&proj, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, false);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uProj, &proj);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    for (int i = 1; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    setMode(kHudFlat);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);   // the colours add up
    C3D_DrawArrays(GPU_TRIANGLES, first * 6, 12);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

void uiEndFrame() {
    ++g_frame;
    g_used = 0;
    loaderCollect();
    if (g_stats.bytes <= kPageBudget) return;
    // least recently drawn icon / map pages first; never one drawn in the last two frames (the GPU may still read it);
    // under the hard limit only pages idle for kPageIdleMs
    const uint64_t now = osGetTime();
    const bool hard = g_stats.bytes > kPageHardLimit;
    std::vector<Tex*> cand;
    for (Tex& t : g_tex)
        if (t.ok && (t.kind == W3D_UITEX_ICONS || t.kind == W3D_UITEX_MAP) && g_frame - t.lastFrame > 2 &&
            (hard || now - t.lastMs > kPageIdleMs))
            cand.push_back(&t);
    std::sort(cand.begin(), cand.end(), [](const Tex* a, const Tex* b) { return a->lastMs < b->lastMs; });
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
        if (g_used >= kMaxQuads) { g_stats.overflow += (uint32_t)(list.q.size() - (size_t)(&q - list.q.data())); break; }
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
    g_stats.quads += (uint32_t)(g_used - start);
    g_stats.batches += (uint32_t)batches.size();
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
