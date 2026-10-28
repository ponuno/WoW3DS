// w3dproto — stage 3: streamed world (tiles load on the second core), sky,
// fog, water, ground collision, and a streaming benchmark.
//
// Stage 4 adds a login screen (bottom screen): server, account, realm and
// character selection; the camera then starts at the character's position.
//
// Stage 5 (online): walking moves the character on the server (MSG_MOVE_*,
// server speeds, B = jump); world objects from SMSG_UPDATE_OBJECT are drawn as
// coloured markers and listed on the bottom screen. SELECT switches to a free
// flying camera; the character stays where it stopped.
//
// Controls offline / free camera: Circle Pad move, C-Stick/D-Pad look, L/R down/up (fly), hold B fast,
//           SELECT walk/fly, X draw distance, A doodads, ZR fog, ZL doodad detail, Y 40 s streaming benchmark,
//           START quit. Online walking: the controller scheme of pad.hpp (Forever or Classic, START menu).
#include "audio.hpp"
#include "unit_state.hpp"
#include "sound_world.hpp"
#include "world.h"
#include "glue_data.hpp"
#include "login.hpp"
#include "creature_view.hpp"
#include "game_ui.hpp"
#include "hud_game.hpp"
#include "pad.hpp"
#include "portal.hpp"
#include "swim.hpp"
#include "action_layout.hpp"
#include "ui_assets.hpp"
#include "ui_render.hpp"

#include <math.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <vector>

#include <malloc.h>  // after <stdlib.h> (pulled in above): newlib declares calloc twice

#define DISPLAY_TRANSFER_FLAGS                                                                        \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                  \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |   \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
// The touch screen keeps the console's RGB565 framebuffer (login and debug pages print to it).
#define BOTTOM_TRANSFER_FLAGS                                                                         \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                  \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// Memory split: libctru caps the GPU-visible linear heap at 32 MB by default.
// Fixing the ordinary heap at 24 MB gives everything else (~90 MB on New 3DS)
// to the linear heap, where terrain, models and texture staging live.
extern "C" {
u32 __ctru_heap_size = 24 * 1024 * 1024;
}

static const char* kOutDir = "sdmc:/3ds/w3dproto";
static const float kDrawDists[5] = {120, 250, 400, 533, 800};

struct Bench {
    bool running = false;
    u64 startTick = 0;
    u32 warmup = 0;
    std::vector<float> frameMs, cpuMs, gpuMs;
    u32 maxTris = 0, maxDraws = 0, missingGroundFrames = 0;
    u32 tileLoadsAtStart = 0;
    double sumTileMsAtStart = 0;
    u32 minLinearFree = 0xFFFFFFFF;
};

struct Report {
    char packPath[128] = {0};
    int tilesInPack = 0;
    double firstFrameMs = 0;
    u32 linearAtStart = 0, vramAtStart = 0, appRegion = 0;
};
static Report g_report;
static LoginFlow g_login;

// Marker colours (0xAABBGGRR).
static const u32 kColUnit = 0xFF2896FF, kColNpc = 0xFF60E0A0, kColDead = 0xFF707070, kColPlayer = 0xFFFF8C3C,
                 kColSelf = 0xFF40FF40, kColObject = 0xFF3CDCE6;

// A fishing bobber (stage 5.26) rides the waves, and dips when a fish bites (SMSG_GAMEOBJECT_CUSTOM_ANIM).
static float goBob(const w3n::WorldObject& o, uint32_t nowMs) {
    if (o.field(w3n::UF::GAMEOBJECT_TYPE_ID) != 17) return 0.0f;
    float z = 0.05f * sinf((float)(nowMs % 2000) / 2000.0f * 6.2831853f);
    if (o.customAnimMs && nowMs - o.customAnimMs < 900) z -= 0.25f * sinf((float)(nowMs - o.customAnimMs) / 900.0f * 3.14159265f);
    return z;
}

// Units with a creature display in the pack, nearest first (models are drawn up to kCreatureRange).
static constexpr float kCreatureRange = 110.0f;
static constexpr size_t kMaxCreatures = 40;
// Players are included (stage 5.5): drawn from their race model + appearance when they are in
// their own shape (display == native display), else from the creature display (forms, ghosts).
// The own character only when the camera is free (in walking mode the camera is its eyes).
static void collectCreatures(w3n::GameSession& g, uint32_t nowMs, const float camPos[3], bool withSelf,
                             std::vector<CreatureSeen>& out) {
    out.clear();
    struct Cand { float d2; CreatureSeen c; };
    static std::vector<Cand> cand;
    cand.clear();
    for (auto& [guid, o] : g.objects.all()) {
        if (!o.hasPos || (o.self && !withSelf)) continue;
        if (o.type != w3n::ObjType::Unit && o.type != w3n::ObjType::Player) continue;
        CreatureSeen c;
        c.guid = guid;
        c.displayId = o.displayId();
        c.look = 0;
        c.lookParts = W3dLook{};
        if (o.type == w3n::ObjType::Player && o.fields.size() > w3n::UF::PLAYER_BYTES_2 &&
            c.displayId == o.field(w3n::UF::UNIT_NATIVEDISPLAYID)) {
            c.lookParts = w3d_look_from_fields(o.field(w3n::UF::UNIT_BYTES_0), o.field(w3n::UF::PLAYER_BYTES),
                                               o.field(w3n::UF::PLAYER_BYTES_2));
            if (c.lookParts.race) c.look = w3d_look_key(&c.lookParts);
        }
        if (!c.displayId && !c.look) continue;
        const bool taxi = o.taxiFlight() && o.pathActive;   // on a flight the server's spline is the truth, for us too
        if (o.self && !taxi) {   // our own position is the one we send, not the table's
            const w3n::MovementInfo& me = g.mover.info();
            c.pos[0] = me.x; c.pos[1] = me.y; c.pos[2] = me.z;
            c.yaw = me.o;
        } else {
            o.positionAt(nowMs, c.pos, &c.yaw);
        }
        float dx = c.pos[0] - camPos[0], dy = c.pos[1] - camPos[1], dz = c.pos[2] - camPos[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > kCreatureRange * kCreatureRange) continue;
        float scale = o.fieldF(w3n::UF::OBJECT_SCALE_X);
        c.scale = (scale > 0.05f && scale < 20.0f) ? scale : 1.0f;
        c.speed = o.self && !taxi ? ((g.mover.info().flags & w3n::MoveFlag::MaskMoving) ? g.mover.speeds.forFlags(g.mover.info().flags) : 0.0f)
                         : o.groundSpeedAt(nowMs);
        fillUnitState(g, o, nowMs, c);   // the animation stage: everything the unit is doing
        cand.push_back({d2, c});
    }
    // Ships and zeppelins (stage 5.21): GAMEOBJECT_TYPE_MO_TRANSPORT objects whose pose ObjectTable::tickTransports computed
    // from the path; their models are CDSP entries under 0x80000000 | GameObjectDisplayInfo id.
    for (auto& [guid, pose] : g.objects.transportPoses()) {
        const w3n::WorldObject* o = g.objects.find(guid);
        if (!o || !o->displayId()) continue;
        CreatureSeen c;
        c.guid = guid;
        c.displayId = 0x80000000u | o->displayId();
        c.pos[0] = pose.x; c.pos[1] = pose.y; c.pos[2] = pose.z;
        c.yaw = pose.o;
        float dx = c.pos[0] - camPos[0], dy = c.pos[1] - camPos[1], dz = c.pos[2] - camPos[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 4 * kCreatureRange * kCreatureRange) continue;
        float scale = o->fieldF(w3n::UF::OBJECT_SCALE_X);
        c.scale = (scale > 0.05f && scale < 20.0f) ? scale : 1.0f;
        cand.push_back({d2, c});
    }
    // Ordinary game objects (stage 5.23: event decorations, chests, lamps ...) whose model is in the pack; the others stay markers.
    for (auto& [guid, o] : g.objects.all()) {
        if (o.type != w3n::ObjType::GameObject || !o.hasPos || !o.displayId() || o.hasTransportProgress) continue;
        const uint32_t did = 0x80000000u | o.displayId();
        float p[3], yaw;
        o.positionAt(nowMs, p, &yaw);
        float dx = p[0] - camPos[0], dy = p[1] - camPos[1], dz = p[2] - camPos[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > kCreatureRange * kCreatureRange) continue;
        if (!streamerHasDisplay(did)) continue;
        CreatureSeen c;
        c.guid = guid;
        c.displayId = did;
        c.pos[0] = p[0]; c.pos[1] = p[1]; c.pos[2] = p[2] + goBob(o, nowMs);
        c.yaw = yaw;
        float scale = o.fieldF(w3n::UF::OBJECT_SCALE_X);
        c.scale = (scale > 0.05f && scale < 20.0f) ? scale : 1.0f;
        cand.push_back({d2, c});
    }
    size_t n = std::min(cand.size(), kMaxCreatures);
    std::partial_sort(cand.begin(), cand.begin() + n, cand.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
    for (size_t i = 0; i < n; ++i) out.push_back(cand[i].c);
}

static float rs_markerSpin(uint32_t nowMs) { return (float)(nowMs % 3000) / 3000.0f * 6.2831853f; }

static void buildMarkers(w3n::GameSession& g, uint32_t nowMs, const float camPos[3], bool showSelf, float range,
                         const CreatureView& view, std::vector<Marker>& out) {
    out.clear();
    const float r2 = range * range;
    for (auto& [guid, o] : g.objects.all()) {
        if (!o.hasPos || o.self) continue;
        bool unit = o.isUnitLike();
        if (!unit && o.type != w3n::ObjType::GameObject) continue;
        if (view.drawn(guid)) continue;   // drawn as an animated model
        Marker mk;
        float yaw;
        o.positionAt(nowMs, mk.pos, &yaw);
        if (!unit) mk.pos[2] += goBob(o, nowMs);
        float dx = mk.pos[0] - camPos[0], dy = mk.pos[1] - camPos[1];
        if (dx * dx + dy * dy > r2) continue;
        mk.yaw = yaw;
        float scale = o.fieldF(w3n::UF::OBJECT_SCALE_X);
        if (!(scale > 0.05f && scale < 20.0f)) scale = 1.0f;
        if (unit) {
            float rad = o.fieldF(w3n::UF::UNIT_BOUNDINGRADIUS);
            if (!(rad > 0.05f && rad < 10.0f)) rad = 0.4f;
            mk.height = 2.0f * scale;
            mk.width = std::max(0.35f, rad * scale);
            mk.color = o.dead() ? kColDead : o.type == w3n::ObjType::Player ? kColPlayer
                       : o.field(w3n::UF::UNIT_NPC_FLAGS) ? kColNpc : kColUnit;
        } else {
            mk.height = 0.8f * scale;
            mk.width = 0.5f * scale;
            mk.color = kColObject;
        }
        out.push_back(mk);
        if (out.size() >= 256) break;
    }
    if (const w3n::WorldObject* t = g.target() ? g.objects.find(g.target()) : nullptr) {   // the target: red diamond above it
        if (t->hasPos) {
            Marker mk;
            t->positionAt(nowMs, mk.pos);
            mk.pos[2] += view.drawn(g.target()) ? 2.6f : 2.3f;
            mk.yaw = rs_markerSpin(nowMs);
            mk.height = 0.5f;
            mk.width = 0.3f;
            mk.color = t->dead() ? 0xFF808080 : 0xFF2020FF;
            out.push_back(mk);
        }
    }
    if (showSelf && !view.drawn(g.objects.self())) {
        const w3n::MovementInfo& me = g.mover.info();
        Marker mk = {{me.x, me.y, me.z}, me.o, 2.0f, 0.45f, kColSelf};
        out.push_back(mk);
    }
}

static const char* findPack() {
    // map0.w3dp: the Eastern Kingdoms of 'w3dconv --all' (stage 5.27, with the shared pool common*.w3dp); test.w3dp: a region
    static const char* candidates[] = {"sdmc:/3ds/w3dproto/map0.w3dp", "sdmc:/3ds/w3dproto/test.w3dp", "sdmc:/3ds/w3dproto/synth.w3dp",
                                       "romfs:/synth.w3dp"};
    for (const char* p : candidates) {
        FILE* f = fopen(p, "rb");
        if (f) { fclose(f); return p; }
    }
    return nullptr;
}

static void writeReport(const Bench* b, float drawDist, const RenderSettings& rs) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(kOutDir, 0777);
    char path[128];
    snprintf(path, sizeof path, "%s/report.txt", kOutDir);
    FILE* f = fopen(path, "w");
    if (!f) return;
    const StreamStats& s = streamerStats();
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    fprintf(f, "w3dproto stage-3 report\npack: %s (%d tiles)\nconsole: %s\n", g_report.packPath, g_report.tilesInPack,
            n3ds ? "New 3DS" : "Old 3DS/2DS");
    fprintf(f, "IO thread: %s\n", s.workerCore == 2 ? "core 2" : s.workerCore == 1 ? "core 1 (30%)" :
                                  s.workerCore == 0 ? "core 0 (shared)" : "synchronous");
    fprintf(f, "terrain pool: %d slots\ntextures that had to wait for memory: %u total, %d now (drawn white until it frees)\ntextures white because not in the pack: %u, because the read failed: %u\n", s.slotsTotal,
            (unsigned)s.texOomTotal, s.texOom, (unsigned)s.texNotInPack, (unsigned)s.texReadFailed);
    fprintf(f, "time to first frame: %.0f ms\n", g_report.firstFrameMs);
    fprintf(f, "APPLICATION region available: %lu KB\n\n", (unsigned long)(g_report.appRegion / 1024));
    fprintf(f, "now resident: %d tiles, %d models, %d textures\n", s.tilesReady, s.models, s.textures);
    fprintf(f, "  textures %lu KB VRAM + %lu KB linear, geometry %lu KB\n", (unsigned long)(s.texVram / 1024),
            (unsigned long)(s.texLinear / 1024), (unsigned long)(s.geomBytes / 1024));
    fprintf(f, "  linear used since start %lu KB, VRAM used %lu KB\n",
            (unsigned long)((g_report.linearAtStart - linearSpaceFree()) / 1024),
            (unsigned long)((g_report.vramAtStart - vramSpaceFree()) / 1024));
    fprintf(f, "tile loads so far: %lu, avg %.0f ms, max %.0f ms (request -> drawable)\n\n", (unsigned long)s.tileLoads,
            s.tileLoads ? s.sumTileMs / s.tileLoads : 0.0, s.maxTileMs);
    if (g_login.authTimings().authTotal > 0) {
        const w3n::Timings& a = g_login.authTimings();
        const w3n::Timings& w = g_login.worldTimings();
        fprintf(f, "network (stage 4): login server connect %.0f ms, SRP %.0f ms, login total %.0f ms, realm list %.0f ms\n",
                a.authConnect, a.srp, a.authTotal, a.realmList);
        fprintf(f, "  world: connect %.0f ms, session auth %.0f ms, character list %.0f ms, enter world %.0f ms\n",
                w.worldConnect, w.worldAuth, w.charEnum, w.enterWorld);
        if (g_login.online()) {
            fprintf(f, "  in world as %s: map %lu, latency %lu ms, %llu bytes received, %u opcodes seen\n",
                    g_login.characterName().c_str(), (unsigned long)g_login.mapId(), (unsigned long)g_login.latencyMs(),
                    (unsigned long long)g_login.bytesReceived(), (unsigned)g_login.opcodesSeen());
            w3n::GameSession& g = g_login.game();
            const w3n::ObjectStats& os = g.objects.stats;
            fprintf(f, "  stage 5: objects %u (players %u, units %u, gameobjects %u), update packets %lu + %lu compressed, failed %lu%s%s\n",
                    (unsigned)g.objects.all().size(), (unsigned)g.objects.count(w3n::ObjType::Player),
                    (unsigned)g.objects.count(w3n::ObjType::Unit), (unsigned)g.objects.count(w3n::ObjType::GameObject),
                    (unsigned long)os.updatePackets, (unsigned long)os.compressedPackets, (unsigned long)os.failedPackets,
                    os.lastError.empty() ? "" : ", last error: ", os.lastError.c_str());
            fprintf(f, "  monster moves %lu (failed %lu), player moves %lu, own movement packets %lu, teleports %lu, speed changes %lu\n\n",
                    (unsigned long)os.monsterMoves, (unsigned long)os.monsterMoveFails, (unsigned long)os.playerMoves,
                    (unsigned long)g.mover.packetsSent, (unsigned long)g.stats.teleports, (unsigned long)g.stats.speedChanges);
        }
    }

    if (b && !b->frameMs.empty()) {
        std::vector<float> sorted = b->frameMs;
        std::sort(sorted.begin(), sorted.end());
        size_t n = sorted.size();
        double sum = 0, cpu = 0, gpu = 0;
        float cpuMax = 0, gpuMax = 0;
        u32 over33 = 0, over50 = 0;
        for (size_t i = 0; i < n; ++i) {
            sum += b->frameMs[i];
            cpu += b->cpuMs[i];
            gpu += b->gpuMs[i];
            cpuMax = std::max(cpuMax, b->cpuMs[i]);
            gpuMax = std::max(gpuMax, b->gpuMs[i]);
            over33 += b->frameMs[i] > 33.4f;
            over50 += b->frameMs[i] > 50.0f;
        }
        float p99 = sorted[std::min(n - 1, (size_t)(n * 0.99))];
        float worst = sorted.back();
        double avgFps = 1000.0 * n / sum;
        u32 loads = s.tileLoads - b->tileLoadsAtStart;
        double loadAvg = loads ? (s.sumTileMs - b->sumTileMsAtStart) / loads : 0;
        fprintf(f, "benchmark: 40 s circuit across tile borders (~59 yd/s), draw distance %.0f yd, doodads %s (range x%.0f), fog %s\n",
                drawDist, rs.doodads ? "on" : "off", rs.doodadRange, rs.fog ? "on" : "off");
        fprintf(f, "  frames %u: avg FPS %.1f, 1%% low %.1f FPS, worst frame %.1f ms\n", (unsigned)n, avgFps,
                1000.0f / p99, worst);
        fprintf(f, "  frames over 33 ms: %lu, over 50 ms: %lu\n", (unsigned long)over33, (unsigned long)over50);
        fprintf(f, "  CPU avg %.2f ms (max %.2f), GPU avg %.2f ms (max %.2f)\n", cpu / n, cpuMax, gpu / n, gpuMax);
        fprintf(f, "  peak tris/frame %lu, peak draw calls %lu\n", (unsigned long)b->maxTris, (unsigned long)b->maxDraws);
        fprintf(f, "  tiles streamed during run: %lu, avg latency %.0f ms\n", (unsigned long)loads, loadAvg);
        fprintf(f, "  frames with the camera's tile not yet loaded: %lu\n", (unsigned long)b->missingGroundFrames);
        fprintf(f, "  lowest free linear heap during run: %lu KB\n", (unsigned long)(b->minLinearFree / 1024));
        bool go = avgFps >= 25.0 && 1000.0f / p99 >= 20.0f && worst < 100.0f && b->missingGroundFrames == 0;
        fprintf(f, "\nGO/NO-GO (stage 3: avg >=25 FPS, 1%% low >=20, no frame >=100 ms, never standing on an unloaded tile): %s\n",
                go ? "GO" : "NO-GO");
        if (g_report.tilesInPack < 4)
            fprintf(f, "note: the pack has fewer than 4 tiles, so the run barely exercises streaming. Convert with --tiles.\n");

        snprintf(path, sizeof path, "%s/bench.csv", kOutDir);
        FILE* c = fopen(path, "w");
        if (c) {
            fprintf(c, "frame,frame_ms,cpu_ms,gpu_ms\n");
            for (size_t i = 0; i < n; ++i)
                fprintf(c, "%u,%.3f,%.3f,%.3f\n", (unsigned)i, b->frameMs[i], b->cpuMs[i], b->gpuMs[i]);
            fclose(c);
        }
    }
    fclose(f);
}

static void fail(const char* msg) {
    printf("\x1b[1;1HFailed:\n%s\n\nSTART to exit.", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

int main() {
    osSetSpeedupEnable(true);  // 804 MHz + L2 on New 3DS
    gfxInitDefault();
    romfsInit();
    PrintConsole bottom;
    consoleInit(GFX_BOTTOM, &bottom);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8);   // 2 MB: thousands of draws per frame

    C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    rendererInit();
    // UI stages 1/7: the touch screen through the GPU while in the game; ui.w3dp (optional) gives the skin
    C3D_RenderTarget* bottomT = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
    if (bottomT) C3D_RenderTargetSetOutput(bottomT, GFX_BOTTOM, GFX_LEFT, BOTTOM_TRANSFER_FLAGS);
    uiRenderInit();
    {
        char uiPath[96];
        snprintf(uiPath, sizeof uiPath, "%s/ui.w3dp", kOutDir);
        ui::assets().open(uiPath);
    }
    bool bottomGpu = false;   // the GPU drew the touch screen last (the console must clear it before printing)

    g_report.linearAtStart = linearSpaceFree();
    g_report.vramAtStart = vramSpaceFree();
    g_report.appRegion = osGetMemRegionSize(MEMREGION_APPLICATION);

    PackIndex index;
    char err[160] = "no pack found (put test.w3dp in sdmc:/3ds/w3dproto/)";
    const char* packPath = findPack();
    bool ok = packPath && index.open(packPath, err, sizeof err) && streamerInit(&index, err, sizeof err);
    if (!ok) {
        fail(err);
        rendererShutdown();
        C3D_Fini();
        romfsExit();
        gfxExit();
        return 0;
    }
    snprintf(g_report.packPath, sizeof g_report.packPath, "%s", packPath);
    for (const W3dEntry& e : index.entries) g_report.tilesInPack += e.type == W3D_ENTRY_TERRAIN;

    // Spawn above the centre of the region.
    int ctx = (index.minTileX + index.maxTileX) / 2, cty = (index.minTileY + index.maxTileY) / 2;
    if (!index.hasTile(ctx, cty))
        for (const W3dEntry& e : index.entries)
            if (e.type == W3D_ENTRY_TERRAIN) { ctx = (int)(e.id / 64); cty = (int)(e.id % 64); break; }
    Camera cam;
    cam.pos[0] = (32.0f - cty - 0.5f) * kTileSize;
    cam.pos[1] = (32.0f - ctx - 0.5f) * kTileSize;
    cam.pos[2] = 0;
    int distIdx = 1;
    cam.drawDist = kDrawDists[distIdx];
    rendererSetProjection(cam.drawDist);
    const float spawn[2] = {cam.pos[0], cam.pos[1]};

    // Wait for the spawn tile (counts as "time to first frame").
    u64 t0 = svcGetSystemTick();
    printf("\x1b[1;1HLoading %s ...", packPath);
    while (aptMainLoop() && !streamerTileReadyAt(cam.pos[0], cam.pos[1])) {
        streamerUpdate(cam.pos, cam.drawDist);
        streamerEndFrame();
        gspWaitForVBlank();
        if (ticksToMs(svcGetSystemTick() - t0) > 20000) break;
    }
    g_report.firstFrameMs = ticksToMs(svcGetSystemTick() - t0);
    float ground = 0;
    if (streamerGroundHeight(cam.pos[0], cam.pos[1], &ground)) cam.pos[2] = ground + 25.0f;
    consoleClear();

    g_login.init();
    std::string netNote;

    RenderSettings rs;
    int detailIdx = 1;
    bool walk = false;
    // Online character state (stage 5): feet position, jump, last held direction.
    float pl[3] = {0, 0, 0}, plVz = 0;
    bool airborne = false, jumped = false, wasOnlineWalk = false, onFloor = false, wasFlight = false, wasFlightSent = false;
    swim::State swimState;           // stage 5.26: momentum of a plunge, floating back up
    bool wallHit = false;          // a wall stopped/deflected this frame's step (HUD)
    float wallUs = 0, wallUsShown = 0;   // wall test time accumulated per HUD refresh
    uint32_t heldDir = 0;
    uint32_t standSentMs = 0;   // sitting and walking off: stand up first (CMSG_STANDSTATECHANGE 0)
    // Third-person camera (online walking): orbits the character's eyes at camDist yards along the look
    // direction; D-pad up/down zooms, 0 = first person. eye = smoothed head position.
    float camDist = 5.0f, eye[3] = {0, 0, 0};
    bool eyeValid = false;
    // Riding a ship or zeppelin (stage 5.21): offset on the transport; the world position is derived from it every frame.
    struct Ride { uint64_t guid = 0; float lx = 0, ly = 0, lz = 0, lo = 0, shipO = 0; } ride;
    uint32_t rideSeenStamp = 0;
    std::vector<Marker> markers;
    markers.reserve(260);
    CreatureView creatureView;
    // Stage 5.29: the 3D scene of the login screens (a GLUE entry: a static backdrop model, its camera and the character's
    // mark) and the character standing in it.
    struct GlueStage {
        uint32_t id = ~0u;
        W3dGlueScene g{};
        bool have = false;
        Model* model = nullptr;
        bool active() const { return id != ~0u; }
        void set(uint32_t sceneId) {
            if (sceneId == id) return;
            release();
            id = sceneId;
            have = streamerReadEntry(W3D_ENTRY_GLUE, sceneId, &g, sizeof g) && g.modelId;
            if (have) model = streamerAcquireItemModel(g.modelId);
        }
        bool ready() const { return model && model->h && model->state == AssetState::Ready && model->texturesReady(); }
        void release() {
            if (model) streamerReleaseItemModel(model);
            model = nullptr;
            have = false;
            id = ~0u;
        }
    } glueStage;
    CreatureView glueView;
    std::vector<CreatureSeen> glueSeen;
    std::vector<CreatureInstance> glueCreatures;
    std::vector<SceneModel> glueModels;
    uint32_t glueRevShown = 0;
    GameUi gameUi;
    SoundWorld soundWorld;           // melee, spells, deaths, aggro, footsteps -> positioned sounds
    std::vector<SoundReq> soundReqs;
    // Controller (stage 5.11): scheme + settings from pad.txt (Old 3DS without ZL/ZR/C-Stick defaults to Classic),
    // hotbars per character, the character's own facing (Forever: separate from the camera).
    PadConfig padCfg;
    {
        bool isNew = true;
        APT_CheckNew3DS(&isNew);
        if (!isNew) padCfg.scheme = PadScheme::Classic;
    }
    char padPath[96];
    snprintf(padPath, sizeof padPath, "%s/pad.txt", kOutDir);
    padCfg.load(padPath);
    // Top screen mode (Settings): 800x240 wide, or stereoscopic 3D with a second target for the right eye.
    C3D_RenderTarget* topWide = nullptr;
    C3D_RenderTarget* topRight = nullptr;
    int topModeNow = 0;
    auto applyTopMode = [&](int want) {
        if (want == topModeNow) return;
        // leave the old mode
        if (topModeNow == 1) {
            gfxSetWide(false);
            C3D_RenderTargetDetachOutput(topWide);
            C3D_RenderTargetDelete(topWide);
            topWide = nullptr;
            C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        } else if (topModeNow == 2) {
            gfxSet3D(false);
            C3D_RenderTargetDetachOutput(topRight);
            C3D_RenderTargetDelete(topRight);
            topRight = nullptr;
            rendererSetEye(0);
        }
        topModeNow = 0;
        if (want == 1) {
            topWide = C3D_RenderTargetCreate(240, 800, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
            if (topWide) {
                gfxSetWide(true);
                C3D_RenderTargetDetachOutput(top);
                C3D_RenderTargetSetOutput(topWide, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
                topModeNow = 1;
            } else {
                netNote = "800x240: not enough VRAM";
            }
        } else if (want == 2) {
            topRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
            if (topRight) {
                gfxSet3D(true);
                C3D_RenderTargetSetOutput(topRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
                topModeNow = 2;
            } else {
                netNote = "3D: not enough VRAM";
            }
        }
    };
    applyTopMode(padCfg.topMode);
    // Sound: sdmc:/3ds/w3dproto/sound/ from w3dconv --sound (else built-in blips); needs the DSP firmware dump
    audio::init((std::string(kOutDir) + "/sound").c_str());
    audio::setVolumes(padCfg.musicVol, padCfg.sfxVol, padCfg.ambVol);
    soundWorld.load(audio::soundsTxt());
    Pad pad;
    ActionLayout bars;
    std::string barsPath;
    float charYaw = 0;
    float portalHold = 0;   // seconds the portal has been closed without a new map
    HudList hud;   // top-screen game HUD (stage 5.9), rebuilt every frame while online
    std::vector<CreatureSeen> seen;
    std::vector<CreatureInstance> creatures;
    Bench bench;
    u64 lastTick = svcGetSystemTick(), fpsStart = lastTick, startTick = lastTick;
    u32 fpsFrames = 0;
    float fps = 0;
    double cpuMsLast = 0;
    FrameCounters fc;

    // Map change (Azeroth <-> Kalimdor, ship/zeppelin crossings, portals): every map is its own pack
    // Portal effect of a map change (stage 5.24). While the map pack is swapped the world cannot be drawn, so the
    // wait loops draw the swirl alone on both screens; in the normal frame it covers the top screen's world.
    PortalFx portal;
    const char* portalText = "";
    // Loading (stage 5.25): after entering the world or any teleport the character is frozen under the map's loading screen
    // until the ground and the collision under it are ready, so nobody falls through floors that are not streamed yet.
    bool loadingWorld = false;
    float loadElapsed = 0, loadShown = 0;
    auto beginLoading = [&](uint32_t mapId, bool alreadyClosed) {
        portal.setPicture(ui::assets().loadingScreen(mapId));
        portal.setProgress(0.0f);
        portalText = "Loading...";
        if (alreadyClosed) portal.startClosed();
        else portal.start();
        loadingWorld = true;
        loadElapsed = 0;
        loadShown = 0;
    };
    auto drawPortalFrame = [&](float dt) {
        portal.update(dt);
        HudList a, b;
        portal.build(a, kHudW, kHudH, portalText);
        portal.build(b, kTouchW, kTouchH, portalText);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        for (int eye = 0; eye < (topModeNow == 2 && topRight ? 2 : 1); ++eye) {
            C3D_RenderTarget* t = eye == 1 ? topRight : (topModeNow == 1 ? topWide : top);
            C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0x240A0EFF, 0);
            C3D_FrameDrawOn(t);
            uiDraw(a, kHudW);
        }
        if (bottomT) {
            C3D_RenderTargetClear(bottomT, C3D_CLEAR_COLOR, 0x240A0EFF, 0);
            C3D_FrameDrawOn(bottomT);
            uiDraw(b, kTouchW);
        }
        C3D_FrameEnd(0);
        uiEndFrame();
    };
    u64 portalTick = svcGetSystemTick();
    auto portalDt = [&]() {
        u64 now = svcGetSystemTick();
        float d = std::min((float)ticksToMs(now - portalTick) / 1000.0f, 0.1f);
        portalTick = now;
        return d;
    };

    // (sdmc:/3ds/w3dproto/map<N>.w3dp; map 0 is test.w3dp or map0.w3dp). The streamer is shut down and started again on
    // the new pack. Without a pack for the map the old one stays and the HUD note says so.
    uint32_t curMap = 0;
    auto ensureMap = [&](uint32_t m, const float at[3]) -> bool {
        if (m == curMap) return true;
        char path[128];
        snprintf(path, sizeof path, "%s/map%lu.w3dp", kOutDir, (unsigned long)m);
        if (m == 0) {
            const char* fp = findPack();
            if (fp) snprintf(path, sizeof path, "%s", fp);
        }
        FILE* probe = fopen(path, "rb");
        if (!probe) {
            char note[96];
            snprintf(note, sizeof note, "no pack for map %lu (%s)", (unsigned long)m, path);
            netNote = note;
            return false;
        }
        fclose(probe);
        char oldPath[128];
        snprintf(oldPath, sizeof oldPath, "%s", index.path);
        creatureView.clear();
        streamerShutdown();
        index = PackIndex();
        char e[160];
        bool good = index.open(path, e, sizeof e) && streamerInit(&index, e, sizeof e);
        if (!good) {   // go back to the previous map's pack
            index = PackIndex();
            char e2[160];
            if (!(index.open(oldPath, e2, sizeof e2) && streamerInit(&index, e2, sizeof e2))) {
                fail(e2);
                exit(0);
            }
            netNote = std::string("map pack failed: ") + e;
            return false;
        }
        curMap = m;
        snprintf(g_report.packPath, sizeof g_report.packPath, "%s", path);
        g_report.tilesInPack = 0;
        for (const W3dEntry& en : index.entries) g_report.tilesInPack += en.type == W3D_ENTRY_TERRAIN;
        // wait (briefly) for the tile under the character; the network is not serviced meanwhile
        u64 w0 = svcGetSystemTick();
        float wp[3] = {at[0], at[1], at[2]};
        // the tile and its collision (a dungeon floor is only collision: without it the character would fall through)
        portalTick = svcGetSystemTick();
        while (aptMainLoop() && !(streamerTileReadyAt(wp[0], wp[1]) && streamerCollisionReadyAt(wp[0], wp[1])) &&
               ticksToMs(svcGetSystemTick() - w0) < 10000) {
            streamerUpdate(wp, cam.drawDist);
            streamerEndFrame();
            if (portal.active()) {
                portal.setProgress(0.1f + (streamerTileReadyAt(wp[0], wp[1]) ? 0.45f : 0.0f) + (streamerCollisionReadyAt(wp[0], wp[1]) ? 0.35f : 0.0f));
                drawPortalFrame(portalDt());
            }
            else gspWaitForVBlank();
        }
        return true;
    };

    while (aptMainLoop()) {
        u64 frameStart = svcGetSystemTick();
        float frameMs = (float)ticksToMs(frameStart - lastTick);
        float dt = std::min(frameMs / 1000.0f, 0.1f);
        lastTick = frameStart;
        rs.timeSec = (float)ticksToMs(frameStart - startTick) / 1000.0f;

        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld();
        if ((down & KEY_START) && g_login.phase() == LoginFlow::Phase::Offline) break;   // flying offline: START quits
        // The login screens (stage 4; the original client's screens since stage 5.29) own both screens and the buttons
        // until the world: login, realms, the characters and their creation.
        touchPosition glueTouch{};
        const bool glueTouching = (held & KEY_TOUCH) != 0;
        if (glueTouching) hidTouchRead(&glueTouch);
        const bool loginUi = g_login.update(down, held, glueTouching, glueTouch.px, glueTouch.py, dt);
        if (g_login.quitRequested()) break;
        if (loginUi) { down = 0; held = 0; }
        else if (glueStage.active()) {   // left the screens: their scene and character go before any map change
            glueStage.release();
            glueView.clear();
        }
        if (g_login.justEnteredWorld()) {
            const float* p = g_login.worldPosition();
            memcpy(pl, p, sizeof pl);   // the character, whether or not its tile is in the pack
            charYaw = g_login.orientation();
            pad.reset();
            barsPath = std::string(kOutDir) + "/bars_" + g_login.characterName() + ".txt";
            bars.clear();
            bars.load(barsPath.c_str());
            airborne = false;
            heldDir = 0;
            int ttx = (int)floorf(32.0f - p[1] / kTileSize), tty = (int)floorf(32.0f - p[0] / kTileSize);
            char note[96];
            if (ensureMap(g_login.mapId(), p) && index.hasTile(ttx, tty)) {
                cam.pos[0] = p[0];   // server coordinates are render coordinates
                cam.pos[1] = p[1];
                cam.pos[2] = p[2] + 1.7f;
                cam.yaw = g_login.orientation();
                cam.pitch = -0.1f;
                walk = true;
                snprintf(note, sizeof note, "%s at tile %d_%d", g_login.characterName().c_str(), ttx, tty);
            } else {
                snprintf(note, sizeof note, "map %lu tile %d_%d not in pack",
                         (unsigned long)g_login.mapId(), ttx, tty);
            }
            netNote = note;
            eyeValid = false;
            gameUi.reset();
            soundWorld.reset();
            consoleClear();
            beginLoading(g_login.mapId(), true);   // the loading screen stays until the ground under the character is ready
        }
        // Game UI on the bottom screen and the controller scheme (stages 5.6-5.11) while online.
        circlePosition cpRaw{}, csRaw{};
        hidCircleRead(&cpRaw);
        hidCstickRead(&csRaw);
        PadOut po;
        GameUi::Output uiOut;
        GameUi::Input uiIn{};
        if (!g_login.online()) audio::setPlace(0, 0, 0, false);   // menus: music and ambience fade out
        if (g_login.online()) {
            w3n::GameSession& gs = g_login.game();
            touchPosition tp{};
            if (held & KEY_TOUCH) hidTouchRead(&tp);
            PadInput pi;
            pi.down = down;
            pi.held = held;
            pi.up = hidKeysUp();
            pi.lx = std::clamp(cpRaw.dx / 156.0f, -1.0f, 1.0f);
            pi.ly = std::clamp(cpRaw.dy / 156.0f, -1.0f, 1.0f);
            pi.rx = std::clamp(csRaw.dx / 156.0f, -1.0f, 1.0f);
            pi.ry = std::clamp(csRaw.dy / 156.0f, -1.0f, 1.0f);
            po = pad.update(pi, padCfg, gs.inCombat(), cam.yaw, dt);
            uiIn = {down, (down & KEY_TOUCH) != 0, tp.px, tp.py, pl, charYaw, g_login.sessionMs(),
                    g_login.characterName().c_str(), &po, &padCfg, &bars};
            uiIn.touchHeld = (held & KEY_TOUCH) != 0;
            uiIn.touchUp = (hidKeysUp() & KEY_TOUCH) != 0;
            uiIn.mapId = g_login.mapId();
            uiOut = gameUi.update(gs, uiIn, walk);
            for (const char* e : uiOut.sounds) audio::play(e);
            if (uiOut.soundChanged) audio::setVolumes(padCfg.musicVol, padCfg.sfxVol, padCfg.ambVol);
            audio::setPlace(g_login.mapId(), pl[0], pl[1], true);
            if (uiOut.faceYaw && walk) {   // turn to the target (spell, loot, BADFACING); the camera follows
                charYaw = uiOut.yaw;
                cam.yaw = uiOut.yaw;
            }
            if (uiOut.keyboardShown) lastTick = svcGetSystemTick();   // the keyboard took seconds: no huge dt
            if (uiOut.configChanged) padCfg.save(padPath);
            if (uiOut.graphicsChanged) {   // the Settings tab: draw distance, world objects
                distIdx = std::clamp(padCfg.drawDist, 0, 3);
                cam.drawDist = kDrawDists[distIdx];
                rendererSetProjection(cam.drawDist);
                rs.doodads = padCfg.doodads;
                applyTopMode(padCfg.topMode);
            }
            if (bars.empty() && !gs.spells.empty() && bars.autofill(gs)) uiOut.layoutChanged = true;
            if (uiOut.layoutChanged && !barsPath.empty()) bars.save(barsPath.c_str());
            if (uiOut.quit) break;
            if (uiOut.freeCamera) {
                walk = !walk;
                if (walk) {   // back into the character
                    cam.pos[0] = pl[0];
                    cam.pos[1] = pl[1];
                    cam.pos[2] = pl[2] + 1.7f;
                    eyeValid = false;
                }
            }
            // The game owns the buttons while walking; the developer keys work in the free camera.
            down &= ~(u32)(KEY_SELECT | KEY_START);
            if (walk) down &= ~(u32)(KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_ZL | KEY_ZR);
        }
        if (down & KEY_A) rs.doodads = !rs.doodads;
        if (down & KEY_ZR) rs.fog = !rs.fog;
        if (down & KEY_ZL) {   // doodad detail: near / normal / far
            static const float kRanges[3] = {25.0f, 45.0f, 80.0f};
            detailIdx = (detailIdx + 1) % 3;
            rs.doodadRange = kRanges[detailIdx];
        }
        if (down & KEY_SELECT) walk = !walk;   // offline (online: the game menu / SELECT via the pad)
        const bool online = g_login.online();
        w3n::GameSession* game = online ? &g_login.game() : nullptr;
        const uint32_t netMs = online ? g_login.sessionMs() : 0;
        if (online) {   // the server announced a map change (instance door, portal, ship to another continent)
            uint32_t pendingMap;
            if (game->takeTransferPending(&pendingMap) && pendingMap != curMap && !loadingWorld) {
                portal.setPicture(ui::assets().loadingScreen(pendingMap));
                portal.setProgress(0.0f);
                portalText = "Loading...";
                portal.start();
            }
        }
        if (portal.phase() == PortalFx::Phase::Hold && !loadingWorld) portalHold += dt;
        else portalHold = 0;
        if (portalHold > 25.0f) { portal.finish(); portalHold = 0; }   // the transfer never arrived (aborted)
        if (loadingWorld) {   // wait for the ground and its collision under the character
            const float* lp = walk ? pl : cam.pos;
            loadElapsed += dt;
            const bool tileOk = streamerTileReadyAt(lp[0], lp[1]);
            const bool collOk = streamerCollisionReadyAt(lp[0], lp[1]);
            const bool quiet = streamerStats().tilesLoading == 0;
            float target = 0.1f + (tileOk ? 0.45f : 0.0f) + (collOk ? 0.35f : 0.0f) + (quiet ? 0.1f : 0.0f);
            loadShown += (target - loadShown) * std::min(1.0f, dt * 6.0f);
            portal.setProgress(loadShown);
            // Stage 5.28: the tile and its collision being "ready" is not enough (the floor of a building can come from a
            // neighbouring tile's data, or the collision grid of the tile still be empty): there must be a floor under the
            // feet within reach of where the server put us, or a long wait (10 s) has passed. Never release into the void.
            float gz = 0;
            const bool groundOk = streamerFloorHeight(lp[0], lp[1], lp[2] + 0.5f, &gz) && std::fabs(gz - lp[2]) < 4.0f;
            if ((tileOk && collOk && (quiet || loadElapsed > 3.0f) && (groundOk || loadElapsed > 10.0f)) || loadElapsed > 25.0f) {
                loadingWorld = false;
                portal.setProgress(1.0f);
                portal.finish();
                eyeValid = false;
                lastTick = svcGetSystemTick();   // no huge dt after the wait
            }
        }
        portal.update(dt);
        if (online) game->objects.tickTransports(netMs);   // ships and zeppelins along their paths, their passengers with them
        if (online) {   // the server moved us (teleport / new map), in any camera mode
            float tp[3], to;
            if (game->takeTeleport(tp, &to)) {
                const bool mapChange = game->mapId() != curMap;
                if (mapChange) {   // close the loading screen over the old world first, then swap the pack under it
                    portal.setPicture(ui::assets().loadingScreen(game->mapId()));
                    portal.setProgress(0.0f);
                    portal.start();
                    portalText = "Loading...";
                    portalTick = svcGetSystemTick();
                    while (aptMainLoop() && !portal.closed()) drawPortalFrame(portalDt());
                }
                ensureMap(game->mapId(), tp);   // a new map: its pack and streamer (no-op within the same map)
                if (mapChange || !(streamerTileReadyAt(tp[0], tp[1]) && streamerCollisionReadyAt(tp[0], tp[1])))
                    beginLoading(game->mapId(), false);   // the destination is not streamed yet: wait for it
                ride.guid = 0;
                memcpy(pl, tp, sizeof pl);
                swimState = swim::State();   // (the mover's reset dropped the swimming flag: the water there decides again)
                airborne = jumped = false;
                plVz = 0;
                heldDir = 0;
                eyeValid = false;
                if (walk) {
                    cam.yaw = to;
                    cam.pos[0] = pl[0];
                    cam.pos[1] = pl[1];
                    cam.pos[2] = pl[2] + 1.7f;
                }
            }
        }
        if (down & KEY_X) {
            distIdx = (distIdx + 1) % 5;
            cam.drawDist = kDrawDists[distIdx];
            rendererSetProjection(cam.drawDist);
        }
        if ((down & KEY_Y) && !bench.running) {
            bench = Bench();
            bench.running = true;
            bench.startTick = frameStart;
            bench.tileLoadsAtStart = streamerStats().tileLoads;
            bench.sumTileMsAtStart = streamerStats().sumTileMs;
        }

        // --- camera
        if (bench.running) {
            if (online && wasOnlineWalk) {   // the camera leaves the character: stop it on the server
                game->mover.stopAll(netMs);
                wasOnlineWalk = false;
                heldDir = 0;
            }
            float t = (float)ticksToMs(frameStart - bench.startTick) / 1000.0f;
            float a = t / 40.0f * 6.2831853f;
            cam.pos[0] = spawn[0] + cosf(a) * kTileSize * 0.7f;
            cam.pos[1] = spawn[1] + sinf(a) * kTileSize * 0.7f;
            if (streamerGroundHeight(cam.pos[0], cam.pos[1], &ground)) cam.pos[2] = ground + 15.0f;
            cam.yaw = a + 1.5707963f;
            cam.pitch = -0.12f;
            if (t >= 40.0f) {
                bench.running = false;
                writeReport(&bench, cam.drawDist, rs);
            }
        } else {
            circlePosition cp = cpRaw, cs = csRaw;
            // Dead zones: a resting C-Stick reads a few units off centre and turned the camera slowly.
            auto dead = [](circlePosition& c, int r) {
                if (c.dx * c.dx + c.dy * c.dy < r * r) { c.dx = 0; c.dy = 0; }
            };
            dead(cp, 18);
            dead(cs, 24);
            const bool onlineWalk = online && walk;
            if (onlineWalk) {   // camera and facing from the controller scheme
                cam.yaw += po.camYaw;
                cam.pitch = std::clamp(cam.pitch + po.camPitch, -1.4f, 1.4f);
                if (po.zoom) camDist = std::clamp(camDist + po.zoom * 8.0f * dt, 0.0f, 15.0f);
                if (po.centerCamera) { cam.yaw = charYaw; cam.pitch = -0.1f; }
                if (padCfg.scheme == PadScheme::Classic) {
                    charYaw = cam.yaw;                       // the camera is the character
                } else if (po.faceYaw && !uiOut.blockMove) {   // turn towards the stick / camera, quickly but not in one frame
                    float d = remainderf(po.yaw - charYaw, 6.2831853f);
                    float step = 14.0f * dt;
                    charYaw += std::clamp(d, -step, step);
                }
            } else {
                float lookX = cs.dx / 156.0f, lookY = cs.dy / 156.0f;
                if (held & KEY_DLEFT) lookX = -1;
                if (held & KEY_DRIGHT) lookX = 1;
                if (held & KEY_DUP) lookY = 1;
                if (held & KEY_DDOWN) lookY = -1;
                cam.yaw -= lookX * 1.8f * dt;
                cam.pitch = std::clamp(cam.pitch + lookY * 1.2f * dt, -1.4f, 1.4f);
            }
            if (online && !onlineWalk && wasOnlineWalk) {   // left the character: release the keys on the server
                game->mover.stopAll(netMs);
                heldDir = 0;
            }
            wasOnlineWalk = onlineWalk;
            wallHit = false;
            // Taxi flight (stage 5.20): the server flies us along a spline (SMSG_MONSTER_MOVE with the flying flag) with
            // UNIT_FLAG_TAXI_FLIGHT set; we send no movement, follow the spline and look around from behind the mount.
            bool flightNow = false;
            float fpos[3] = {0, 0, 0}, fyaw = 0;
            if (onlineWalk) {
                const w3n::WorldObject* me = game->objects.find(game->selfGuid());
                if (me && me->taxiFlight() && me->pathActive) {
                    flightNow = true;
                    me->positionAt(netMs, fpos, &fyaw);
                }
            }
            if (wasFlight && !flightNow && onlineWalk) {   // landed: stand on the ground where the flight ended
                float gz;
                if (streamerFloorHeight(pl[0], pl[1], pl[2] + 3.0f, &gz)) pl[2] = gz;
                game->mover.reset(pl[0], pl[1], pl[2], charYaw, netMs);
                airborne = jumped = false;
                plVz = 0;
                heldDir = 0;
                eyeValid = false;
            }
            wasFlight = flightNow;
            // Stage 5.28: walking into a dungeon door / the tram entrance / any area trigger tells the server (CMSG_AREATRIGGER);
            // not while a map is loading, not on a taxi (the server ignores those anyway), not under the transfer screen.
            if (game && onlineWalk && !flightNow && !loadingWorld && portal.phase() == PortalFx::Phase::Idle) game->checkAreaTriggers(pl, netMs);
            if (flightNow) {
                if (!wasFlightSent) { game->mover.stopAll(netMs); wasFlightSent = true; }
                pl[0] = fpos[0]; pl[1] = fpos[1]; pl[2] = fpos[2];
                charYaw = fyaw;
                airborne = jumped = false;
                plVz = 0;
                heldDir = 0;
                eye[0] = pl[0]; eye[1] = pl[1]; eye[2] = pl[2] + 2.2f;   // above the mount's back
                eyeValid = true;
                const float d = std::max(camDist, 9.0f);
                float fw[3] = {cosf(cam.pitch) * cosf(cam.yaw), cosf(cam.pitch) * sinf(cam.yaw), sinf(cam.pitch)};
                cam.pos[0] = eye[0] - fw[0] * d;
                cam.pos[1] = eye[1] - fw[1] * d;
                cam.pos[2] = eye[2] - fw[2] * d + 0.4f;
                audio::setListener(pl[0], pl[1], pl[2], cam.yaw);
            } else {
                wasFlightSent = false;
            }
            if (flightNow) {
                // handled above
            } else if (onlineWalk) {
                soundReqs.clear();   // this frame's sounds (the movement below may add a splash)
                // --- ships: the server may have put us on one (login, a zeppelin transfer); we carry our offset with it
                if (!ride.guid) {
                    const w3n::WorldObject* me = game->objects.find(game->selfGuid());
                    if (me && (me->mv.flags & w3n::MoveFlag::OnTransport) && me->mv.time != rideSeenStamp && me->mv.transportGuid &&
                        game->objects.transportPose(me->mv.transportGuid)) {
                        rideSeenStamp = me->mv.time;
                        ride.guid = me->mv.transportGuid;
                        ride.lx = me->mv.tx; ride.ly = me->mv.ty; ride.lz = me->mv.tz; ride.lo = me->mv.to;
                        ride.shipO = game->objects.transportPose(ride.guid)->o;
                        game->mover.setTransport(ride.guid, ride.lx, ride.ly, ride.lz, ride.lo, netMs);
                    }
                }
                if (ride.guid) {
                    const w3n::TransportPose* tp = game->objects.transportPose(ride.guid);
                    if (!tp) {   // the ship is gone (out of range, zoned): stay where we are
                        ride.guid = 0;
                        game->mover.clearTransport(netMs);
                    } else {
                        float w[3], wo;
                        w3n::transportToWorld(*tp, ride.lx, ride.ly, ride.lz, ride.lo, w, &wo);
                        pl[0] = w[0]; pl[1] = w[1]; pl[2] = w[2];
                        charYaw += tp->o - ride.shipO;   // the ship turns, we turn with it
                        ride.shipO = tp->o;
                    }
                }
                // Ground under (x, y): a deck at about the feet height first (also from the quay: boarding), else the world.
                auto floorQ = [&](float x, float y, float zref, float* g, bool* onF = nullptr) -> bool {
                    for (auto& [guid, pose] : game->objects.transportPoses()) {
                        const w3n::WorldObject* go = game->objects.find(guid);
                        const w3n::DeckInfo* dk = go ? game->objects.transports.deck(go->displayId()) : nullptr;
                        if (!dk) continue;
                        float l[3];
                        w3n::worldToTransport(pose, x, y, zref, 0, l, nullptr);
                        if (!dk->contains(l[0], l[1])) continue;
                        const float dz = pose.z + dk->z;
                        if (std::fabs(zref - dz) <= (ride.guid == guid ? 3.0f : 1.6f)) {
                            *g = dz;
                            if (onF) *onF = true;
                            return true;
                        }
                    }
                    return streamerFloorHeight(x, y, zref, g, onF);
                };
                // Water walking (ghosts, the spell): the surface of a liquid is a floor to walk on (stage 5.26).
                const bool waterWalker = (game->mover.info().flags & w3n::MoveFlag::WaterWalking) != 0;
                auto floorW = [&](float x, float y, float zref, float* g, bool* onF = nullptr) -> bool {
                    bool ok = floorQ(x, y, zref, g, onF);
                    if (waterWalker && !ride.guid) {
                        LiquidHere lq;
                        if (streamerLiquidAt(x, y, zref, &lq) && (!ok || lq.level > *g) && zref >= lq.level - 1.0f) {
                            *g = lq.level;
                            if (onF) *onF = false;
                            ok = true;
                        }
                    }
                    return ok;
                };
                // WoW movement is digital: keys (from the stick by pad.cpp), walk or run. Keys are frozen in the air.
                uint32_t dir = 0;
                if (!airborne) {
                    dir = po.dir;
                    heldDir = dir;
                } else {
                    dir = heldDir;
                }
                if (game->mover.rooted() || uiOut.blockMove || loadingWorld) dir = 0;   // dead bodies do not walk; nor does anyone under a loading screen
                if (dir) {
                    const w3n::WorldObject* me = game->objects.find(game->selfGuid());
                    if (me && me->standState() != 0 && netMs - standSentMs > 1000) {
                        game->setStandState(0, netMs);
                        standSentMs = netMs;
                    }
                }
                if (dir && !airborne) game->mover.setWalk(po.walk, netMs);
                uint32_t other = game->mover.info().flags & ~(uint32_t)w3n::MoveFlag::MaskMoving;
                float speed = game->mover.speeds.forFlags(dir | other);
                if (game->mover.swimming() && !ride.guid && !loadingWorld) {
                    // --- swimming (stage 5.26): swim.cpp moves us; the surface, walls and the bottom bound it
                    swim::World sw;
                    sw.floor = [&](float x, float y, float z, float* g) { return floorQ(x, y, z, g); };
                    sw.liquid = [&](float x, float y, float z, float* lv, uint8_t* fl) {
                        LiquidHere lq;
                        if (!streamerLiquidAt(x, y, z, &lq)) return false;
                        *lv = lq.level; *fl = lq.flags;
                        return true;
                    };
                    sw.walls = [&](float fx, float fy, float z, float* tx, float* ty) {
                        u64 w0 = svcGetSystemTick();
                        wallHit |= streamerCollideMove(fx, fy, z, tx, ty);
                        wallUs += (float)(ticksToMs(svcGetSystemTick() - w0) * 1000.0);
                    };
                    swim::Input si;
                    si.dt = dt;
                    memcpy(si.pos, pl, sizeof si.pos);
                    si.yaw = charYaw;
                    si.dir = dir;
                    si.speed = dir ? speed : game->mover.speeds.swim;   // swimming straight up: the swim speed
                    si.camPitch = cam.pitch;
                    si.ascend = po.ascend && !uiOut.blockJump;
                    si.jump = po.jump && !uiOut.blockJump;
                    si.rooted = game->mover.rooted();
                    swim::Result sr = swim::step(swimState, sw, si);
                    if (sr.jumpOut) {   // out of the water with a jump: STOP_SWIM, then JUMP
                        game->mover.stopSwim(netMs);
                        game->mover.update(dir, pl[0], pl[1], pl[2], charYaw, netMs);
                        game->mover.jump(netMs);
                        airborne = jumped = true;
                        plVz = 7.9555473f;
                        heldDir = dir;
                    } else {
                        memcpy(pl, sr.pos, sizeof pl);
                        if (sr.stop) {
                            game->mover.stopSwim(netMs);
                            if (floorQ(pl[0], pl[1], pl[2], &ground) && ground < pl[2] - 1.0f) { airborne = true; jumped = false; plVz = 0; }
                            else if (floorQ(pl[0], pl[1], pl[2], &ground)) pl[2] = ground;
                            swimState = swim::State();
                        } else {
                            game->mover.setSwimPitch(sr.pitch, netMs);
                        }
                    }
                } else {
                if (dir) {
                    float fx = (dir & w3n::MoveFlag::Forward) ? 1.f : (dir & w3n::MoveFlag::Backward) ? -1.f : 0.f;
                    float sx = (dir & w3n::MoveFlag::StrafeLeft) ? 1.f : (dir & w3n::MoveFlag::StrafeRight) ? -1.f : 0.f;
                    float a = charYaw + atan2f(sx, fx);
                    float nx = pl[0] + cosf(a) * speed * dt, ny = pl[1] + sinf(a) * speed * dt;
                    // Walls first (the corrected position is what the server gets), then the floor there.
                    u64 w0 = svcGetSystemTick();
                    wallHit = ride.guid ? false : streamerCollideMove(pl[0], pl[1], pl[2], &nx, &ny);   // no walls on a ship (yet)
                    wallUs += (float)(ticksToMs(svcGetSystemTick() - w0) * 1000.0);
                    if (floorW(nx, ny, pl[2], &ground)) { pl[0] = nx; pl[1] = ny; }
                    else if (!airborne) dir = 0;   // never walk onto an unloaded tile
                }
                // Ground = terrain or a building floor within a step of the feet (stairs, upper floors).
                bool haveGround = floorW(pl[0], pl[1], pl[2], &ground, &onFloor);
                if (loadingWorld) { haveGround = false; airborne = false; plVz = 0; }   // frozen until the ground is there
                if (po.jump && !uiOut.blockJump && !airborne && !game->mover.rooted() && !loadingWorld) {
                    game->mover.update(dir, pl[0], pl[1], pl[2], charYaw, netMs);
                    game->mover.jump(netMs);
                    airborne = jumped = true;
                    plVz = 7.9555473f;
                }
                if (!airborne && haveGround && ground < pl[2] - 1.0f) {   // walked off a ledge: fall
                    airborne = true;
                    jumped = false;
                    plVz = 0;
                }
                // Into the water (stage 5.26): feet deeper than W3D_SWIM_DEPTH under a surface - walking down a lake's
                // bed, or falling / jumping in (the fall ends with START_SWIM: no landing, no fall damage).
                LiquidHere lq;
                auto deepWater = [&]() {
                    return !ride.guid && !loadingWorld && !waterWalker && !game->mover.rooted() &&
                           streamerLiquidAt(pl[0], pl[1], pl[2], &lq) && swim::deepEnough(lq.level, pl[2]);
                };
                if (airborne && !streamerCollisionReadyAt(pl[0], pl[1]) && !haveGround) {
                    plVz = 0;   // stage 5.28: the data under us is not loaded (yet): hang here instead of dropping through the world
                } else if (airborne) {
                    plVz -= 19.291105f * dt;   // the client's gravity
                    pl[2] += plVz * dt;
                    if (deepWater()) {
                        swimState = swim::State();
                        swimState.vz = std::max(plVz * 0.5f, -12.0f);   // the plunge carries on a little
                        swimState.surfacing = true;
                        game->mover.update(dir, pl[0], pl[1], pl[2], charYaw, netMs);
                        game->mover.startSwim(netMs);
                        soundWorld.splash(pl, -plVz, netMs, soundReqs);
                        airborne = jumped = false;
                        plVz = 0;
                    } else if (haveGround && plVz < 0 && pl[2] <= ground) {
                        pl[2] = ground;
                        airborne = false;
                        if (jumped) game->mover.land(ground, netMs);
                        jumped = false;
                    }
                } else if (haveGround) {
                    pl[2] = ground;
                    if (deepWater()) {   // waded in deep enough: float up to the surface
                        swimState = swim::State();
                        swimState.surfacing = true;
                        game->mover.update(dir, pl[0], pl[1], pl[2], charYaw, netMs);
                        game->mover.startSwim(netMs);
                        soundWorld.splash(pl, 2.0f, netMs, soundReqs);
                    }
                }
                }   // not swimming
                // Which ship are we on? (standing on a deck, or still in the air above the one we were on)
                {
                    uint64_t on = 0;
                    const w3n::TransportPose* onPose = nullptr;
                    for (auto& [guid, pose] : game->objects.transportPoses()) {
                        const w3n::WorldObject* go = game->objects.find(guid);
                        const w3n::DeckInfo* dk = go ? game->objects.transports.deck(go->displayId()) : nullptr;
                        if (!dk) continue;
                        float l[3];
                        w3n::worldToTransport(pose, pl[0], pl[1], pl[2], 0, l, nullptr);
                        if (!dk->contains(l[0], l[1])) continue;
                        const float above = pl[2] - (pose.z + dk->z);
                        if ((!airborne && std::fabs(above) < 0.6f) || (airborne && ride.guid == guid && above > -1.0f && above < 12.0f)) {
                            on = guid;
                            onPose = &pose;
                            break;
                        }
                    }
                    if (on != ride.guid) {
                        if (on) {
                            float l[3], lo;
                            w3n::worldToTransport(*onPose, pl[0], pl[1], pl[2], charYaw, l, &lo);
                            ride = Ride{on, l[0], l[1], l[2], lo, onPose->o};
                            game->mover.setTransport(on, l[0], l[1], l[2], lo, netMs);
                        } else {
                            ride.guid = 0;
                            game->mover.clearTransport(netMs);
                        }
                    }
                    if (ride.guid && onPose) {
                        float l[3], lo;
                        w3n::worldToTransport(*onPose, pl[0], pl[1], pl[2], charYaw, l, &lo);
                        ride.lx = l[0]; ride.ly = l[1]; ride.lz = l[2]; ride.lo = lo;
                        if (game->mover.onTransport()) game->mover.setTransportOffset(l[0], l[1], l[2], lo);
                        else game->mover.setTransport(ride.guid, l[0], l[1], l[2], lo, netMs);   // after a reset
                    }
                }
                game->mover.update(dir, pl[0], pl[1], pl[2], charYaw, netMs);
                // Sounds of the world around the character (heard from the character, facing the camera's way)
                audio::setListener(pl[0], pl[1], pl[2], cam.yaw);
                int inWater = game->mover.swimming() ? 2 : 0;
                if (!inWater) {
                    LiquidHere lw;
                    if (streamerLiquidAt(pl[0], pl[1], pl[2], &lw) && lw.level > pl[2] + 0.15f && lw.kind() <= W3D_LIQUID_OCEAN) inWater = 1;
                }
                soundWorld.update(*game, pl, dir != 0, !airborne, po.walk, netMs, soundReqs, inWater);
                for (const SoundReq& q : soundReqs) {
                    if (q.stop) audio::stopTag(q.tag);
                    else audio::playAt(q.sound, q.x, q.y, q.z, q.self, q.tag, q.gain);
                }
                // Eyes: x/y follow exactly, height smoothed on stairs (not in the air).
                float target = pl[2] + 1.7f;
                if (!eyeValid) { eye[2] = target; eyeValid = true; }
                eye[0] = pl[0];
                eye[1] = pl[1];
                eye[2] = airborne ? target : eye[2] + (target - eye[2]) * std::min(1.0f, dt * 12.0f);
                float d = camDist < 0.8f ? 0.0f : camDist;
                float fw[3] = {cosf(cam.pitch) * cosf(cam.yaw), cosf(cam.pitch) * sinf(cam.yaw), sinf(cam.pitch)};
                float cp3[3] = {eye[0] - fw[0] * d, eye[1] - fw[1] * d, eye[2] - fw[2] * d + (d > 0 ? 0.4f : 0.0f)};
                if (d > 0) {
                    // Not through walls (the camera slides along them like a walker at head height)
                    // and never under the ground.
                    streamerCollideMove(eye[0], eye[1], eye[2] - 1.2f, &cp3[0], &cp3[1]);
                    float g;
                    if (streamerGroundHeight(cp3[0], cp3[1], &g) && cp3[2] < g + 0.5f) cp3[2] = g + 0.5f;
                }
                cam.pos[0] = cp3[0];
                cam.pos[1] = cp3[1];
                cam.pos[2] = cp3[2];
            } else {
                float speed = walk ? ((held & KEY_B) ? 14.0f : 7.0f) : ((held & KEY_B) ? 80.0f : 20.0f);
                float mf = cp.dy / 156.0f, ms = cp.dx / 156.0f;
                float cy = cosf(cam.yaw), sy = sinf(cam.yaw);
                float nx = cam.pos[0] + (cy * mf + sy * ms) * speed * dt;
                float ny = cam.pos[1] + (sy * mf - cy * ms) * speed * dt;
                if (walk) {   // offline walking: the same walls (flying goes through everything)
                    u64 w0 = svcGetSystemTick();
                    wallHit = streamerCollideMove(cam.pos[0], cam.pos[1], cam.pos[2] - 1.7f, &nx, &ny);
                    wallUs += (float)(ticksToMs(svcGetSystemTick() - w0) * 1000.0);
                }
                bool haveGround = streamerGroundHeight(nx, ny, &ground);
                if (!walk || haveGround) {   // walking never steps onto an unloaded tile
                    cam.pos[0] = nx;
                    cam.pos[1] = ny;
                }
                if (walk) {
                    if (streamerFloorHeight(cam.pos[0], cam.pos[1], cam.pos[2] - 1.7f, &ground, &onFloor)) {
                        float target = ground + 1.7f;
                        cam.pos[2] += (target - cam.pos[2]) * std::min(1.0f, dt * 12.0f);
                    }
                } else {
                    if (held & KEY_R) cam.pos[2] += speed * dt;
                    if (held & KEY_L) cam.pos[2] -= speed * dt;
                    if (haveGround && cam.pos[2] < ground + 1.0f) cam.pos[2] = ground + 1.0f;
                }
            }
        }
        if (online) {
            game->update(netMs);   // movement packets, name/creature queries
            const bool selfVisible = !walk || camDist >= 0.8f;   // free camera or third person
            collectCreatures(*game, netMs, cam.pos, selfVisible, seen);
            creatureView.update(seen, netMs, creatures);
            {   // the Settings tab's crowd limit: the nearest N creatures
                static const size_t kCrowd[4] = {10, 20, 40, 1000};
                size_t cap = kCrowd[std::clamp(padCfg.crowd, 0, 3)];
                if (creatures.size() > cap) {
                    auto d2 = [&](const CreatureInstance& c) {
                        float dx = c.pos[0] - cam.pos[0], dy = c.pos[1] - cam.pos[1];
                        return dx * dx + dy * dy;
                    };
                    std::nth_element(creatures.begin(), creatures.begin() + (long)cap, creatures.end(),
                                     [&](const CreatureInstance& a, const CreatureInstance& b) { return d2(a) < d2(b); });
                    creatures.resize(cap);
                }
            }
            buildMarkers(*game, netMs, cam.pos, selfVisible, std::min(cam.drawDist, 200.0f), creatureView, markers);
            if (walk && po.mode != PadOut::None) {   // L / R held: a ring under the character, the arrow at the aim (or the facing)
                Marker rg;
                rg.pos[0] = pl[0]; rg.pos[1] = pl[1]; rg.pos[2] = pl[2] + 0.12f;
                rg.yaw = po.aiming ? po.aimYaw : charYaw;
                rg.height = 1.0f;
                rg.width = 1.3f;
                rg.color = po.mode == PadOut::Enemy ? 0xFF3030FF : 0xFF40D040;   // red: enemies, green: friends
                rg.shape = 1;
                markers.push_back(rg);
            }
        } else {
            markers.clear();
            creatures.clear();
            if (creatureView.tracked()) creatureView.clear();
        }

        // --- top-screen HUD: unit frames, minimap, name plates, cast bar, chat
        if (online && !bench.running) {
            HudInput hi;
            hi.game = game;
            hi.nowMs = netMs;
            memcpy(hi.camPos, cam.pos, sizeof hi.camPos);
            hi.camYaw = cam.yaw;
            hi.camPitch = cam.pitch;
            hi.selfName = g_login.characterName().c_str();
            static HudPadView pv;
            pv = HudPadView();
            if (walk && gameUi.gamePage()) {   // UI stage 2: the crossbar in both schemes
                pv.show = true;
                pv.set = gameUi.shownSet();
                pv.page = bars.page;
                pv.mode = po.mode;
                pv.aim = gameUi.aimed();
                pv.binding = gameUi.binding();
                for (int b = 0; b < kPadButtons; ++b) {
                    int slot = pv.set * kPadButtons + b;
                    HudBarCell& c = pv.cells[b];
                    if (pv.set == 0 && b >= 4) { c.name = padFixedName(b); c.fixed = true; continue; }
                    const ActionSlot& a = bars.at(slot);
                    if (a.type == ActionSlot::Empty) continue;
                    c.name = bars.label(*game, slot);
                    c.cdMs = bars.cooldownMs(*game, slot, netMs);
                    if (a.type == ActionSlot::Spell) {
                        c.icon = ui::assets().spellIcon(a.id);
                        if (!c.icon.ok() && a.id == w3n::kSpellAttack) c.icon = ui::assets().namedIcon("attack");
                        c.cdKey = a.id;
                    } else {
                        const w3n::ItemInfo* info = game->itemInfoIfKnown(a.id);
                        if (info) c.icon = ui::assets().itemIcon(info->displayId);
                        c.cdKey = a.id | 0x80000000u;
                        size_t x = c.name.rfind(" x");
                        if (x != std::string::npos) { c.count = (uint32_t)atoi(c.name.c_str() + x + 2); c.name.resize(x); }
                    }
                    c.active = a.type == ActionSlot::Spell && ((game->cast.active && game->cast.spell == a.id) ||
                                                              (a.id == w3n::kSpellAttack && game->attackWanted()));
                }
                if (game->life() == w3n::Life::Alive && !game->lootWindow.open && !game->gossip.open && !game->vendor.open &&
                    !game->questDialog.open())
                    pv.soft = game->softInteract(pl, charYaw, netMs, &pv.softVerb);
            }
            hi.pad = &pv;
            hi.questTracker = padCfg.questTracker;
            hi.dim = padCfg.dimHud;
            hudBuildGame(hud, hi);
        } else {
            hud.clear();
        }
        portal.build(hud, kHudW, kHudH);

        // --- the touch screen's draw list (game page: GPU; login / debug pages: the console)
        bool bottomDirty = false;
        if (loginUi) {   // the login screens' touch screen, when it changed
            bottomDirty = !bottomGpu || g_login.glueRevision() != glueRevShown;
            glueRevShown = g_login.glueRevision();
            bottomGpu = true;
        } else if (g_login.online() && gameUi.gamePage()) {
            uiIn.pos = pl;
            uiIn.nowMs = g_login.sessionMs();
            bottomDirty = gameUi.draw(g_login.game(), uiIn, fps, g_login.latencyMs()) || !bottomGpu;
            bottomGpu = true;
        } else if (bottomGpu) {
            bottomGpu = false;
            consoleClear();
        }

        // --- under a liquid's surface? (stage 5.26: fog in the water's colour, short sight)
        {
            LiquidHere lc;
            rs.underwater = streamerLiquidAt(cam.pos[0], cam.pos[1], cam.pos[2], &lc) && cam.pos[2] < lc.level - 0.05f;
            if (rs.underwater) {
                memcpy(rs.waterFog, lc.look.fog, 3);
                rs.waterFogEnd = lc.look.fogEnd;
            }
        }
        // --- the login screens' scene (stage 5.29): the backdrop of the screen (the Dark Portal, a racial home) seen through
        // its own camera; on the character screens the character stands on the scene's mark, framed like WoWee's preview
        // (the scene camera's direction, the distance from the character's height), turned with L / R
        Camera glueCam;
        float glueFov = 50.0f, glueNear = 0.3f, glueFar = 600.0f;
        bool glueScene = false;
        glueModels.clear();
        glueCreatures.clear();
        if (loginUi) {
            const LoginFlow::Scene sc = g_login.scene();
            glueStage.set(sc.id);
            // a pack without the scenes (older conversions): the character alone, on the clear colour, seen from the front
            static const W3dGlueScene kBare = {0, {3.0f, 0.0f, 1.2f}, {0.0f, 0.0f, 1.0f}, 0.0f, 0.3f, 100.0f, {0, 0, 0},
                                               W3D_GLUE_HAS_CAMERA | W3D_GLUE_HAS_STAND, 2.0f, {0, 0, 0}};
            const bool bare = !glueStage.have && sc.character;
            if (glueStage.ready() || bare) {
                const W3dGlueScene& g = bare ? kBare : glueStage.g;
                if (!bare) {
                    SceneModel smod;
                    smod.model = glueStage.model;
                    glueModels.push_back(smod);
                }
                float dir[3] = {1, 0, 0.15f};   // from the target towards the camera
                if (g.flags & W3D_GLUE_HAS_CAMERA)
                    for (int k = 0; k < 3; ++k) dir[k] = g.camPos[k] - g.camTarget[k];
                float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                if (dl < 1e-3f) { dir[0] = 1; dir[1] = 0; dir[2] = 0; dl = 1; }
                for (float& d : dir) d /= dl;
                // the M2 camera's field of view is diagonal: the vertical one of a 400x240 screen
                if (g.fov > 0.2f && g.fov < 2.8f)
                    glueFov = 2.0f * atanf(tanf(g.fov * 0.5f) * 240.0f / 466.5f) * 180.0f / 3.14159265f;
                glueFov = std::clamp(glueFov, 25.0f, 75.0f);
                float eyePos[3], target[3];
                if (!sc.character) {   // the login screen: the artist's own camera
                    for (int k = 0; k < 3; ++k) { eyePos[k] = g.camPos[k]; target[k] = g.camTarget[k]; }
                    glueFar = std::max(600.0f, g.radius * 2.5f);
                } else {
                    const glue::RaceInfo* ri = glue::race(sc.look.race);
                    float height = ri ? ri->height[sc.look.sex & 1] : 1.9f;
                    float yaw = atan2f(dir[1], dir[0]) + sc.yaw;   // facing the camera, plus the player's turn
                    CreatureSeen c;
                    c.guid = 0xFFFF000000000001ull;
                    c.look = w3d_look_key(&sc.look);
                    c.lookParts = sc.look;
                    memcpy(c.pos, g.stand, sizeof c.pos);
                    c.yaw = yaw;
                    memcpy(c.equip, sc.equip, sizeof c.equip);
                    memcpy(c.equipInv, sc.equipInv, sizeof c.equipInv);
                    glueSeen.assign(1, c);
                    glueView.update(glueSeen, (uint32_t)(ticksToMs(frameStart - startTick)), glueCreatures);
                    if (!glueCreatures.empty() && glueCreatures[0].disp && glueCreatures[0].disp->ready()) {
                        const W3dCreatureHeader* h = glueCreatures[0].disp->model->h;
                        const float sh = (h->bmax[2] - h->bmin[2]) * glueCreatures[0].disp->d.scale;
                        if (sh > 0.3f && sh < 6.0f) height = sh;
                    }
                    const float tanV = tanf(glueFov * 0.5f * 3.14159265f / 180.0f);
                    const float dist = (height * 1.3f * 0.5f) / tanV;
                    // the character list takes the right of the screen: the character stands left of the middle
                    const float shift = g_login.phase() == LoginFlow::Phase::CharCreate ? 0.0f : 0.33f;
                    const float right[3] = {-dir[1], dir[0], 0};   // the camera's right: forward (-dir) x up
                    const float hw = dist * tanV * 400.0f / 240.0f;  // half the visible width at the character
                    for (int k = 0; k < 3; ++k) {
                        target[k] = g.stand[k] + (k == 2 ? height * 0.52f : 0.0f) + right[k] * shift * hw;
                        eyePos[k] = target[k] + dir[k] * dist;
                    }
                    glueNear = std::max(0.2f, dist * 0.08f);
                    glueFar = std::max(400.0f, g.radius * 2.5f);
                }
                float f[3] = {target[0] - eyePos[0], target[1] - eyePos[1], target[2] - eyePos[2]};
                const float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                if (fl > 1e-4f) {
                    memcpy(glueCam.pos, eyePos, sizeof glueCam.pos);
                    glueCam.yaw = atan2f(f[1], f[0]);
                    glueCam.pitch = asinf(std::clamp(f[2] / fl, -1.0f, 1.0f));
                    glueCam.drawDist = glueFar;
                    glueScene = true;
                }
            }
            g_login.setSceneDrawn(glueScene);
        }

        // --- stream, then draw
        streamerUpdate(cam.pos, cam.drawDist);
        fc = FrameCounters();
        u64 beforeWait = svcGetSystemTick();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        u64 afterWait = svcGetSystemTick();
        {
            C3D_RenderTarget* left = topModeNow == 1 ? topWide : top;   // wide: the 400-wide HUD layout is stretched x2 (pixels are half as wide)
            const float slider = topModeNow == 2 ? osGet3DSliderState() : 0.0f;
            const bool stereo = slider > 0.02f && topRight;
            const float iod = slider * 0.12f;   // yards; the zero-parallax plane is 5 yd away (render.cpp)
            for (int eye = 0; eye < (stereo ? 2 : 1); ++eye) {
                C3D_RenderTarget* t = eye == 0 ? left : topRight;
                if (stereo) rendererSetEye(eye == 0 ? -iod : iod);
                C3D_RenderTargetClear(t, C3D_CLEAR_ALL, loginUi ? 0x05080CFF : rendererClearColor(rs), 0);
                C3D_FrameDrawOn(t);
                if (loginUi) {   // the login screens: their scene, then their frames
                    if (glueScene) rendererDrawScene(glueCam, glueFov, glueNear, glueFar, glueModels, &glueCreatures, fc);
                    uiDraw(g_login.glueTop(), kHudW);
                } else {
                    rendererDraw(t, cam, rs, fc, &markers, &creatures);
                    uiDraw(hud, kHudW);
                }
            }
            if (stereo) rendererSetEye(0);
        }
        if (bottomDirty && bottomT) {   // the touch screen only when its list changed
            C3D_RenderTargetClear(bottomT, C3D_CLEAR_COLOR, 0x080B0EFF, 0);
            C3D_FrameDrawOn(bottomT);
            uiDraw(loginUi ? g_login.glueBottom() : gameUi.screen(), kTouchW);
        }
        C3D_FrameEnd(0);
        uiEndFrame();
        streamerEndFrame();
        cpuMsLast = ticksToMs((beforeWait - frameStart) + (svcGetSystemTick() - afterWait));

        if (bench.running && ++bench.warmup > 3) {
            bench.frameMs.push_back(frameMs);
            bench.cpuMs.push_back((float)cpuMsLast);
            bench.gpuMs.push_back(C3D_GetDrawingTime());
            bench.maxTris = std::max(bench.maxTris, fc.tris);
            bench.maxDraws = std::max(bench.maxDraws, fc.drawCalls);
            if (!streamerTileReadyAt(cam.pos[0], cam.pos[1])) bench.missingGroundFrames++;
            bench.minLinearFree = std::min(bench.minLinearFree, (u32)linearSpaceFree());
        }

        // --- HUD (twice a second)
        ++fpsFrames;
        double windowMs = ticksToMs(svcGetSystemTick() - fpsStart);
        const bool gamePage = g_login.online() && gameUi.gamePage();
        if (windowMs >= 500.0 && !loginUi && gamePage) {   // keep the counters running
            fps = (float)(fpsFrames * 1000.0 / windowMs);
            wallUsShown = wallUs / (float)std::max<u32>(1, fpsFrames);
            wallUs = 0;
            fpsFrames = 0;
            fpsStart = svcGetSystemTick();
        } else if (windowMs >= 500.0 && !loginUi) {
            fps = (float)(fpsFrames * 1000.0 / windowMs);
            wallUsShown = wallUs / (float)std::max<u32>(1, fpsFrames);   // walls: microseconds per frame
            wallUs = 0;
            fpsFrames = 0;
            fpsStart = svcGetSystemTick();
            const StreamStats& s = streamerStats();
            int tx = (int)floorf(32.0f - cam.pos[1] / kTileSize), ty = (int)floorf(32.0f - cam.pos[0] / kTileSize);
            printf("\x1b[1;1Hw3dproto  stage 5: world          ");
            printf("\x1b[3;1HFPS %5.1f  CPU %5.2f  GPU %5.2f ms  ", fps, cpuMsLast, C3D_GetDrawingTime());
            printf("\x1b[4;1Htris %6lu draws %4lu binds %3lu     ", (unsigned long)fc.tris, (unsigned long)fc.drawCalls,
                   (unsigned long)fc.texBinds);
            printf("\x1b[5;1Hbatch %3lu wat %3lu dood %4lu bld %3lu ", (unsigned long)fc.visibleBatches,
                   (unsigned long)fc.waterBatches, (unsigned long)fc.visibleInstances, (unsigned long)fc.buildings);
            printf("\x1b[7;1Htile %d_%d  ready %d  loading %d  slots %d/%d  ", tx, ty, s.tilesReady, s.tilesLoading,
                   s.slotsUsed, s.slotsTotal);
            printf("\x1b[8;1Hjobs %3d uploads %3d (%.1f ms) core %d  ", s.jobsQueued, s.uploadsPending,
                   s.uploadMsLastFrame, s.workerCore);
            printf("\x1b[9;1Hloads %lu  last %4.0f ms  max %4.0f ms   ", (unsigned long)s.tileLoads, s.lastTileMs,
                   s.maxTileMs);
            printf("\x1b[10;1Hmodels %3d tex %3d crt %2lu/%2d %4.1fms ", s.models, s.textures,
                   (unsigned long)fc.creatures, creatureView.ready(), fc.poseMs);
            printf("\x1b[11;1HVRAM tex %4lu KB  geom %5lu KB      ", (unsigned long)(s.texVram / 1024),
                   (unsigned long)(s.geomBytes / 1024));
            printf("\x1b[12;1Hlinear free %6lu KB  VRAM free %5lu KB ", (unsigned long)(linearSpaceFree() / 1024),
                   (unsigned long)(vramSpaceFree() / 1024));
            printf("\x1b[6;1Hterrain LOD 0/1/2: %3lu/%3lu/%3lu batches  ", (unsigned long)fc.lodBatches[0],
                   (unsigned long)fc.lodBatches[1], (unsigned long)fc.lodBatches[2]);
            printf("\x1b[13;1Hwalls %-7s %4.0fus coll %4luKB%s     ", walk ? (wallHit ? "CONTACT" : "clear") : "off",
                   wallUsShown, (unsigned long)(s.collBytes / 1024), s.collFailed ? " NOMEM" : "");
            static const char* kDetail[3] = {"near", "norm", "far"};
            printf("\x1b[14;1H%s dist %3.0f doodads %-3s %-4s fog %-3s  ",
                   walk ? (g_login.online() && g_login.game().mover.swimming() ? "SWIM " : onFloor ? "FLOOR" : "WALK ") : "FLY  ", cam.drawDist,
                   rs.doodads ? "on" : "off", kDetail[detailIdx], rs.fog ? "on" : "off");
            printf("\x1b[15;1H%-40s", bench.running ? "BENCHMARK RUNNING (40 s)..." : "Y: 40 s streaming benchmark");
            printf("\x1b[17;1HCirclePad move/turn CStick look   ");
            printf("\x1b[18;1HL/R down/up  B fast%s SELECT walk/fly", g_login.online() ? "/jump" : "     ");
            printf("\x1b[19;1HX dist  A doodads  ZR fog  ZL detail   ");
            printf("\x1b[20;1HSTART quit                             ");
            if (g_login.online()) {
                printf("\x1b[22;1HNET %-22.22s %4lu ms %5llu KB ", netNote.c_str(), (unsigned long)g_login.latencyMs(),
                       (unsigned long long)(g_login.bytesReceived() / 1024));
                w3n::GameSession& g = g_login.game();
                const w3n::ObjectStats& os = g.objects.stats;
                printf("\x1b[23;1HOBJ pl %3u npc %3u go %3u  upd %4lu bad %lu  ", (unsigned)g.objects.count(w3n::ObjType::Player),
                       (unsigned)g.objects.count(w3n::ObjType::Unit), (unsigned)g.objects.count(w3n::ObjType::GameObject),
                       (unsigned long)(os.updatePackets + os.compressedPackets), (unsigned long)os.failedPackets);
                printf("\x1b[24;1HMOVE sent %5lu tp %lu run %4.1f %s %s  ", (unsigned long)g.mover.packetsSent,
                       (unsigned long)g.stats.teleports, g.mover.speeds.run, g.mover.rooted() ? "ROOT" : "    ",
                       g_login.logging() ? "LOG" : "   ");
                // nearest units / players
                struct Near { float d; const w3n::WorldObject* o; };
                static std::vector<Near> near;
                near.clear();
                const uint32_t now = g_login.sessionMs();
                for (auto& [guid, o] : g.objects.all()) {
                    if (o.self || !o.hasPos || !o.isUnitLike()) continue;
                    float q[3];
                    o.positionAt(now, q);
                    float d = sqrtf((q[0] - pl[0]) * (q[0] - pl[0]) + (q[1] - pl[1]) * (q[1] - pl[1]) + (q[2] - pl[2]) * (q[2] - pl[2]));
                    near.push_back({d, &o});
                }
                int nn = (int)std::min<size_t>(5, near.size());
                std::partial_sort(near.begin(), near.begin() + nn, near.end(), [](const Near& x, const Near& y) { return x.d < y.d; });
                const Near* nearest = near.data();
                for (int i = 0; i < 5; ++i) {
                    if (i < nn) {
                        const w3n::WorldObject& o = *nearest[i].o;
                        unsigned pct = o.maxHealth() ? (unsigned)(100ull * o.health() / o.maxHealth()) : 0;
                        printf("\x1b[%d;1H%5.1f %-19.19s L%-2lu %3u%%%s      ", 25 + i, nearest[i].d,
                               o.name.empty() ? "..." : o.name.c_str(), (unsigned long)o.level(), pct,
                               o.type == w3n::ObjType::Player ? " P" : "  ");
                    } else {
                        printf("\x1b[%d;1H%-39s", 25 + i, "");
                    }
                }
            }
            else if (g_login.phase() == LoginFlow::Phase::Offline)
                printf("\x1b[22;1HNET offline                              ");
        }
    }

    writeReport(bench.frameMs.empty() ? nullptr : &bench, cam.drawDist, rs);
    g_login.shutdown();
    creatureView.clear();
    streamerShutdown();
    audio::shutdown();
    uiRenderShutdown();
    rendererShutdown();
    C3D_Fini();
    romfsExit();
    gfxExit();
    return 0;
}
