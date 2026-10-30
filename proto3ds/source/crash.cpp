// crash.cpp — stage 6.31: what the game was doing when it crashed, written to sdmc:/3ds/w3dproto/crash.txt.
//
// A data abort on the console shows Luma's screen: registers and a stack dump, nothing about the game. The handler here
// (libctru threadOnException, one per thread) writes the registers, the fault, the main thread's last perf mark, the GPU
// command buffer's state (where libctru writes the next command, against where citro3d put the buffer) and the heaps' ranges,
// plus 64 words from the faulting stack (return addresses: llvm-symbolizer --obj=w3dproto.elf). Then it hands over to the
// system (svcBreak): Luma still shows its screen.
// The handler runs on its own stack and avoids the heap: FS calls straight to the SD card, a hand-made hex formatter.
// Also: crashCheckCmdBuf() once a frame after C3D_FrameBegin - libctru's command buffer pointer must be citro3d's buffer
// (6.31: a crash at 533 yd in Stormwind wrote through gpuCmdBuf = 0x301B0210, a page the process does not have).
#include <3ds.h>
#include <string.h>

#include "perflog.hpp"

extern "C" {
extern u32* gpuCmdBuf;
extern u32 gpuCmdBufSize, gpuCmdBufOffset;
extern u32 __ctru_linear_heap, __ctru_linear_heap_size, __ctru_heap, __ctru_heap_size;
}

namespace {
alignas(8) uint8_t g_stack[16 * 1024];
u32* g_cmdBase = nullptr;   // citro3d's buffer, as seen at the first frame
u32 g_cmdWords = 0;
bool g_cmdWarned = false;
char g_buf[3072];
size_t g_len = 0;

void put(const char* s) {
    while (*s && g_len + 1 < sizeof g_buf) g_buf[g_len++] = *s++;
}
void hex(u32 v) {
    char t[11] = "0x00000000";
    for (int i = 0; i < 8; ++i) t[9 - i] = "0123456789ABCDEF"[(v >> (i * 4)) & 15];
    put(t);
}
void dec(u32 v) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) { char c[2] = {t[--n], 0}; put(c); }
}
void kv(const char* k, u32 v) { put(k); put(" "); hex(v); put("\n"); }

void writeFile() {
    Handle f;
    if (R_FAILED(FSUSER_OpenFileDirectly(&f, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, "/3ds/w3dproto/crash.txt"),
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        return;
    u32 done = 0;
    FSFILE_SetSize(f, 0);
    FSFILE_Write(f, &done, 0, g_buf, (u32)g_len, FS_WRITE_FLUSH);
    FSFILE_Close(f);
}

void handler(ERRF_ExceptionInfo* ex, CpuRegisters* r) {
    g_len = 0;
    put("w3dproto crash (stage 6.31)\n");
    static const char* kType[] = {"prefetch abort", "data abort", "undefined instruction", "VFP"};
    put("type: "); put((u32)ex->type < 4u ? kType[(u32)ex->type] : "?"); put("\n");
    kv("FSR", ex->fsr); kv("FAR", ex->far); kv("FPEXC", ex->fpexc);
    kv("PC", r->pc); kv("LR", r->lr); kv("SP", r->sp); kv("CPSR", r->cpsr);
    for (int i = 0; i < 13; ++i) { put("R"); dec((u32)i); put(" "); hex(r->r[i]); put(i & 1 ? "\n" : "   "); }
    put("\nmain thread at: "); put(perf::g_state.where ? perf::g_state.where : "?"); put("\nframes: "); dec(perf::g_state.frames); put("\n");
    put("thread id: "); { u32 id = 0; svcGetThreadId(&id, CUR_THREAD_HANDLE); dec(id); } put("\n");
    kv("gpuCmdBuf", (u32)gpuCmdBuf); kv("gpuCmdBufSize (words)", gpuCmdBufSize); kv("gpuCmdBufOffset (words)", gpuCmdBufOffset);
    kv("citro3d buffer", (u32)g_cmdBase); kv("citro3d buffer words", g_cmdWords);
    kv("linear heap", __ctru_linear_heap); kv("linear heap size", __ctru_linear_heap_size);
    kv("heap", __ctru_heap); kv("heap size", __ctru_heap_size);
    put("stack (from SP):\n");
    const u32 sp = r->sp & ~3u;
    if (sp >= __ctru_heap && sp + 256 <= __ctru_heap + __ctru_heap_size)
        for (int i = 0; i < 64; ++i) { hex(((const u32*)sp)[i]); put(i % 4 == 3 ? "\n" : " "); }
    else
        put("(SP outside the heap: not read)\n");
    writeFile();
    svcBreak(USERBREAK_PANIC);
    for (;;) svcSleepThread(1000000000LL);
}
}  // namespace

void crashInstall() {
    threadOnException(handler, g_stack + sizeof g_stack, WRITE_DATA_TO_HANDLER_STACK);
}

void crashCheckCmdBuf() {
    if (!g_cmdBase) {
        g_cmdBase = gpuCmdBuf;
        g_cmdWords = gpuCmdBufSize;
        return;
    }
    // (right after C3D_FrameBegin the pointer is the buffer's start again and the whole size is free)
    if ((gpuCmdBuf != g_cmdBase || gpuCmdBufSize != g_cmdWords) && !g_cmdWarned) {
        g_cmdWarned = true;
        perf::event("GPU command buffer moved: libctru writes to %p (%lu words, offset %lu), citro3d's buffer is %p (%lu words) - "
                    "memory corruption before this frame (crash.txt will follow if it crashes)",
                    (void*)gpuCmdBuf, (unsigned long)gpuCmdBufSize, (unsigned long)gpuCmdBufOffset, (void*)g_cmdBase, (unsigned long)g_cmdWords);
    }
}
