// spell_view.hpp — spell effects in the world (stage 5.33): the session's spell events into sfx::Director (spellfx.cpp),
// the effect models from the pack (MODL entries named by the SVFX table, acquired once and kept), the units' points from
// the renderer (rendererUnitPoint) or, for a unit not drawn, from its position and size. Out: SceneModels for
// rendererDraw's effects list (mesh + particles + glows).
#pragma once
#include <unordered_map>
#include <vector>

#include "game.hpp"
#include "spellfx.hpp"
#include "world.h"

class SpellView {
public:
    // The SVFX table of the pack (pool); false when it has none (an older conversion: no spell effects).
    bool load();
    bool loaded() const { return table_.loaded(); }
    // Every frame in the world, before the sounds take the events (SoundWorld::update clears them).
    void update(w3n::GameSession& g, uint32_t nowMs, const float cam[3], std::vector<SceneModel>& out);
    void clear();                 // map change / logout: every effect and model let go
    const sfx::Stats& stats() const { return dir_.stats(); }
    size_t active() const { return dir_.active(); }
    size_t models() const;
    const sfx::Lasting& lasting() const { return lasting_; }
    // 5.35b, for report.txt: the last spell events and what became of them
    struct EvLog { uint32_t spell = 0; uint8_t kind = 0, result = 0, targets = 0; bool self = false; float dist = -1; bool hasVisual = false; };
    static constexpr int kLog = 16;
    const EvLog* log(int i) const { return i < logN_ ? &log_[(logAt_ + kLog - logN_ + i) % kLog] : nullptr; }
    int logCount() const { return logN_; }

private:
    Model* modelFor(uint16_t effect);
    sfx::Lasting lasting_;        // stage 5.34: auras, channels, loot sparkles, level-up
    std::vector<sfx::UnitState> units_;
    sfx::Table table_;
    sfx::Director dir_;
    std::vector<Model*> models_;  // by effect index (null = not acquired yet)
    std::vector<sfx::Placed> placed_;
    bool tried_ = false;
    EvLog log_[kLog];
    int logAt_ = 0, logN_ = 0;
};
