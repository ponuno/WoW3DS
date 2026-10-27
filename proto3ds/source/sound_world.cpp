#include "sound_world.hpp"

#include <algorithm>
#include <cmath>

using namespace w3n;

namespace {
constexpr float kHearRange = 45.0f;      // units farther away are not scanned for deaths / aggro
constexpr uint32_t kAggroRepeatMs = 15000;
}

void SoundWorld::reset() {
    units_.clear();
    havePos_ = false;
    stepDist_ = 0;
}

const snd::CreatureSounds* SoundWorld::soundsOf(const WorldObject& o) const {
    const snd::CreatureSounds* c = tables_.creature(o.field(UF::UNIT_DISPLAYID));
    if (!c) c = tables_.creature(o.field(UF::UNIT_NATIVEDISPLAYID));
    return c;
}

// Weapon of the attacker: creatures carry class / subclass in UNIT_VIRTUAL_ITEM_INFO (byte 0 class 2 = weapon,
// byte 1 subclass), players show their main hand (equipment slot 15) whose item we may know.
uint32_t SoundWorld::weaponSubclass(GameSession& g, const WorldObject& o) const {
    if (o.type == ObjType::Player) {
        const uint32_t entry = o.field((uint16_t)(UF::PLAYER_VISIBLE_ITEM_1_0 + 15 * UF::PLAYER_VISIBLE_ITEM_STRIDE));
        if (!entry) return ~0u;
        const ItemInfo* it = g.itemInfoIfKnown(entry);
        return it && it->cls == 2 ? it->subclass : 7u;   // unknown yet: a one-handed sword
    }
    const uint32_t info = o.field(UF::UNIT_VIRTUAL_ITEM_INFO);
    if ((info & 0xFF) == 2) return (info >> 8) & 0xFF;
    return ~0u;
}

void SoundWorld::at(const GameSession& g, uint64_t guid, uint32_t sound, const char* what, std::vector<SoundReq>& out, uint64_t tag) {
    if (!sound) return;
    const WorldObject* o = g.objects.find(guid);
    SoundReq r;
    r.sound = sound;
    r.what = what;
    r.tag = tag;
    if (o && o->hasPos) {
        float p[3];
        o->positionAt(now_, p, nullptr);   // along its spline when it walks
        r.x = p[0];
        r.y = p[1];
        r.z = p[2];
    }
    r.self = guid == g.selfGuid() || !o;
    out.push_back(r);
}

void SoundWorld::update(GameSession& g, const float pos[3], bool moving, bool onGround, bool walking, uint32_t nowMs,
                        std::vector<SoundReq>& out, int water) {
    const uint64_t me = g.selfGuid();
    now_ = nowMs;
    // ---- the session's events
    for (const GameSession::WorldSound& w : g.worldSounds) {
        const WorldObject* src = g.objects.find(w.src);
        switch (w.kind) {
            case GameSession::WorldSound::Melee: {
                if (!src) break;
                const WorldObject* dst = g.objects.find(w.dst);
                const bool miss = (w.hitInfo & 0x10) != 0, crit = (w.hitInfo & 0x80) != 0;
                const bool landed = !miss && (w.victimState == 1 || w.victimState == 0);
                const snd::CreatureSounds* cs = soundsOf(*src);
                const uint32_t sub = weaponSubclass(g, *src);
                if (landed) {
                    // the blow: the weapon's impact on flesh, or the creature's own attack (a bite, a claw)
                    uint32_t s = 0;
                    if (sub != ~0u) {
                        auto [imp, cr] = tables_.weapon(sub);
                        s = crit && cr ? cr : imp;
                    }
                    if (!s && cs) s = crit && cs->attackCrit ? cs->attackCrit : cs->attack;
                    if (!s) { auto [imp, cr] = tables_.weapon(7); s = crit && cr ? cr : imp; }
                    at(g, w.dst, s, "impact", out);
                    // the victim's cry: always on a crit, one blow in three otherwise
                    if (dst) {
                        const snd::CreatureSounds* vs = soundsOf(*dst);
                        if (vs && (crit || rng_() % 3 == 0)) at(g, w.dst, crit && vs->woundCrit ? vs->woundCrit : vs->wound, "wound", out);
                    }
                } else if (cs && cs->attack && sub == ~0u && rng_() % 2 == 0) {
                    at(g, w.src, cs->attack, "swing", out);   // a creature missing still growls
                }
                break;
            }
            case GameSession::WorldSound::SpellStart: {
                const snd::SpellSounds* ss = tables_.spell(w.spell);
                if (ss && w.castMs) at(g, w.src, ss->precast, "precast", out, w.src | 1);
                break;
            }
            case GameSession::WorldSound::SpellGo: {
                SoundReq stop;
                stop.stop = true;
                stop.tag = w.src | 1;
                out.push_back(stop);
                const snd::SpellSounds* ss = tables_.spell(w.spell);
                if (!ss) break;
                at(g, w.src, ss->cast ? ss->cast : ss->missile, "cast", out);
                for (int i = 0; i < w.nTargets && i < 2; ++i)
                    if (w.targets[i] && (w.targets[i] != w.src || !ss->cast)) at(g, w.targets[i], ss->impact, "spellimpact", out);
                if (!w.nTargets && !ss->cast) at(g, w.src, ss->impact, "spellimpact", out);
                break;
            }
            case GameSession::WorldSound::SpellFail: {
                SoundReq stop;
                stop.stop = true;
                stop.tag = w.src | 1;
                out.push_back(stop);
                break;
            }
        }
    }
    g.worldSounds.clear();

    // ---- deaths and aggro of the units around
    for (const auto& [guid, o] : g.objects.all()) {
        if (o.type != ObjType::Unit && o.type != ObjType::Player) continue;
        if (!o.hasPos || guid == me) continue;
        const float dx = o.mv.x - pos[0], dy = o.mv.y - pos[1];
        if (dx * dx + dy * dy > kHearRange * kHearRange) continue;
        const bool known = units_.count(guid) != 0;
        UnitState& u = units_[guid];
        const bool dead = o.field(UF::UNIT_MAXHEALTH) && o.field(UF::UNIT_HEALTH) == 0;
        const uint64_t tgt = (uint64_t)o.field(UF::UNIT_TARGET) | ((uint64_t)o.field(UF::UNIT_TARGET + 1) << 32);
        if (known) {
            const snd::CreatureSounds* cs = soundsOf(o);
            if (dead && !u.dead && cs) at(g, guid, cs->death, "death", out);
            if (!dead && tgt == me && u.target != me && o.type == ObjType::Unit && g.enemyLike(o) && cs &&
                (!u.aggroMs || nowMs - u.aggroMs > kAggroRepeatMs)) {
                at(g, guid, cs->aggro, "aggro", out);
                u.aggroMs = nowMs | 1;
            }
        }
        u.dead = dead;
        u.target = tgt;
        u.seenMs = nowMs;
    }
    if (units_.size() > 300)
        for (auto it = units_.begin(); it != units_.end();)
            it = nowMs - it->second.seenMs > 10000 ? units_.erase(it) : std::next(it);

    // ---- a fish bites (stage 5.26): the splash the server announces at our bobber
    if (g.fishBiteMs && g.fishBiteMs != biteMs_) {
        biteMs_ = g.fishBiteMs;
        if (uint32_t s = tables_.liquid("fishbite")) at(g, g.fishBiteGuid, s, "fishbite", out);
    }
    // ---- swimming: a stroke now and then while moving (no footsteps)
    if (water == 2) {
        if (moving && nowMs - strokeMs_ > 1100) {
            strokeMs_ = nowMs;
            SoundReq r;
            r.sound = tables_.liquid("swim");
            r.self = true;
            r.what = "swim";
            r.gain = 0.45f;
            if (r.sound) out.push_back(r);
        }
        lastPos_[0] = pos[0]; lastPos_[1] = pos[1]; lastPos_[2] = pos[2];
        havePos_ = true;
        return;
    }
    // ---- our footsteps: one per stride on the ground
    if (havePos_ && moving && onGround) {
        const float dx = pos[0] - lastPos_[0], dy = pos[1] - lastPos_[1];
        const float d = sqrtf(dx * dx + dy * dy);
        if (d < 12) stepDist_ += d;   // a teleport is not a walk
        const float stride = walking ? 1.5f : 2.7f;
        if (stepDist_ >= stride) {
            stepDist_ = fmodf(stepDist_, stride);
            uint32_t foot = 0;
            if (const WorldObject* self = g.objects.find(me))
                if (const snd::CreatureSounds* cs = soundsOf(*self)) foot = cs->footstep;
            SoundReq r;
            r.sound = tables_.footstep(foot);
            r.self = true;
            r.what = "step";
            r.gain = 0.35f;
            if (water == 1 && tables_.liquid("wade")) {   // wading: the step splashes
                r.sound = tables_.liquid("wade");
                r.what = "wade";
                r.gain = 0.45f;
            }
            if (r.sound) out.push_back(r);
        }
    } else if (!moving) {
        stepDist_ = 1.0f;   // the first step comes soon after starting
    }
    lastPos_[0] = pos[0];
    lastPos_[1] = pos[1];
    lastPos_[2] = pos[2];
    havePos_ = true;
}

void SoundWorld::splash(const float pos[3], float speed, uint32_t nowMs, std::vector<SoundReq>& out) {
    (void)nowMs;
    SoundReq r;
    r.sound = tables_.liquid("splash");
    r.x = pos[0]; r.y = pos[1]; r.z = pos[2];
    r.self = true;
    r.what = "splash";
    r.gain = std::min(1.0f, 0.4f + std::max(0.0f, speed) * 0.05f);
    if (r.sound) out.push_back(r);
    strokeMs_ = nowMs;
}
