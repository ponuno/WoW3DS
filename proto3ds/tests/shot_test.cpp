// 6.95: arrows and bullets - Director::shoot over an in-memory SVFX table (36-byte header with arrow / bullet).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <vector>

#include "spellfx.hpp"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

struct Unit { float pos[3]; bool here = true; };
static std::map<uint64_t, Unit> units;
static bool where(uint64_t g, int slot, float out[3], float* yaw) {
    auto it = units.find(g);
    if (it == units.end() || !it->second.here) return false;
    memcpy(out, it->second.pos, sizeof(float) * 3);
    if (slot == W3D_SLOT_CHEST) out[2] += 1.3f;
    if (slot == W3D_SLOT_RHAND) out[2] += 1.1f;
    if (yaw) *yaw = 0;
    return true;
}

static void run(sfx::Director& d, uint32_t from, uint32_t to, std::vector<sfx::Placed>& out) {   // 16 ms frames
    for (uint32_t t = from; t <= to; t += 16) d.update(t, where, out);
}

static std::vector<uint8_t> blob(bool newHeader) {
    const size_t hs = newHeader ? sizeof(W3dSpellFxHeader) : 32;
    std::vector<uint8_t> b(hs + 2 * sizeof(W3dSpellEffect), 0);
    W3dSpellFxHeader h{};
    h.magic = W3D_SVFX_MAGIC;
    h.effectCount = 2;
    h.offEffects = (uint32_t)hs;
    h.offVisuals = h.offSpells = (uint32_t)b.size();
    h.levelUp = h.loot = W3D_SFX_NONE;
    h.arrow = 0;
    h.bullet = 1;
    memcpy(b.data(), &h, hs);
    W3dSpellEffect e[2] = {{7, 1.0f}, {8, 0.5f}};
    memcpy(b.data() + hs, e, sizeof e);
    return b;
}

int main() {
    CHECK(sizeof(W3dSpellFxHeader) == 36, "header is 36 bytes");
    std::vector<uint8_t> nb = blob(true), ob = blob(false);
    sfx::Table t, old;
    CHECK(t.load(nb.data(), (uint32_t)nb.size()), "new table loads");
    CHECK(t.arrow() == 0 && t.bullet() == 1, "arrow / bullet indices");
    CHECK(old.load(ob.data(), (uint32_t)ob.size()), "old table loads");
    CHECK(old.arrow() == W3D_SFX_NONE && old.bullet() == W3D_SFX_NONE, "old pack: no ammo");

    sfx::Director d;
    d.setTable(&t);
    units[1] = {{0, 0, 0}};
    units[2] = {{35, 0, 0}};
    const float cam[3] = {0, 0, 0};
    std::vector<sfx::Placed> out;
    CHECK(d.shoot(1, 2, t.arrow(), 1000, cam, where), "shot starts");
    d.update(1000, where, out);
    CHECK(out.size() == 1 && out[0].stage == W3D_SFX_MISSILE && out[0].emitting, "one flying arrow");
    run(d, 1016, 1400, out);
    CHECK(out.size() == 1 && out[0].pos[0] > 10 && out[0].pos[0] < 25, "half way after 0.4 s (x=%.1f)", out.empty() ? -1.f : out[0].pos[0]);
    CHECK(out[0].rot[1] != 0 || out[0].rot[2] == 0, "nose along +x");
    run(d, 1416, 1950, out);
    bool flying = false;
    for (auto& p : out) flying = flying || p.emitting;
    CHECK(!flying, "arrived after 0.8 s at 35 yd");
    run(d, 1966, 4200, out);
    CHECK(out.empty() && d.active() == 0, "removed after the linger");

    // short range: not instant
    units[2].pos[0] = 3;
    CHECK(d.shoot(1, 2, t.bullet(), 5000, cam, where), "close shot starts");
    d.update(5050, where, out);
    bool still = false;
    for (auto& p : out) still = still || p.emitting;
    CHECK(still, "close shot still in the air after 50 ms");
    run(d, 5066, 5300, out);
    still = false;
    for (auto& p : out) still = still || p.emitting;
    CHECK(!still, "close shot done after 0.3 s");
    d.clear();

    // skips
    units[2].pos[0] = 35;
    CHECK(!d.shoot(1, 1, t.arrow(), 0, cam, where), "no shot at oneself");
    CHECK(!d.shoot(1, 9, t.arrow(), 0, cam, where), "no shot at an unknown target");
    CHECK(!d.shoot(1, 2, W3D_SFX_NONE, 0, cam, where), "no effect, no shot");
    const float far[3] = {500, 0, 0};
    CHECK(!d.shoot(1, 2, t.arrow(), 0, far, where), "caster out of range");
    units[2].here = false;
    CHECK(d.shoot(1, 2, t.arrow(), 0, cam, where) == false, "target gone at launch");
    units[2].here = true;
    CHECK(d.shoot(1, 2, t.arrow(), 100, cam, where), "again");
    units[2].here = false;
    d.update(200, where, out);
    flying = false;
    for (auto& p : out) flying = flying || p.emitting;
    CHECK(!flying, "target vanished: arrow fades");
    d.clear();
    int ok = 0;
    for (int i = 0; i < 60; ++i) ok += d.shoot(1, 2, t.arrow(), 10, cam, where);
    units[2].here = true;
    CHECK(ok <= sfx::Director::kMaxActive, "budget kept (%d)", ok);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
