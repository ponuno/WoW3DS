// perflog.hpp — the perf log (stage 6): one CSV row a second with everything that costs time or memory, and a text file
// of events (hitches, memory running out, settings and map changes, a session summary at the end).
//
//   sdmc:/3ds/w3dproto/perf.csv          one row per second (column legend: docs/PERFLOG.md)
//   sdmc:/3ds/w3dproto/perf_events.txt   the session's header (console, memory layout), events, the summary
//   (the previous session's files are kept as perf_prev.csv / perf_prev_events.txt)
//
// The main loop marks where its time goes: lap(phase) after each part (the time since the previous mark goes to that
// phase), Scope for a block that blocks (a map load), endFrame() at the bottom, which also starts the next frame's marks
// (frameStart() is for the first frame; init / setEnabled call it). Once a second (due()) the main loop fills a Context
// and calls second(); its own time goes to the next frame's 'other'. A low-priority thread writes the files, so the SD
// card's write time stays out of the frame.
#pragma once
#include <3ds.h>

#include <cstdint>

struct FrameCounters;

namespace perf {

enum Phase : uint8_t {
    kInput,      // hidScanInput, sticks
    kNet,        // the login flow: its screens and the network (server packets are parsed here)
    kUi,         // the touch screen's logic, the controller scheme, zone music, map-change checks
    kMove,       // camera, movement, collision, swimming, ships
    kSpells,     // spell effects (SpellView)
    kSound,      // world sounds, voices
    kGame,       // GameSession::update, units -> creature models, markers
    kHud,        // the top screen's HUD list
    kTouch,      // the touch screen's draw list
    kGlue,       // the login screens' 3D scene
    kStream,     // streamerUpdate (reads collected, texture uploads, tiles)
    kPrepare,    // stage 6.4: rendererPrepare - the visibility pass, before the wait for the GPU
    kWait,       // C3D_FrameBegin: waiting for the vertical blank and for the GPU to finish the previous frame
    kPortrait,   // the unit frames' portraits (their own render targets)
    kWorld,      // rendererDraw (both eyes in 3D): building the GPU command list
    kHudDraw,    // uiDraw of the top screen
    kTouchDraw,  // uiDraw of the touch screen
    kFlush,      // C3D_FrameEnd, uiEndFrame, streamerEndFrame (deferred frees)
    kMapLoad,    // blocking waits of a map change / teleport (Scope)
    kOther,      // the rest: report counters, the debug page, the perf log itself
    kPhases
};
const char* phaseName(int p);

// --- per frame (cheap; nothing happens while the log is off)
struct State {
    bool on = false;
    u64 last = 0, start = 0;
    u64 cur[kPhases] = {};
    // stage 6.6: for the freeze watchdog (another thread reads them): frames finished, the last finished phase, the last
    // mark - a fixed string the main thread passes on its way (mark())
    volatile uint32_t frames = 0;
    volatile uint8_t lastLap = 0;
    const char* volatile where = "start";
};
extern State g_state;
// Stage 6.6: where the main thread is (a pointer store; the watchdog names it if no frame finishes for 3 s).
// 6.35b: and libctru's VRAM allocator watched at every mark - its pool reset to "all free" (overwritten) is found between the
// two marks it happened between (perf_events "VRAM JUMP")
void vramWatch(const char* where);
inline void mark(const char* where) { vramWatch(where); g_state.where = where; }
inline void frameStart() {
    if (!g_state.on) return;
    g_state.start = g_state.last = svcGetSystemTick();
    for (u64& c : g_state.cur) c = 0;
}
inline void lap(Phase p) {
    g_state.lastLap = (uint8_t)p;
    if (!g_state.on) return;
    const u64 t = svcGetSystemTick();
    g_state.cur[p] += t - g_state.last;
    g_state.last = t;
}
// A block charged to one phase whatever lap comes next (map loads: their waits are not the phase they happen in).
struct Scope {
    Phase p;
    u64 t0;
    explicit Scope(Phase ph) : p(ph), t0(svcGetSystemTick()) {}
    ~Scope() {
        if (!g_state.on) return;
        const u64 d = svcGetSystemTick() - t0;
        g_state.cur[p] += d;
        g_state.last += d;
    }
};
void endFrame(const FrameCounters& fc);

// --- once a second
bool due();
struct Context {
    const char* phase = "";         // login / loading / portal / world / offline / bench
    uint32_t map = 0;
    int tileX = 0, tileY = 0;
    float pos[3] = {0, 0, 0}, yaw = 0, pitch = 0;
    const char* move = "";          // walk / floor / swim / fly / taxi / ship
    float drawDist = 0, doodadRange = 0;
    bool doodads = true, fog = true, underwater = false, stereo = false;
    int effects = 0, crowdCap = 0, topMode = 0, scheme = 0;
    float slider = 0;
    bool gpuKick = false;           // stage 6.3: the early GPU kick is on (the setting)
    bool fadeIn = true;             // stage 6.11: objects fade in (the setting)
    bool vsync = false;             // stage 6.4: waiting for a vertical blank before every frame (the setting)
    // units and effects
    int unitsSeen = 0, unitsDrawn = 0, unitsReady = 0, unitsTracked = 0, markers = 0;
    int spellActive = 0, spellModels = 0, auras = 0, channels = 0;
    bool inCombat = false;
    // network (cumulative counters; the log takes the differences)
    bool online = false;
    uint64_t netBytesIn = 0, netBytesOut = 0, netPacketsIn = 0, netPacketsOut = 0;
    uint32_t netPending = 0, latencyMs = 0, pongs = 0;
    uint32_t objects = 0, players = 0, units = 0, gameObjects = 0;
    uint32_t updatePackets = 0, updateFailed = 0, monsterMoves = 0, moveSent = 0;
    // VRAM the main loop owns (render targets, the reserve for 800x240 / 3D, portraits)
    uint32_t vramTargets = 0, vramReserve = 0, vramPortraits = 0;
    uint32_t cmdBufBytes = 0;       // citro3d's command buffer (linear)
};
void second(const Context& c);

// --- events (a line in perf_events.txt, with the time since the start)
void event(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// --- life
// dir: where the files go; on = write them (the setting can change it later)
void init(const char* dir, bool on);
void setEnabled(bool on);
bool enabled();
void shutdown();   // writes the summary, flushes and closes

}  // namespace perf
