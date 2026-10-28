// Stage 5.33: the spell effect director (source/spellfx.cpp) over the SVFX table w3dconv --spell-fx wrote from
// converter/tests/gen_spellfx_data.py: stages, hands, missiles and their speed, impacts on arrival, failing, range,
// the limit, units going away, lingering.
//   spellfx_test <sfx.w3dp> <expect.json>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "spellfx.hpp"
#include "w3d_format.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static std::vector<uint8_t> readEntry(const char* path, uint32_t type, uint32_t id) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (all.size() < sizeof(W3dPackHeader)) return {};
    W3dPackHeader h;
    memcpy(&h, all.data(), sizeof h);
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        W3dEntry e;
        memcpy(&e, all.data() + h.indexOffset + i * sizeof e, sizeof e);
        if (e.type == type && e.id == id && e.offset + e.size <= all.size())
            return std::vector<uint8_t>(all.begin() + e.offset, all.begin() + e.offset + e.size);
    }
    return {};
}

struct Unit { float pos[3]; float yaw; bool here = true; };
static std::map<uint64_t, Unit> units;
static bool where(uint64_t g, int slot, float out[3], float* yaw) {
    auto it = units.find(g);
    if (it == units.end() || !it->second.here) return false;
    const Unit& u = it->second;
    memcpy(out, u.pos, 3 * sizeof(float));
    if (slot == W3D_SLOT_CHEST) out[2] += 1.3f;
    if (slot == W3D_SLOT_HEAD) out[2] += 1.9f;
    if (slot == W3D_SLOT_LHAND) { out[1] += 0.4f; out[2] += 1.1f; }
    if (slot == W3D_SLOT_RHAND) { out[1] -= 0.4f; out[2] += 1.1f; }
    if (yaw) *yaw = u.yaw;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::vector<uint8_t> blob = readEntry(argv[1], W3D_ENTRY_SPELLFX, 0);
    CHECK(!blob.empty(), "SVFX entry in the pack");
    sfx::Table t;
    CHECK(t.load(blob.data(), (uint32_t)blob.size()), "table loads");
    std::ifstream ej(argv[2]);
    std::stringstream ss;
    ss << ej.rdbuf();
    const std::string ex = ss.str();
    auto num = [&](const char* k) { std::smatch m; std::regex r(std::string("\"") + k + "\": ([0-9]+)"); return std::regex_search(ex, m, r) ? std::stoi(m[1]) : -1; };
    CHECK((int)t.spellCount() == num("spells"), "spells in the table: %u (expected %d)", t.spellCount(), num("spells"));
    float sp = 0;
    const W3dSpellVisual* fb = t.visualOf(133, &sp);
    CHECK(fb && fabsf(sp - 20.0f) < 0.01f, "Fireball: visual, speed 20 (%.1f)", sp);
    const W3dSpellVisual* fr = t.visualOf(116, &sp);
    CHECK(fr && fabsf(sp - 28.0f) < 0.01f, "Frostbolt: speed 28 (%.1f)", sp);
    CHECK(!t.visualOf(585, nullptr), "Smite: its model is missing, no visual");
    CHECK(!t.visualOf(78, nullptr), "Heroic Strike: nothing");
    if (fb) {
        CHECK(fb->effect[W3D_SFX_PRECAST][0] != 0xFFFF && fb->effect[W3D_SFX_PRECAST][1] != 0xFFFF &&
              fb->slot[W3D_SFX_PRECAST][0] == W3D_SLOT_LHAND && fb->slot[W3D_SFX_PRECAST][1] == W3D_SLOT_RHAND, "Fireball precast: both hands");
        CHECK(fb->slot[W3D_SFX_CAST][0] == W3D_SLOT_BASE && fb->slot[W3D_SFX_IMPACT][0] == W3D_SLOT_CHEST, "cast at the feet, impact on the chest");
        CHECK(fb->effect[W3D_SFX_MISSILE][0] != 0xFFFF, "Fireball missile");
        const W3dSpellEffect* me = t.effect(fb->effect[W3D_SFX_MISSILE][0]);
        CHECK(me && fabsf(me->scale - 1.5f) < 0.01f, "missile scale from SpellVisualEffectName");
    }

    const float cam[3] = {0, 0, 0};
    sfx::Director d;
    d.setTable(&t);
    std::vector<sfx::Placed> out;
    units[1] = {{0, 0, 0}, 0.0f};
    units[2] = {{20, 0, 0}, 3.14f};
    uint32_t now = 1000;
    // ---- Fireball: 3 s of fire in the hands
    sfx::Event e;
    e.kind = sfx::Event::Start; e.caster = 1; e.spell = 133; e.castMs = 3000;
    d.event(e, now, cam, where);
    d.update(now, where, out);
    CHECK(out.size() == 2, "precast: two effects (%zu)", out.size());
    bool hands = out.size() == 2 && fabsf(fabsf(out[0].pos[1]) - 0.4f) < 0.01f && out[0].pos[1] * out[1].pos[1] < 0;
    CHECK(hands, "on both hands");
    now += 2900;
    d.update(now, where, out);
    CHECK(out.size() == 2 && out[0].emitting, "still casting at 2.9 s");
    // ---- it goes off at the target 20 yd away
    e = sfx::Event(); e.kind = sfx::Event::Go; e.caster = 1; e.spell = 133; e.targets[0] = 2; e.nTargets = 1;
    d.event(e, now, cam, where);
    d.update(now, where, out);
    int pre = 0, cast = 0, mis = 0, imp = 0, preEmitting = 0;
    for (const sfx::Placed& p : out) {
        pre += p.stage == W3D_SFX_PRECAST; preEmitting += p.stage == W3D_SFX_PRECAST && p.emitting;
        cast += p.stage == W3D_SFX_CAST; mis += p.stage == W3D_SFX_MISSILE; imp += p.stage == W3D_SFX_IMPACT;
    }
    CHECK(pre == 2 && preEmitting == 0, "the precast stops (and lingers)");
    CHECK(cast == 1 && mis == 1 && imp == 0, "cast at the feet and a missile, no impact yet (%d %d %d)", cast, mis, imp);
    // fly: 20 yd at 20 yd/s, frames of 33 ms
    uint32_t arrived = 0;
    float lastX = -1;
    bool forward = true, aimed = true;
    for (int f = 0; f < 60 && !arrived; ++f) {
        now += 33;
        d.update(now, where, out);
        for (const sfx::Placed& p : out) {
            if (p.stage == W3D_SFX_MISSILE && p.emitting) {
                forward &= p.pos[0] >= lastX;
                lastX = p.pos[0];
                aimed &= fabsf(p.rot[2]) < 0.05f;   // flying along +X
            }
            if (p.stage == W3D_SFX_IMPACT && !arrived) arrived = now;
        }
    }
    const float flight = (arrived - 3900) / 1000.0f;   // it went off at 1000 + 2900
    CHECK(arrived && flight > 0.8f && flight < 1.2f, "impact after ~1 s of flight (%.2f s)", flight);
    CHECK(forward && aimed, "the missile flies toward the target, nose first");
    bool onChest = false;
    for (const sfx::Placed& p : out) if (p.stage == W3D_SFX_IMPACT) onChest = fabsf(p.pos[0] - 20) < 0.01f && fabsf(p.pos[2] - 1.3f) < 0.01f;
    CHECK(onChest, "impact on the target's chest");
    // the target moves: the impact follows it
    units[2].pos[1] = 3;
    now += 33;
    d.update(now, where, out);
    bool follows = false;
    for (const sfx::Placed& p : out) if (p.stage == W3D_SFX_IMPACT) follows = fabsf(p.pos[1] - 3) < 0.01f;
    CHECK(follows, "effects follow their unit");
    // everything over and faded: 1.5 s impact + 1.5 s linger
    now += 3200;
    d.update(now, where, out);
    CHECK(out.empty() && d.active() == 0, "all gone after ending and lingering (%zu left)", out.size());

    // ---- Frostbolt: missile only, 28 yd/s
    units[2].pos[1] = 0;
    e = sfx::Event(); e.kind = sfx::Event::Go; e.caster = 1; e.spell = 116; e.targets[0] = 2; e.nTargets = 1;
    const uint32_t t0 = now;
    d.event(e, now, cam, where);
    uint32_t done = 0;
    for (int f = 0; f < 60 && !done; ++f) {
        now += 33;
        d.update(now, where, out);
        bool flying = false;
        for (const sfx::Placed& p : out) flying |= p.stage == W3D_SFX_MISSILE && p.emitting;
        if (!flying) done = now;
    }
    const float fl2 = (done - t0) / 1000.0f;
    CHECK(done && fl2 > 0.6f && fl2 < 0.85f, "Frostbolt arrives in ~0.71 s (%.2f)", fl2);
    now += 4000;
    d.update(now, where, out);

    // ---- Heal on oneself: precast on the chest, impact at the feet at once (no missile)
    e = sfx::Event(); e.kind = sfx::Event::Start; e.caster = 1; e.spell = 2050; e.castMs = 1500;
    d.event(e, now, cam, where);
    d.update(now, where, out);
    CHECK(out.size() == 1 && out[0].stage == W3D_SFX_PRECAST && fabsf(out[0].pos[2] - 1.3f) < 0.01f, "heal precast on the chest");
    e = sfx::Event(); e.kind = sfx::Event::Go; e.caster = 1; e.spell = 2050; e.targets[0] = 1; e.nTargets = 1;
    d.event(e, now + 1500, cam, where);
    d.update(now + 1500, where, out);
    int healImp = 0;
    for (const sfx::Placed& p : out) healImp += p.stage == W3D_SFX_IMPACT && p.emitting && fabsf(p.pos[2]) < 0.01f;
    CHECK(healImp == 1, "self heal: impact at the feet right away");
    now += 6000;
    d.update(now, where, out);

    // ---- failing: the precast stops
    e = sfx::Event(); e.kind = sfx::Event::Start; e.caster = 1; e.spell = 133; e.castMs = 3000;
    d.event(e, now, cam, where);
    e.kind = sfx::Event::Fail;
    d.event(e, now + 500, cam, where);
    d.update(now + 500, where, out);
    bool stopped = !out.empty();
    for (const sfx::Placed& p : out) stopped &= !p.emitting;
    CHECK(stopped, "a failed cast stops its precast");
    now += 4000;
    d.update(now, where, out);
    CHECK(out.empty(), "and fades");

    // ---- too far, the limit, a unit going away
    units[3] = {{150, 0, 0}, 0};
    e = sfx::Event(); e.kind = sfx::Event::Start; e.caster = 3; e.spell = 133; e.castMs = 2000;
    d.event(e, now, cam, where);
    d.update(now, where, out);
    CHECK(out.empty() && d.stats().skippedFar == 1, "a caster 150 yd away shows nothing");
    for (int u = 0; u < 40; ++u) {
        units[100 + u] = {{(float)(u % 8), (float)(u / 8), 0}, 0};
        e = sfx::Event(); e.kind = sfx::Event::Start; e.caster = 100 + u; e.spell = 133; e.castMs = 5000;
        d.event(e, now, cam, where);
    }
    d.update(now, where, out);
    CHECK((int)out.size() <= sfx::Director::kMaxActive && d.stats().skippedFull > 0, "never more than %d (%zu, %u skipped)",
          sfx::Director::kMaxActive, out.size(), d.stats().skippedFull);
    for (int u = 0; u < 40; ++u) units[100 + u].here = false;
    now += 33;
    d.update(now, where, out);
    bool allEnded = true;
    for (const sfx::Placed& p : out) allEnded &= !p.emitting;
    CHECK(allEnded, "units gone: their effects end");
    now += 2000;
    d.update(now, where, out);
    CHECK(out.empty(), "and are dropped after lingering");

    printf("SPELL FX: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
