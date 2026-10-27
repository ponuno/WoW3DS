// PC implementations of the libctru/citro3d calls used by streamer.cpp.
#include "3ds.h"
#include "citro3d.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

struct ThreadImpl { std::thread t; };
Thread threadCreate(ThreadFunc f, void* arg, size_t, int, int core, bool) {
    if (core == 2 || core == 1) return new ThreadImpl{std::thread(f, arg)};   // pretend New 3DS core 2 exists
    return new ThreadImpl{std::thread(f, arg)};
}
Result threadJoin(Thread t, u64) { if (t->t.joinable()) t->t.join(); return 0; }
void threadFree(Thread t) { delete t; }

void LightLock_Init(LightLock* l) { l->m = new std::mutex; }
void LightLock_Lock(LightLock* l) { static_cast<std::mutex*>(l->m)->lock(); }
void LightLock_Unlock(LightLock* l) { static_cast<std::mutex*>(l->m)->unlock(); }
struct Ev { std::mutex m; std::condition_variable cv; bool set = false; };
void LightEvent_Init(LightEvent* e, int) { e->impl = new Ev; }
void LightEvent_Signal(LightEvent* e) { Ev* v = (Ev*)e->impl; { std::lock_guard<std::mutex> g(v->m); v->set = true; } v->cv.notify_one(); }
void LightEvent_Wait(LightEvent* e) { Ev* v = (Ev*)e->impl; std::unique_lock<std::mutex> g(v->m); v->cv.wait(g, [&] { return v->set; }); v->set = false; }

u64 svcGetSystemTick() {
    using namespace std::chrono;
    return (u64)(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() * (SYSCLOCK_ARM11 / 1e9));
}
Result svcGetThreadPriority(s32* out, Handle) { *out = 0x30; return 0; }
Result svcFlushProcessDataCache(Handle, u32, u32) { return 0; }
Result GSPGPU_FlushDataCache(const void*, u32) { return 0; }
Result APT_CheckNew3DS(bool* out) { *out = true; return 0; }
Result APT_SetAppCpuTimeLimit(u32) { return 0; }

// Linear heap with the New 3DS budget (~90 MB) and an allocation ledger.
static std::mutex g_linMx;
static std::map<void*, size_t> g_lin;
static size_t g_linUsed = 0, g_linPeak = 0;
const size_t kLinearBudget = 90u << 20;
void* linearAlloc(size_t size) {
    std::lock_guard<std::mutex> g(g_linMx);
    if (g_linUsed + size > kLinearBudget) return nullptr;
    void* p = std::aligned_alloc(0x80, (size + 0x7F) & ~(size_t)0x7F);
    g_lin[p] = size; g_linUsed += size; g_linPeak = std::max(g_linPeak, g_linUsed);
    return p;
}
void linearFree(void* p) {
    if (!p) return;
    std::lock_guard<std::mutex> g(g_linMx);
    auto it = g_lin.find(p);
    if (it == g_lin.end()) { std::fprintf(stderr, "linearFree of unknown pointer %p\n", p); std::abort(); }
    g_linUsed -= it->second; g_lin.erase(it); std::free(p);
}
u32 linearSpaceFree() { std::lock_guard<std::mutex> g(g_linMx); return (u32)(kLinearBudget - g_linUsed); }
size_t stubLinearUsed() { std::lock_guard<std::mutex> g(g_linMx); return g_linUsed; }
size_t stubLinearPeak() { return g_linPeak; }

// VRAM: 6 MB budget, textures count against it.
static size_t g_vram = 0, g_texLive = 0;
const size_t kVram = 6u << 20;
u32 vramSpaceFree() { return (u32)(kVram - g_vram); }
size_t stubTexturesLive() { return g_texLive; }
static size_t texBytes(const C3D_TexInitParams& p) {
    size_t bpp = p.format == GPU_ETC1 ? 4 : p.format == GPU_ETC1A4 ? 8 : 16, total = 0;
    for (int l = 0; l <= p.maxLevel; ++l) total += (size_t)(p.width >> l) * (p.height >> l) * bpp / 8;
    return total;
}
bool C3D_TexInitWithParams(C3D_Tex* t, void*, C3D_TexInitParams p) {
    if (!p.width || !p.height || (p.width & (p.width - 1)) || (p.height & (p.height - 1)) || p.width < 8 || p.height < 8) {
        std::fprintf(stderr, "bad texture size %ux%u\n", p.width, p.height); std::abort();
    }
    size_t b = texBytes(p);
    if (p.onVram && g_vram + b > kVram) return false;
    t->data = std::malloc(b); t->size = (u32)b; t->width = p.width; t->height = p.height; t->fmt = p.format;
    t->levels = p.maxLevel + 1; t->onVram = p.onVram;
    if (p.onVram) g_vram += b;
    ++g_texLive;
    return true;
}
bool C3D_TexInit(C3D_Tex* t, u16 w, u16 h, GPU_TEXCOLOR f) { C3D_TexInitParams p{w, h, 0, f, GPU_TEX_2D, false}; return C3D_TexInitWithParams(t, nullptr, p); }
void C3D_TexLoadImage(C3D_Tex* t, const void* data, GPU_TEXFACE, int level) {
    if (level < 0 || level >= t->levels || !data) { std::fprintf(stderr, "bad mip level %d\n", level); std::abort(); }
    size_t bpp = t->fmt == GPU_ETC1 ? 4 : t->fmt == GPU_ETC1A4 ? 8 : 16;
    volatile uint8_t sink = 0;   // touch the source like a DMA would (ASan catches overreads)
    const uint8_t* s = (const uint8_t*)data;
    size_t n = (size_t)(t->width >> level) * (t->height >> level) * bpp / 8;
    for (size_t i = 0; i < n; i += 64) sink ^= s[i];
    sink ^= s[n - 1];
}
void C3D_TexUpload(C3D_Tex* t, const void* data) { C3D_TexLoadImage(t, data, GPU_TEXFACE_2D, 0); }
void C3D_TexSetFilter(C3D_Tex*, GPU_TEXTURE_FILTER_PARAM, GPU_TEXTURE_FILTER_PARAM) {}
void C3D_TexSetFilterMipmap(C3D_Tex*, GPU_TEXTURE_FILTER_PARAM) {}
void C3D_TexSetWrap(C3D_Tex*, GPU_TEXTURE_WRAP_PARAM, GPU_TEXTURE_WRAP_PARAM) {}
void C3D_TexDelete(C3D_Tex* t) {
    if (!t->data) return;
    if (t->onVram) g_vram -= t->size;
    std::free(t->data); t->data = nullptr; --g_texLive;
}
