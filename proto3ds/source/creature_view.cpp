#include "creature_view.hpp"

#include <initializer_list>

#include "w3d_anim.h"

namespace {

int findSeq(const uint8_t* blob, std::initializer_list<uint16_t> ids) {
    for (uint16_t id : ids) {
        int s = w3d_find_seq(blob, id);
        if (s >= 0) return s;
    }
    return -1;
}

const W3dAnimSeq* seqAt(const uint8_t* blob, int s) {
    const W3dCreatureHeader* h = w3d_creature(blob);
    return s >= 0 && (uint32_t)s < h->seqCount ? (const W3dAnimSeq*)(blob + h->offSeqs) + s : nullptr;
}

float moveRate(const uint8_t* blob, int s, float speed) {
    const W3dAnimSeq* sq = seqAt(blob, s);
    if (!sq || sq->moveSpeed <= 0.1f) return 1.0f;
    float r = speed / sq->moveSpeed;
    return r < 0.3f ? 0.3f : r > 3.0f ? 3.0f : r;
}

constexpr uint32_t kBackward = 0x2, kStrafeLeft = 0x4, kStrafeRight = 0x8, kForward = 0x1;
constexpr uint32_t kJumping = 0x2000, kFallingFar = 0x4000;
constexpr uint8_t kPrioReact = 1, kPrioEmote = 2, kPrioCast = 3, kPrioAttack = 4, kPrioTransition = 5;

int attackSeq(const uint8_t* blob, CreatureWeapon w, bool off) {
    if (off) {
        int s = findSeq(blob, {W3D_ANIM_ATTACK_OFF});
        if (s >= 0) return s;
    }
    switch (w) {
        case CreatureWeapon::TwoHand: return findSeq(blob, {W3D_ANIM_ATTACK_2H, W3D_ANIM_ATTACK_2HL, W3D_ANIM_ATTACK_1H, W3D_ANIM_ATTACK_UNARMED});
        case CreatureWeapon::TwoHandLoose: return findSeq(blob, {W3D_ANIM_ATTACK_2HL, W3D_ANIM_ATTACK_2H, W3D_ANIM_ATTACK_1H, W3D_ANIM_ATTACK_UNARMED});
        case CreatureWeapon::Unarmed: return findSeq(blob, {W3D_ANIM_ATTACK_UNARMED, W3D_ANIM_ATTACK_1H, W3D_ANIM_ATTACK_2H, W3D_ANIM_ATTACK_2HL});
        default: return findSeq(blob, {W3D_ANIM_ATTACK_1H, W3D_ANIM_ATTACK_UNARMED, W3D_ANIM_ATTACK_2H, W3D_ANIM_ATTACK_2HL});
    }
}

int readySeq(const uint8_t* blob, CreatureWeapon w) {
    switch (w) {
        case CreatureWeapon::TwoHand: return findSeq(blob, {W3D_ANIM_READY_2H, W3D_ANIM_READY_2HL, W3D_ANIM_READY_1H, W3D_ANIM_READY_UNARMED});
        case CreatureWeapon::TwoHandLoose: return findSeq(blob, {W3D_ANIM_READY_2HL, W3D_ANIM_READY_2H, W3D_ANIM_READY_1H, W3D_ANIM_READY_UNARMED});
        case CreatureWeapon::Bow: return findSeq(blob, {W3D_ANIM_READY_BOW, W3D_ANIM_READY_1H, W3D_ANIM_READY_UNARMED});
        case CreatureWeapon::Rifle: return findSeq(blob, {W3D_ANIM_READY_RIFLE, W3D_ANIM_READY_BOW, W3D_ANIM_READY_1H, W3D_ANIM_READY_UNARMED});
        case CreatureWeapon::Thrown: return findSeq(blob, {W3D_ANIM_READY_THROWN, W3D_ANIM_READY_1H, W3D_ANIM_READY_UNARMED});
        case CreatureWeapon::Unarmed: return findSeq(blob, {W3D_ANIM_READY_UNARMED, W3D_ANIM_READY_1H});
        default: return findSeq(blob, {W3D_ANIM_READY_1H, W3D_ANIM_READY_UNARMED});
    }
}

int reactSeq(const uint8_t* blob, uint8_t react, CreatureWeapon w) {
    switch (react) {
        case 1: return findSeq(blob, {W3D_ANIM_COMBAT_WOUND});
        case 2: return findSeq(blob, {W3D_ANIM_COMBAT_CRITICAL, W3D_ANIM_COMBAT_WOUND});
        case 3: return findSeq(blob, {W3D_ANIM_DODGE});
        case 4:
            switch (w) {
                case CreatureWeapon::TwoHand: return findSeq(blob, {W3D_ANIM_PARRY_2H, W3D_ANIM_PARRY_1H});
                case CreatureWeapon::TwoHandLoose: return findSeq(blob, {W3D_ANIM_PARRY_2HL, W3D_ANIM_PARRY_2H, W3D_ANIM_PARRY_1H});
                case CreatureWeapon::Unarmed: return findSeq(blob, {W3D_ANIM_PARRY_UNARMED, W3D_ANIM_PARRY_1H});
                default: return findSeq(blob, {W3D_ANIM_PARRY_1H, W3D_ANIM_PARRY_UNARMED});
            }
        case 5: return findSeq(blob, {W3D_ANIM_SHIELD_BLOCK, W3D_ANIM_BLOCK});
        default: return -1;
    }
}

int castGoSeq(const uint8_t* blob, CastLook look, bool directed) {
    switch (look) {
        case CastLook::Bow: return findSeq(blob, {W3D_ANIM_FIRE_BOW, W3D_ANIM_ATTACK_BOW, W3D_ANIM_ATTACK_RIFLE});
        case CastLook::Rifle: return findSeq(blob, {W3D_ANIM_ATTACK_RIFLE, W3D_ANIM_FIRE_BOW, W3D_ANIM_ATTACK_BOW});
        case CastLook::Thrown: return findSeq(blob, {W3D_ANIM_ATTACK_THROWN, W3D_ANIM_ATTACK_1H});
        default:
            return directed ? findSeq(blob, {W3D_ANIM_SPELL_CAST_DIRECTED, W3D_ANIM_SPELL_CAST, W3D_ANIM_SPELL_CAST_OMNI})
                            : findSeq(blob, {W3D_ANIM_SPELL_CAST_OMNI, W3D_ANIM_SPELL_CAST, W3D_ANIM_SPELL_CAST_DIRECTED});
    }
}

// Loops of a unit standing still, in order: casting, its stand state, its emote state, looting, combat, swimming.
int idleSeq(const uint8_t* blob, const CreatureSeen& c) {
    int s = -1;
    switch (c.casting) {
        case 1: s = findSeq(blob, {W3D_ANIM_READY_SPELL_DIRECTED, W3D_ANIM_SPELL_PRECAST, W3D_ANIM_READY_SPELL_OMNI}); break;
        case 2: s = findSeq(blob, {W3D_ANIM_READY_SPELL_OMNI, W3D_ANIM_SPELL_PRECAST, W3D_ANIM_READY_SPELL_DIRECTED}); break;
        case 3: s = findSeq(blob, {W3D_ANIM_CHANNEL_CAST_DIRECTED, W3D_ANIM_READY_SPELL_DIRECTED, W3D_ANIM_SPELL_PRECAST}); break;
        case 4: s = findSeq(blob, {W3D_ANIM_CHANNEL_CAST_OMNI, W3D_ANIM_READY_SPELL_OMNI, W3D_ANIM_SPELL_PRECAST}); break;
        default: break;
    }
    if (s >= 0) return s;
    switch (c.standState) {
        case 1: s = findSeq(blob, {W3D_ANIM_SIT_GROUND, 72}); break;
        case 2: case 5: s = findSeq(blob, {W3D_ANIM_SIT_CHAIR_MED, W3D_ANIM_SIT_CHAIR_LOW, W3D_ANIM_SIT_GROUND}); break;
        case 4: s = findSeq(blob, {W3D_ANIM_SIT_CHAIR_LOW, W3D_ANIM_SIT_CHAIR_MED, W3D_ANIM_SIT_GROUND}); break;
        case 6: s = findSeq(blob, {W3D_ANIM_SIT_CHAIR_HIGH, W3D_ANIM_SIT_CHAIR_MED, W3D_ANIM_SIT_GROUND}); break;
        case 3: s = findSeq(blob, {W3D_ANIM_SLEEP, 71}); break;
        case 7: s = findSeq(blob, {W3D_ANIM_DEAD}); break;
        case 8: s = findSeq(blob, {W3D_ANIM_KNEEL_LOOP, 75}); break;
        default: break;
    }
    if (s >= 0) return s;
    if (c.loopAnim) {
        s = w3d_find_seq(blob, c.loopAnim);
        if (s >= 0) return s;
    }
    if (c.looting) {
        s = findSeq(blob, {W3D_ANIM_LOOT, W3D_ANIM_KNEEL_LOOP});
        if (s >= 0) return s;
    }
    if (c.inCombat) {
        s = readySeq(blob, c.weapon);
        if (s >= 0) return s;
    }
    if (c.swimming) {
        s = findSeq(blob, {W3D_ANIM_SWIM_IDLE});
        if (s >= 0) return s;
    }
    return findSeq(blob, {W3D_ANIM_STAND});
}

// Moving: swimming, backwards, strafing, walking, running (playback follows the ground speed).
int moveSeq(const uint8_t* blob, const CreatureSeen& c, float* rate) {
    const bool back = (c.moveFlags & kBackward) && !(c.moveFlags & kForward);
    const bool strafeOnly = (c.moveFlags & (kStrafeLeft | kStrafeRight)) && !(c.moveFlags & (kForward | kBackward));
    const bool left = (c.moveFlags & kStrafeLeft) != 0;
    int s = -1;
    if (c.swimming) {
        if (back) s = findSeq(blob, {W3D_ANIM_SWIM_BACKWARDS, W3D_ANIM_SWIM});
        else if (strafeOnly) s = findSeq(blob, {(uint16_t)(left ? W3D_ANIM_SWIM_LEFT : W3D_ANIM_SWIM_RIGHT), W3D_ANIM_SWIM});
        if (s < 0) s = findSeq(blob, {W3D_ANIM_SWIM});   // else walks / runs below
    } else if (back) {
        s = findSeq(blob, {W3D_ANIM_WALK_BACKWARDS, W3D_ANIM_WALK, W3D_ANIM_RUN});
    } else if (strafeOnly) {
        s = findSeq(blob, {(uint16_t)(left ? W3D_ANIM_SHUFFLE_LEFT : W3D_ANIM_SHUFFLE_RIGHT)});
        if (s >= 0) {   // shuffling is authored slow: speed it up rather than sliding
            *rate = c.speed > 4.5f ? 1.6f : 1.0f;
            return s;
        }
    }
    if (s < 0) s = c.speed < 4.5f ? findSeq(blob, {W3D_ANIM_WALK, W3D_ANIM_RUN}) : findSeq(blob, {W3D_ANIM_RUN, W3D_ANIM_WALK});
    *rate = moveRate(blob, s, c.speed);
    return s;
}

}  // namespace

int creaturePickSeq(const uint8_t* blob, bool dead, float speed, bool swimming, bool attacking, float* rate, bool* loop) {
    *rate = 1.0f;
    *loop = true;
    if (dead) {
        int s = findSeq(blob, {W3D_ANIM_DEATH});
        if (s >= 0) { *loop = false; return s; }
        return findSeq(blob, {W3D_ANIM_STAND});
    }
    if (attacking && speed <= 0.1f) {
        int s = attackSeq(blob, CreatureWeapon::OneHand, false);
        if (s >= 0) { *loop = false; return s; }
    }
    if (speed > 0.1f) {
        CreatureSeen c;
        c.speed = speed;
        c.swimming = swimming;
        c.moveFlags = kForward;
        int s = moveSeq(blob, c, rate);
        if (s >= 0) return s;
    }
    if (swimming) {
        int s = findSeq(blob, {W3D_ANIM_SWIM_IDLE});
        if (s >= 0) return s;
    }
    return findSeq(blob, {W3D_ANIM_STAND});
}

// ------------------------------------------------------------------ UnitAnimator
void UnitAnimator::startOneShot(const uint8_t* blob, int seq, uint8_t prio, uint32_t nowMs) {
    const W3dAnimSeq* sq = seqAt(blob, seq);
    if (!sq || !sq->durationMs) return;
    if (oneSeq_ >= 0 && nowMs - oneStart_ < oneDur_ && prio < onePrio_) return;   // something more important plays
    oneSeq_ = seq;
    onePrio_ = prio;
    oneStart_ = nowMs;
    oneDur_ = sq->durationMs;
    oneCancelOnMove_ = prio == kPrioReact || prio == kPrioEmote;
    seq_ = -4;   // restart the clock
}

AnimChoice UnitAnimator::update(const uint8_t* blob, const CreatureSeen& c, bool riding, uint32_t nowMs) {
    AnimChoice a;
    float dt = started_ ? (float)(int32_t)(nowMs - lastMs_) : 0.0f;
    if (dt < 0 || dt > 1000) dt = 0;
    lastMs_ = nowMs;
    if (!started_) {   // the first look: no events from the past, a dead unit lies already
        started_ = true;
        firstSeenDead_ = c.dead;
        lastStand_ = c.standState;
        lastSwing_ = c.swingMs;
        lastHit_ = c.hitMs;
        lastEmote_ = c.emoteMs;
        lastCastGo_ = c.castGoMs;
        wasAirborne_ = (c.moveFlags & (kJumping | kFallingFar)) != 0;
    }
    const bool moving = c.speed > 0.1f;
    const bool airborne = (c.moveFlags & (kJumping | kFallingFar)) != 0 && !c.swimming;

    // ---- events
    if (c.dead) {
        oneSeq_ = -1;
    } else {
        if (c.standState != lastStand_) {   // sitting down / getting up / lying down / kneeling
            const uint8_t from = lastStand_, to = c.standState;
            lastStand_ = to;
            int s = -1;
            if (to == 1) s = findSeq(blob, {W3D_ANIM_SIT_GROUND_DOWN});
            else if (to == 3) s = findSeq(blob, {W3D_ANIM_SLEEP_DOWN});
            else if (to == 8) s = findSeq(blob, {W3D_ANIM_KNEEL_START});
            else if (to == 0 && from == 1) s = findSeq(blob, {W3D_ANIM_SIT_GROUND_UP});
            else if (to == 0 && from == 3) s = findSeq(blob, {W3D_ANIM_SLEEP_UP});
            else if (to == 0 && from == 8) s = findSeq(blob, {W3D_ANIM_KNEEL_END});
            if (s >= 0 && !moving) startOneShot(blob, s, kPrioTransition, nowMs);
        }
        if (airborne && !wasAirborne_ && !riding) {
            int s = findSeq(blob, {W3D_ANIM_JUMP_START});
            if (s >= 0) startOneShot(blob, s, kPrioTransition, nowMs);
        } else if (!airborne && wasAirborne_ && !moving && !riding) {
            int s = findSeq(blob, {W3D_ANIM_JUMP_END});
            if (s >= 0) startOneShot(blob, s, kPrioTransition, nowMs);
        }
        if (c.swingMs && c.swingMs != lastSwing_) {
            int s = attackSeq(blob, c.weapon, c.swingOff);
            if (s >= 0 && !moving) startOneShot(blob, s, kPrioAttack, nowMs);
        }
        if (c.castGoMs && c.castGoMs != lastCastGo_) {
            int s = castGoSeq(blob, c.castGoLook, c.castGoDirected);
            if (s >= 0 && !moving) startOneShot(blob, s, kPrioCast, nowMs);
        }
        if (c.hitMs && c.hitMs != lastHit_ && !moving) {
            int s = reactSeq(blob, c.hitReact, c.weapon);
            if (s >= 0) startOneShot(blob, s, kPrioReact, nowMs);
        }
        if (c.emoteMs && c.emoteMs != lastEmote_ && c.emoteAnim && !moving) {
            int s = w3d_find_seq(blob, c.emoteAnim);
            if (s >= 0) startOneShot(blob, s, kPrioEmote, nowMs);
        }
    }
    lastSwing_ = c.swingMs;
    lastCastGo_ = c.castGoMs;
    lastHit_ = c.hitMs;
    lastEmote_ = c.emoteMs;
    wasAirborne_ = airborne;
    if (oneSeq_ >= 0 && (nowMs - oneStart_ >= oneDur_ || (moving && oneCancelOnMove_) || c.stunned)) oneSeq_ = -1;

    // ---- the state
    int s = -1;
    if (c.dead) {
        s = findSeq(blob, {W3D_ANIM_DEATH});
        a.loop = false;
        if (s < 0) { s = findSeq(blob, {W3D_ANIM_DEAD, W3D_ANIM_STAND}); a.loop = true; }
    } else if (riding) {
        s = findSeq(blob, {W3D_ANIM_MOUNT, W3D_ANIM_SIT_GROUND, W3D_ANIM_STAND});
    } else if (c.flying) {   // a taxi mount in the air
        s = findSeq(blob, {W3D_ANIM_FLY, W3D_ANIM_RUN, W3D_ANIM_WALK, W3D_ANIM_STAND});
    } else if (c.stunned) {
        s = findSeq(blob, {W3D_ANIM_STUN, W3D_ANIM_STAND});
    } else if (oneSeq_ >= 0) {
        s = oneSeq_;
        a.loop = false;
    } else if (airborne) {
        s = (c.moveFlags & kFallingFar) ? findSeq(blob, {W3D_ANIM_FALL, W3D_ANIM_JUMP}) : findSeq(blob, {W3D_ANIM_JUMP, W3D_ANIM_FALL});
        if (s < 0) s = moving ? moveSeq(blob, c, &a.rate) : idleSeq(blob, c);
    } else if (moving) {
        s = moveSeq(blob, c, &a.rate);
    } else {
        s = idleSeq(blob, c);
    }
    a.seq = s;
    if (s != seq_) {
        // A unit first seen dead lies at the end of its death animation.
        timeMs_ = (c.dead && firstSeenDead_) ? 1e9f : 0;
        seq_ = s;
    } else {
        timeMs_ += dt * a.rate;
        if (a.loop && timeMs_ > 1e7f) timeMs_ = 0;   // keep float precision
    }
    if (!c.dead) firstSeenDead_ = false;
    wasDead_ = c.dead;
    if (const W3dAnimSeq* sq = seqAt(blob, s)) a.animId = sq->animId;
    return a;
}

namespace {

// ------------------------------------------------------------------ worn items (stage 5.19)
// Where an item hangs on its bearer: attachment candidates in order (W3dAttachId); none = not drawn.
// Players sheathe their weapons out of combat (hip, back) and draw them in combat; creatures always hold theirs.
// side: shoulders 0 = left, 1 = right. shooting: a bow / gun / throw is being fired (the ranged weapon is in hand).
int wornAttach(const CreatureSeen& c, int slot, int side, bool shooting, int out[3]) {
    const int inv = c.equipInv[slot];
    const bool player = c.look != 0;
    const bool handsFree = !player || c.inCombat;   // weapons in hand
    int n = 0;
    auto add = [&](int a) { out[n++] = a; };
    switch (slot) {
        case W3D_EQ_HEAD: add(W3D_ATT_HELM); break;
        case W3D_EQ_SHOULDERS: add(side ? W3D_ATT_RSHOULDER : W3D_ATT_LSHOULDER); break;
        case W3D_EQ_MAINHAND:
            if (shooting) break;
            if (handsFree) { add(W3D_ATT_RHAND); }
            else if (inv == 17) { add(W3D_ATT_LARGE_LEFT); add(W3D_ATT_LARGE_RIGHT); add(W3D_ATT_BACK); }   // two-hander on the back
            else { add(W3D_ATT_SHEATH_MAIN); add(W3D_ATT_HIP_RIGHT); add(W3D_ATT_HIP_LEFT); }
            break;
        case W3D_EQ_OFFHAND:
            if (inv == 14) {   // shield: on the arm, else on the back
                if (handsFree) add(W3D_ATT_SHIELD);
                else { add(W3D_ATT_SHEATH_SHIELD); add(W3D_ATT_BACK); }
            } else if (inv == 23) {   // held in off hand (book, orb): only in hand
                if (handsFree && !shooting) add(W3D_ATT_LHAND);
            } else if (!shooting) {
                if (handsFree) add(W3D_ATT_LHAND);
                else { add(W3D_ATT_SHEATH_OFF); add(W3D_ATT_HIP_LEFT); add(W3D_ATT_HIP_RIGHT); }
            }
            break;
        case W3D_EQ_RANGED:
            if (shooting) { add(inv == 15 ? W3D_ATT_LHAND : W3D_ATT_RHAND); }                  // bow / crossbow in the left hand
            else if (inv == 15 || (inv == 26 && !player)) { add(W3D_ATT_LARGE_RIGHT); add(W3D_ATT_LARGE_LEFT); add(W3D_ATT_BACK); }   // bows, guns on the back
            else if (inv == 26) { add(W3D_ATT_LARGE_RIGHT); add(W3D_ATT_LARGE_LEFT); }
            break;
        default: break;
    }
    return n;
}

bool findAttach(const uint8_t* blob, const int* ids, int n, CreatureInstance::Worn* w) {
    const W3dCreatureHeader* h = w3d_creature(blob);
    if (!h->attachCount || !h->offAttach) return false;
    const W3dAttach* at = (const W3dAttach*)(blob + h->offAttach);
    for (int i = 0; i < n; ++i)
        for (uint32_t k = 0; k < h->attachCount; ++k)
            if (at[k].id == ids[i] && at[k].bone >= 0 && (uint32_t)at[k].bone < h->boneCount) {
                w->bone = at[k].bone;
                w->pos[0] = at[k].pos[0]; w->pos[1] = at[k].pos[1]; w->pos[2] = at[k].pos[2];
                return true;
            }
    return false;
}

constexpr int kWornEq[5] = {W3D_EQ_HEAD, W3D_EQ_SHOULDERS, W3D_EQ_MAINHAND, W3D_EQ_OFFHAND, W3D_EQ_RANGED};

void releaseWorn(UnitWorn& w) {
    for (Model*& m : w.model) if (m) { streamerReleaseItemModel(m); m = nullptr; }
    if (w.item) streamerReleaseItem(w.item);
    w = UnitWorn();
}

// Models of the item in 'slot' of a unit: acquired once its ITEM entry is in. Helmets are per race / sex.
void updateWorn(UnitWorn& w, const CreatureSeen& c, int slot) {
    const uint32_t want = c.equip[slot];
    if (w.disp != want) {
        releaseWorn(w);
        w.disp = want;
        w.item = want ? streamerAcquireItem(want) : nullptr;
    }
    if (!w.item || w.resolved || w.item->state != AssetState::Ready) return;
    w.resolved = true;
    if (!w.item->ok) return;
    const W3dItemDisplay& d = w.item->d;
    uint32_t id[2] = {0, 0};
    if (slot == W3D_EQ_HEAD) {
        if (c.lookParts.race >= 1 && c.lookParts.race <= 8) id[0] = d.helm[(c.lookParts.race - 1) * 2 + (c.lookParts.sex & 1)];
    } else if (slot == W3D_EQ_SHOULDERS) {
        id[0] = d.model[0] ? d.model[0] : d.model[1];
        id[1] = d.model[1] ? d.model[1] : d.model[0];
    } else {
        id[0] = d.model[0];
    }
    for (int i = 0; i < 2; ++i) w.model[i] = id[i] ? streamerAcquireItemModel(id[i]) : nullptr;
}

}  // namespace

// ------------------------------------------------------------------ CreatureView
bool CreatureView::seatPosition(const uint8_t* blob, int seq, float timeMs, bool loop, const float pos[3], float yaw,
                                float scale, float out[3]) {
    const W3dCreatureHeader* h = w3d_creature(blob);
    if (h->seatBone < 0 || (uint32_t)h->seatBone >= h->boneCount) return false;
    static std::vector<W3dMat34> pose;
    if (pose.size() < h->boneCount) pose.resize(h->boneCount);
    w3d_anim_pose(blob, seq, timeMs, loop ? 1 : 0, pose.data());
    const float* M = pose[h->seatBone].m;
    float p[3];
    for (int r = 0; r < 3; ++r) p[r] = (M[r * 4] * h->seat[0] + M[r * 4 + 1] * h->seat[1] + M[r * 4 + 2] * h->seat[2] + M[r * 4 + 3]) * scale;
    const float cy = cosf(yaw), sy = sinf(yaw);
    out[0] = pos[0] + p[0] * cy - p[1] * sy;
    out[1] = pos[1] + p[0] * sy + p[1] * cy;
    out[2] = pos[2] + p[2];
    return true;
}

void CreatureView::update(const std::vector<CreatureSeen>& seen, uint32_t nowMs, std::vector<CreatureInstance>& out) {
    out.clear();
    ++frame_;
    ready_ = 0;
    for (const CreatureSeen& c : seen) {
        Unit& u = units_[c.guid];
        u.frame = frame_;
        u.drawn = false;
        const bool fresh = !u.disp && !u.missing;
        const uint64_t eqKey = c.look ? w3d_equip_key(c.equip) : 0;
        if (u.displayId != c.displayId || u.look != c.look || fresh) {   // new, morph, new look
            if (u.disp) streamerReleaseDisplay(u.disp);
            if (u.next) streamerReleaseDisplay(u.next);
            u.next = nullptr;
            u.displayId = c.displayId;
            u.look = c.look;
            u.eqKey = eqKey;
            if (c.look) {   // a player in its own shape; no race model in the pack -> its creature display
                u.disp = streamerAcquirePlayerDisplay(c.lookParts, c.equip);
                if (!u.disp && c.displayId) u.disp = streamerAcquireDisplay(c.displayId);
            } else {
                u.disp = c.displayId ? streamerAcquireDisplay(c.displayId) : nullptr;
            }
            u.missing = !u.disp;
            u.anim.reset();
        } else if (c.look && u.eqKey != eqKey) {
            // armour changed (or its item info arrived): build the new display while the old one is still drawn
            if (u.next) streamerReleaseDisplay(u.next);
            u.eqKey = eqKey;
            u.next = streamerAcquirePlayerDisplay(c.lookParts, c.equip);
            if (!u.next && !u.disp) u.missing = true;
        }
        if (u.next && (u.next->ready() || u.next->failed || !u.disp || !u.disp->ready())) {
            if (u.next->failed && u.disp) {   // keep the old one
                streamerReleaseDisplay(u.next);
            } else {
                if (u.disp) streamerReleaseDisplay(u.disp);
                u.disp = u.next;
            }
            u.next = nullptr;
            u.missing = !u.disp;
        }
        if (u.mountDisplay != c.mountDisplay) {   // mounting / dismounting
            if (u.mount) streamerReleaseDisplay(u.mount);
            u.mount = c.mountDisplay ? streamerAcquireDisplay(c.mountDisplay) : nullptr;
            u.mountDisplay = c.mountDisplay;
            u.mountAnim.reset();
        }
        if (!u.disp || !u.disp->ready()) continue;
        const uint8_t* blob = u.disp->model->blob;
        CreatureInstance ci;
        ci.disp = u.disp;
        ci.pos[0] = c.pos[0]; ci.pos[1] = c.pos[1]; ci.pos[2] = c.pos[2];
        ci.yaw = c.yaw;
        ci.scale = c.scale;
        // riding: the mount moves (its own walk / run / jump), the unit sits in its seat
        bool riding = false;
        if (u.mount && u.mount->ready() && !c.dead) {
            const uint8_t* mb = u.mount->model->blob;
            CreatureSeen mc = c;
            mc.inCombat = false;
            mc.casting = 0;
            mc.standState = 0;
            mc.loopAnim = 0;
            mc.swingMs = mc.hitMs = mc.castGoMs = mc.emoteMs = 0;
            AnimChoice ma = u.mountAnim.update(mb, mc, false, nowMs);
            CreatureInstance mi = ci;
            mi.disp = u.mount;
            mi.seq = ma.seq;
            mi.timeMs = u.mountAnim.timeMs();
            mi.loop = ma.loop;
            const float ms = c.scale * (u.mount->d.scale > 0.01f ? u.mount->d.scale : 1.0f);
            float seat[3];
            if (seatPosition(mb, ma.seq, mi.timeMs, ma.loop, c.pos, c.yaw, ms, seat)) {
                ci.pos[0] = seat[0]; ci.pos[1] = seat[1]; ci.pos[2] = seat[2];
                riding = true;
            }
            out.push_back(mi);
        }
        AnimChoice a = u.anim.update(blob, c, riding, nowMs);
        ci.seq = a.seq;
        ci.timeMs = u.anim.timeMs();
        ci.loop = a.loop;
        ci.wornCount = 0;
        // item models: helmet, shoulders, weapons
        const bool shooting = c.castGoMs && nowMs - c.castGoMs < 1500 && c.castGoLook != CastLook::Spell && c.equip[W3D_EQ_RANGED];
        for (int k = 0; k < CreatureView::kWornSlots; ++k) {
            const int slot = kWornEq[k];
            UnitWorn& w = u.worn[k];
            updateWorn(w, c, slot);
            if (!w.resolved) continue;
            for (int side = 0; side < (slot == W3D_EQ_SHOULDERS ? 2 : 1); ++side) {
                if (!w.model[side] || w.model[side]->state != AssetState::Ready || !w.model[side]->h ||
                    ci.wornCount >= CreatureInstance::kMaxWorn) continue;
                int cand[4];
                int n = wornAttach(c, slot, side, shooting, cand);
                CreatureInstance::Worn wi;
                wi.model = w.model[side];
                if (n && findAttach(blob, cand, n, &wi)) ci.worn[ci.wornCount++] = wi;
            }
        }
        out.push_back(ci);
        u.drawn = true;
        ++ready_;
    }
    for (auto it = units_.begin(); it != units_.end();) {
        if (it->second.frame != frame_) {
            if (it->second.disp) streamerReleaseDisplay(it->second.disp);
            if (it->second.next) streamerReleaseDisplay(it->second.next);
            if (it->second.mount) streamerReleaseDisplay(it->second.mount);
            for (UnitWorn& w : it->second.worn) releaseWorn(w);
            it = units_.erase(it);
        } else {
            ++it;
        }
    }
}

bool CreatureView::drawn(uint64_t guid) const {
    auto it = units_.find(guid);
    return it != units_.end() && it->second.drawn;
}

void CreatureView::clear() {
    for (auto& [g, u] : units_) {
        if (u.disp) streamerReleaseDisplay(u.disp);
        if (u.next) streamerReleaseDisplay(u.next);
        if (u.mount) streamerReleaseDisplay(u.mount);
        for (UnitWorn& w : u.worn) releaseWorn(w);
    }
    units_.clear();
    ready_ = 0;
}
