// PC stand-in for libctru: just enough for streamer.cpp (threads = std::thread,
// linear heap = malloc with a budget, GPU calls = no-ops that validate inputs).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s32 Result; typedef u32 Handle;
#define R_SUCCEEDED(r) ((r) >= 0)
#define U64_MAX UINT64_MAX
#define SYSCLOCK_ARM11 268111856ULL
#define CUR_PROCESS_HANDLE 0xFFFF8001
#define CUR_THREAD_HANDLE 0xFFFF8000
typedef struct ThreadImpl* Thread;
typedef void (*ThreadFunc)(void*);
Thread threadCreate(ThreadFunc f, void* arg, size_t stack, int prio, int core, bool detached);
Result threadJoin(Thread t, u64 timeout);
void threadFree(Thread t);
typedef struct { void* m; } LightLock;
void LightLock_Init(LightLock* l); void LightLock_Lock(LightLock* l); void LightLock_Unlock(LightLock* l);
enum { RESET_ONESHOT = 0, RESET_STICKY = 1 };
typedef struct { void* impl; } LightEvent;
void LightEvent_Init(LightEvent* e, int type); void LightEvent_Signal(LightEvent* e); void LightEvent_Wait(LightEvent* e);
u64 svcGetSystemTick(void);
Result svcGetThreadPriority(s32* out, Handle h);
Result svcFlushProcessDataCache(Handle h, u32 addr, u32 size);
Result GSPGPU_FlushDataCache(const void* p, u32 size);
Result APT_CheckNew3DS(bool* out);
Result APT_SetAppCpuTimeLimit(u32 percent);
void* linearAlloc(size_t size); void linearFree(void* p); u32 linearSpaceFree(void);
u32 vramSpaceFree(void);
