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
#include <unordered_map>
#include <vector>

#include "w3d_fx.h"

namespace sfx {

struct Table {
    bool load(const uint8_t* blob, uint32_t size);   // keeps a copy
    bool loaded() const { return h_ != nullptr; }
    // The visual of a spell and its missile speed (yd/s, 0 = unknown); null when the spell shows nothing.
    const W3dSpellVisual* visualOf(uint32_t spell, float* speed) const;
    const W3dSpellEffect* effect(uint16_t index) const;
    uint16_t levelUp() const { return h_ ? h_->levelUp : W3D_SFX_NONE; }   // stage 5.34: not spells
    uint16_t loot() const { return h_ ? h_->loot : W3D_SFX_NONE; }
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
    uint32_t ageMs = 0;           // stage 5.35: since it started (the bone animation of its model)
};

struct Stats { uint32_t started = 0, missiles = 0, impacts = 0, skippedFar = 0, skippedFull = 0, active = 0;
               uint32_t events = 0, noVisual = 0, noCaster = 0; };   // 5.35b: what became of the session's events
// What event() did with one event (report.txt shows the last ones).
enum EventResult : uint8_t { kEvNoTable, kEvNoVisual, kEvFail, kEvNoCaster, kEvFar, kEvStarted, kEvNothing };

class Director {
public:
    static constexpr int kMaxActive = 40;
    static constexpr float kRange = 80.0f;
    static constexpr uint32_t kLingerMs = 1500;
    void setTable(const Table* t) { table_ = t; }
    EventResult event(const Event& e, uint32_t nowMs, const float cam[3], const Resolver& where, float* distOut = nullptr);
    // Every frame: moves missiles, ends what is over, fills out (effects to show and keep alive).
    void update(uint32_t nowMs, const Resolver& where, std::vector<Placed>& out);
    void clear() { active_.clear(); }
    // Stage 5.34, effects that last as long as something on the unit does (an aura, a channel, a lootable corpse): the
    // stage's effects of the visual on the unit until stop(); keys[] gets their keys (0 = not started). Returns how many.
    int startLoop(const W3dSpellVisual* v, int stage, uint64_t unit, uint32_t nowMs, uint32_t keys[2], int tagStage = -1);   // tagStage: what it counts as
    uint32_t startEffect(uint16_t effect, uint64_t unit, uint8_t slot, uint32_t nowMs, uint32_t durMs, uint8_t stage);
    void stop(uint32_t key, uint32_t nowMs);         // ends it (it lingers, then goes)
    bool running(uint32_t key) const;                // started and not stopped / dropped
    const Stats& stats() const { return stats_; }
    size_t active() const { return active_.size(); }

private:
    struct Active {
        uint32_t key;
        uint16_t effect;
        uint8_t stage, slot;
        uint64_t unit, target;    // unit: where it hangs (missile: the caster); target: missile's goal
        uint32_t spell;
        uint32_t bornMs = 0;      // when it started (startMs of a missile is "last moved")
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

// Stage 5.34: what lasts on units - the state kit of an aura (the first of the unit's auras that has one), a channel
// (its channel kit, else its precast kit), the sparkle of a corpse we may loot - and the level-up flash of a player
// whose level rose. Fed every frame with the units near the camera (nearest first is not needed: it sorts).
struct UnitState {
    uint64_t guid = 0;
    float dist2 = 0;              // squared distance from the camera
    bool player = false, dead = false, lootable = false;
    uint32_t level = 0;
    uint32_t channel = 0;         // UNIT_CHANNEL_SPELL
    const uint32_t* auras = nullptr;   // UNIT_FIELD_AURA[0 .. auraCount)
    int auraCount = 0;
};
class Lasting {
public:
    static constexpr float kRange = 40.0f;
    static constexpr size_t kMaxAuras = 12, kMaxLoot = 6;
    void update(Director& d, const Table& t, const std::vector<UnitState>& units, uint32_t nowMs);
    void clear() { auras_.clear(); channels_.clear(); loot_.clear(); levels_.clear(); }
    size_t auras() const { return auras_.size(); }
    size_t channels() const { return channels_.size(); }
    size_t loot() const { return loot_.size(); }
    uint32_t levelUps() const { return levelUps_; }
private:
    struct Loop { uint32_t spell = 0; uint32_t keys[2] = {0, 0}; uint32_t frame = 0; };
    void set(Director& d, const Table& t, std::unordered_map<uint64_t, Loop>& m, uint64_t guid, uint32_t spell, int stage,
             uint32_t nowMs);
    std::unordered_map<uint64_t, Loop> auras_, channels_, loot_;
    std::unordered_map<uint64_t, uint32_t> levels_;
    uint32_t frame_ = 0, levelUps_ = 0;
};

}  // namespace sfx
