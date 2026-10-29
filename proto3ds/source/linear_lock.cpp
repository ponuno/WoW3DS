// linear_lock.cpp — stage 6.6: one lock around libctru's linear-heap allocator.
//
// libctru's linearAlloc / linearFree keep a free list and an address tree with no lock at all, and two threads use them:
// the main thread (texture staging, models, citro3d's textures and command buffers) and the sound thread on core 2
// (the decoded world sounds' cache, audio.cpp). Two calls at the same time can break the tree - a corrupted heap, or a
// thread spinning forever in it: a freeze "when the objects appear" (their sounds start while their textures stream in).
// The Makefile links with -Wl,--wrap=linearAlloc,... so every call from another object file comes here; calls inside
// libctru's linear.o itself (linearAlloc -> linearMemAlign) are not redirected, and the lock is recursive anyway.
#include <3ds.h>

extern "C" {
void* __real_linearAlloc(size_t size);
void* __real_linearMemAlign(size_t size, size_t alignment);
void* __real_linearRealloc(void* mem, size_t size);
size_t __real_linearGetSize(void* mem);
void __real_linearFree(void* mem);
u32 __real_linearSpaceFree(void);

static RecursiveLock s_lock;
static bool s_init = false;   // (first used on the main thread, before any other thread exists)

static inline void lockLinear() {
    if (!s_init) { RecursiveLock_Init(&s_lock); s_init = true; }
    RecursiveLock_Lock(&s_lock);
}
static inline void unlockLinear() { RecursiveLock_Unlock(&s_lock); }

void* __wrap_linearAlloc(size_t size) {
    lockLinear();
    void* p = __real_linearAlloc(size);
    unlockLinear();
    return p;
}
void* __wrap_linearMemAlign(size_t size, size_t alignment) {
    lockLinear();
    void* p = __real_linearMemAlign(size, alignment);
    unlockLinear();
    return p;
}
void* __wrap_linearRealloc(void* mem, size_t size) {
    lockLinear();
    void* p = __real_linearRealloc(mem, size);
    unlockLinear();
    return p;
}
size_t __wrap_linearGetSize(void* mem) {
    lockLinear();
    size_t n = __real_linearGetSize(mem);
    unlockLinear();
    return n;
}
void __wrap_linearFree(void* mem) {
    lockLinear();
    __real_linearFree(mem);
    unlockLinear();
}
u32 __wrap_linearSpaceFree(void) {
    lockLinear();
    u32 n = __real_linearSpaceFree();
    unlockLinear();
    return n;
}
}
