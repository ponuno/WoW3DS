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

uint32_t Director::startEffect(uint16_t effect, uint64_t unit, uint8_t slot, uint32_t nowMs, uint32_t durMs, uint8_t stage) {
    const W3dSpellEffect* ef = table_ ? table_->effect(effect) : nullptr;
    if (!ef) return 0;
    Active a{};
    a.effect = effect;
    a.stage = stage;
    a.slot = slot;
    a.unit = unit;
    a.startMs = nowMs;
    a.endMs = nowMs + durMs;
    a.scale = ef->scale;
    a.impact[0] = a.impact[1] = W3D_SFX_NONE;
    return add(a) ? active_.back().key : 0;
}

int Director::startLoop(const W3dSpellVisual* v, int stage, uint64_t unit, uint32_t nowMs, uint32_t keys[2]) {
    keys[0] = keys[1] = 0;
    if (!v || stage < 0 || stage >= W3D_SFX_STAGES) return 0;
    int n = 0;
    for (int k = 0; k < 2; ++k) {
        if (v->effect[stage][k] == W3D_SFX_NONE) continue;
        uint8_t slot = v->slot[stage][k];
        if (slot == W3D_SLOT_NONE) slot = W3D_SLOT_CHEST;
        keys[k] = startEffect(v->effect[stage][k], unit, slot, nowMs, 0x40000000u, (uint8_t)stage);   // until stopped
        n += keys[k] != 0;
    }
    return n;
}

void Director::stop(uint32_t key, uint32_t nowMs) {
    for (Active& a : active_)
        if (a.key == key && (int32_t)(nowMs - a.endMs) < 0) a.endMs = nowMs;
}

bool Director::running(uint32_t key) const {
    for (const Active& a : active_)
        if (a.key == key) return true;
    return false;
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

// ------------------------------------------------------------------ lasting effects (stage 5.34)
// One per unit and kind: started when the spell appears, stopped when it changes or goes; a loop the director dropped
// (its unit was out of sight for a moment) starts again.
void Lasting::set(Director& d, const Table& t, std::unordered_map<uint64_t, Loop>& m, uint64_t guid, uint32_t spell, int stage,
                  uint32_t nowMs) {
    Loop& l = m[guid];
    l.frame = frame_;
    const bool alive = (l.keys[0] && d.running(l.keys[0])) || (l.keys[1] && d.running(l.keys[1]));
    if (l.spell == spell && alive) return;
    for (uint32_t& k : l.keys) { if (k) d.stop(k, nowMs); k = 0; }
    l.spell = spell;
    if (stage < 0) {   // the loot sparkle: not a spell
        l.keys[0] = d.startEffect(t.loot(), guid, W3D_SLOT_BASE, nowMs, 0x40000000u, W3D_SFX_STATE);
        return;
    }
    const W3dSpellVisual* v = t.visualOf(spell, nullptr);
    if (!v) return;
    int st = stage;
    if (stage == W3D_SFX_CHANNEL && v->effect[W3D_SFX_CHANNEL][0] == W3D_SFX_NONE) st = W3D_SFX_PRECAST;   // no channel kit
    d.startLoop(v, st, guid, nowMs, l.keys);
}

void Lasting::update(Director& d, const Table& t, const std::vector<UnitState>& units, uint32_t nowMs) {
    ++frame_;
    if (!t.loaded()) return;
    struct Cand { float d2; uint64_t guid; uint32_t aura, channel; bool loot; };
    static std::vector<Cand> cand;
    cand.clear();
    for (const UnitState& u : units) {
        if (u.player) {   // the level-up flash: a player whose level rose
            auto it = levels_.find(u.guid);
            if (it != levels_.end() && it->second && u.level > it->second && t.levelUp() != W3D_SFX_NONE &&
                u.dist2 < Director::kRange * Director::kRange) {
                if (d.startEffect(t.levelUp(), u.guid, W3D_SLOT_BASE, nowMs, 3000, W3D_SFX_CAST)) ++levelUps_;
            }
            if (u.level) levels_[u.guid] = u.level;
        }
        if (u.dist2 > kRange * kRange) continue;
        Cand c{u.dist2, u.guid, 0, 0, false};
        if (!u.dead) {
            for (int k = 0; k < u.auraCount && !c.aura; ++k) {   // the first aura that shows something while it lasts
                const W3dSpellVisual* v = u.auras[k] ? t.visualOf(u.auras[k], nullptr) : nullptr;
                if (v && v->effect[W3D_SFX_STATE][0] != W3D_SFX_NONE) c.aura = u.auras[k];
            }
            if (u.channel && t.visualOf(u.channel, nullptr)) c.channel = u.channel;
        } else {
            c.loot = u.lootable && t.loot() != W3D_SFX_NONE;
        }
        if (c.aura || c.channel || c.loot) cand.push_back(c);
    }
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
    size_t na = 0, nl = 0;
    for (const Cand& c : cand) {
        if (c.aura && na < kMaxAuras) { set(d, t, auras_, c.guid, c.aura, W3D_SFX_STATE, nowMs); ++na; }
        if (c.channel) set(d, t, channels_, c.guid, c.channel, W3D_SFX_CHANNEL, nowMs);
        if (c.loot && nl < kMaxLoot) { set(d, t, loot_, c.guid, 1, -1, nowMs); ++nl; }
    }
    for (auto* m : {&auras_, &channels_, &loot_})   // what was not seen this frame: over
        for (auto it = m->begin(); it != m->end();) {
            if (it->second.frame != frame_) {
                for (uint32_t k : it->second.keys) if (k) d.stop(k, nowMs);
                it = m->erase(it);
            } else {
                ++it;
            }
        }
    if (levels_.size() > 512) levels_.clear();
}

}  // namespace sfx
