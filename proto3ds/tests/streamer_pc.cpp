#include <algorithm>
// Runs proto3ds/source/streamer.cpp on the PC with stubbed libctru: the camera
// flies a circuit over a pack and every frame the resident data is checked the
// way the GPU would use it (model indices within the vertex buffer, textures
// uploaded, submesh ranges). At the end every linear/VRAM byte must be freed.
//   streamer_pc <pack.w3dp> [frames] [cx cy radius]
#include "../source/world.h"
#include "../source/creature_view.hpp"
#include "../source/fx.hpp"
#include "w3d_anim.h"
#include "w3d_farlod.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <set>
#include <thread>

size_t stubLinearUsed();
size_t stubLinearPeak();
size_t stubTexturesLive();

// Stage 5.30: the reads as slow as an SD card when SD_KBPS is set (+ SD_SEEK_US for a jump in the file), so the order of
// the reads shows in the timings.
void pcTestIoDelay(int file, uint32_t offset, uint32_t size) {
    static const char* kb = std::getenv("SD_KBPS");
    static const char* sk = std::getenv("SD_SEEK_US");
    if (!kb) return;
    static int lastFile = -1;
    static uint32_t lastEnd = 0;
    double us = size * 1000.0 / std::atof(kb);
    if (file != lastFile || offset != lastEnd) us += sk ? std::atof(sk) : 0.0;
    lastFile = file;
    lastEnd = offset + size;
    std::this_thread::sleep_for(std::chrono::microseconds((long long)us));
}

static size_t g_farFrames = 0, g_farOverlap = 0;
static size_t g_foFrames = 0, g_foWrong = 0, g_foHiddenFrames = 0;   // 6.29: far buildings
static size_t g_pk[5] = {0, 0, 0, 0, 0};   // submesh problems by kind: not ready, index range, texture, vertex index, radius/position

static int g_maxRingTiles = 0;   // 6.31
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    PackIndex index;
    char err[160];
    if (!index.open(argv[1], err, sizeof err) || !streamerInit(&index, err, sizeof err)) { std::printf("init: %s\n", err); return 2; }
    int frames = argc > 2 ? std::atoi(argv[2]) : 3000;
    const char* paceEnv = std::getenv("PACE_US");
    int pace = paceEnv ? std::atoi(paceEnv) : 1000;   // per frame, so the IO thread gets to run (TSan is slow)
    float cx, cy, rad;
    if (argc > 5) { cx = std::atof(argv[3]); cy = std::atof(argv[4]); rad = std::atof(argv[5]); }
    else {
        int tx = (index.minTileX + index.maxTileX) / 2, ty = (index.minTileY + index.maxTileY) / 2;
        cx = (32.0f - ty - 0.5f) * kTileSize; cy = (32.0f - tx - 0.5f) * kTileSize; rad = kTileSize * 0.8f;
    }
    size_t bldgProblems = 0, bldgGroups = 0, bldgCompleteClosed = 0;
    size_t checks = 0, problems = 0, maxInst = 0, maxModels = 0, chunkProblems = 0, chunksSeen = 0;
    const auto tStart = std::chrono::steady_clock::now();
    std::set<uint64_t> shownKey;            // placements already counted
    std::vector<std::pair<double, float>> shownAt;   // (ms since start, distance then)
    std::set<uint64_t> groundSeen;
    std::vector<double> groundAt;                    // when each tile's ground first showed
    std::set<uint64_t> firstShown;      // stage 5.30: placements in the order they showed ...
    std::vector<float> shownOrder;      // ... and their distance from the camera then
    std::set<uint32_t> modelsSeen;
    std::set<uint32_t> fxModels;   // stage 5.32
    size_t fxPeak = 0, fxQuadPeak = 0, fxProblems = 0;
    // Creatures (stage 5.3): synthetic units cycling through every display in the pack,
    // appearing/disappearing, changing display, walking, running and dying.
    std::vector<uint32_t> displayIds;
    for (const W3dEntry& e : index.entries) if (e.type == W3D_ENTRY_DISPLAY) displayIds.push_back(e.id);
    displayIds.push_back(987654321);   // not in the pack: must stay a marker
    // Players (stage 5.5): if the pack has race models, 6 more units are players whose looks
    // change now and then (incl. values the tables do not carry, and a race not in the pack).
    std::vector<uint32_t> charIds;
    for (const W3dEntry& e : index.entries) if (e.type == W3D_ENTRY_CHARACTER) charIds.push_back(e.id);
    size_t playersDrawn = 0, playerProblems = 0, playerLooks = 0;
    // Worn items (stage 5.19): players (and some creatures) wear ITEM entries of the pack in changing slots.
    std::vector<uint32_t> itemIds;
    for (const W3dEntry& e : index.entries) if (e.type == W3D_ENTRY_ITEM) itemIds.push_back(e.id);
    size_t wornDrawn = 0, wornProblems = 0;
    auto equipUnit = [&](CreatureSeen& c, int u, int f) {
        if (itemIds.empty()) return;
        static const int slots[] = {W3D_EQ_HEAD, W3D_EQ_SHOULDERS, W3D_EQ_SHIRT, W3D_EQ_CHEST, W3D_EQ_LEGS, W3D_EQ_FEET, W3D_EQ_BACK,
                                    W3D_EQ_MAINHAND, W3D_EQ_OFFHAND, W3D_EQ_RANGED, W3D_EQ_TABARD};
        for (int slot : slots) {
            int pick = (u * 7 + slot * 3 + f / 131) % (int)(itemIds.size() + 2);   // two out of range values = empty
            if (pick >= (int)itemIds.size()) continue;
            c.equip[slot] = itemIds[pick];
            c.equipInv[slot] = slot == W3D_EQ_OFFHAND ? (u % 2 ? 14 : 22) : slot == W3D_EQ_MAINHAND ? (u % 2 ? 17 : 13)
                               : slot == W3D_EQ_RANGED ? (u % 2 ? 15 : 26) : 0;
        }
        c.inCombat = (f / 61 + u) % 2 == 0;
        if ((f / 101 + u) % 5 == 0) { c.castGoMs = 1 + (uint32_t)(f / 45) * 1500; c.castGoLook = CastLook::Bow; }
    };
    std::set<uint64_t> looksSeen;
    CreatureView view;
    std::vector<CreatureSeen> seen;
    std::vector<CreatureInstance> crt;
    std::vector<W3dMat34> pose;
    size_t crtDrawn = 0, crtChecks = 0, crtProblems = 0, deathSeqs = 0, walkSeqs = 0, runSeqs = 0, attackSeqs = 0;
    uint32_t nowMs = 0;
    int memWalks = 0, memMismatch = 0;
    const bool inFrame = std::getenv("UPLOAD_IN_FRAME") != nullptr;
    for (int f = 0; f < frames; ++f) {
        float a = (float)f / frames * 6.2831853f * 2;
        float r = rad * (0.25f + 0.75f * std::fabs(std::sin(a * 0.5f)));
        float cam[3] = {cx + std::cos(a) * r, cy + std::sin(a) * r, 120.0f};
        if (std::getenv("CAM_WALK")) {   // stage 5.30: arrive in the middle and walk slowly east (a city visit)
            cam[0] = cx; cam[1] = cy + f * std::atof(std::getenv("CAM_WALK")); cam[2] = 60.0f;
        }
        float dist = std::getenv("CAM_WALK") ? 250.0f : (f / 500) % 2 ? 800.0f : 250.0f;   // switches the 3x3 / 5x5 radius too
        if (inFrame) { streamerUpdate(cam, dist, true); streamerUploadTextures(); }   // stage 6: the main loop's way
        else streamerUpdate(cam, dist);
        {   // 6.24: the far ground - drawn only for tiles without their detailed ground, its buffers always valid
            static std::vector<FarDraw> far;
            GpuTex* atlas = nullptr;
            const uint16_t* fidx = nullptr;
            float zs = 0;
            if (streamerFarDraws(cam, dist, far, &atlas, &fidx, &zs)) {
                ++g_farFrames;
                for (const FarDraw& d : far) {
                    const int tx = (int)std::floor(32.0f - (d.origin[1] + 1.0f) / kTileSize), ty = (int)std::floor(32.0f - (d.origin[0] + 1.0f) / kTileSize);
                    for (Tile* t : streamerReadyTiles()) if (t->tx == tx && t->ty == ty) ++g_farOverlap;
                    if (!d.verts || !fidx || !atlas || zs <= 0) ++g_farOverlap;
                    volatile int16_t probe = d.verts[W3D_FAR_VERTS - 1].z;   // ASan: the buffer is alive
                    (void)probe;
                }
            }
        }
        {   // 6.29: the far buildings - never drawn from a tile that shows all of its buildings, the ranges inside live buffers
            static std::vector<FarObjDraw> fo;
            GpuTex* pal = nullptr;
            float zs = 0;
            const bool any = streamerFarObjectDraws(cam, dist, fo, &pal, &zs);
            if (any) ++g_foFrames;
            // a tile "shows everything": every building group of it is shown (its Bldg open, or a one-group building drawable)
            auto showsAll = [](const Tile* t) {
                bool any = false;
                for (const Instance& in : t->instances) {
                    if (!in.model || !in.model->building) continue;
                    any = true;
                    if (in.bldg ? !(in.bldg <= t->bldgs.size() && t->bldgs[in.bldg - 1].open) : !in.model->drawable) return false;
                }
                return any;
            };
            for (const FarObjDraw& d : fo) {
                if (!d.verts || !d.idx || !d.count || d.count % 3 || !pal || zs <= 0) ++g_foWrong;
                const int tx = (int)std::floor(32.0f - (d.origin[1] + 1.0f) / kTileSize), ty = (int)std::floor(32.0f - (d.origin[0] + 1.0f) / kTileSize);
                for (Tile* t : streamerReadyTiles())
                    if (t->tx == tx && t->ty == ty && showsAll(t)) {
                        if (!g_foWrong && std::getenv("FAROBJ_DEBUG")) std::printf("wrong: tile %d_%d, draw of %u indices\n", tx, ty, d.count);
                        ++g_foWrong;
                    }
                volatile uint16_t probe = d.idx[d.count - 1];   // ASan: the buffers are alive
                volatile int16_t probeV = d.verts[probe].z;
                (void)probeV;
            }
            // a frame where a tile shows all its buildings and nothing of it is drawn far: the shells did go
            for (Tile* t : streamerReadyTiles())
                if (showsAll(t)) { ++g_foHiddenFrames; break; }
        }
        {   // stage 5.32: the effects of what is near, as render.cpp drawFx does (models freed meanwhile must take their
            // particles with them: ASan would catch a particle reading a freed blob)
            static std::vector<fx::Source> fsrc;
            static std::vector<fx::Quad> fq;
            fsrc.clear();
            for (Tile* t : streamerReadyTiles())
                for (const Instance& in : t->instances) {
                    const Model* m = in.model;
                    if (!m->fx || !m->drawable) continue;
                    if ((size_t)m->fxTexBase + m->fx->texCount > m->tex.size()) { ++fxProblems; continue; }
                    fxModels.insert(m->id);
                    fx::Source fs;
                    fs.key = &in; fs.owner = m; fs.blob = m->blob; fs.h = m->fx;
                    fs.tex = (const void* const*)(m->tex.data() + m->fxTexBase);
                    for (int k = 0; k < 3; ++k) { fs.pos[k] = in.pos[k]; fs.rot[k] = in.rot[k]; }
                    fs.scale = in.scale;
                    fsrc.push_back(fs);
                }
            static const float rows[3][3] = {{0, -1, 0}, {0, 0, 1}, {-1, 0, 0}};
            fx::Budget fb = fx::budgetFor(2);
            fb.emitRange = fb.glowRange = 5000.0f;   // the camera flies high and far here: every lamp runs
            fx::global().setBudget(fb);
            fx::global().frame(1.0f / 30, f / 30.0f, cam, rows, fsrc, fq);
            for (const fx::Quad& q : fq) {
                if (q.tex) {   // a texture of a model that is still alive (forgotten ones took their particles)
                    const GpuTex* gt = (const GpuTex*)q.tex;
                    fxProblems += gt->refs <= 0;
                }
                fxProblems += !std::isfinite(q.c[0]) || !std::isfinite(q.half);
            }
            fxPeak = std::max(fxPeak, fx::global().alive());
            fxQuadPeak = std::max(fxQuadPeak, fq.size());
        }
        size_t inst = 0;
        for (Tile* t : streamerReadyTiles())
            if (groundSeen.insert((uint64_t)t->tx << 8 | (uint64_t)t->ty).second)
                groundAt.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count());
        for (Tile* t : streamerReadyTiles()) {   // stage 6.10: the chunks cover the instances, each inside its sphere
            uint32_t next = 0;
            for (const Tile::Chunk& ch : t->chunks) {
                if (ch.begin != next || ch.end <= ch.begin || ch.end > t->instances.size()) { ++chunkProblems; break; }
                for (uint32_t i = ch.begin; i < ch.end; ++i) {
                    const float* p = t->instances[i].pos;
                    const float d = std::sqrt((p[0] - ch.center[0]) * (p[0] - ch.center[0]) + (p[1] - ch.center[1]) * (p[1] - ch.center[1]) +
                                              (p[2] - ch.center[2]) * (p[2] - ch.center[2]));
                    if (d > ch.half * 1.0001f + 0.01f) ++chunkProblems;
                }
                next = ch.end;
                ++chunksSeen;
            }
            if (next != t->instances.size()) ++chunkProblems;
        }
        for (Tile* t : streamerReadyTiles()) {   // stage 6.11: the buildings (placements sharing a transform)
            int closed = 0;
            for (size_t b = 0; b < t->bldgs.size(); ++b) {
                const Tile::Bldg& g = t->bldgs[b];
                if (!g.open) ++closed;
                ++bldgGroups;
                for (uint32_t i : g.members) {
                    if (i >= t->instances.size() || t->instances[i].bldg != b + 1) { ++bldgProblems; continue; }
                    const Instance& a = t->instances[i];
                    const Instance& f = t->instances[g.members[0]];
                    if (memcmp(a.rot, f.rot, sizeof a.rot) || a.scale != f.scale) ++bldgProblems;   // (the position is each group's own)
                }
                if (g.members.size() < 2 && !g.open) ++bldgProblems;   // nothing to wait for
            }
            for (const Instance& in : t->instances) if (in.bldg > t->bldgs.size()) ++bldgProblems;
            if (closed != t->bldgsClosed) ++bldgProblems;
            if (t->complete && t->bldgsClosed) ++bldgCompleteClosed;   // a complete tile has every model drawable: nothing waits
        }
        for (Tile* t : streamerReadyTiles()) {
            for (Instance& in : t->instances) {
                ++inst;
                Model* m = in.model;
                // stage 5.30: as the renderer, each model once it and its textures are in (the tile shows its ground first)
                if (!m->drawable) continue;
                if (m->state != AssetState::Ready || !m->h || !m->texturesReady()) { ++problems; ++g_pk[0]; continue; }
                if (in.radius <= 0) in.radius = m->h->radius * in.scale;
                {
                    const uint64_t k2 = (uint64_t)(uintptr_t)t << 20 ^ (uint64_t)(&in - t->instances.data());
                    if (shownKey.insert(k2).second) {
                        const float dx = in.pos[0] - cam[0], dy = in.pos[1] - cam[1];
                        shownAt.push_back({std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count(),
                                           std::sqrt(dx * dx + dy * dy)});
                    }
                }
                {   // when each placement first showed, and how far from the camera it was then
                    const uint64_t key = (uint64_t)(uintptr_t)t << 20 ^ (uint64_t)(&in - t->instances.data());
                    if (firstShown.insert(key).second) {
                        const float dx = in.pos[0] - cam[0], dy = in.pos[1] - cam[1];
                        shownOrder.push_back(std::sqrt(dx * dx + dy * dy));
                    }
                }
                const W3dModelHeader* h = m->h;
                const uint16_t* idx = (const uint16_t*)(m->blob + h->offIndices);
                for (u32 s = 0; s < h->submeshCount; ++s) {
                    const W3dSubmesh& sm = m->subs[s];
                    ++checks;
                    if ((u64)sm.firstIndex + sm.indexCount > h->indexCount || sm.indexCount % 3) { ++problems; ++g_pk[1]; continue; }
                    if (sm.tex != W3D_NO_TEXTURE && (sm.tex >= m->tex.size() || !m->tex[sm.tex])) { ++problems; ++g_pk[2]; continue; }
                }
                if (modelsSeen.insert(m->id).second)
                    for (u32 k = 0; k < h->indexCount; ++k) if (idx[k] >= h->vertexCount) { ++problems; ++g_pk[3]; break; }
                // (a model with radius 0 is a one-vertex stand-in that only carries collision - 6.14: the renderer skips it; the check
                // counted every such placement as a problem since then)
                if (!std::isfinite(in.pos[0]) || (!(in.radius > 0) && m->h->radius > 0 && !m->meshHidden)) { ++problems; ++g_pk[4]; }
            }
        }
        maxInst = std::max(maxInst, inst);
        if (!displayIds.empty()) {
            nowMs += 33;
            seen.clear();
            for (int u = 0; u < 12; ++u) {
                if (((f / 97 + u) % 5) == 0) continue;   // units leave and come back
                CreatureSeen c{};
                c.guid = 0xF130000000000000ull + u;
                c.displayId = displayIds[(u + f / 211) % displayIds.size()];   // morphs now and then
                c.pos[0] = cam[0] + u; c.pos[1] = cam[1]; c.pos[2] = 0;
                c.yaw = 0.3f * u;
                c.scale = 1.0f;
                int st = (f / 50 + u) % 4;
                c.speed = st == 1 ? 2.5f : st == 2 ? 7.0f : 0.0f;
                c.dead = st == 3;
                c.swimming = u == 7;
                c.swingMs = (u % 3 == 0) ? 1 + (uint32_t)(f / 45) * 1500 : 0;   // melee now and then (stage 5.7)
                if (u % 4 == 1) equipUnit(c, u, f);
                seen.push_back(c);
            }
            for (int u = 0; u < 6 && !charIds.empty(); ++u) {
                if (((f / 89 + u) % 4) == 0) continue;
                CreatureSeen c{};
                c.guid = 0x0000000000000100ull + u;
                uint32_t cid = charIds[u % charIds.size()];
                int v = (f / 173 + u) % 7;
                c.lookParts = W3dLook{(uint8_t)(u == 5 ? 2 : cid / 2), (uint8_t)(cid & 1), (uint8_t)(v % 3), (uint8_t)(v % 6),
                                      (uint8_t)(v % 2 ? 4 : 3), (uint8_t)(v % 2 ? 7 : 3), (uint8_t)(v % 3 == 0 ? 2 : 0)};
                c.look = w3d_look_key(&c.lookParts);
                equipUnit(c, u, f);
                c.displayId = displayIds[u % displayIds.size()];   // fallback when the race is not packed
                c.pos[0] = cam[0] - u; c.pos[1] = cam[1] + 2; c.pos[2] = 0;
                c.scale = 1.0f;
                c.speed = (f / 40 + u) % 3 == 0 ? 7.0f : 0.0f;
                if (looksSeen.insert(c.look).second) ++playerLooks;
                seen.push_back(c);
            }
            view.update(seen, nowMs, crt);
            for (const CreatureInstance& ci : crt) {
                ++crtDrawn;
                const CreatureDisplay* d = ci.disp;
                if (!d || !d->ready() || d->id == 987654321u) { ++crtProblems; continue; }
                if (d->look) {   // player: composited body texture, geosets chosen from the model
                    ++playersDrawn;
                    bool okTex = d->tex[3] && d->tex[3]->state == AssetState::Ready &&
                                 (d->tex[3]->bytes == W3D_CHAR_ATLAS * W3D_CHAR_ATLAS * 2 || d->tex[3] == streamerWhiteTexture());
                    const W3dSkinSubmesh* psm = (const W3dSkinSubmesh*)(d->model->blob + d->model->h->offSubmeshes);
                    bool okGeo = d->d.geosetCount > 0;
                    for (int k = 0; k < d->d.geosetCount && okGeo; ++k) {
                        bool found = false;
                        for (u32 q = 0; q < d->model->h->submeshCount; ++q) found |= psm[q].geoset == d->d.geosets[k];
                        okGeo = found;
                    }
                    if (!okTex || !okGeo || d->build) ++playerProblems;
                }
                for (int w = 0; w < ci.wornCount; ++w) {   // item models: resident, bone and geometry valid
                    const CreatureInstance::Worn& wi = ci.worn[w];
                    const Model* m = wi.model;
                    ++wornDrawn;
                    bool ok = m && m->state == AssetState::Ready && m->h && wi.bone >= 0 && (u32)wi.bone < d->model->h->boneCount &&
                              std::isfinite(wi.pos[0]) && std::isfinite(wi.pos[1]) && std::isfinite(wi.pos[2]);
                    for (u32 q = 0; ok && q < m->h->submeshCount; ++q) {
                        const W3dSubmesh& sm2 = m->subs[q];
                        ok = (u64)sm2.firstIndex + sm2.indexCount <= m->h->indexCount && sm2.indexCount % 3 == 0 &&
                             (sm2.tex == W3D_NO_TEXTURE || sm2.tex < m->tex.size());
                    }
                    if (!ok) {
                        if (wornProblems < 3)
                            std::printf("worn problem: model %p state %d h %p bone %d/%u pos %f %f %f\n", (const void*)m, m ? (int)m->state : -1,
                                        m ? (const void*)m->h : nullptr, wi.bone, d->model->h->boneCount, wi.pos[0], wi.pos[1], wi.pos[2]);
                        ++wornProblems;
                    }
                }
                const uint8_t* b = d->model->blob;
                const W3dCreatureHeader* h = d->model->h;
                const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(b + h->offSubmeshes);
                const W3dBonePalette* pal = (const W3dBonePalette*)(b + h->offPalettes);
                const W3dSkinVertex* vx = (const W3dSkinVertex*)(b + h->offVertices);
                const W3dCreatureTex* ct = (const W3dCreatureTex*)(b + h->offTex);
                const uint16_t* idx = (const uint16_t*)(b + h->offIndices);
                if (ci.seq >= 0) {
                    uint16_t id = ((const W3dAnimSeq*)(b + h->offSeqs))[ci.seq].animId;
                    deathSeqs += id == W3D_ANIM_DEATH;
                    walkSeqs += id == W3D_ANIM_WALK;
                    runSeqs += id == W3D_ANIM_RUN;
                    attackSeqs += id >= W3D_ANIM_ATTACK_UNARMED && id <= W3D_ANIM_ATTACK_2HL;
                }
                pose.resize(h->boneCount);
                w3d_anim_pose(b, ci.seq, ci.timeMs, ci.loop, pose.data());
                for (const W3dMat34& m : pose)
                    for (float v : m.m) if (!std::isfinite(v)) { ++crtProblems; break; }
                for (u32 s = 0; s < h->submeshCount; ++s) {
                    ++crtChecks;
                    const W3dSkinSubmesh& m = sm[s];
                    if ((u64)m.firstIndex + m.indexCount > h->indexCount || m.indexCount % 3 || m.palette >= h->paletteCount) { ++crtProblems; continue; }
                    if (m.texSlot != W3D_NO_TEXTURE && m.texSlot >= h->texCount) { ++crtProblems; continue; }
                    if (m.texSlot != W3D_NO_TEXTURE && ct[m.texSlot].kind == W3D_CTEX_FIXED && !d->model->slotTex[m.texSlot]) ++crtProblems;
                    const W3dBonePalette& p = pal[m.palette];
                    for (int k = 0; k < p.count; ++k) if (p.bones[k] >= h->boneCount) { ++crtProblems; break; }
                    for (u32 q = 0; q < m.indexCount; ++q) {
                        uint16_t i = idx[m.firstIndex + q];
                        if (i >= h->vertexCount) { ++crtProblems; break; }
                        const W3dSkinVertex& v = vx[i];
                        if (v.bone[0] % 3 || v.bone[1] % 3 || v.bone[0] / 3 >= p.count || v.bone[1] / 3 >= p.count ||
                            v.weight[0] + v.weight[1] != 255) { ++crtProblems; break; }
                    }
                }
            }
            for (const CreatureSeen& c : seen)
                if (c.displayId == 987654321u && !c.look && view.drawn(c.guid)) ++crtProblems;
        }
        maxModels = std::max(maxModels, (size_t)streamerStats().models);
        if (dist > 450.0f) g_maxRingTiles = std::max(g_maxRingTiles, (int)streamerReadyTiles().size());   // 6.31
        if (f % 25 == 0) {   // stage 6 (perf log): the memory walk agrees with the running texture totals
            StreamMemory m;
            streamerMemory(&m);
            uint64_t sum = (uint64_t)m.bodyVramBytes + m.bodyLinearBytes;
            for (int c = 0; c < kStreamClasses; ++c) sum += (uint64_t)m.texVramByClass[c] + m.texLinearByClass[c];
            const StreamStats& ss = streamerStats();
            if (sum != (uint64_t)ss.texVram + ss.texLinear) {
                ++memMismatch;
                if (memMismatch < 4) std::printf("memory walk: textures %llu bytes, totals %llu\n", (unsigned long long)sum,
                                                 (unsigned long long)ss.texVram + ss.texLinear);
            }
            memWalks++;
        }
        streamerEndFrame();
        std::this_thread::sleep_for(std::chrono::microseconds(pace));
    }
    // Shut down in the middle of loading: jump to the far corner, one update, stop.
    // Creatures: a fresh set of units (displays still loading) is left acquired, so the
    // streamer's own force-release path at shutdown is exercised too.
    {
        float cam[3] = {cx - rad, cy - rad, 120.0f};
        view.clear();
        for (CreatureSeen& c : seen) c.guid += 1000;
        view.update(seen, nowMs, crt);
        streamerUpdate(cam, 800.0f);
        streamerEndFrame();
    }
    if (!displayIds.empty())
        std::printf("creatures: %zu drawn, %zu submesh checks, %zu problems; seq death %zu walk %zu run %zu attack %zu; displays now %d\n",
                    crtDrawn, crtChecks, crtProblems, deathSeqs, walkSeqs, runSeqs, attackSeqs, streamerStats().displays);
    if (!charIds.empty())
        std::printf("players: %zu drawn, %zu looks, %u body textures composited, %zu problems; player displays now %d\n",
                    playersDrawn, playerLooks, streamerStats().composites, playerProblems, streamerStats().players);
    if (!itemIds.empty())
        std::printf("worn items: %zu models drawn on units, %zu problems (%zu ITEM entries)\n", wornDrawn, wornProblems, itemIds.size());
    problems += crtProblems + playerProblems + wornProblems;
    if (!charIds.empty() && playersDrawn == 0) ++problems;
    if (displayIds.size() > 1 && crtDrawn == 0) ++problems;
    const StreamStats& s = streamerStats();
    {   // stage 5.30: how soon what is near shows (placements within 250 yd of the camera when they appeared)
        std::vector<double> nearT, allT;
        for (auto& [ms, d] : shownAt) { allT.push_back(ms); if (d < 250.0f) nearT.push_back(ms); }
        std::sort(nearT.begin(), nearT.end());
        std::sort(allT.begin(), allT.end());
        auto pct = [](const std::vector<double>& v, double q) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
        std::printf("ground: %zu tiles shown, first at %.0f ms, last at %.0f ms\n", groundAt.size(),
                    groundAt.empty() ? 0.0 : groundAt.front(), groundAt.empty() ? 0.0 : groundAt.back());
        std::printf("shown: near (<250 yd) %zu, 50%% at %.0f ms, 90%% at %.0f ms; all %zu, 50%% at %.0f ms, 90%% at %.0f ms, last at %.0f ms\n",
                    nearT.size(), pct(nearT, 0.5), pct(nearT, 0.9), allT.size(), pct(allT, 0.5), pct(allT, 0.9), allT.empty() ? 0.0 : allT.back());
    }
    std::printf("effects: %zu models with effects, peak %zu particles, %zu quads, %zu problems\n", fxModels.size(), fxPeak,
                fxQuadPeak, fxProblems);
    problems += fxProblems;
    std::printf("%d frames: tile loads %u, max resident instances %zu, models %zu, distinct models drawn %zu\n", frames,
                s.tileLoads, maxInst, maxModels, modelsSeen.size());
    std::printf("progressive: ground after avg %.0f ms, complete after avg %.0f ms (%u tiles); read queue max %d, dropped %u\n",
                s.tileLoads ? s.sumTileMs / s.tileLoads : 0.0, s.tileFullLoads ? s.sumTileFullMs / s.tileFullLoads : 0.0,
                s.tileFullLoads, s.maxQueued, s.jobsCancelled);
    if (shownOrder.size() >= 10) {   // the nearest show first: the first fifth closer on average than the last fifth
        const size_t q = shownOrder.size() / 5;
        double a = 0, b = 0;
        for (size_t i = 0; i < q; ++i) { a += shownOrder[i]; b += shownOrder[shownOrder.size() - 1 - i]; }
        std::printf("show order: first fifth avg %.0f yd, last fifth avg %.0f yd (%zu placements)\n", a / q, b / q, shownOrder.size());
        if (std::getenv("ORDER_EXPECT") && !(a < b)) { std::printf("show order: the far ones came first\n"); ++problems; }
    }
    std::printf("submesh checks %zu, problems %zu (not ready %zu, index range %zu, texture %zu, vertex index %zu, radius %zu); linear peak %.1f MB\n", checks, problems, g_pk[0], g_pk[1], g_pk[2], g_pk[3], g_pk[4], stubLinearPeak() / 1048576.0);
    {   // stage 6: IO accounting for the perf log
        uint64_t kb = 0;
        uint32_t rd = 0;
        for (int k = 0; k < kStreamJobKinds; ++k) { kb += s.ioBytesKind[k]; rd += s.ioReadsKind[k]; }
        std::printf("perf: %u reads (%u failed), %.1f MB, read time %.0f ms, IO CPU %.0f ms; memory walks %d, mismatches %d\n",
                    (unsigned)s.ioReads, (unsigned)s.ioFails, s.ioBytes / 1048576.0, s.ioReadMs, s.ioCpuMs, memWalks, memMismatch);
        if (kb != s.ioBytes || rd != s.ioReads || memMismatch || !s.ioReads) ++problems;
    }
    // every texture a tile or model asked for was in the pack(s) and was read (stage 5.27: also from the pool parts)
    std::printf("textures not in the pack %u, reads failed %u\n", (unsigned)s.texNotInPack, (unsigned)s.texReadFailed);
    if (s.texNotInPack || s.texReadFailed) ++problems;
    // 6.17 (env HOLE_EXPECT=N): the terrain holes of converter/tests/gen_terrain_layers.py - N sample points of the tile not on
    // drawn ground; inside a hole the filled-in height (a membrane) is never a floor to step up onto, beside it the ground is.
    int holeBad = 0;
    if (const char* he = std::getenv("HOLE_EXPECT")) {
        const float hc[3] = {cx, cy, 200.0f};
        for (int f = 0; f < 2000 && !streamerTileReadyAt(cx, cy); ++f) {
            streamerUpdate(hc, 250.0f);
            streamerEndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        int uncovered = 0, membraneChecks = 0, groundChecks = 0;
        for (Tile* t : streamerReadyTiles()) {
            if (t->covered.size() != (size_t)kHeightN * kHeightN) { ++holeBad; continue; }
            for (int y = 1; y < kHeightN - 1; ++y)
                for (int x = 1; x < kHeightN - 1; ++x) {
                    const float wx = t->th->origin[0] + (float)x / (kHeightN - 1) * kTileSize;
                    const float wy = t->th->origin[1] + (float)y / (kHeightN - 1) * kTileSize;
                    float g, fz;
                    bool hole = false;
                    if (!streamerGroundHeightEx(wx, wy, &g, &hole)) { ++holeBad; continue; }
                    const bool ok = streamerFloorHeight(wx, wy, g - 1.0f, &fz);   // feet 1 yd under the ground's height there
                    if (!t->covered[y * kHeightN + x]) {
                        ++uncovered;
                        ++membraneChecks;
                        if (!hole) { ++holeBad; std::printf("hole FAIL: point %d,%d not reported as a hole\n", x, y); }
                        if (ok && fz > g - 0.5f) { ++holeBad; std::printf("hole FAIL: %d,%d stepped up onto the membrane\n", x, y); }
                    } else if (!hole) {
                        ++groundChecks;
                        if (!ok || std::fabs(fz - g) > 0.01f) { ++holeBad; std::printf("hole FAIL: ground %d,%d not a step up\n", x, y); }
                    }
                }
        }
        if (uncovered != std::atoi(he)) { ++holeBad; std::printf("hole FAIL: %d uncovered sample points, want %s\n", uncovered, he); }
        std::printf("terrain holes: %d uncovered points (membrane checks %d), ground checks %d, %d wrong\n", uncovered, membraneChecks,
                    groundChecks, holeBad);
        problems += holeBad;
    }
    if (const char* fe = std::getenv("FAROBJ_EXPECT")) {   // 6.29: the far buildings
        const StreamStats& ss = streamerStats();
        std::printf("far buildings: %d in the map, drawn in %zu frames, hidden behind the real ones in %zu, %zu wrong\n", ss.farObjects,
                    g_foFrames, g_foHiddenFrames, g_foWrong);
        if (ss.farObjects != std::atoi(fe) || g_foWrong || !g_foFrames || !g_foHiddenFrames) { std::printf("far buildings FAIL\n"); ++problems; }
    }
    if (const char* fe = std::getenv("FAR_EXPECT")) {   // 6.24: the pack's far ground: its tiles, frames it was drawn in, overlaps
        const StreamStats& ss = streamerStats();
        std::printf("far ground: %d tiles, drawn in %zu frames, %zu wrong (drawn over a tile with its detailed ground)\n", ss.farTiles,
                    g_farFrames, g_farOverlap);
        if (ss.farTiles != std::atoi(fe) || g_farOverlap) { std::printf("far FAIL\n"); ++problems; }
    }
    // 6.18a (env GROUND_POINTS=file from converter/tests/ground_ref.py, GROUND_EXPECT=exact|old): the ground height and the hole
    // flag of streamerGroundHeightEx() at the reference points. exact: with GRND every point within 0.01 yd (and the hole flags
    // right); old: the same pack converted with --no-exact-ground - the 129x129 grid must be off by more than 0.2 yd somewhere
    // (the negative control: the test is able to tell the two apart).
    int groundBad = 0;
    if (const char* gp = std::getenv("GROUND_POINTS")) {
        const bool exact = !std::getenv("GROUND_EXPECT") || std::string(std::getenv("GROUND_EXPECT")) == "exact";
        const float hc[3] = {cx, cy, 200.0f};
        for (int f = 0; f < 2000 && !streamerTileReadyAt(cx, cy); ++f) {
            streamerUpdate(hc, 250.0f);
            streamerEndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::ifstream pf(gp);
        double x, y, z, nz;
        int hole, n = 0, wrongHole = 0, missing = 0;
        double worst = 0;
        while (pf >> x >> y >> z >> hole >> nz) {
            ++n;
            float g;
            bool h = false;
            if (!streamerGroundHeightEx((float)x, (float)y, &g, &h)) { ++missing; continue; }
            worst = std::max(worst, (double)std::fabs(g - z));
            wrongHole += (h ? 1 : 0) != hole;
        }
        std::printf("ground via the streamer (%s): %d points, worst error %.4f yd, %d missing, %d wrong hole flags\n", exact ? "GRND" : "old grid", n,
                    worst, missing, wrongHole);
        if (n < 1000 || missing) ++groundBad;
        if (exact && (worst > 0.01 || wrongHole)) ++groundBad;
        if (!exact && worst < 0.2) { ++groundBad; std::printf("ground FAIL: the old grid was expected to be off by more than 0.2 yd\n"); }
        problems += groundBad;
    }
    // Optional: walk the floor-height expectations of converter/tests/gen_wmo_data.py
    // (env WALK_EXPECT=expect.json) through the real streamerFloorHeight().
    int walkBad = 0;
    const char* wj = std::getenv("WALK_EXPECT");
    if (wj && *wj) {
        std::ifstream ej(wj);
        std::string js((std::istreambuf_iterator<char>(ej)), {});
        size_t at = js.find("\"walk\":"), open = js.find('[', at), close = js.find(']', open);
        std::string arr = js.substr(open, close - open);
        std::regex obj("\\{[^}]*\\}"), kv("\"(\\w+)\": (-?[0-9.eE+-]+)");
        std::vector<std::map<std::string, double>> pts;
        for (auto it = std::sregex_iterator(arr.begin(), arr.end(), obj); it != std::sregex_iterator(); ++it) {
            std::map<std::string, double> m;
            std::string o = it->str();
            for (auto k = std::sregex_iterator(o.begin(), o.end(), kv); k != std::sregex_iterator(); ++k) m[(*k)[1]] = std::stod((*k)[2]);
            pts.push_back(m);
        }
        float cam[3] = {(float)pts[0]["x"], (float)pts[0]["y"], 90.0f};
        for (int f = 0; f < 600 && !(streamerCollisionReadyAt(cam[0], cam[1]) && streamerCollisionReadyAt(cam[0], cam[1] + 30)); ++f) {
            streamerUpdate(cam, 250.0f);
            streamerEndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        float z = 80.0f;
        int floorPts = 0;
        for (auto& q : pts) {
            bool onFloor = false;
            float g;
            if (!streamerFloorHeight((float)q["x"], (float)q["y"], z, &g, &onFloor)) { ++walkBad; continue; }
            z = g;
            floorPts += onFloor;
            if (std::fabs(z - q["z"]) > 0.05) { ++walkBad; std::printf("walk FAIL (%.2f, %.2f) %.3f want %.3f\n", q["x"], q["y"], z, q["z"]); }
        }
        std::printf("floor walk through the streamer: %zu points, %d on building floors, %d wrong\n", pts.size(), floorPts, walkBad);

        // Walls: the named cases of gen_wmo_data.py through the real streamerCollideMove()
        // (tiles gathered by the streamer, a fence across a tile border included).
        at = js.find("\"wall_cases\":");
        if (at != std::string::npos) {
            open = js.find('[', at); close = js.find(']', open);
            arr = js.substr(open, close - open);
            int cases = 0, wallBad = 0, contacts = 0;
            for (auto it = std::sregex_iterator(arr.begin(), arr.end(), obj); it != std::sregex_iterator(); ++it) {
                std::map<std::string, double> m;
                std::string o = it->str();
                for (auto k = std::sregex_iterator(o.begin(), o.end(), kv); k != std::sregex_iterator(); ++k) m[(*k)[1]] = std::stod((*k)[2]);
                float x = (float)m["x"], y = (float)m["y"];
                float c2[3] = {x, y, 90.0f};
                for (int f = 0; f < 600 && !(streamerCollisionReadyAt(x - 15, y - 15) && streamerCollisionReadyAt(x + 15, y + 15) &&
                                             streamerCollisionReadyAt(x - 15, y + 15) && streamerCollisionReadyAt(x + 15, y - 15)); ++f) {
                    streamerUpdate(c2, 250.0f);
                    streamerEndFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                for (int st = 0; st < (int)m["steps"]; ++st) {
                    float tx = x + (float)m["dx"], ty = y + (float)m["dy"];
                    contacts += streamerCollideMove(x, y, (float)m["z"], &tx, &ty);
                    x = tx; y = ty;
                }
                double err = std::hypot(x - m["want_x"], y - m["want_y"]);
                if (err > 0.03) { ++wallBad; std::printf("wall FAIL case %d: (%.3f, %.3f) want (%.3f, %.3f)\n", cases, x, y, m["want_x"], m["want_y"]); }
                ++cases;
            }
            std::printf("wall cases through the streamer: %d cases, %d steps in contact, %d wrong\n", cases, contacts, wallBad);
            walkBad += wallBad + (cases == 0);
        }
    }
    // Stage 5.26: the liquid queries of converter/tests/gen_water_data.py (env LIQUID_EXPECT=expect.txt, Q lines)
    // through the real streamerLiquidAt() (LIQD loaded with the tiles; feet at the surface).
    const char* lq = std::getenv("LIQUID_EXPECT");
    if (lq && *lq) {
        std::ifstream ef(lq);
        std::string line;
        int n = 0, wrong = 0;
        while (std::getline(ef, line)) {
            if (line.size() < 2 || line[0] != 'Q') continue;
            float x, y, level; int wet, flags;
            if (std::sscanf(line.c_str() + 2, "%f %f %d %d %f", &x, &y, &wet, &flags, &level) != 5) continue;
            float c2[3] = {x, y, 90.0f};
            for (int f = 0; f < 600 && !streamerTileReadyAt(x, y); ++f) {
                streamerUpdate(c2, 250.0f);
                streamerEndFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            LiquidHere h;
            const bool got = streamerLiquidAt(x, y, wet ? level : 80.0f, &h);
            ++n;
            if (got != (wet != 0) || (got && (h.flags != flags || std::fabs(h.level - level) > 0.05f))) {
                ++wrong;
                std::printf("liquid FAIL (%.2f, %.2f): %d flags %d level %.2f, want %d %d %.2f\n", x, y, got, got ? h.flags : 0,
                            got ? h.level : 0.0f, wet, flags, level);
            }
        }
        std::printf("liquid queries through the streamer: %d, %d wrong\n", n, wrong);
        walkBad += wrong + (n == 0);
    }
    streamerShutdown();
    size_t leakLin = stubLinearUsed(), leakTex = stubTexturesLive();
    std::printf("after shutdown: linear in use %zu bytes, textures alive %zu\n", leakLin, leakTex);
    std::printf("tile chunks: %zu checked, %zu problems\n", chunksSeen, chunkProblems);
    std::printf("buildings: %zu groups seen, %zu problems, %zu complete tiles with a closed building; found %u, opened with every group %u, late %u, closed now %d\n",
                bldgGroups, bldgProblems, bldgCompleteClosed, s.bldgFound, s.bldgOpenedFull, s.bldgOpenedLate, s.bldgClosed);
    {   // 6.31: the heap reserve - with little heap left (W3D_HEAP_FREE_MB) the 5 x 5 ring of the far draw distance stays 3 x 3
        std::printf("ring at 533+ yd: at most %d tiles ready; heap shrinks %u, COLL in linear memory %u\n", g_maxRingTiles, (unsigned)s.heapShrinks,
                    (unsigned)s.collHeapMiss);
        if (const char* he = std::getenv("HEAP_EXPECT")) {
            const bool small = !std::strcmp(he, "small");
            const bool good = small ? (s.heapShrinks > 0 && g_maxRingTiles <= 9) : (s.heapShrinks == 0 && g_maxRingTiles > 9);
            std::printf("HEAP RESERVE (%s): %s\n", he, good ? "PASS" : "FAIL");
            if (!good) ++problems;
        }
    }
    bool ok = problems == 0 && leakLin == 0 && leakTex == 0 && s.tileLoads > 0 && walkBad == 0 && chunkProblems == 0 && chunksSeen > 0 && bldgProblems == 0 && bldgCompleteClosed == 0;
    std::printf("STREAMER: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
