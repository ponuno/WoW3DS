// Stage 5.33: spell effects with the real streamer (stubbed libctru) over the pack of w3dconv --spell-fx: the SVFX entry
// read through streamerEntrySize / streamerReadEntry, effect models acquired as SpellView does, their emitters run by the
// particle system as priority sources, everything let go at the end (no linear memory or texture left; ASan: no particle
// reading a freed model).   spell_stream_test <sfx.w3dp>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <thread>
#include <vector>

#include "../source/fx.hpp"
#include "../source/spellfx.hpp"
#include "../source/world.h"

size_t stubLinearUsed();
size_t stubTexturesLive();
void pcTestIoDelay(int, uint32_t, uint32_t) {}

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    PackIndex index;
    char err[160];
    if (!index.open(argv[1], err, sizeof err) || !streamerInit(&index, err, sizeof err)) { printf("init: %s\n", err); return 2; }
    const uint32_t size = streamerEntrySize(W3D_ENTRY_SPELLFX, 0);
    std::vector<uint8_t> buf(size);
    CHECK(size && streamerReadEntry(W3D_ENTRY_SPELLFX, 0, buf.data(), size), "SVFX read through the streamer (%u bytes)", size);
    sfx::Table table;
    CHECK(table.load(buf.data(), size), "table");
    sfx::Director dir;
    dir.setTable(&table);
    std::map<uint64_t, std::array<float, 3>> units = {{1, {0, 0, 0}}, {2, {18, 4, 0}}};
    auto where = [&](uint64_t g, int slot, float out[3], float* yaw) {
        auto it = units.find(g);
        if (it == units.end()) return false;
        memcpy(out, it->second.data(), 12);
        out[2] += slot == W3D_SLOT_BASE ? 0.0f : 1.2f;
        if (yaw) *yaw = 0;
        return true;
    };
    std::vector<Model*> models;
    auto modelFor = [&](uint16_t e) -> Model* {
        const W3dSpellEffect* ef = table.effect(e);
        if (!ef) return nullptr;
        if (models.size() <= e) models.resize(e + 1, nullptr);
        if (!models[e]) models[e] = streamerAcquireItemModel(ef->model);
        return models[e];
    };
    fx::System& fxs = fx::global();
    fxs.setBudget(fx::budgetFor(2));
    const float cam[3] = {-5, 0, 2};
    const float rows[3][3] = {{0, -1, 0}, {0, 0, 1}, {-1, 0, 0}};   // looking along +X
    std::vector<sfx::Placed> placed;
    std::vector<fx::Source> src;
    std::vector<fx::Quad> quads;
    size_t peakParticles = 0, maxPlaced = 0, drawnModels = 0;
    uint32_t now = 1000;
    int casts = 0;
    for (int f = 0; f < 900; ++f, now += 33) {
        streamerUpdate(cam, 250.0f);
        if (f % 90 == 0) {   // a Fireball every 3 s: 1 s of casting, then the missile
            sfx::Event e;
            e.kind = sfx::Event::Start; e.caster = 1; e.spell = casts % 2 ? 116 : 133; e.castMs = casts % 2 ? 0 : 1000;
            dir.event(e, now, cam, where);
            ++casts;
        }
        if (f % 90 == 30) {
            sfx::Event e;
            e.kind = sfx::Event::Go; e.caster = 1; e.spell = casts % 2 ? 133 : 116; e.targets[0] = 2; e.nTargets = 1;
            dir.event(e, now, cam, where);
        }
        units[2][1] = 4.0f * sinf(f * 0.05f);   // the target walks about
        dir.update(now, where, placed);
        maxPlaced = std::max(maxPlaced, placed.size());
        src.clear();
        for (const sfx::Placed& p : placed) {
            Model* m = modelFor(p.effect);
            if (!m || !m->drawable) continue;
            ++drawnModels;
            if (!p.emitting || !m->fx) continue;
            fx::Source s;
            s.key = (const void*)(uintptr_t)p.key;
            s.owner = m; s.blob = m->blob; s.h = m->fx;
            s.tex = (const void* const*)(m->tex.data() + m->fxTexBase);
            memcpy(s.pos, p.pos, 12); memcpy(s.rot, p.rot, 12);
            s.scale = p.scale;
            s.priority = true;
            src.push_back(s);
        }
        fxs.frame(0.033f, f * 0.033f, cam, rows, src, quads);
        peakParticles = std::max(peakParticles, fxs.alive());
        for (const fx::Quad& q : quads) CHECK(std::isfinite(q.c[0]) && q.half > 0 && q.half < 3, "quad sane");
        streamerEndFrame();
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    const sfx::Stats& st = dir.stats();
    printf("spell effects: %u started, %u missiles, %u impacts; at most %zu at once, %zu models, peak %zu particles\n", st.started,
           st.missiles, st.impacts, maxPlaced, models.size(), peakParticles);
    CHECK(st.missiles >= 8 && st.impacts >= 4, "missiles flew, the fireballs hit (frostbolt has no impact kit)");
    CHECK(drawnModels > 0, "the effect models were read and became drawable");
    CHECK(peakParticles > 20, "their emitters ran (%zu)", peakParticles);
    // ---- letting go: the models (as SpellView::clear), then the streamer
    dir.clear();
    for (Model*& m : models) { if (m) streamerReleaseItemModel(m); m = nullptr; }
    for (int f = 0; f < 5; ++f) {   // the particles of freed models are gone with them (fxForgetModel)
        fxs.frame(0.033f, 0, cam, rows, {}, quads);
        streamerEndFrame();
    }
    CHECK(fxs.alive() == 0, "no particle outlives its model (%zu)", fxs.alive());
    streamerShutdown();
    CHECK(stubLinearUsed() == 0 && stubTexturesLive() == 0, "everything freed (linear %zu, textures %zu)", stubLinearUsed(),
          stubTexturesLive());
    printf("SPELL STREAM: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
