// vram_guard.hpp — 6.43: never let libctru's VRAM allocator run dry.
//
// libctru (source/allocator/vram.cpp) decides "not initialised yet" in vramInit() by MemPool::Ready(), which is "the pool still has a
// free block". When the last free byte of both pools (VRAM A and B) has been handed out, the next vramAlloc - a texture, a render
// target, citro3d's C3D_TexInitVRAM - re-creates both pools as one free block each and forgets every allocation: all 6 MB count as free
// again, and the next textures are put over the ones still in use and over the render targets. That was the "broken textures" (perf
// log: "VRAM JUMP: free 0 KB -> 6141 KB between texture uploads and portraits", right after VRAM had filled up to 0 KB).
// So no VRAM allocation may be asked for unless kVramKeep bytes stay free after it: one free block always remains.
#pragma once
#include <3ds.h>

static constexpr u32 kVramKeep = 16 * 1024;
static inline bool vramRoomFor(u32 bytes) { return vramSpaceFree() >= bytes + kVramKeep; }
