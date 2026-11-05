// 6.43: libctru's own VRAM pool code (allocator/mem_pool.cpp, compiled as is) driven the way vram.cpp drives it: two pools of 3 MB,
// vramInit() re-creating both when neither has a free block (MemPool::Ready). Shows the fault - filling VRAM to the last byte makes
// the next allocation "re-initialise" the allocator, every byte free again over the textures in use - and that the rule of
// vram_guard.hpp (an allocation only while kVramKeep bytes stay free after it) never gets there.
//   g++ -I<libctru>/libctru/include -I<libctru>/libctru/source/allocator vram_pool_test.cpp <libctru>/.../mem_pool.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mem_pool.h"

static int bad = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++bad; } }

struct Vram {   // vram.cpp without the address map
    MemPool a{}, b{};
    u8* base = (u8*)0x1F000000;
    int reinits = 0;
    bool init() {
        if (a.Ready() || b.Ready()) return true;
        if (reinits++ == 0 || true) {
            a = MemPool{}; b = MemPool{};
            a.AddBlock(MemBlock::Create(base, 0x300000));
            b.AddBlock(MemBlock::Create(base + 0x300000, 0x300000));
        }
        return true;
    }
    u32 freeSpace() { return a.GetFreeSpace() + b.GetFreeSpace(); }
    bool alloc(u32 size, MemChunk& c) {
        init();
        bool preferA = a.GetFreeSpace() >= b.GetFreeSpace();
        MemPool& p1 = preferA ? a : b; MemPool& p2 = preferA ? b : a;
        return p1.Allocate(c, size, 7) || p2.Allocate(c, size, 7);
    }
};

int main() {
    const u32 kKeep = 16 * 1024;
    // 1. the fault: textures of 128 KB until nothing fits, then the rest in small pieces to the last byte, then one more allocation
    {
        Vram v;
        v.init();
        v.reinits = 0;
        std::vector<MemChunk> used;
        MemChunk c;
        for (u32 sz : {131072u, 4096u, 128u})
            while (v.freeSpace() && v.alloc(sz, c)) used.push_back(c);   // (stops at 0 bytes: one more call would already re-create the pools)
        check(v.freeSpace() == 0, "VRAM filled to the last byte");
        const bool got = v.alloc(65536, c);   // the next texture
        check(got && v.reinits == 1 && v.freeSpace() > 6000000, "libctru re-creates its pools: all 6 MB free again (the fault)");
        bool overlaps = false;
        for (const MemChunk& u : used) overlaps |= c.addr < u.addr + u.size && u.addr < c.addr + c.size;
        check(overlaps, "and the new block lies on a texture still in use");
    }
    // 2. the rule of vram_guard.hpp: an allocation only while kKeep bytes stay free after it - never a re-initialisation
    {
        Vram v;
        v.init();
        v.reinits = 0;
        std::vector<MemChunk> used;
        MemChunk c;
        srand(7);
        for (int i = 0; i < 20000; ++i) {
            const u32 sz = (u32)(128 << (rand() % 11));   // 128 B .. 128 KB
            if (!used.empty() && rand() % 3 == 0) {
                const size_t k = rand() % used.size();
                MemPool& p = used[k].addr < v.base + 0x300000 ? v.a : v.b;
                p.Deallocate(used[k]);
                used.erase(used.begin() + k);
                continue;
            }
            if (v.freeSpace() < sz + kKeep) continue;   // vramRoomFor
            if (v.alloc(sz, c)) used.push_back(c);
        }
        check(v.reinits == 0, "with the guard the pools are never re-created");
        u32 sum = 0;
        for (const MemChunk& u : used) sum += u.size;
        check(sum + v.freeSpace() == 0x600000, "the books add up: used + free = 6 MB");
    }
    std::printf("VRAM POOL: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
