// spellfx.cpp — see spellfx.hpp. The stages follow WoWee's SpellVisualSystem (playSpellVisualPrecast: the precast kit for
// the cast time; the missile flies caster -> target, the impact plays on arrival; hand effects on both hands).
#include "spellfx.hpp"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace sfx {

bool Table::load(const uint8_t* blob, uint32_t size) {
    h_ = nullptr;
    data_.assign(blob, blob + size);
    if (size < sizeof(W3dSpellFxHeader)) return false;
    const W3dSpellFxHeader* h = (const W3dSpellFxHeader*)data_.data();
    if (h->magic != W3D_SVFX_MAGIC) return false;
    if (h->offEffects + (uint64_t)h->effectCount * sizeof(W3dSpellEffect) > size ||
        h->offVisuals + (uint64_t)h->visualCount * sizeof(W3dSpellVisual) > size ||
        h->offSpells + (uint64_t)h->spellCount * sizeof(W3dSpellRef) > size || ((h->offEffects | h->offVisuals | h->offSpells) & 3))
        return false;
    h_ = h;
    return true;
}

const W3dSpellVisual* Table::visualOf(uint32_t spell, float* speed) const {
    if (!h_) return nullptr;
    const W3dSpellRef* r = (const W3dSpellRef*)(data_.data() + h_->offSpells);
    const W3dSpellRef* end = r + h_->spellCount;
    const W3dSpellRef* it = std::lower_bound(r, end, spell, [](const W3dSpellRef& a, uint32_t s) { return a.spell < s; });
    if (it == end || it->spell != spell || it->visual >= h_->visualCount) return nullptr;
    if (speed) *speed = it->speed;
    return (const W3dSpellVisual*)(data_.data() + h_->offVisuals) + it->visual;
}

const W3dSpellEffect* Table::effect(uint16_t index) const {
    if (!h_ || index >= h_->effectCount) return nullptr;
    return (const W3dSpellEffect*)(data_.data() + h_->offEffects) + index;
}

bool Director::add(Active a) {
    if ((int)active_.size() >= kMaxActive) {   // the oldest lingering one makes room, else the newest is not shown
        auto it = std::find_if(active_.begin(), active_.end(), [&](const Active& x) { return x.endMs <= a.startMs; });
        if (it == active_.end()) { ++stats_.skippedFull; return false; }
        active_.erase(it);
    }
    a.key = nextKey_++;
    if (!nextKey_) nextKey_ = 1;
    active_.push_back(a);
    ++stats_.started;
    return true;
}

void Director::startStage(const W3dSpellVisual* v, int stage, uint64_t unit, uint32_t spell, uint32_t nowMs, uint32_t durMs) {
    for (int k = 0; k < 2; ++k) {
        const uint16_t e = v->effect[stage][k];
        const W3dSpellEffect* ef = table_->effect(e);
        if (!ef) continue;
        Active a{};
        a.effect = e;
        a.stage = (uint8_t)stage;
        a.slot = v->slot[stage][k];
        a.unit = unit;
        a.spell = spell;
        a.startMs = nowMs;
        a.endMs = nowMs + durMs;
        a.scale = ef->scale;
        a.impact[0] = a.impact[1] = 0xFFFF;
        add(a);
        // a hand effect alone on one hand shows on both (WoWee mirrors the right hand's onto the left)
        if (k == 0 && v->effect[stage][1] == 0xFFFF && (a.slot == W3D_SLOT_RHAND || a.slot == W3D_SLOT_LHAND)) {
            a.slot = a.slot == W3D_SLOT_RHAND ? W3D_SLOT_LHAND : W3D_SLOT_RHAND;
            add(a);
        }
    }
}

void Director::event(const Event& ev, uint32_t nowMs, const float cam[3], const Resolver& where) {
    if (!table_ || !table_->loaded()) return;
    float speed = 0;
    const W3dSpellVisual* v = table_->visualOf(ev.spell, &speed);
    // whatever this caster was preparing ends here (a new cast, the spell going off, or failing)
    if (ev.kind != Event::Start || v)
        for (Active& a : active_)
            if (a.stage == W3D_SFX_PRECAST && a.unit == ev.caster && a.endMs > nowMs) a.endMs = nowMs;
    if (!v || ev.kind == Event::Fail) return;
    float cp[3], yaw = 0;
    if (!where(ev.caster, W3D_SLOT_CHEST, cp, &yaw)) return;
    const float dx = cp[0] - cam[0], dy = cp[1] - cam[1], dz = cp[2] - cam[2];
    if (dx * dx + dy * dy + dz * dz > kRange * kRange) { ++stats_.skippedFar; return; }
    if (ev.kind == Event::Start) {
        if (ev.castMs > 0) startStage(v, W3D_SFX_PRECAST, ev.caster, ev.spell, nowMs, std::min<uint32_t>(ev.castMs + 300, 10000));
        return;
    }
    // ---- the spell went off
    startStage(v, W3D_SFX_CAST, ev.caster, ev.spell, nowMs, 1200);
    const W3dSpellEffect* mis = table_->effect(v->effect[W3D_SFX_MISSILE][0]);
    const int n = std::min<int>(ev.nTargets, 4);
    for (int t = 0; t < n; ++t) {
        const uint64_t target = ev.targets[t];
        if (!target) continue;
        if (mis && target != ev.caster) {
            Active a{};
            a.effect = v->effect[W3D_SFX_MISSILE][0];
            a.stage = W3D_SFX_MISSILE;
            a.slot = W3D_SLOT_NONE;
            a.unit = ev.caster;
            a.target = target;
            a.spell = ev.spell;
            a.startMs = nowMs;
            a.endMs = nowMs + 4000;   // never longer than that in the air
            if (!where(ev.caster, W3D_SLOT_RHAND, a.pos, &yaw)) memcpy(a.pos, cp, sizeof cp);
            a.speed = speed > 0 ? speed : 20.0f;
            a.scale = mis->scale;
            a.impact[0] = v->effect[W3D_SFX_IMPACT][0];
            a.impact[1] = v->effect[W3D_SFX_IMPACT][1];
            a.impactSlot[0] = v->slot[W3D_SFX_IMPACT][0];
            a.impactSlot[1] = v->slot[W3D_SFX_IMPACT][1];
            if (add(a)) ++stats_.missiles;
        } else {
            startStage(v, W3D_SFX_IMPACT, target, ev.spell, nowMs, 1500);
            ++stats_.impacts;
        }
    }
}

void Director::update(uint32_t nowMs, const Resolver& where, std::vector<Placed>& out) {
    out.clear();
    std::vector<Active> arrivals;
    for (size_t i = 0; i < active_.size();) {
        Active& a = active_[i];
        const bool emitting = (int32_t)(nowMs - a.endMs) < 0;
        if (!emitting && (int32_t)(nowMs - (a.endMs + kLingerMs)) >= 0) {   // faded out: gone
            active_[i] = active_.back();
            active_.pop_back();
            continue;
        }
        Placed p;
        p.key = a.key;
        p.effect = a.effect;
        p.scale = a.scale;
        p.stage = a.stage;
        p.emitting = emitting;
        if (a.stage == W3D_SFX_MISSILE) {
            if (emitting && !a.arrived) {
                float goal[3], yaw;
                if (!where(a.target, W3D_SLOT_CHEST, goal, &yaw)) {
                    a.endMs = nowMs;   // the target is gone: the missile just fades
                    p.emitting = false;
                } else {
                    const float dt = std::min(0.1f, (float)(nowMs - a.startMs) / 1000.0f);
                    a.startMs = nowMs;   // from here on startMs is "last moved"
                    float d[3] = {goal[0] - a.pos[0], goal[1] - a.pos[1], goal[2] - a.pos[2]};
                    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    const float step = a.speed * dt;
                    if (len <= step || len < 0.3f) {
                        memcpy(a.pos, goal, sizeof goal);
                        a.arrived = true;
                        a.endMs = nowMs;   // the missile stops emitting, its trail fades
                        Active hit{};
                        hit.unit = a.target;
                        hit.spell = a.spell;
                        for (int k = 0; k < 2; ++k) hit.impact[k] = a.impact[k], hit.impactSlot[k] = a.impactSlot[k];
                        arrivals.push_back(hit);
                    } else {
                        for (int k = 0; k < 3; ++k) a.pos[k] += d[k] / len * step;
                    }
                    if (len > 1e-3f) {
                        p.rot[2] = atan2f(d[1], d[0]);
                        p.rot[1] = -asinf(std::clamp(d[2] / len, -1.0f, 1.0f));   // nose up toward a higher target
                    }
                }
            }
            memcpy(p.pos, a.pos, sizeof a.pos);
        } else {
            float yaw = 0;
            if (!where(a.unit, a.slot == W3D_SLOT_NONE ? (int)W3D_SLOT_BASE : (int)a.slot, p.pos, &yaw)) {
                if (emitting) a.endMs = nowMs;   // the unit went away
                p.emitting = false;
                memcpy(p.pos, a.pos, sizeof a.pos);
            } else {
                memcpy(a.pos, p.pos, sizeof a.pos);
            }
            p.rot[2] = yaw;
        }
        out.push_back(p);
        ++i;
    }
    for (const Active& h : arrivals) {   // impacts of the missiles that arrived this frame
        for (int k = 0; k < 2; ++k) {
            const W3dSpellEffect* ef = table_ ? table_->effect(h.impact[k]) : nullptr;
            if (!ef) continue;
            Active a{};
            a.effect = h.impact[k];
            a.stage = W3D_SFX_IMPACT;
            a.slot = h.impactSlot[k];
            a.unit = h.unit;
            a.spell = h.spell;
            a.startMs = nowMs;
            a.endMs = nowMs + 1500;
            a.scale = ef->scale;
            a.impact[0] = a.impact[1] = 0xFFFF;
            if (add(a)) ++stats_.impacts;
        }
    }
    stats_.active = (uint32_t)active_.size();
}

}  // namespace sfx
