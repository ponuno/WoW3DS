// anim_test — the animation state machine (creature_view.cpp UnitAnimator) on hand-made models, the mount seat,
// and what the session gives fillUnitState (unit_state.cpp) after real packets: SMSG_EMOTE, a cast with a
// target, SPELL_GO at someone, a parried blow, a channel, sitting.
//   anim_test [fight.w3dl]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "creature_view.hpp"
#include "unit_state.hpp"
#include "w3d_anim.h"

// the streamer is not part of this test
CreatureDisplay* streamerAcquireDisplay(uint32_t) { return nullptr; }
CreatureDisplay* streamerAcquirePlayerDisplay(const W3dLook&, const uint32_t*) { return nullptr; }
ItemDisplay* streamerAcquireItem(uint32_t) { return nullptr; }
void streamerReleaseItem(ItemDisplay*) {}
Model* streamerAcquireItemModel(uint32_t) { return nullptr; }
void streamerReleaseItemModel(Model*) {}
void streamerReleaseDisplay(CreatureDisplay*) {}
bool CreatureDisplay::ready() const { return false; }

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// A model with one bone and the given animations (1000 ms each; walk / run authored at 2.5 / 7 yd/s).
static std::vector<uint8_t> makeModel(const std::vector<uint16_t>& ids, bool seat = false) {
    std::vector<uint8_t> b(sizeof(W3dCreatureHeader) + sizeof(W3dBone) + ids.size() * sizeof(W3dAnimSeq) + 3 * 4, 0);
    W3dCreatureHeader* h = (W3dCreatureHeader*)b.data();
    h->boneCount = 1;
    h->seqCount = (uint32_t)ids.size();
    h->offBones = sizeof(W3dCreatureHeader);
    h->offSeqs = h->offBones + sizeof(W3dBone);
    const uint32_t offChan = h->offSeqs + (uint32_t)(ids.size() * sizeof(W3dAnimSeq));
    h->transScale = 1;
    h->seatBone = seat ? 0 : -1;
    h->seat[0] = 0.2f; h->seat[1] = 0; h->seat[2] = 1.5f;
    W3dBone* bone = (W3dBone*)(b.data() + h->offBones);
    bone->parent = -1;
    W3dAnimSeq* s = (W3dAnimSeq*)(b.data() + h->offSeqs);
    for (size_t i = 0; i < ids.size(); ++i) {
        s[i].animId = ids[i];
        s[i].frames = 1;
        s[i].durationMs = 1000;
        s[i].moveSpeed = ids[i] == W3D_ANIM_WALK ? 2.5f : ids[i] == W3D_ANIM_RUN ? 7.0f : 0.0f;
        s[i].offChannels = offChan;
    }
    uint32_t none[3] = {W3D_CHAN_NONE, W3D_CHAN_NONE, W3D_CHAN_NONE};
    memcpy(b.data() + offChan, none, sizeof none);
    return b;
}

static uint16_t idOf(const uint8_t* blob, int seq) {
    if (seq < 0) return 0xFFFF;
    return ((const W3dAnimSeq*)(blob + w3d_creature(blob)->offSeqs))[seq].animId;
}

struct Clock {
    UnitAnimator a;
    const uint8_t* blob;
    uint32_t now = 1000;
    AnimChoice step(const CreatureSeen& c, uint32_t ms = 50, bool riding = false) {
        now += ms;
        return a.update(blob, c, riding, now);
    }
    uint16_t id(const CreatureSeen& c, uint32_t ms = 50, bool riding = false) { return idOf(blob, step(c, ms, riding).seq); }
};

static void stateMachine() {
    // a humanoid with (nearly) everything
    std::vector<uint16_t> all;
    for (int i = 0; i <= 138; ++i) all.push_back((uint16_t)i);
    std::vector<uint8_t> hum = makeModel(all);
    Clock k{UnitAnimator(), hum.data()};
    CreatureSeen c;
    CHECK(k.id(c) == W3D_ANIM_STAND, "stand");
    c.speed = 7; c.moveFlags = 0x1;
    CHECK(k.id(c) == W3D_ANIM_RUN, "run");
    c.speed = 2.5f;
    CHECK(k.id(c) == W3D_ANIM_WALK, "walk");
    c.speed = 4.5f; c.moveFlags = 0x2;
    CHECK(k.id(c) == W3D_ANIM_WALK_BACKWARDS, "backwards");
    c.moveFlags = 0x4;
    CHECK(k.id(c) == W3D_ANIM_SHUFFLE_LEFT, "strafe left");
    c.moveFlags = 0x1 | 0x8;
    CHECK(k.id(c) == W3D_ANIM_RUN, "forward + strafe at 4.5 yd/s: runs");
    c.swimming = true; c.moveFlags = 0x2;
    CHECK(k.id(c) == W3D_ANIM_SWIM_BACKWARDS, "swim backwards");
    c.speed = 0; c.moveFlags = 0;
    CHECK(k.id(c) == W3D_ANIM_SWIM_IDLE, "swim idle");
    c.swimming = false;
    // combat: ready stance by weapon, swings, off hand, reactions
    c.inCombat = true; c.weapon = CreatureWeapon::OneHand;
    CHECK(k.id(c) == W3D_ANIM_READY_1H, "ready 1H");
    c.weapon = CreatureWeapon::TwoHand;
    CHECK(k.id(c) == W3D_ANIM_READY_2H, "ready 2H");
    c.swingMs = 5000;
    CHECK(k.id(c) == W3D_ANIM_ATTACK_2H, "2H swing");
    CHECK(!k.step(c).loop, "a swing plays once");
    c.hitMs = 5100; c.hitReact = 1;   // struck while swinging: the swing goes on
    CHECK(k.id(c) == W3D_ANIM_ATTACK_2H, "a wound does not cut a swing");
    CHECK(k.id(c, 1000) == W3D_ANIM_READY_2H, "back to ready after the swing");
    c.hitMs = 6000; c.hitReact = 4;
    CHECK(k.id(c) == W3D_ANIM_PARRY_2H, "parry 2H");
    c.weapon = CreatureWeapon::OneHand;
    CHECK(k.id(c, 1100) == W3D_ANIM_READY_1H, "parry over");
    c.hitMs = 7000; c.hitReact = 3;
    CHECK(k.id(c) == W3D_ANIM_DODGE, "dodge");
    c.hitMs = 7100; c.hitReact = 2; c.swingMs = 7100; c.swingOff = true;
    CHECK(k.id(c) == W3D_ANIM_ATTACK_OFF, "off-hand swing beats a crit wound");
    k.id(c, 1100);
    c.hitMs = 8000; c.hitReact = 5;
    CHECK(k.id(c) == W3D_ANIM_SHIELD_BLOCK, "block");
    c.inCombat = false;
    k.id(c, 1100);
    // casting
    c.casting = 1;
    CHECK(k.id(c) == W3D_ANIM_READY_SPELL_DIRECTED, "casting at someone");
    c.casting = 2;
    CHECK(k.id(c) == W3D_ANIM_READY_SPELL_OMNI, "casting around itself");
    c.casting = 0; c.castGoMs = 9000; c.castGoDirected = true;
    CHECK(k.id(c) == W3D_ANIM_SPELL_CAST_DIRECTED, "release at someone");
    k.id(c, 1100);
    c.castGoMs = 9500; c.castGoDirected = false;
    CHECK(k.id(c) == W3D_ANIM_SPELL_CAST_OMNI, "release around");
    k.id(c, 1100);
    c.castGoMs = 9900; c.castGoLook = CastLook::Bow;
    CHECK(k.id(c) == W3D_ANIM_FIRE_BOW, "auto shot with a bow");
    k.id(c, 1100);
    c.casting = 3;
    CHECK(k.id(c) == W3D_ANIM_CHANNEL_CAST_DIRECTED, "channel");
    c.casting = 0;
    // emotes: one-shot, cancelled by moving; looping emote state
    c.emoteAnim = 67; c.emoteMs = 10000;
    CHECK(k.id(c) == 67, "wave");
    c.speed = 7; c.moveFlags = 1;
    CHECK(k.id(c) == W3D_ANIM_RUN, "running cuts the wave");
    c.speed = 0; c.moveFlags = 0;
    c.loopAnim = 69;
    CHECK(k.id(c) == 69 && k.step(c).loop, "dancing (emote state)");
    c.loopAnim = 0;
    // sitting down / up, sleeping, kneeling, chairs
    c.standState = 1;
    CHECK(k.id(c) == W3D_ANIM_SIT_GROUND_DOWN, "sitting down");
    CHECK(k.id(c, 1100) == W3D_ANIM_SIT_GROUND, "sitting");
    c.standState = 0;
    CHECK(k.id(c) == W3D_ANIM_SIT_GROUND_UP, "getting up");
    CHECK(k.id(c, 1100) == W3D_ANIM_STAND, "standing");
    c.standState = 3;
    k.id(c);
    CHECK(k.id(c, 1100) == W3D_ANIM_SLEEP, "sleeping");
    c.standState = 5;
    CHECK(k.id(c) == W3D_ANIM_SIT_CHAIR_MED, "chair");
    c.standState = 8;
    CHECK(k.id(c) == W3D_ANIM_KNEEL_START && k.id(c, 1100) == W3D_ANIM_KNEEL_LOOP, "kneeling");
    c.standState = 0;
    k.id(c, 1100);
    k.id(c, 1100);
    c.looting = true;
    CHECK(k.id(c) == W3D_ANIM_LOOT, "looting");
    c.looting = false;
    // jumping
    c.moveFlags = 0x2000;
    CHECK(k.id(c) == W3D_ANIM_JUMP_START, "jump start");
    CHECK(k.id(c, 1100) == W3D_ANIM_JUMP, "in the air");
    c.moveFlags = 0x4000;
    CHECK(k.id(c) == W3D_ANIM_FALL, "falling far");
    c.moveFlags = 0;
    CHECK(k.id(c) == W3D_ANIM_JUMP_END, "landing");
    CHECK(k.id(c, 1100) == W3D_ANIM_STAND, "landed");
    // stun, riding, death
    c.stunned = true;
    CHECK(k.id(c) == W3D_ANIM_STUN, "stunned");
    c.stunned = false;
    c.speed = 14; c.moveFlags = 1;
    CHECK(k.id(c, 50, true) == W3D_ANIM_MOUNT, "riding: the rider sits");
    c.speed = 0; c.moveFlags = 0;
    c.dead = true;
    AnimChoice d = k.step(c);
    CHECK(idOf(hum.data(), d.seq) == W3D_ANIM_DEATH && !d.loop && k.a.timeMs() < 100, "dying plays from the start");
    // a unit first seen dead lies at the end
    UnitAnimator fresh;
    fresh.update(hum.data(), c, false, 1);
    CHECK(fresh.timeMs() > 1e8f, "first seen dead: already lying");
    // events from before the first look do not play
    CreatureSeen old;
    old.swingMs = 50;
    old.emoteMs = 40;
    old.emoteAnim = 67;
    UnitAnimator late;
    CHECK(idOf(hum.data(), late.update(hum.data(), old, false, 100).seq) == W3D_ANIM_STAND, "old events are not replayed");

    // a wolf: stand, walk, run, unarmed attack, death only -> sensible fallbacks
    std::vector<uint8_t> wolf = makeModel({0, 1, 4, 5, 16});
    Clock w{UnitAnimator(), wolf.data()};
    CreatureSeen v;
    v.inCombat = true;
    v.weapon = CreatureWeapon::TwoHand;
    CHECK(w.id(v) == W3D_ANIM_STAND, "no ready stance: stands");
    v.swingMs = 100;
    CHECK(w.id(v) == W3D_ANIM_ATTACK_UNARMED, "any attack it has");
    w.id(v, 1100);
    v.hitMs = 200; v.hitReact = 4;
    CHECK(w.id(v) == W3D_ANIM_STAND, "no parry: stands");
    v.casting = 1;
    CHECK(w.id(v) == W3D_ANIM_STAND, "no spell animations: stands");
    v.casting = 0;
    v.speed = 3; v.moveFlags = 0x2; v.inCombat = false;
    CHECK(w.id(v) == W3D_ANIM_WALK, "no backwards: walks");
    v.swimming = true; v.moveFlags = 1;
    CHECK(w.id(v) == W3D_ANIM_WALK, "no swim: walks");
    AnimChoice r = w.step(v);
    CHECK(std::fabs(r.rate - 3.0f / 2.5f) < 0.01f, "walk rate follows the speed (%.2f)", r.rate);

    // the old entry point still works
    float rate;
    bool loop;
    CHECK(idOf(wolf.data(), creaturePickSeq(wolf.data(), true, 0, false, false, &rate, &loop)) == W3D_ANIM_DEATH && !loop, "pick: dead");
    CHECK(idOf(wolf.data(), creaturePickSeq(wolf.data(), false, 7, false, false, &rate, &loop)) == W3D_ANIM_RUN, "pick: run");

    // the mount seat: bone at rest, scaled 2x, turned 90 degrees
    std::vector<uint8_t> horse = makeModel({0, 4, 5}, true);
    float pos[3] = {100, 200, 10}, seat[3];
    CHECK(CreatureView::seatPosition(horse.data(), 0, 0, true, pos, 1.5707963f, 2.0f, seat), "seat");
    CHECK(std::fabs(seat[0] - 100) < 1e-3f && std::fabs(seat[1] - 200.4f) < 1e-3f && std::fabs(seat[2] - 13) < 1e-3f,
          "seat at %.2f %.2f %.2f", seat[0], seat[1], seat[2]);
    CHECK(!CreatureView::seatPosition(wolf.data(), 0, 0, true, pos, 0, 1, seat), "no seat on a wolf");
}

// ------------------------------------------------------------------ the session's side
static void sessionSide(const char* log) {
    using namespace w3n;
    std::vector<LoggedPacket> pk;
    if (!readPacketLog(log, pk)) { std::printf("  (no %s: session checks skipped)\n", log); return; }
    GameSession s;
    for (const LoggedPacket& p : pk) {
        if (p.dir == 1) continue;
        s.handle(p.opcode, p.data, p.ms);
        s.objects.takeCreated();
        s.update(p.ms);
    }
    const WorldObject* wolf = nullptr;
    const WorldObject* me = s.objects.find(s.selfGuid());
    for (auto& [g, o] : s.objects.all()) if (o.entry() == 69) wolf = &o;
    CHECK(wolf && me, "fight log: the wolf and us");
    if (!wolf || !me) return;
    const uint64_t wg = wolf->guid, mg = me->guid;
    uint32_t now = pk.back().ms + 100;
    CreatureSeen c;
    fillUnitState(s, *me, now, c);
    CHECK(c.swingMs != 0, "we swung at the wolf");
    fillUnitState(s, *wolf, now, c);
    CHECK(c.hitMs != 0 && c.hitReact >= 1, "the wolf was struck (%u)", c.hitReact);
    auto packed = [](Writer& w, uint64_t g) { w.packedGuid(g); };
    // SMSG_EMOTE: the wolf "talks" (1 = talk -> animation 60)
    {
        Writer w;
        w.u32(1);
        w.u64(wg);
        s.handle(0x103, w.b, now);
        fillUnitState(s, *s.objects.find(wg), now, c);
        CHECK(c.emoteAnim == 60 && c.emoteMs == now, "emote -> talk animation (%u)", c.emoteAnim);
    }
    // SMSG_SPELL_START by the wolf at us with a cast time, then SPELL_GO hitting us
    {
        Writer w;
        packed(w, wg); packed(w, wg);
        w.u32(133); w.u16(0); w.u32(2000);
        w.u16(0x2); packed(w, mg);
        s.handle(0x131, w.b, now);
        fillUnitState(s, *s.objects.find(wg), now + 500, c);
        CHECK(c.casting == 1, "casting at us: directed (%u)", c.casting);
        Writer g;
        packed(g, wg); packed(g, wg);
        g.u32(133); g.u16(0); g.u8(1); g.u64(mg); g.u8(0); g.u16(0x2); packed(g, mg);
        s.handle(0x132, g.b, now + 2000);
        fillUnitState(s, *s.objects.find(wg), now + 2000, c);
        CHECK(c.castGoMs == now + 2000 && c.castGoDirected && c.casting == 0, "the release, at us");
    }
    // a self-cast (no unit target) is omni
    {
        Writer w;
        packed(w, wg); packed(w, wg);
        w.u32(2061); w.u16(0); w.u32(1500); w.u16(0);
        s.handle(0x131, w.b, now + 3000);
        fillUnitState(s, *s.objects.find(wg), now + 3100, c);
        CHECK(c.casting == 2, "self cast: omni (%u)", c.casting);
    }
    // our blow parried by the wolf, the wolf's off-hand swing
    {
        Writer w;
        w.u32(0x4); packed(w, wg); packed(w, mg); w.u32(0); w.u8(0); w.u32(3); w.u32(0); w.u32(0);
        s.handle(0x14A, w.b, now + 4000);
        fillUnitState(s, *s.objects.find(mg), now + 4000, c);
        CHECK(c.hitReact == 4 && c.hitMs == now + 4000, "we parried (%u)", c.hitReact);
        fillUnitState(s, *s.objects.find(wg), now + 4000, c);
        CHECK(c.swingOff, "the wolf's off hand");
    }
    std::printf("  session: emote, cast at us / around, release, parry, off hand\n");
}

int main(int argc, char** argv) {
    stateMachine();
    sessionSide(argc > 1 ? argv[1] : "/tmp/w3d_stage5/fight.w3dl");
    std::printf("ANIM: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
