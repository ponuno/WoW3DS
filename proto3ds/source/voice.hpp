// voice.hpp — voices of the characters (UI stage 8), like the original client:
//   * an NPC says its greeting when we select it or open its window (gossip, vendor, trainer, quests, bank, flight
//     master, auctioneer, innkeeper), its farewell when that window closes, and something annoyed when we keep clicking
//     it (NPCSounds.dbc through CreatureDisplayInfo; humanoids without a set of their own use their race's voice);
//   * our character says what went wrong in the voice of our race and gender ("Not enough rage", "Out of range",
//     "I can't carry any more"...: GameSession::vocals, the files <Race><Gender>_err_<token>NN.wav);
//   * a text emote (/cheer, /laugh, /hello ...) is heard in the voice of who did it (EmotesTextSound.dbc).
// Platform-free: the requests go out as SoundReq (positioned world.w3ds sounds, sounds.txt lines N / V / T), so the PC
// tests replay sessions through it (tests/voice_test.cpp).
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "audio_core.hpp"
#include "game.hpp"
#include "sound_world.hpp"

class VoiceDirector {
public:
    // Every frame while in the world, after GameSession::update. Drains g.vocals and g.heardEmotes.
    void update(w3n::GameSession& g, const snd::SoundTables& t, uint32_t nowMs, std::vector<SoundReq>& out);
    void reset();

    struct Stats {
        uint32_t greetings = 0, farewells = 0, pissed = 0, errors = 0, emotes = 0;
        uint32_t silent = 0;          // a line was wanted but the tables have no sound for it (race, display ...)
    } stats;
    // Race (1 human ... 8 troll) and gender (0 male, 1 female) of a unit: UNIT_FIELD_BYTES_0 bytes 0 and 2.
    static void raceGender(const w3n::WorldObject& o, uint32_t* race, uint32_t* gender);

    static constexpr uint32_t kGreetCooldownMs = 2000;    // the same NPC again (WoWee: 2 s)
    static constexpr uint32_t kPissedClicks = 5;          // selections of one NPC within kClickWindowMs ...
    static constexpr uint32_t kClickWindowMs = 15000;
    static constexpr uint32_t kErrorGapMs = 900;          // our error lines: not on top of each other ...
    static constexpr uint32_t kSameErrorMs = 2500;        // ... and the same one not again at once (a key held on cooldown)

private:
    void npcLine(w3n::GameSession& g, const snd::SoundTables& t, const w3n::WorldObject& o, int kind, uint32_t nowMs,
                 std::vector<SoundReq>& out);   // kind 0 greeting, 1 farewell, 2 pissed
    struct Npc { uint32_t lastMs = 0, firstClickMs = 0, clicks = 0; };
    std::unordered_map<uint64_t, Npc> npcs_;
    uint64_t target_ = 0, window_ = 0;
    uint32_t errorMs_ = 0;
    w3n::Vocal lastError_ = w3n::Vocal::None;
    bool started_ = false;
};
