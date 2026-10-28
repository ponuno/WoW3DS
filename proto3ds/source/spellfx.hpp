// spellfx.hpp — what spells look like (stage 5.33), without any 3DS API (tests/spellfx_test.cpp runs it on the PC).
//
// The SVFX table (common/w3d_fx.h) says which effect models a spell shows at each stage; the session's spell events
// (SMSG_SPELL_START / GO / FAILURE, the same ones the sounds use) start them:
//   precast  on the caster while it casts (until the spell goes off or fails, at most the cast time + a little);
//   cast     on the caster when it goes off (1.2 s);
//   missile  from the caster's hand to each target at the spell's speed (20 yd/s when the table has none), homing;
//   impact   on each target when the missile arrives, or at once when the spell has no missile (1.5 s).
// Effects follow their unit (a point on it: hands, chest, head, feet). After an effect ends its model is kept a moment
// longer (linger) so the particles it threw fade out instead of vanishing with the model.
// Budget: at most kMaxActive effects; casters farther than kRange from the camera are not shown.
#pragma once
#include <stdint.h>

#include <functional>
#include <vector>

#include "w3d_fx.h"

namespace sfx {

struct Table {
    bool load(const uint8_t* blob, uint32_t size);   // keeps a copy
    bool loaded() const { return h_ != nullptr; }
    // The visual of a spell and its missile speed (yd/s, 0 = unknown); null when the spell shows nothing.
    const W3dSpellVisual* visualOf(uint32_t spell, float* speed) const;
    const W3dSpellEffect* effect(uint16_t index) const;
    uint32_t spellCount() const { return h_ ? h_->spellCount : 0; }
private:
    std::vector<uint8_t> data_;
    const W3dSpellFxHeader* h_ = nullptr;
};

struct Event {
    enum Kind : uint8_t { Start, Go, Fail };
    Kind kind = Start;
    uint64_t caster = 0;
    uint32_t spell = 0;
    uint32_t castMs = 0;
    uint64_t targets[4] = {0, 0, 0, 0};
    uint8_t nTargets = 0;
};

// Where a point of a unit is now: false when the unit is not known (gone, too far). yaw = its facing.
using Resolver = std::function<bool(uint64_t guid, int slot, float out[3], float* yaw)>;

struct Placed {                   // one effect model to draw / emit this frame
    uint32_t key = 0;             // stable while the effect lives (the particle system's emitter state)
    uint16_t effect = 0xFFFF;     // index into the table's effects
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};
    float scale = 1;
    bool emitting = true;         // false: lingering (mesh hidden, its particles fading)
    uint8_t stage = 0;
};

struct Stats { uint32_t started = 0, missiles = 0, impacts = 0, skippedFar = 0, skippedFull = 0, active = 0; };

class Director {
public:
    static constexpr int kMaxActive = 32;
    static constexpr float kRange = 80.0f;
    static constexpr uint32_t kLingerMs = 1500;
    void setTable(const Table* t) { table_ = t; }
    void event(const Event& e, uint32_t nowMs, const float cam[3], const Resolver& where);
    // Every frame: moves missiles, ends what is over, fills out (effects to show and keep alive).
    void update(uint32_t nowMs, const Resolver& where, std::vector<Placed>& out);
    void clear() { active_.clear(); }
    const Stats& stats() const { return stats_; }
    size_t active() const { return active_.size(); }

private:
    struct Active {
        uint32_t key;
        uint16_t effect;
        uint8_t stage, slot;
        uint64_t unit, target;    // unit: where it hangs (missile: the caster); target: missile's goal
        uint32_t spell;
        uint32_t startMs, endMs;  // endMs: emitting until then, lingering kLingerMs more
        float pos[3];             // missile: where it is
        float speed;
        uint16_t impact[2];       // missile: what to start on arrival
        uint8_t impactSlot[2];
        float scale;
        bool arrived;
    };
    bool add(Active a);
    void startStage(const W3dSpellVisual* v, int stage, uint64_t unit, uint32_t spell, uint32_t nowMs, uint32_t durMs);
    const Table* table_ = nullptr;
    std::vector<Active> active_;
    uint32_t nextKey_ = 1;
    Stats stats_;
};

}  // namespace sfx
