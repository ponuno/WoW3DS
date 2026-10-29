// sound_world.hpp — what the world sounds like this frame: the session's melee and spell events, deaths and aggro of
// the units around, our own footsteps -> positioned sound requests (SoundEntries ids of world.w3ds, sounds.txt).
// Platform-free (tests/sound_world_test.cpp replays sessions through it); audio.cpp plays the requests.
#pragma once
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "audio_core.hpp"
#include "game.hpp"

struct SoundReq {
    uint32_t sound = 0;          // SoundEntries id (world.w3ds)
    float x = 0, y = 0, z = 0;
    bool self = false;           // ours: centred, full volume
    uint64_t tag = 0;            // precast loops: stopped by a request with stop = true and the same tag
    bool stop = false;
    uint32_t loopMs = 0;         // with a tag: repeat the clip while it is shorter than this (a cast's length) or until stopped
    float gain = 1.0f;           // volume factor (own footsteps are quiet)
    const char* what = "";       // for the tests / the debug page: "impact", "wound", "death", "aggro", "step", ...
};

class SoundWorld {
public:
    bool load(const char* soundsTxt) { return tables_.load(soundsTxt); }
    bool loaded() const { return tables_.loaded(); }
    snd::SoundTables& tables() { return tables_; }
    // Every frame while in the world. pos = our character; moving / onGround from the movement code; water (stage
    // 5.26): 0 dry, 1 wading (feet under a surface), 2 swimming - strokes instead of steps, splashing steps.
    void update(w3n::GameSession& g, const float pos[3], bool moving, bool onGround, bool walking, uint32_t nowMs,
                std::vector<SoundReq>& out, int water = 0);
    // Into deep water (stage 5.26): a splash, louder the faster we came down (speed in yards per second).
    void splash(const float pos[3], float speed, uint32_t nowMs, std::vector<SoundReq>& out);
    void reset();
private:
    void at(const w3n::GameSession& g, uint64_t guid, uint32_t sound, const char* what, std::vector<SoundReq>& out, uint64_t tag = 0, uint32_t loopMs = 0);
    const snd::CreatureSounds* soundsOf(const w3n::WorldObject& o) const;
    uint32_t weaponSubclass(w3n::GameSession& g, const w3n::WorldObject& o) const;   // ~0u: no weapon
    snd::SoundTables tables_;
    std::minstd_rand rng_{99};
    struct UnitState { bool dead = false; uint64_t target = 0; uint32_t aggroMs = 0; uint32_t seenMs = 0; };
    std::unordered_map<uint64_t, UnitState> units_;
    float stepDist_ = 0, lastPos_[3] = {0, 0, 0};
    uint32_t strokeMs_ = 0, biteMs_ = 0;
    bool havePos_ = false;
    uint32_t now_ = 0;
};
