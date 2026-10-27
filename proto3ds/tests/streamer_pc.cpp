// Runs proto3ds/source/streamer.cpp on the PC with stubbed libctru: the camera
// flies a circuit over a pack and every frame the resident data is checked the
// way the GPU would use it (model indices within the vertex buffer, textures
// uploaded, submesh ranges). At the end every linear/VRAM byte must be freed.
//   streamer_pc <pack.w3dp> [frames] [cx cy radius]
#include "../source/world.h"
#include "../source/creature_view.hpp"
#include "w3d_anim.h"

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
    size_t checks = 0, problems = 0, maxInst = 0, maxModels = 0;
    std::set<uint32_t> modelsSeen;
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
    for (int f = 0; f < frames; ++f) {
        float a = (float)f / frames * 6.2831853f * 2;
        float r = rad * (0.25f + 0.75f * std::fabs(std::sin(a * 0.5f)));
        float cam[3] = {cx + std::cos(a) * r, cy + std::sin(a) * r, 120.0f};
        float dist = (f / 500) % 2 ? 800.0f : 250.0f;   // switches the 3x3 / 5x5 radius too
        streamerUpdate(cam, dist);
        size_t inst = 0;
        for (Tile* t : streamerReadyTiles()) {
            for (const Instance& in : t->instances) {
                ++inst;
                Model* m = in.model;
                if (m->state != AssetState::Ready || !m->h) continue;
                const W3dModelHeader* h = m->h;
                const uint16_t* idx = (const uint16_t*)(m->blob + h->offIndices);
                for (u32 s = 0; s < h->submeshCount; ++s) {
                    const W3dSubmesh& sm = m->subs[s];
                    ++checks;
                    if ((u64)sm.firstIndex + sm.indexCount > h->indexCount || sm.indexCount % 3) { ++problems; continue; }
                    if (sm.tex != W3D_NO_TEXTURE && (sm.tex >= m->tex.size() || !m->tex[sm.tex])) { ++problems; continue; }
                }
                if (modelsSeen.insert(m->id).second)
                    for (u32 k = 0; k < h->indexCount; ++k) if (idx[k] >= h->vertexCount) { ++problems; break; }
                if (!(in.radius > 0) || !std::isfinite(in.pos[0])) ++problems;
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
    std::printf("%d frames: tile loads %u, max resident instances %zu, models %zu, distinct models drawn %zu\n", frames,
                s.tileLoads, maxInst, maxModels, modelsSeen.size());
    std::printf("submesh checks %zu, problems %zu; linear peak %.1f MB\n", checks, problems, stubLinearPeak() / 1048576.0);
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
    bool ok = problems == 0 && leakLin == 0 && leakTex == 0 && s.tileLoads > 0 && walkBad == 0;
    std::printf("STREAMER: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
