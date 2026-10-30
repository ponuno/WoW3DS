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
    // 6.36: opening / gathering at a game object kneels (the spell-casting hands were shown before)
    c.casting = 5;
    CHECK(k.id(c) == W3D_ANIM_KNEEL_START, "gathering: kneels down");
    CHECK(k.id(c, 1100) == W3D_ANIM_KNEEL_LOOP, "gathering: the kneeling pose while the bar runs");
    c.casting = 0; c.castGoMs = 9950; c.castGoObject = true; c.castGoDirected = false;
    CHECK(k.id(c) == W3D_ANIM_KNEEL_END, "the gathering is done: gets up, no spell release");
    c.castGoObject = false;
    k.id(c, 1100);
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
    CHECK(k.id(c) == W3D_ANIM_KNEEL_LOOP, "looting: the kneeling pose, not the repeated one-shot bend");
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
    v.casting = 5;
    CHECK(w.id(v) == W3D_ANIM_STAND, "gathering without kneeling animations: stands");
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

    // 6.36: w3d_seq_animated / the diagnostics for models that cannot move (the hand-made models have no keys: all still)
    CHECK(!w3d_seq_animated(wolf.data(), 0), "a still stand is not animated");
    CHECK(!w3d_seq_animated(wolf.data(), -1) && !w3d_seq_animated(wolf.data(), 99), "no such sequence: not animated");
    {
        std::vector<uint8_t> m = makeModel({0, 4, 5});
        W3dCreatureHeader* h = (W3dCreatureHeader*)m.data();
        W3dAnimSeq* sq = (W3dAnimSeq*)(m.data() + h->offSeqs);
        sq[0].frames = 10;   // ten frames, but the one bone's channels are all NONE
        CHECK(!w3d_seq_animated(m.data(), 0), "frames without keyed channels: still");
        uint32_t* ch = (uint32_t*)(m.data() + sq[0].offChannels);
        ch[0] = W3D_CHAN_ANIM | 0;   // rotation keyed over time
        CHECK(w3d_seq_animated(m.data(), 0), "a keyed channel over several frames: animated");
    }

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
    // 6.36: a cast at a game object (Opening a chest, a quest object): kneeling, and no spell release at the end
    {
        const uint64_t go = 0xF110000000000123ull;
        auto fb = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        Writer u;
        u.u32(1); u.u8(0); u.u8(3); u.packedGuid(go); u.u8(5);
        u.u8(0x40);
        u.f32(10); u.f32(20); u.f32(30); u.f32(0);
        u.u8(1); u.u32(1u << UF::GAMEOBJECT_TYPE_ID); u.u32(3);   // a chest
        (void)fb;
        CHECK(s.objects.parseUpdate(u.b.data(), u.b.size(), now + 5000), "game object created");
        CHECK(s.objects.find(go) && s.objects.find(go)->type == ObjType::GameObject, "it is a game object");
        Writer w;
        packed(w, mg); packed(w, mg);
        w.u32(6247); w.u16(0); w.u32(2000);
        w.u16(0x800); packed(w, go);
        s.handle(0x131, w.b, now + 5000);
        fillUnitState(s, *s.objects.find(mg), now + 5500, c);
        CHECK(c.casting == 5, "opening a chest: casting = at an object (%u)", c.casting);
        Writer g;
        packed(g, mg); packed(g, mg);
        g.u32(6247); g.u16(0); g.u8(0); g.u8(0); g.u16(0x800); packed(g, go);
        s.handle(0x132, g.b, now + 7000);
        fillUnitState(s, *s.objects.find(mg), now + 7000, c);
        CHECK(c.castGoMs == now + 7000 && c.castGoObject && !c.castGoDirected && c.casting == 0, "the release is at an object (%d %d %u)",
              (int)c.castGoObject, (int)c.castGoDirected, c.casting);
    }
    std::printf("  session: emote, cast at us / around, release, parry, off hand, cast at a game object\n");
}

// Game objects (stage 5.41): doors and chests play Open / Close when their state changes, rest in the pose of the state; corpses and
// tilted objects get their data from the update fields.
static void gameObjects() {
    using namespace w3n;
    // a door with all four door animations (1000 ms each) + stand
    std::vector<uint8_t> door = makeModel({W3D_ANIM_STAND, W3D_ANIM_CLOSE, W3D_ANIM_CLOSED, W3D_ANIM_OPEN, W3D_ANIM_OPENED});
    {
        Clock k{UnitAnimator(), door.data()};
        CreatureSeen c;
        c.isGo = true;
        c.goState = 1;      // created closed
        CHECK(k.id(c) == W3D_ANIM_CLOSED, "closed door rests in Closed");
        CHECK(k.id(c, 500) == W3D_ANIM_CLOSED, "still closed");
        c.goState = 0;
        c.goStateMs = k.now + 10;   // it just opened
        k.now += 20;
        AnimChoice a = k.a.update(door.data(), c, false, k.now);
        CHECK(idOf(door.data(), a.seq) == W3D_ANIM_OPEN && !a.loop, "opening plays Open once");
        CHECK(k.a.timeMs() < 50, "Open starts at 0, is %.0f", k.a.timeMs());
        a = k.a.update(door.data(), c, false, k.now + 400);
        CHECK(idOf(door.data(), a.seq) == W3D_ANIM_OPEN && std::fabs(k.a.timeMs() - 410) < 5, "Open runs on (%.0f)", k.a.timeMs());
        a = k.a.update(door.data(), c, false, k.now + 1200);
        CHECK(idOf(door.data(), a.seq) == W3D_ANIM_OPENED && a.loop, "then rests in Opened");
        // closes again
        c.goState = 1;
        c.goStateMs = k.now + 2000;
        a = k.a.update(door.data(), c, false, k.now + 2100);
        CHECK(idOf(door.data(), a.seq) == W3D_ANIM_CLOSE && !a.loop, "closing plays Close");
        a = k.a.update(door.data(), c, false, k.now + 3300);
        CHECK(idOf(door.data(), a.seq) == W3D_ANIM_CLOSED, "and rests closed");
        // first seen open: no animation, straight to Opened
        UnitAnimator fresh;
        CreatureSeen o2;
        o2.isGo = true; o2.goState = 0; o2.goStateMs = 0;
        a = fresh.update(door.data(), o2, false, 5000);
        CHECK(idOf(door.data(), a.seq) == W3D_ANIM_OPENED, "a door first seen open is open");
    }
    {   // a model with only Open / Close (no Opened / Closed): rests at the end of the transition, closed + never changed = Stand
        std::vector<uint8_t> m = makeModel({W3D_ANIM_STAND, W3D_ANIM_CLOSE, W3D_ANIM_OPEN});
        UnitAnimator an;
        CreatureSeen c;
        c.isGo = true; c.goState = 1;
        AnimChoice a = an.update(m.data(), c, false, 1000);
        CHECK(idOf(m.data(), a.seq) == W3D_ANIM_STAND, "closed, never changed: stand");
        c.goState = 0; c.goStateMs = 0;
        a = an.update(m.data(), c, false, 1100);
        CHECK(idOf(m.data(), a.seq) == W3D_ANIM_OPEN && !a.loop && an.timeMs() > 1e8f, "open without Opened: end of Open");
    }
    {   // a static model (stand only) keeps standing in both states
        std::vector<uint8_t> m = makeModel({W3D_ANIM_STAND});
        UnitAnimator an;
        CreatureSeen c;
        c.isGo = true; c.goState = 0;
        CHECK(idOf(m.data(), an.update(m.data(), c, false, 1000).seq) == W3D_ANIM_STAND, "static object stands");
    }
    {   // fields -> CreatureSeen
        WorldObject o;
        o.type = ObjType::GameObject;
        o.fields.assign(32, 0);
        o.fields[UF::GAMEOBJECT_STATE] = 0;
        o.stateChangeMs = 777;
        const float qx = std::sin(0.3f), qw = std::cos(0.3f);
        std::memcpy(&o.fields[UF::GAMEOBJECT_ROTATION], &qx, 4);
        std::memcpy(&o.fields[UF::GAMEOBJECT_ROTATION + 3], &qw, 4);
        CreatureSeen c;
        fillGameObjectState(o, c);
        CHECK(c.isGo && c.goState == 0 && c.goStateMs == 777 && c.tilted, "go state and tilt");
        CHECK(std::fabs(c.rot[8] - std::cos(0.6f)) < 1e-4f && std::fabs(c.rot[7] - std::sin(0.6f)) < 1e-4f, "tilt matrix");
    }
    {   // a corpse: race 2 (orc) female, skin 1, face 3, hair 4/5, facial 2; chest 1234 (invtype 5), helm 55 (invtype 1); hide helm
        WorldObject o;
        o.type = ObjType::Corpse;
        o.hasPos = true;
        o.fields.assign(64, 0);
        o.fields[UF::CORPSE_FIELD_BYTES_1] = (2u << 8) | (1u << 16) | (1u << 24);
        o.fields[UF::CORPSE_FIELD_BYTES_2] = 3u | (4u << 8) | (5u << 16) | (2u << 24);
        o.fields[UF::CORPSE_FIELD_DISPLAY_ID] = 1478;
        o.fields[(size_t)UF::CORPSE_FIELD_ITEM + (size_t)W3D_EQ_CHEST] = 1234u | (5u << 24);
        o.fields[(size_t)UF::CORPSE_FIELD_ITEM + (size_t)W3D_EQ_HEAD] = 55u | (1u << 24);
        o.fields[UF::CORPSE_FIELD_FLAGS] = 0x04 | 0x08;
        CreatureSeen c;
        CHECK(fillCorpse(o, c), "corpse accepted");
        CHECK(c.dead && c.look != 0 && c.lookParts.race == 2 && c.lookParts.sex == 1 && c.lookParts.skin == 1 && c.lookParts.face == 3 &&
              c.lookParts.hairStyle == 4 && c.lookParts.hairColor == 5 && c.lookParts.facialHair == 2, "corpse look");
        CHECK(c.displayId == 1478 && c.equip[W3D_EQ_CHEST] == 1234 && c.equipInv[W3D_EQ_CHEST] == 5, "corpse gear");
        CHECK(c.equip[W3D_EQ_HEAD] == 0, "helm hidden by the flag");
        o.fields[UF::CORPSE_FIELD_FLAGS] = 0x01;   // bones: no model
        CHECK(!fillCorpse(o, c), "bones are not drawn");
        o.fields[UF::CORPSE_FIELD_FLAGS] = 0;
        o.fields[UF::CORPSE_FIELD_BYTES_1] = 0;     // race missing
        CHECK(!fillCorpse(o, c), "unknown race refused");
    }
}

int main(int argc, char** argv) {
    gameObjects();
    stateMachine();
    sessionSide(argc > 1 ? argv[1] : "/tmp/w3d_stage5/fight.w3dl");
    std::printf("ANIM: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
