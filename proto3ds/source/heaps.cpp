// heaps.cpp — stage 6.32: the split between the normal heap and the linear (GPU-visible) heap, by the memory the system gives.
//
// libctru's __system_allocateHeaps is weak; this one replaces it. The .3dsx under the Homebrew Launcher gets the 124 MB
// APPLICATION region of the New 3DS; the CIA (cia/w3dproto.rsf, SystemModeExt 178MB) gets 178 MB.
// 6.32d: the linear heap lives in the 128 MB window 0x30000000-0x38000000 (virtual = physical + offset), so it cannot take the
// whole 178 MB: the 7o .elf in the 178 MB CIA (24 MB heap fixed, ~150 MB linear asked) hit svcBreak in libctru's version. So:
//   the linear heap is allocated first: the region less a 24 MB minimum heap, at most 120 MB, stepping down 8 MB while the
//   kernel says no;
//   the normal heap gets everything else (124 MB region: 24 MB as before 6.32; 178 MB region: ~55 MB).
// 7o crashed at 533 yd in Stormwind with the 24 MB heap run dry (Part 78); the heap reserve of 6.31 stays in any case.
#include <3ds.h>

extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap, __ctru_linear_heap;
u32 __ctru_heap_size = 0;
u32 __ctru_linear_heap_size = 0;

void __system_allocateHeaps(void) {
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
    s64 maxCommit = 0, currentCommit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&maxCommit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &type, 1);
    svcCloseHandle(reslimit);
    const u32 remaining = (u32)(maxCommit - currentCommit) & ~0xFFFu;
    const MemPerm rw = (MemPerm)(MEMPERM_READ | MEMPERM_WRITE);

    const u32 kMinHeap = 24u << 20, kMaxLinear = 120u << 20, kStep = 8u << 20;
    u32 lin = remaining > kMinHeap ? remaining - kMinHeap : remaining / 2;
    if (lin > kMaxLinear) lin = kMaxLinear;
    lin &= ~0xFFFu;
    for (;;) {
        if (R_SUCCEEDED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, lin, MEMOP_ALLOC_LINEAR, rw))) break;
        if (lin <= kStep + (16u << 20)) svcBreak(USERBREAK_PANIC);   // (under 24 MB of linear memory nothing works anyway)
        lin -= kStep;
    }
    __ctru_linear_heap_size = lin;

    u32 heap = remaining - lin;
    const u32 kHeapArea = OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN;   // 96 MB of address space
    if (heap > kHeapArea) heap = kHeapArea;
    heap &= ~0xFFFu;
    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, heap, MEMOP_ALLOC, rw))) svcBreak(USERBREAK_PANIC);
    __ctru_heap_size = heap;

    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char*)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}
}
