// perflog.cpp — the perf log (stage 6), see perflog.hpp. Column legend: docs/PERFLOG.md; summary: tools/perflog/analyze.py.
#include "perflog.hpp"

#include <citro3d.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "audio.hpp"
#include "fx.hpp"
#include "perf_core.hpp"
#include "ui_render.hpp"
#include "world.h"

#include <malloc.h>  // after <stdlib.h> (pulled in above): newlib declares calloc twice

extern "C" {
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;
}

namespace perf {

State g_state;

namespace {

constexpr double kHitchMs = 50.0;           // a frame this long is written to the events with its breakdown
constexpr int kHitchesPerSecond = 4;
constexpr size_t kQueueLimit = 512 * 1024;  // the writer is behind by this much: drop rows (counted)
constexpr u32 kVramSize = 6 * 1024 * 1024;

const char* kPhaseNames[kPhases] = {"input", "net", "ui", "move", "spells", "sound", "game", "hud", "touch", "glue",
                                    "stream", "prepare", "wait", "portrait", "world", "huddraw", "touchdraw", "flush", "mapload", "other"};
const char* kPassNames[FrameCounters::kPasses] = {"sky", "terrain", "collect", "opaque", "markers", "units", "water", "alpha", "fx"};

double ms(u64 ticks) { return (double)ticks * 1000.0 / (double)SYSCLOCK_ARM11; }
double kb(double bytes) { return bytes / 1024.0; }

// ------------------------------------------------------------------ the writer thread
struct Writer {
    FILE* csv = nullptr;
    FILE* ev = nullptr;
    Thread thread = nullptr;
    LightLock lock;
    LightEvent wake;
    std::atomic<bool> quit{false};
    std::string qCsv, qEv;           // guarded by lock
    std::vector<std::vector<Row::Val>> qRows;   // CSV rows still as numbers (guarded): the writer prints them
    size_t qRowsBytes = 0;
    uint32_t dropped = 0;            // rows / events dropped because the writer was behind (guarded)
    double writeMs = 0;              // time in fwrite + fflush since the last take (guarded)
    uint64_t written = 0;            // bytes written (guarded)
    int core = -1;

    void writeNow(std::string& a, std::string& b) {
        const u64 t0 = svcGetSystemTick();
        if (!a.empty() && csv) { fwrite(a.data(), 1, a.size(), csv); fflush(csv); }
        if (!b.empty() && ev) { fwrite(b.data(), 1, b.size(), ev); fflush(ev); }
        const double d = ms(svcGetSystemTick() - t0);
        LightLock_Lock(&lock);
        writeMs += d;
        written += a.size() + b.size();
        LightLock_Unlock(&lock);
        a.clear();
        b.clear();
    }
    static void main(void* arg) {
        Writer* w = (Writer*)arg;
        std::string a, b;
        std::vector<std::vector<Row::Val>> rows;
        for (;;) {
            LightEvent_WaitTimeout(&w->wake, 500 * 1000000LL);
            LightLock_Lock(&w->lock);
            a.swap(w->qCsv);
            b.swap(w->qEv);
            rows.swap(w->qRows);
            w->qRowsBytes = 0;
            const bool q = w->quit;
            LightLock_Unlock(&w->lock);
            for (const auto& r : rows) Row::format(r, a);   // (a header is a row of one text field: it keeps its place)
            rows.clear();
            if (!a.empty() || !b.empty()) w->writeNow(a, b);
            if (q) break;
        }
    }
    void pushRow(std::vector<Row::Val>&& vals) {
        if (!thread) {
            std::string a, b;
            Row::format(vals, a);
            writeNow(a, b);
            return;
        }
        LightLock_Lock(&lock);
        if (qCsv.size() + qRowsBytes > kQueueLimit) ++dropped;
        else { qRowsBytes += vals.size() * 6; qRows.push_back(std::move(vals)); }
        LightLock_Unlock(&lock);
        LightEvent_Signal(&wake);
    }
    void push(bool toCsv, const std::string& s) {
        if (!thread) {   // no thread: write here
            std::string a = toCsv ? s : std::string(), b = toCsv ? std::string() : s;
            writeNow(a, b);
            return;
        }
        LightLock_Lock(&lock);
        std::string& q = toCsv ? qCsv : qEv;
        if (q.size() > kQueueLimit) ++dropped;
        else q += s;
        LightLock_Unlock(&lock);
        LightEvent_Signal(&wake);
    }
    bool syncInit = false;
    void start() {
        if (!syncInit) {   // once: the log can be switched off and on again
            LightLock_Init(&lock);
            LightEvent_Init(&wake, RESET_ONESHOT);
            syncInit = true;
        }
        quit = false;
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        // below the main thread, the streamer's IO and the sound: it runs while they wait (the SD card, the GPU)
        const int lowest = prio + 2 > 0x3F ? 0x3F : prio + 2;
        if (n3ds && (thread = threadCreate(main, this, 16 * 1024, lowest, 2, false))) core = 2;
        if (!thread && (thread = threadCreate(main, this, 16 * 1024, lowest, 1, false))) core = 1;
        if (!thread && (thread = threadCreate(main, this, 16 * 1024, lowest, 0, false))) core = 0;
    }
    void stop() {
        if (thread) {
            quit = true;
            LightEvent_Signal(&wake);
            threadJoin(thread, U64_MAX);
            threadFree(thread);
            thread = nullptr;
        }
        std::string a, b;
        a.swap(qCsv);
        b.swap(qEv);
        for (const auto& r : qRows) Row::format(r, a);
        qRows.clear();
        qRowsBytes = 0;
        writeNow(a, b);
        if (csv) fclose(csv);
        if (ev) fclose(ev);
        csv = ev = nullptr;
    }
};
Writer g_w;

// ------------------------------------------------------------------ one second of frames
struct Window {
    u64 start = 0;
    uint32_t frames = 0;
    std::vector<float> frameMs;
    Acc frame, cpu, wait, gpu, c3dCpu, cmdBuf;
    Acc phase[kPhases];
    Acc passMs[FrameCounters::kPasses], passDraws[FrameCounters::kPasses], passTris[FrameCounters::kPasses];
    Acc draws, tris, binds, batches, lod[3], water, inst, bld, markers, units, bones, pose, fxMs, fxQuads, fxParticles, fxGlows,
        fxSources, eyes, kicks, kickLead, kickMs, mtxMs, chCull, chTest, fading, mlod0, mlod1, mlod2, farUnits, sharedPoses;
    Acc stUpd, stDone, stPlayers, stUpload, stTiles, stSched, stDefer;
    uint32_t stJobsDone = 0, stUploads = 0;
    uint64_t stUploadBytes = 0;
    uint32_t over33 = 0, over50 = 0, over100 = 0, hitches = 0, hitchesLogged = 0;
    double worstMs = 0;
    int worstPhase = -1;
};
Window g_win;

// the previous second's cumulative counters (the row shows the differences)
StreamStats g_prevSt;
UiTexStats g_prevUi;
audio::Stats g_prevSnd;
fx::Stats g_prevFx;
Context g_prevCtx;
bool g_havePrev = false;

// the session
struct Session {
    u64 start = 0;
    uint32_t seconds = 0, worldSeconds = 0, hitches = 0, droppedRows = 0;
    Histogram all, world;
    double phaseSum[kPhases] = {};
    uint64_t phaseFrames = 0;
    double worstFps = 1e9, worstFpsAt = 0;
    char worstFpsWhere[64] = "";
    double minLinFree = 1e18, minVramFree = 1e18, minVramLargest = 1e18, minHeapFree = 1e18, maxCmdBuf = 0;
    uint32_t maxWhite = 0, maxQueue = 0;
    double maxGpu = 0;
    std::string lastPhase, lastSettings;
    uint32_t lastMap = ~0u;
    bool wasOnline = false;
};
Session g_ses;

LowWatch g_linWatch[3] = {{4096, 1024}, {2048, 512}, {1024, 512}};   // KB
LowWatch g_vramWatch = {256, 128};                                   // largest free VRAM block, KB
LowWatch g_heapWatch = {1024, 512};                                  // KB of the heap left
bool g_cmdBufWarned = false, g_whiteWarned = false;
char g_dir[96] = "";
bool g_inited = false, g_wantOn = false;
uint32_t g_lastLinFree = 0;

// The largest VRAM block one allocation could get (VRAM is allocated on the main thread only, so probing is safe):
// a binary search with real allocations, 4 KB steps, ~11 tries.
u32 vramLargest() {
    u32 lo = 0, hi = vramSpaceFree();
    if (hi < 128) return 0;
    if (void* p = vramAlloc(hi)) { vramFree(p); return hi; }
    while (hi - lo > 4096) {
        const u32 mid = ((lo + hi) / 2) & ~0x7Fu;
        if (void* p = vramAlloc(mid)) { vramFree(p); lo = mid; } else hi = mid;
    }
    return lo;
}

double sinceStartS() { return ms(svcGetSystemTick() - g_ses.start) / 1000.0; }

void writeHeader() {
    char b[1024];
    time_t now = time(nullptr);
    const struct tm* lt = localtime(&now);
    char when[32] = "?";
    if (lt) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", lt);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    u32 cpuLimit = 0;
    APT_GetAppCpuTimeLimit(&cpuLimit);
    std::string h;
    snprintf(b, sizeof b, "== w3dproto perf log, %s\nconsole: %s; core 1 time limit %lu%%; perf writer on core %d\n", when,
             n3ds ? "New 3DS (804 MHz, L2)" : "Old 3DS / 2DS (268 MHz)", (unsigned long)cpuLimit, g_w.core);
    h += b;
    snprintf(b, sizeof b, "memory: APPLICATION region %lu KB (used %lu KB); heap %lu KB; linear heap %lu KB (free now %lu KB); VRAM free now %lu KB "
                          "(largest block %lu KB)\n",
             (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024), (unsigned long)(osGetMemRegionUsed(MEMREGION_APPLICATION) / 1024),
             (unsigned long)(__ctru_heap_size / 1024), (unsigned long)(__ctru_linear_heap_size / 1024), (unsigned long)(linearSpaceFree() / 1024),
             (unsigned long)(vramSpaceFree() / 1024), (unsigned long)(vramLargest() / 1024));
    h += b;
    snprintf(b, sizeof b, "rows: one per second in perf.csv (legend: docs/PERFLOG.md); hitch = a frame over %.0f ms (at most %d a second are "
                          "written below)\n\n", kHitchMs, kHitchesPerSecond);
    h += b;
    g_w.push(false, h);
}

void openFiles() {
    char csv[128], ev[128], pc[128], pe[128];
    snprintf(csv, sizeof csv, "%s/perf.csv", g_dir);
    snprintf(ev, sizeof ev, "%s/perf_events.txt", g_dir);
    snprintf(pc, sizeof pc, "%s/perf_prev.csv", g_dir);
    snprintf(pe, sizeof pe, "%s/perf_prev_events.txt", g_dir);
    static bool once = false;   // switched on again in the same session: go on in the same files
    if (!once) {
        remove(pc);
        remove(pe);
        rename(csv, pc);
        rename(ev, pe);
    }
    g_w.csv = fopen(csv, once ? "a" : "w");
    g_w.ev = fopen(ev, once ? "a" : "w");
    once = true;
    if (g_w.csv) setvbuf(g_w.csv, nullptr, _IOFBF, 16 * 1024);
    if (g_w.ev) setvbuf(g_w.ev, nullptr, _IOFBF, 4 * 1024);
}

std::string settingsLine(const Context& c) {
    char b[192];
    snprintf(b, sizeof b, "draw distance %.0f, doodads %s (range %.0f), fog %s, particles %d, crowd %d, top screen %s, scheme %s, GPU %s", c.drawDist,
             c.doodads ? "on" : "off", c.doodadRange, c.fog ? "on" : "off", c.effects, c.crowdCap,
             c.topMode == 1 ? "800x240" : c.topMode == 2 ? "3D" : "400x240", c.scheme ? "Classic" : "Forever", c.gpuKick ? "early" : "at end");
    if (strlen(b) + 12 < sizeof b) strcat(b, c.vsync ? ", vsync" : ", no vsync");
    return b;
}

}  // namespace

const char* phaseName(int p) { return p >= 0 && p < kPhases ? kPhaseNames[p] : "?"; }

// ------------------------------------------------------------------ stage 6.6: the freeze watchdog
// A thread of its own (core 1 when it can have it: the sound and the streamer's IO run on core 2 and one of them may be
// the one stuck) that looks every half second whether the main loop finished a frame. After 3 s without one it writes
// to perf_stall.txt (opened, appended, closed - the log's writer may be stuck as well) and to the events: where the main
// thread is (the last mark and the last finished phase) and whether the sound thread still turns.
namespace {
Thread g_dog = nullptr;
std::atomic<bool> g_dogQuit{false};
void dogWrite(const char* line) {
    char path[128];
    snprintf(path, sizeof path, "%s/perf_stall.txt", g_dir);
    if (FILE* f = fopen(path, "a")) { fputs(line, f); fputc('\n', f); fclose(f); }
    if (g_state.on) {
        std::string s = line;
        s += '\n';
        g_w.push(false, s);
    }
}
void dogMain(void*) {
    uint32_t seen = __atomic_load_n(&g_state.frames, __ATOMIC_RELAXED), sndSeen = 0;
    u64 since = svcGetSystemTick(), reported = 0;
    bool stalled = false;
    while (!g_dogQuit) {
        svcSleepThread(500 * 1000000LL);
        const u64 now = svcGetSystemTick();
        const uint32_t f = __atomic_load_n(&g_state.frames, __ATOMIC_RELAXED);   // (written by the main thread)
        audio::Stats snd;
        audio::stats(&snd);
        if (f != seen) {
            if (stalled) {
                char b[160];
                snprintf(b, sizeof b, "[%8.1f s] STALL over after %.1f s (frames go on)", sinceStartS(), ms(now - since) / 1000.0);
                dogWrite(b);
            }
            seen = f;
            since = now;
            stalled = false;
            sndSeen = snd.busyUs;
            continue;
        }
        const double stuck = ms(now - since);
        if (stuck < 3000.0 || (stalled && ms(now - reported) < 10000.0)) continue;
        const char* where = g_state.where;
        char b[320];
        snprintf(b, sizeof b, "[%8.1f s] STALL: no frame for %.1f s; main thread at \"%s\" (last finished phase: %s); sound thread %s; "
                              "linear free %lu KB, VRAM free %lu KB",
                 sinceStartS(), stuck / 1000.0, where ? where : "?", phaseName(g_state.lastLap),
                 !snd.on ? "off" : snd.busyUs != sndSeen ? "running" : "NOT running",
                 (unsigned long)(g_lastLinFree / 1024), (unsigned long)(vramSpaceFree() / 1024));
        dogWrite(b);
        sndSeen = snd.busyUs;
        stalled = true;
        reported = now;
    }
}
void dogStart() {
    if (g_dog) return;
    g_dogQuit = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    const int p = prio - 3 < 0x18 ? 0x18 : prio - 3;   // above the others: it must run even if one of them spins
    u32 limit = 0;
    APT_GetAppCpuTimeLimit(&limit);
    if (!limit) APT_SetAppCpuTimeLimit(5);
    g_dog = threadCreate(dogMain, nullptr, 8 * 1024, p, 1, false);
    if (!g_dog) g_dog = threadCreate(dogMain, nullptr, 8 * 1024, p, 2, false);
    if (!g_dog) g_dog = threadCreate(dogMain, nullptr, 8 * 1024, p, 0, false);
}
void dogStop() {
    if (!g_dog) return;
    g_dogQuit = true;
    threadJoin(g_dog, U64_MAX);
    threadFree(g_dog);
    g_dog = nullptr;
}
}  // namespace

void event(const char* fmt, ...) {
    if (!g_state.on) return;
    char b[640];
    int n = snprintf(b, sizeof b, "[%8.1f s] ", sinceStartS());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap);
    va_end(ap);
    std::string s = b;
    s += '\n';
    g_w.push(false, s);
}

void init(const char* dir, bool on) {
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    g_ses = Session();
    g_ses.start = svcGetSystemTick();
    g_win = Window();
    g_win.start = g_ses.start;
    g_win.frameMs.reserve(128);
    g_havePrev = false;
    g_inited = true;
    g_wantOn = false;
    setEnabled(on);
    dogStart();   // (stage 6.6: the watchdog runs whether the log is on or not)
}

void setEnabled(bool on) {
    if (!g_inited || on == g_wantOn) return;
    g_wantOn = on;
    if (on) {
        openFiles();
        g_w.start();
        g_state.on = g_w.csv != nullptr;
        if (!g_state.on) { g_w.stop(); return; }
        writeHeader();
        g_win = Window();
        g_win.start = svcGetSystemTick();
        g_win.frameMs.reserve(128);
        g_havePrev = false;
        frameStart();
    } else {
        event("perf log switched off in the settings");
        g_state.on = false;
        g_w.stop();
    }
}

bool enabled() { return g_state.on; }

void endFrame(const FrameCounters& fc) {
    __atomic_store_n(&g_state.frames, g_state.frames + 1, __ATOMIC_RELAXED);
    mark("between frames");
    if (!g_state.on) return;
    lap(kOther);
    const u64 now = svcGetSystemTick();
    const double total = ms(now - g_state.start);
    Window& w = g_win;
    ++w.frames;
    w.frameMs.push_back((float)total);
    w.frame.add(total);
    const double wait = ms(g_state.cur[kWait]);
    w.cpu.add(total - wait);
    w.wait.add(wait);
    w.gpu.add(C3D_GetDrawingTime());
    w.c3dCpu.add(C3D_GetProcessingTime());
    w.cmdBuf.add(C3D_GetCmdBufUsage() * 100.0);
    int top = 0;
    for (int p = 0; p < kPhases; ++p) {
        const double v = ms(g_state.cur[p]);
        w.phase[p].add(v);
        if (g_state.cur[p] > g_state.cur[top]) top = p;
        g_ses.phaseSum[p] += v;
    }
    ++g_ses.phaseFrames;
    for (int k = 0; k < FrameCounters::kPasses; ++k) {
        w.passMs[k].add(fc.passMs[k]);
        w.passDraws[k].add(fc.passDraws[k]);
        w.passTris[k].add(fc.passTris[k]);
    }
    w.draws.add(fc.drawCalls); w.tris.add(fc.tris); w.binds.add(fc.texBinds); w.batches.add(fc.visibleBatches);
    for (int l = 0; l < 3; ++l) w.lod[l].add(fc.lodBatches[l]);
    w.water.add(fc.waterBatches); w.inst.add(fc.visibleInstances); w.bld.add(fc.buildings); w.markers.add(fc.markers);
    w.units.add(fc.creatures); w.bones.add(fc.boneUploads); w.pose.add(fc.poseMs); w.fxMs.add(fc.fxMs);
    w.fxQuads.add(fc.fxQuads); w.fxParticles.add(fc.fxParticles); w.fxGlows.add(fc.fxGlows); w.fxSources.add(fc.fxSources);
    w.eyes.add(fc.eyes);
    w.kicks.add(fc.kicks); w.kickLead.add(fc.kickLeadMs); w.kickMs.add(fc.kickMs);
    w.mtxMs.add(fc.matrixMs); w.chCull.add(fc.chunksCulled); w.chTest.add(fc.chunksTested); w.fading.add(fc.fading);
    w.mlod0.add(fc.lodDraws[0]); w.mlod1.add(fc.lodDraws[1]); w.mlod2.add(fc.lodDraws[2]);
    w.farUnits.add(fc.farUnits); w.sharedPoses.add(fc.sharedPoses);
    const StreamStats& s = streamerStats();
    w.stUpd.add(s.updateMsLastFrame); w.stDone.add(s.doneMsLastFrame); w.stPlayers.add(s.playersMsLastFrame);
    w.stUpload.add(s.uploadMsLastFrame); w.stTiles.add(s.tilesMsLastFrame); w.stSched.add(s.scheduleMsLastFrame);
    w.stDefer.add(s.deferredMsLastFrame);
    w.stJobsDone += (uint32_t)s.jobsDoneLastFrame;
    w.stUploads += (uint32_t)s.uploadsLastFrame;
    w.stUploadBytes += s.uploadBytesLastFrame;
    w.over33 += total > 33.4;
    w.over50 += total > 50.0;
    w.over100 += total > 100.0;
    if (total > w.worstMs) { w.worstMs = total; w.worstPhase = top; }
    if (total > kHitchMs) {
        ++w.hitches;
        ++g_ses.hitches;
        if (w.hitchesLogged < kHitchesPerSecond) {   // what the long frame was made of
            ++w.hitchesLogged;
            char b[640];
            int n = snprintf(b, sizeof b, "hitch %.1f ms:", total);
            for (int p = 0; p < kPhases && n < (int)sizeof b - 40; ++p)
                if (ms(g_state.cur[p]) >= 1.0) n += snprintf(b + n, sizeof b - (size_t)n, " %s %.1f", kPhaseNames[p], ms(g_state.cur[p]));
            n += snprintf(b + n, sizeof b - (size_t)n, " | gpu %.1f", C3D_GetDrawingTime());
            for (int k = 0; k < FrameCounters::kPasses && n < (int)sizeof b - 40; ++k)
                if (fc.passMs[k] >= 1.0) n += snprintf(b + n, sizeof b - (size_t)n, " r.%s %.1f", kPassNames[k], fc.passMs[k]);
            if (n < (int)sizeof b - 120)
                n += snprintf(b + n, sizeof b - (size_t)n, " | stream: done %.1f (%d reads) players %.1f upload %.1f (%d) tiles %.1f sched %.1f defer %.1f",
                              s.doneMsLastFrame, s.jobsDoneLastFrame, s.playersMsLastFrame, s.uploadMsLastFrame, s.uploadsLastFrame,
                              s.tilesMsLastFrame, s.scheduleMsLastFrame, s.deferredMsLastFrame);
            if (n < (int)sizeof b - 60)
                snprintf(b + n, sizeof b - (size_t)n, " | draws %lu tris %lu units %lu", (unsigned long)fc.drawCalls, (unsigned long)fc.tris,
                         (unsigned long)fc.creatures);
            event("%s", b);
        }
    }
    frameStart();   // the next frame's marks start here (the loop's top calls it again: no gap either way)
}

bool due() { return g_state.on && ms(svcGetSystemTick() - g_win.start) >= 1000.0; }

void second(const Context& c) {
    if (!g_state.on) return;
    const u64 t0 = svcGetSystemTick();
    Window& w = g_win;
    const double winMs = std::max(1.0, ms(t0 - w.start));
    const double winS = winMs / 1000.0;
    const StreamStats& s = streamerStats();
    const UiTexStats& u = uiTexStats();
    audio::Stats snd;
    audio::stats(&snd);
    const fx::Stats fxs = fx::global().stats();
    // the walk over every model / texture is the log's dearest part (first console run: the whole row took 3.5 ms):
    // every 5 s; the memory totals (free linear / VRAM / heap) stay per second
    static StreamMemory m;
    static uint32_t walkAt = 0;
    double memMs = 0;
    if (!g_havePrev || g_ses.seconds >= walkAt) {
        const u64 m0 = svcGetSystemTick();
        streamerMemory(&m);
        memMs = ms(svcGetSystemTick() - m0);
        walkAt = g_ses.seconds + 5;
    }
    if (!g_havePrev) { g_prevSt = s; g_prevUi = u; g_prevSnd = snd; g_prevFx = fxs; g_prevCtx = c; g_havePrev = true; }
    const StreamStats& ps = g_prevSt;
    const bool world = !strcmp(c.phase, "world");
    // --- frames into the session histograms
    for (float f : w.frameMs) { g_ses.all.add(f); if (world) g_ses.world.add(f); }
    ++g_ses.seconds;
    g_ses.worldSeconds += world;
    const double fps = w.frames / winS;
    std::vector<float> sorted = w.frameMs;
    const double p50 = percentile(sorted, 0.5), p90 = percentile(sorted, 0.9);
    const char* bound = "none";
    if (w.frames) {
        if (fps >= 57.0) bound = "vsync";
        else if (w.gpu.avg() + w.kickLead.avg() > w.cpu.avg()) bound = "gpu";   // (6.3: after an early kick the GPU starts before FrameEnd)
        else bound = "cpu";
    }
    if (world && w.frames && fps < g_ses.worstFps) {
        g_ses.worstFps = fps;
        g_ses.worstFpsAt = sinceStartS();
        snprintf(g_ses.worstFpsWhere, sizeof g_ses.worstFpsWhere, "map %lu tile %d_%d (%.0f, %.0f)", (unsigned long)c.map, c.tileX, c.tileY,
                 c.pos[0], c.pos[1]);
    }
    // --- memory
    const u32 linFree = linearSpaceFree();
    g_lastLinFree = linFree;
    const u32 linTotal = __ctru_linear_heap_size;
    const u32 linUsed = linTotal > linFree ? linTotal - linFree : 0;
    const u32 vFree = vramSpaceFree(), vLargest = vramLargest();
    const u32 vUsed = kVramSize > vFree ? kVramSize - vFree : 0;
    const struct mallinfo mi = mallinfo();
    const u32 heapUsed = (u32)mi.uordblks, heapArena = (u32)mi.arena;
    const u32 heapFree = __ctru_heap_size > heapUsed ? __ctru_heap_size - heapUsed : 0;
    u32 texVram = m.bodyVramBytes, texLin = m.bodyLinearBytes;
    for (int k = 0; k < kStreamClasses; ++k) { texVram += m.texVramByClass[k]; texLin += m.texLinearByClass[k]; }
    const u32 staging = m.stagingReadingBytes + m.stagingUploadBytes + m.stagingRetryBytes;
    const u32 sndLin = snd.sfxBytes + snd.streamBytes + snd.cacheBytes;
    const u32 uiLin = u.bytes + u.vertexBytes + u.fontBytes;   // (UI textures are made with C3D_TexInit: linear memory)
    const u32 linKnown = m.poolBytes + m.waterBytes + m.buildingBytes + m.doodadBytes + m.unitModelBytes + m.modelLoadingBytes +
                         m.creatureBytes + texLin + staging + m.collLinearBytes + sndLin + uiLin + c.cmdBufBytes;
    const u32 vramKnown = texVram + c.vramTargets + c.vramReserve + c.vramPortraits;
    const u32 heapKnown = m.heightBytes + m.instanceBytes + m.collHeapBytes + m.liquidBytes + m.groundBytes + m.charImgBytes + m.charInfoBytes + m.indexBytes;
    g_ses.minLinFree = std::min(g_ses.minLinFree, (double)linFree);
    g_ses.minVramFree = std::min(g_ses.minVramFree, (double)vFree);
    g_ses.minVramLargest = std::min(g_ses.minVramLargest, (double)vLargest);
    g_ses.minHeapFree = std::min(g_ses.minHeapFree, (double)heapFree);
    g_ses.maxCmdBuf = std::max(g_ses.maxCmdBuf, w.cmdBuf.max());
    g_ses.maxWhite = std::max(g_ses.maxWhite, m.texWhiteNow);
    g_ses.maxQueue = std::max(g_ses.maxQueue, (uint32_t)s.jobsQueued);
    g_ses.maxGpu = std::max(g_ses.maxGpu, w.gpu.max());
    // the writer's own numbers
    double writeMs;
    uint64_t written;
    uint32_t dropped;
    size_t queued;
    LightLock_Lock(&g_w.lock);
    writeMs = g_w.writeMs;
    g_w.writeMs = 0;
    written = g_w.written;
    dropped = g_w.dropped;
    queued = g_w.qCsv.size() + g_w.qEv.size() + g_w.qRowsBytes;
    LightLock_Unlock(&g_w.lock);
    g_ses.droppedRows = dropped;

    // --- the row
    static Row r;
    r.begin();
    r.num("t_s", sinceStartS(), 1);
    r.num("t_win_ms", winMs, 0);
    r.text("phase", c.phase);
    r.integer("map", c.map);
    r.integer("tile_x", c.tileX);
    r.integer("tile_y", c.tileY);
    r.num("x", c.pos[0], 1); r.num("y", c.pos[1], 1); r.num("z", c.pos[2], 1);
    r.num("yaw", c.yaw, 2);
    r.text("move", c.move);
    r.integer("online", c.online);
    // frames
    r.integer("f_frames", w.frames);
    r.num("f_fps", fps, 1);
    r.num("f_ms_avg", w.frame.avg()); r.num("f_ms_min", w.frame.min()); r.num("f_ms_p50", p50); r.num("f_ms_p90", p90);
    r.num("f_ms_max", w.frame.max());
    r.integer("f_over33", w.over33); r.integer("f_over50", w.over50); r.integer("f_over100", w.over100);
    r.integer("f_hitches", w.hitches);
    r.text("f_worst_phase", w.worstPhase >= 0 ? kPhaseNames[w.worstPhase] : "");
    r.num("f_cpu_avg", w.cpu.avg()); r.num("f_cpu_max", w.cpu.max());
    r.num("f_wait_avg", w.wait.avg()); r.num("f_wait_max", w.wait.max());
    r.num("f_gpu_avg", w.gpu.avg()); r.num("f_gpu_max", w.gpu.max());
    r.num("f_c3dcpu_avg", w.c3dCpu.avg()); r.num("f_c3dcpu_max", w.c3dCpu.max());
    r.num("f_cmdbuf_max", w.cmdBuf.max(), 1);
    // stage 6.3: early GPU kicks - how many a frame, the GPU's head start before FrameEnd (f_gpu_* count from FrameEnd:
    // the GPU's whole span is about f_gpu_avg + f_kick_lead), their CPU cost
    r.num("f_kicks", w.kicks.avg(), 1);
    r.num("f_kick_lead", w.kickLead.avg(), 2);
    r.num("f_gpu_span", w.gpu.avg() + w.kickLead.avg(), 2);
    r.num("f_kick_ms", w.kickMs.avg(), 3);
    r.text("f_bound", bound);
    // phases (ms per frame)
    char name[48];
    for (int p = 0; p < kPhases; ++p) { snprintf(name, sizeof name, "ph_%s", kPhaseNames[p]); r.num(name, w.phase[p].avg(), 3); }
    for (int p = 0; p < kPhases; ++p) { snprintf(name, sizeof name, "phx_%s", kPhaseNames[p]); r.num(name, w.phase[p].max(), 2); }
    // render passes
    for (int k = 0; k < FrameCounters::kPasses; ++k) { snprintf(name, sizeof name, "r_%s_ms", kPassNames[k]); r.num(name, w.passMs[k].avg(), 3); }
    for (int k = 0; k < FrameCounters::kPasses; ++k) { snprintf(name, sizeof name, "r_%s_draws", kPassNames[k]); r.num(name, w.passDraws[k].avg(), 0); }
    for (int k = 0; k < FrameCounters::kPasses; ++k) { snprintf(name, sizeof name, "r_%s_tris", kPassNames[k]); r.num(name, w.passTris[k].avg(), 0); }
    r.num("r_eyes", w.eyes.avg(), 1);
    r.num("r_draws", w.draws.avg(), 0); r.num("r_draws_max", w.draws.max(), 0);
    r.num("r_tris", w.tris.avg(), 0); r.num("r_tris_max", w.tris.max(), 0);
    r.num("r_binds", w.binds.avg(), 0);
    r.num("r_tbatches", w.batches.avg(), 0);
    r.num("r_lod0", w.lod[0].avg(), 0); r.num("r_lod1", w.lod[1].avg(), 0); r.num("r_lod2", w.lod[2].avg(), 0);
    r.num("r_wbatches", w.water.avg(), 0);
    r.num("r_doodads", w.inst.avg(), 0);
    r.num("r_buildings", w.bld.avg(), 0);
    r.num("r_mtx_ms", w.mtxMs.avg(), 3);   // stage 6.10: the instances' matrices, once a frame
    r.num("r_chunks_cull", w.chCull.avg(), 0); r.num("r_chunks_test", w.chTest.avg(), 0);
    r.num("r_fading", w.fading.avg(), 1);
    // stage 6.12: draws of models and units by mesh level (0 = full), far units and how many of them reused a pose
    r.num("r_mlod0", w.mlod0.avg(), 0); r.num("r_mlod1", w.mlod1.avg(), 0); r.num("r_mlod2", w.mlod2.avg(), 0);
    r.num("r_far_units", w.farUnits.avg(), 1); r.num("r_shared_poses", w.sharedPoses.avg(), 1);   // stage 6.11: instances drawn alpha-blended (appearing / edge of the distance)
    r.num("r_markers", w.markers.avg(), 0);
    r.num("r_units", w.units.avg(), 1);
    r.num("r_bone_uploads", w.bones.avg(), 0);
    r.num("r_pose_ms", w.pose.avg(), 3); r.num("r_pose_max", w.pose.max(), 2);
    r.num("r_fxsim_ms", w.fxMs.avg(), 3); r.num("r_fxsim_max", w.fxMs.max(), 2);   // the particle simulation inside the fx pass
    r.num("r_fx_quads", w.fxQuads.avg(), 0); r.num("r_fx_particles", w.fxParticles.avg(), 0);
    r.num("r_fx_glows", w.fxGlows.avg(), 0); r.num("r_fx_sources", w.fxSources.avg(), 0);
    r.integer("r_fx_alive", fxs.alive);
    r.num("r_fx_emit_s", (double)(fxs.emitted - g_prevFx.emitted) / winS, 0);
    r.num("r_fx_drop_s", (double)(fxs.dropped - g_prevFx.dropped) / winS, 0);
    // the streamer: the main thread's cost by stage (ms per frame)
    r.num("st_upd_ms", w.stUpd.avg(), 3); r.num("st_upd_max", w.stUpd.max(), 2);
    r.num("st_done_ms", w.stDone.avg(), 3); r.num("st_done_max", w.stDone.max(), 2);
    r.num("st_players_ms", w.stPlayers.avg(), 3); r.num("st_players_max", w.stPlayers.max(), 2);
    r.num("st_upload_ms", w.stUpload.avg(), 3); r.num("st_upload_max", w.stUpload.max(), 2);
    r.num("st_tiles_ms", w.stTiles.avg(), 3); r.num("st_tiles_max", w.stTiles.max(), 2);
    r.num("st_sched_ms", w.stSched.avg(), 3); r.num("st_sched_max", w.stSched.max(), 2);
    r.num("st_defer_ms", w.stDefer.avg(), 3); r.num("st_defer_max", w.stDefer.max(), 2);
    r.integer("st_reads_done", w.stJobsDone);
    r.integer("st_uploads", w.stUploads);
    r.num("st_upload_kb", kb((double)w.stUploadBytes), 0);
    // the streamer's state
    r.integer("st_tiles_ready", s.tilesReady); r.integer("st_tiles_loading", s.tilesLoading);
    r.integer("st_tiles_complete", m.tilesComplete);
    r.integer("st_slots_used", s.slotsUsed); r.integer("st_slots", s.slotsTotal);
    r.integer("st_queue", s.jobsQueued); r.num("st_queue_kb", kb((double)s.queuedBytes), 0);
    for (int k = 0; k < kStreamClasses; ++k) { snprintf(name, sizeof name, "st_q_%s", streamerClassName(k)); r.integer(name, s.queuedByClass[k]); }
    r.integer("st_done_waiting", s.ioDoneWaiting);
    r.integer("st_uploads_pending", s.uploadsPending);
    r.integer("st_models", m.models); r.integer("st_models_loading", m.modelsLoading); r.integer("st_models_wait_tex", s.modelsWaiting);
    r.integer("st_models_failed", m.modelsFailed);
    r.integer("st_buildings", m.buildings); r.integer("st_interiors", m.interiors); r.integer("st_doodads", m.doodads);
    r.integer("st_fx_models", m.fxModels);
    // stage 6.11: buildings (placements sharing a transform) shown whole: closed now, opened this second with every group / late
    r.integer("st_bldg_closed", s.bldgClosed); r.integer("st_bldg_full", s.bldgOpenedFull - ps.bldgOpenedFull);
    r.integer("st_bldg_late", s.bldgOpenedLate - ps.bldgOpenedLate); r.num("st_bldg_wait_max", s.bldgWaitMsMax, 0);
    r.integer("st_textures", m.textures); r.integer("st_tex_vram_n", m.texVramCount); r.integer("st_tex_linear_n", m.texLinearCount);
    r.integer("st_tex_white", m.texWhiteNow);
    r.integer("st_displays", m.displays); r.integer("st_player_displays", m.playerDisplays); r.integer("st_players_pending", m.pendingPlayers);
    r.integer("st_creature_models", m.creatureModels); r.integer("st_items", m.items);
    r.integer("st_deferred", m.deferred);
    r.integer("st_instances", m.instances);
    const uint32_t loads = s.tileLoads - ps.tileLoads, full = s.tileFullLoads - ps.tileFullLoads;
    r.integer("st_ground_n", loads);
    r.num("st_ground_ms", loads ? (s.sumTileMs - ps.sumTileMs) / loads : 0.0, 0);
    r.integer("st_full_n", full);
    r.num("st_full_ms", full ? (s.sumTileFullMs - ps.sumTileFullMs) / full : 0.0, 0);
    r.integer("st_cancelled", s.jobsCancelled - ps.jobsCancelled);
    r.integer("st_composites", s.composites - ps.composites);
    // the SD card / the IO thread
    const uint32_t reads = s.ioReads - ps.ioReads;
    const double ioMs = s.ioReadMs - ps.ioReadMs;
    const double ioBytes = (double)(s.ioBytes - ps.ioBytes);
    r.integer("io_reads", reads);
    r.num("io_kb", kb(ioBytes), 0);
    r.num("io_kbps_busy", ioMs > 0.5 ? kb(ioBytes) / (ioMs / 1000.0) : 0.0, 0);   // the card's speed while reading
    r.num("io_busy_pct", std::min(100.0, ioMs / winMs * 100.0), 1);   // (reads are counted when collected: a long one spills over)
    r.num("io_cpu_ms", s.ioCpuMs - ps.ioCpuMs, 1);
    r.num("io_avg_kb", reads ? kb(ioBytes) / reads : 0.0, 1);
    r.num("io_max_read_ms", streamerTakeMaxReadMs(), 1);
    r.integer("io_fails", s.ioFails - ps.ioFails);
    for (int k = 0; k < kStreamJobKinds; ++k) { snprintf(name, sizeof name, "io_kb_%s", streamerJobKindName(k)); r.num(name, kb((double)(s.ioBytesKind[k] - ps.ioBytesKind[k])), 0); }
    for (int k = 0; k < kStreamJobKinds; ++k) { snprintf(name, sizeof name, "io_ms_%s", streamerJobKindName(k)); r.num(name, s.ioReadMsKind[k] - ps.ioReadMsKind[k], 1); }
    // what did not fit (per second) and the last failed allocation
    r.integer("oom_staging", s.oomStaging - ps.oomStaging);
    r.integer("oom_model", s.oomModel - ps.oomModel);
    r.integer("oom_creature", s.oomCreature - ps.oomCreature);
    r.integer("oom_water", s.oomWater - ps.oomWater);
    r.integer("oom_upload", s.oomUpload - ps.oomUpload);
    r.integer("oom_coll", s.collFailed - ps.collFailed);
    r.num("oom_last_kb", kb(s.oomLastSize), 0);
    r.num("oom_last_free_kb", kb(s.oomLastFree), 0);
    r.integer("tex_vram_miss", s.texVramMiss - ps.texVramMiss);
    r.num("tex_vram_miss_kb", kb((double)(s.texVramMissBytes - ps.texVramMissBytes)), 0);
    r.integer("tex_waited", s.texOomTotal - ps.texOomTotal);
    r.integer("tex_not_in_pack", s.texNotInPack - ps.texNotInPack);
    r.integer("tex_read_failed", s.texReadFailed - ps.texReadFailed);
    r.integer("coll_heap_miss", s.collHeapMiss - ps.collHeapMiss);
    // linear memory (KB)
    r.num("lin_total", kb(linTotal), 0); r.num("lin_free", kb(linFree), 0); r.num("lin_used", kb(linUsed), 0);
    r.num("lin_pool", kb(m.poolBytes), 0); r.num("lin_pool_used", kb(m.poolUsedBytes), 0);
    r.num("lin_water", kb(m.waterBytes), 0);
    r.num("lin_buildings", kb(m.buildingBytes), 0); r.num("lin_doodads", kb(m.doodadBytes), 0);
    r.num("lin_unit_models", kb(m.unitModelBytes), 0); r.num("lin_models_loading", kb(m.modelLoadingBytes), 0);
    r.num("lin_creatures", kb(m.creatureBytes), 0);
    r.num("lin_tex", kb(texLin), 0);
    r.num("lin_staging", kb(staging), 0);
    r.num("lin_staging_retry", kb(m.stagingRetryBytes), 0);
    r.num("lin_coll", kb(m.collLinearBytes), 0);
    r.num("lin_sound", kb(sndLin), 0);
    r.num("lin_ui", kb(uiLin), 0);
    r.num("lin_cmdbuf", kb(c.cmdBufBytes), 0);
    r.num("lin_other", kb(linUsed > linKnown ? linUsed - linKnown : 0), 0);
    for (int k = 0; k < kStreamClasses; ++k) { snprintf(name, sizeof name, "lin_tex_%s", streamerClassName(k)); r.num(name, kb(m.texLinearByClass[k]), 0); }
    r.num("lin_tex_bodies", kb(m.bodyLinearBytes), 0);
    // VRAM (KB)
    r.num("vram_free", kb(vFree), 0); r.num("vram_largest", kb(vLargest), 0);
    r.num("vram_tex", kb(texVram), 0);
    r.num("vram_targets", kb(c.vramTargets), 0); r.num("vram_reserve", kb(c.vramReserve), 0); r.num("vram_portraits", kb(c.vramPortraits), 0);
    r.num("vram_other", kb(vUsed > vramKnown ? vUsed - vramKnown : 0), 0);
    for (int k = 0; k < kStreamClasses; ++k) { snprintf(name, sizeof name, "vram_tex_%s", streamerClassName(k)); r.num(name, kb(m.texVramByClass[k]), 0); }
    r.num("vram_tex_bodies", kb(m.bodyVramBytes), 0);
    // the heap (KB)
    r.num("heap_size", kb(__ctru_heap_size), 0); r.num("heap_used", kb(heapUsed), 0); r.num("heap_free", kb(heapFree), 0);
    r.num("heap_arena", kb(heapArena), 0); r.num("heap_arena_free", kb((double)mi.fordblks), 0);
    r.num("heap_top", kb((double)mi.keepcost), 0);
    r.num("heap_heights", kb(m.heightBytes), 0); r.num("heap_instances", kb(m.instanceBytes), 0);
    r.num("heap_coll", kb(m.collHeapBytes), 0); r.num("heap_liquid", kb(m.liquidBytes), 0);
    r.num("heap_charimg", kb(m.charImgBytes + m.charInfoBytes), 0); r.num("heap_index", kb(m.indexBytes), 0);
    r.num("heap_other", kb(heapUsed > heapKnown ? heapUsed - heapKnown : 0), 0);
    r.num("app_used", kb(osGetMemRegionUsed(MEMREGION_APPLICATION)), 0);
    r.num("app_free", kb(osGetMemRegionFree(MEMREGION_APPLICATION)), 0);
    // the interface
    r.num("ui_quads", w.frames ? (double)(u.quads - g_prevUi.quads) / w.frames : 0.0, 0);
    r.num("ui_batches", w.frames ? (double)(u.batches - g_prevUi.batches) / w.frames : 0.0, 0);
    r.integer("ui_overflow", u.overflow - g_prevUi.overflow);
    r.integer("ui_pages", u.resident); r.num("ui_pages_kb", kb(u.bytes), 0);
    r.integer("ui_loads", u.loads - g_prevUi.loads);
    r.num("ui_load_ms", u.loadMs - g_prevUi.loadMs, 1);
    r.integer("ui_evictions", u.evictions - g_prevUi.evictions);
    r.integer("ui_failed", u.failed - g_prevUi.failed);
    r.integer("ui_async_loads", u.asyncLoads - g_prevUi.asyncLoads);   // stage 6.2: pages read by the loader thread
    r.integer("ui_reading", u.queued);
    r.integer("ui_sync_loads", u.syncLoads - g_prevUi.syncLoads);       // read on the main thread (skin, fonts)
    r.num("ui_sync_ms", u.syncLoadMs - g_prevUi.syncLoadMs, 1);
    // sound
    r.integer("snd_on", snd.on);
    r.num("snd_cache_kb", kb(snd.cacheBytes), 0); r.integer("snd_clips", snd.cacheClips);
    r.integer("snd_decodes", snd.cacheMisses - g_prevSnd.cacheMisses);
    r.integer("snd_fails", snd.cacheFails - g_prevSnd.cacheFails);
    r.integer("snd_playing", snd.worldPlaying);
    r.num("snd_busy_pct", (double)(uint32_t)(snd.busyUs - g_prevSnd.busyUs) / 1000.0 / winMs * 100.0, 1);
    r.integer("snd_requests", snd.requests - g_prevSnd.requests);
    r.integer("snd_dropped", snd.requestsDropped - g_prevSnd.requestsDropped);
    // the network
    const Context& pc = g_prevCtx;
    const bool sameNet = c.online && pc.online && c.netBytesIn >= pc.netBytesIn;
    r.num("net_in_kbps", sameNet ? kb((double)(c.netBytesIn - pc.netBytesIn)) / winS : 0.0, 2);
    r.num("net_out_kbps", sameNet ? kb((double)(c.netBytesOut - pc.netBytesOut)) / winS : 0.0, 2);
    r.integer("net_pk_in", sameNet ? (long long)(c.netPacketsIn - pc.netPacketsIn) : 0);
    r.integer("net_pk_out", sameNet ? (long long)(c.netPacketsOut - pc.netPacketsOut) : 0);
    r.num("net_pending_kb", kb(c.netPending), 1);
    r.integer("net_ping", c.latencyMs); r.integer("net_pongs", c.pongs);
    r.integer("net_objects", c.objects); r.integer("net_players", c.players); r.integer("net_units", c.units); r.integer("net_gobjects", c.gameObjects);
    r.integer("net_updates", sameNet ? (long long)c.updatePackets - pc.updatePackets : 0);
    r.integer("net_update_fails", sameNet ? (long long)c.updateFailed - pc.updateFailed : 0);
    r.integer("net_monster_moves", sameNet ? (long long)c.monsterMoves - pc.monsterMoves : 0);
    r.integer("net_moves_sent", sameNet ? (long long)c.moveSent - pc.moveSent : 0);
    r.integer("wifi", osGetWifiStrength());
    // the world and the settings
    r.num("w_dist", c.drawDist, 0); r.integer("w_doodads", c.doodads); r.num("w_doodad_range", c.doodadRange, 0);
    r.integer("w_fog", c.fog); r.integer("w_effects", c.effects); r.integer("w_crowd", c.crowdCap); r.integer("w_top", c.topMode);
    r.num("w_slider", c.slider, 2); r.integer("w_stereo", c.stereo); r.integer("w_underwater", c.underwater);
    r.integer("w_units_seen", c.unitsSeen); r.integer("w_units_drawn", c.unitsDrawn); r.integer("w_units_ready", c.unitsReady);
    r.integer("w_units_tracked", c.unitsTracked); r.integer("w_markers", c.markers);
    r.integer("w_spells", c.spellActive); r.integer("w_spell_models", c.spellModels); r.integer("w_auras", c.auras);
    r.integer("w_channels", c.channels); r.integer("w_combat", c.inCombat);
    r.integer("w_gpukick", c.gpuKick);
    r.integer("w_vsync", c.vsync);
    r.integer("w_fadein", c.fadeIn);
    // the log itself
    r.num("log_write_ms", writeMs, 1);
    r.num("log_kb", kb((double)written), 0);
    r.num("log_queue_kb", kb((double)queued), 1);
    r.integer("log_dropped", dropped);
    r.num("log_mem_ms", memMs, 2);   // the memory walk in this row (0: the numbers are from the last walk, up to 5 s old)
    r.num("log_cost_ms", ms(svcGetSystemTick() - t0), 2);
    if (r.headerChanged()) {   // as a row of one text field: it keeps its place among the rows
        Row::Val h;
        h.kind = 2;
        h.s = (g_ses.seconds > 1 ? "# new columns\n" : "") + r.header();
        h.s.pop_back();   // (format() ends the line)
        std::vector<Row::Val> hv;
        hv.push_back(std::move(h));
        g_w.pushRow(std::move(hv));
    }
    g_w.pushRow(r.takeValues());   // printed by the writer thread

    // --- events from this second's numbers
    if (g_ses.lastPhase != c.phase) {
        event("phase: %s -> %s", g_ses.lastPhase.empty() ? "(start)" : g_ses.lastPhase.c_str(), c.phase);
        g_ses.lastPhase = c.phase;
    }
    if (c.map != g_ses.lastMap) {
        if (g_ses.lastMap != ~0u) event("map %lu -> %lu (tile %d_%d)", (unsigned long)g_ses.lastMap, (unsigned long)c.map, c.tileX, c.tileY);
        g_ses.lastMap = c.map;
    }
    if (c.online != g_ses.wasOnline) {
        event(c.online ? "online (in the world)" : "offline (left the world / disconnected)");
        g_ses.wasOnline = c.online;
    }
    const std::string set = settingsLine(c);
    if (set != g_ses.lastSettings) { event("settings: %s", set.c_str()); g_ses.lastSettings = set; }
    for (LowWatch& lw : g_linWatch)
        if (lw.check(kb(linFree)))
            event("LINEAR MEMORY LOW: %lu KB free (under %.0f KB); textures %lu KB, models %lu KB, staging %lu KB, pool %lu KB, other %lu KB",
                  (unsigned long)(linFree / 1024), lw.threshold, (unsigned long)(texLin / 1024),
                  (unsigned long)((m.buildingBytes + m.doodadBytes + m.unitModelBytes + m.creatureBytes) / 1024), (unsigned long)(staging / 1024),
                  (unsigned long)(m.poolBytes / 1024), (unsigned long)((linUsed > linKnown ? linUsed - linKnown : 0) / 1024));
    if (g_vramWatch.check(kb(vLargest)))
        event("VRAM: largest free block %lu KB (free %lu KB): new textures go to linear memory (slower to sample)", (unsigned long)(vLargest / 1024),
              (unsigned long)(vFree / 1024));
    if (g_heapWatch.check(kb(heapFree)))
        event("HEAP LOW: %lu KB of %lu KB left (COLL %lu KB, heights %lu KB, instances %lu KB, other %lu KB)", (unsigned long)(heapFree / 1024),
              (unsigned long)(__ctru_heap_size / 1024), (unsigned long)(m.collHeapBytes / 1024), (unsigned long)(m.heightBytes / 1024),
              (unsigned long)(m.instanceBytes / 1024), (unsigned long)((heapUsed > heapKnown ? heapUsed - heapKnown : 0) / 1024));
    const uint32_t oomNow = (s.oomStaging - ps.oomStaging) + (s.oomModel - ps.oomModel) + (s.oomCreature - ps.oomCreature) +
                            (s.oomWater - ps.oomWater) + (s.oomUpload - ps.oomUpload) + (s.collFailed - ps.collFailed);
    if (oomNow)
        event("OUT OF MEMORY x%lu (staging %lu, model %lu, creature %lu, water %lu, upload %lu, coll %lu): last asked %lu KB with %lu KB linear free%s",
              (unsigned long)oomNow, (unsigned long)(s.oomStaging - ps.oomStaging), (unsigned long)(s.oomModel - ps.oomModel),
              (unsigned long)(s.oomCreature - ps.oomCreature), (unsigned long)(s.oomWater - ps.oomWater), (unsigned long)(s.oomUpload - ps.oomUpload),
              (unsigned long)(s.collFailed - ps.collFailed), (unsigned long)(s.oomLastSize / 1024), (unsigned long)(s.oomLastFree / 1024),
              s.oomLastFree > s.oomLastSize * 2 ? " (FRAGMENTED: much free, no block large enough)" : "");
    if (m.texWhiteNow && !g_whiteWarned) { event("textures white while waiting for memory: %lu", (unsigned long)m.texWhiteNow); g_whiteWarned = true; }
    if (!m.texWhiteNow && g_whiteWarned) { event("no white textures any more"); g_whiteWarned = false; }
    if (w.cmdBuf.max() > 85.0 && !g_cmdBufWarned) { event("GPU command buffer %.0f%% full (2 MB; over 100%% = crash)", w.cmdBuf.max()); g_cmdBufWarned = true; }
    if (w.cmdBuf.max() < 70.0) g_cmdBufWarned = false;
    if (u.overflow != g_prevUi.overflow) event("interface: %lu quads did not fit in the vertex buffer (not drawn)", (unsigned long)(u.overflow - g_prevUi.overflow));
    if (snd.cacheFails != g_prevSnd.cacheFails) event("sound: %lu world sounds found no memory (cache %lu KB)", (unsigned long)(snd.cacheFails - g_prevSnd.cacheFails),
                                                   (unsigned long)(snd.cacheBytes / 1024));
    if (snd.requestsDropped != g_prevSnd.requestsDropped) event("sound: %lu requests dropped (queue full)", (unsigned long)(snd.requestsDropped - g_prevSnd.requestsDropped));
    if (sameNet && c.updateFailed != pc.updateFailed) event("network: %lu update packets failed to parse", (unsigned long)(c.updateFailed - pc.updateFailed));
    if (c.netPending > 64 * 1024) event("network: %lu KB received but not parsed yet", (unsigned long)(c.netPending / 1024));
    if (s.ioFails != ps.ioFails) event("SD card: %lu reads failed", (unsigned long)(s.ioFails - ps.ioFails));
    static uint32_t prevDropped = 0;
    if (dropped != prevDropped) { event("perf log: the writer fell behind, %lu rows / events dropped so far", (unsigned long)dropped); prevDropped = dropped; }

    g_prevSt = s;
    g_prevUi = u;
    g_prevSnd = snd;
    g_prevFx = fxs;
    g_prevCtx = c;
    w = Window();
    w.start = svcGetSystemTick();
    w.frameMs.reserve(128);
    // this call runs after endFrame(): its time belongs to the next frame's 'other', not to its input
    const u64 cost = svcGetSystemTick() - t0;
    g_state.cur[kOther] += cost;
    g_state.last += cost;
}

void shutdown() {
    if (!g_inited) return;
    dogStop();
    if (g_state.on) {
        Session& s = g_ses;
        std::string o = "\n== session summary\n";
        char b[512];
        snprintf(b, sizeof b, "%lu s logged (%lu in the world), %llu frames, %lu hitches over %.0f ms, %lu rows dropped by the writer\n",
                 (unsigned long)s.seconds, (unsigned long)s.worldSeconds, (unsigned long long)s.all.n, (unsigned long)s.hitches, kHitchMs,
                 (unsigned long)s.droppedRows);
        o += b;
        auto hist = [&](const char* what, const Histogram& h) {
            if (!h.n) return;
            snprintf(b, sizeof b, "%s: avg %.1f FPS (%.2f ms), p50 %.1f ms, p90 %.1f, p99 %.1f, p99.9 %.1f, worst %.1f; over 33 ms %llu (%.1f%%), "
                                  "over 50 ms %llu, over 100 ms %llu\n",
                     what, 1000.0 * (double)h.n / std::max(h.sum, 1.0), h.sum / (double)h.n, h.percentile(0.5), h.percentile(0.9), h.percentile(0.99),
                     h.percentile(0.999), h.mx, (unsigned long long)h.over(33.5), 100.0 * (double)h.over(33.5) / (double)h.n,
                     (unsigned long long)h.over(50.0), (unsigned long long)h.over(100.0));
            o += b;
        };
        hist("all frames", s.all);
        hist("in the world", s.world);
        if (s.worstFps < 1e8) {
            snprintf(b, sizeof b, "worst second in the world: %.1f FPS at %.0f s, %s\n", s.worstFps, s.worstFpsAt, s.worstFpsWhere);
            o += b;
        }
        if (s.phaseFrames) {
            o += "where the frame goes (ms per frame, whole session):";
            for (int p = 0; p < kPhases; ++p)
                if (s.phaseSum[p] / (double)s.phaseFrames >= 0.05) {
                    snprintf(b, sizeof b, " %s %.2f", kPhaseNames[p], s.phaseSum[p] / (double)s.phaseFrames);
                    o += b;
                }
            o += "\n";
        }
        const StreamStats& st = streamerStats();
        snprintf(b, sizeof b, "memory low points: linear free %.0f KB, VRAM free %.0f KB (largest block %.0f KB), heap free %.0f KB; "
                              "command buffer peak %.0f%%; GPU peak %.1f ms\n",
                 kb(s.minLinFree), kb(s.minVramFree), kb(s.minVramLargest), kb(s.minHeapFree), s.maxCmdBuf, s.maxGpu);
        o += b;
        snprintf(b, sizeof b, "shortages (whole session): staging %lu, models %lu, creatures %lu, water %lu, uploads %lu, COLL %lu; "
                              "textures to linear (VRAM full) %lu (%.0f KB); moved out of VRAM for 3D / wide %lu; white textures peak %lu; read queue peak %lu\n",
                 (unsigned long)st.oomStaging, (unsigned long)st.oomModel, (unsigned long)st.oomCreature, (unsigned long)st.oomWater,
                 (unsigned long)st.oomUpload, (unsigned long)st.collFailed, (unsigned long)st.texVramMiss, kb((double)st.texVramMissBytes),
                 (unsigned long)st.texEvicted, (unsigned long)s.maxWhite, (unsigned long)s.maxQueue);
        o += b;
        snprintf(b, sizeof b, "SD card: %lu reads, %.1f MB, %.1f s reading (%.0f KB/s while reading), %lu failed\n", (unsigned long)st.ioReads,
                 st.ioBytes / 1048576.0, st.ioReadMs / 1000.0, st.ioReadMs > 1 ? kb((double)st.ioBytes) / (st.ioReadMs / 1000.0) : 0.0,
                 (unsigned long)st.ioFails);
        o += b;
        g_w.push(false, o);
        g_state.on = false;
        g_w.stop();
    }
    g_inited = false;
}

}  // namespace perf
