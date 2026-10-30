// heaps.cpp — stage 6.32: the split between the normal heap and the linear (GPU-visible) heap, by the memory the system gives.
//
// libctru's __system_allocateHeaps is weak; this one is the same (libctru 2.x, source/system/allocateHeaps.c) except for the
// split. The .3dsx under the Homebrew Launcher gets the 124 MB APPLICATION region of the New 3DS; the CIA (cia/w3dproto.rsf,
// SystemModeExt: 178MB) gets 178 MB:
//   under 150 MB free: heap 24 MB (as before 6.32), the rest linear (~100 MB);
//   150 MB or more:    heap 48 MB, the rest linear (~130 MB).
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

    __ctru_heap_size = remaining >= (150u << 20) ? (48u << 20) : (24u << 20);
    if (__ctru_heap_size > remaining / 2) __ctru_heap_size = (remaining / 2) & ~0xFFFu;   // (a small region: Old 3DS)
    __ctru_linear_heap_size = remaining - __ctru_heap_size;

    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char*)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}
}
