// spell_view.cpp — see spell_view.hpp.
#include "spell_view.hpp"

#include <math.h>
#include <string.h>

#include <algorithm>

bool SpellView::load() {
    if (table_.loaded()) return true;
    if (tried_) return false;
    tried_ = true;
    const uint32_t size = streamerEntrySize(W3D_ENTRY_SPELLFX, 0);
    if (!size || size > 4u * 1024 * 1024) return false;
    std::vector<uint8_t> buf(size);
    if (!streamerReadEntry(W3D_ENTRY_SPELLFX, 0, buf.data(), size)) return false;
    if (!table_.load(buf.data(), size)) return false;
    dir_.setTable(&table_);
    return true;
}

Model* SpellView::modelFor(uint16_t effect) {
    const W3dSpellEffect* e = table_.effect(effect);
    if (!e) return nullptr;
    if (models_.size() <= effect) models_.resize(effect + 1, nullptr);
    if (!models_[effect]) models_[effect] = streamerAcquireItemModel(e->model);   // read at once (unit priority)
    return models_[effect];
}

size_t SpellView::models() const {
    size_t n = 0;
    for (const Model* m : models_) n += m != nullptr;
    return n;
}

void SpellView::clear() {
    dir_.clear();
    for (Model*& m : models_) {
        if (m) streamerReleaseItemModel(m);
        m = nullptr;
    }
    models_.clear();
    tried_ = false;   // the next pack may have a table (or another one)
}

void SpellView::update(w3n::GameSession& g, uint32_t nowMs, const float cam[3], std::vector<SceneModel>& out) {
    out.clear();
    if (!load()) {
        return;
    }
    // where a point of a unit is: the renderer's skeleton when it drew the unit lately, else the position and a guess
    // by height (hands at 55% to the sides, chest 65%, head 92%)
    auto where = [&](uint64_t guid, int slot, float p[3], float* yaw) -> bool {
        const w3n::WorldObject* o = g.objects.find(guid);
        float base[3], yw = 0;
        if (o && o->self) {
            const w3n::MovementInfo& me = g.mover.info();
            base[0] = me.x; base[1] = me.y; base[2] = me.z;
            yw = me.o;
        } else if (o && o->hasPos) {
            o->positionAt(nowMs, base, &yw);
        } else {
            return false;
        }
        if (yaw) *yaw = yw;
        if (rendererUnitPoint(guid, slot, p)) return true;
        float sc = o->fieldF(w3n::UF::OBJECT_SCALE_X);
        if (!(sc > 0.05f && sc < 20.0f)) sc = 1.0f;
        const float hgt = 2.0f * sc;
        memcpy(p, base, 3 * sizeof(float));
        switch (slot) {
            case W3D_SLOT_HEAD: p[2] += hgt * 0.92f; break;
            case W3D_SLOT_CHEST: p[2] += hgt * 0.65f; break;
            case W3D_SLOT_LHAND: case W3D_SLOT_RHAND: {
                const float side = slot == W3D_SLOT_LHAND ? 1.0f : -1.0f;   // left of the facing: +90 degrees
                p[0] += -sinf(yw) * 0.35f * sc * side + cosf(yw) * 0.2f * sc;
                p[1] += cosf(yw) * 0.35f * sc * side + sinf(yw) * 0.2f * sc;
                p[2] += hgt * 0.55f;
                break;
            }
            default: break;
        }
        return true;
    };
    for (const w3n::GameSession::WorldSound& w : g.worldSounds) {
        sfx::Event e;
        if (w.kind == w3n::GameSession::WorldSound::SpellStart) e.kind = sfx::Event::Start;
        else if (w.kind == w3n::GameSession::WorldSound::SpellGo) e.kind = sfx::Event::Go;
        else if (w.kind == w3n::GameSession::WorldSound::SpellFail) e.kind = sfx::Event::Fail;
        else continue;
        e.caster = w.src;
        e.spell = w.spell;
        e.castMs = w.castMs;
        e.nTargets = std::min<uint8_t>(w.nTargets, 4);
        for (int k = 0; k < e.nTargets; ++k) e.targets[k] = w.targets[k];
        if (e.kind == sfx::Event::Go && !e.nTargets && w.dst) { e.targets[0] = w.dst; e.nTargets = 1; }
        dir_.event(e, nowMs, cam, where);
    }
    dir_.update(nowMs, where, placed_);
    for (const sfx::Placed& p : placed_) {
        Model* m = modelFor(p.effect);
        if (!m) continue;
        SceneModel sm;
        sm.model = m;
        memcpy(sm.pos, p.pos, sizeof sm.pos);
        memcpy(sm.rot, p.rot, sizeof sm.rot);
        sm.scale = p.scale;
        sm.fxKey = p.key;
        sm.emit = p.emitting;
        out.push_back(sm);
    }
}
