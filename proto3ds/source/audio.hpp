// audio.hpp — sound on the 3DS (NDSP): event sounds, zone music, ambience. The files come from w3dconv --sound
// (sdmc:/3ds/w3dproto/sound/); without them the built-in synthesized blips play for the interface events.
// NDSP needs the DSP firmware (sdmc:/3ds/dspfirm.cdc, dumped by DSP1); without it the game stays silent.
#pragma once
#include <cstdint>

namespace audio {

bool init(const char* soundDir);                    // false: no DSP firmware (silent)
void shutdown();
void play(const char* event);                       // "click", "open", "coin", "levelup", ... (audio_core.cpp)
void setVolumes(int music, int sfx, int ambience);  // 0..10 each
// Where the player is (the zone picks music and ambience); inWorld false fades both out (login, loading).
void setPlace(uint32_t map, float x, float y, bool inWorld);
// World sounds (world.w3ds, by SoundEntries id): positioned against the listener (our character, the camera's
// facing). self: our own sound, centred. tag: a precast loop that stopTag(tag) ends early.
void setListener(float x, float y, float z, float yaw);
void playAt(uint32_t sound, float x, float y, float z, bool self, uint64_t tag = 0, float gain = 1.0f,
            uint32_t loopMs = 0);
void stopTag(uint64_t tag);
const char* soundsTxt();                            // sounds.txt next to world.w3ds (for SoundWorld::load)
const char* status();                               // one line for the debug page
uint32_t area();                                    // the area id the player is in (0 unknown)

// Stage 6 (perf log): where the sound's memory and time go.
struct Stats {
    bool on = false;
    int core = -1;                                  // the sound thread's core (-1: none)
    uint32_t sfxBytes = 0, streamBytes = 0;         // event sounds, the music / ambience wave buffers (linear)
    uint32_t cacheBytes = 0, cacheClips = 0;        // decoded world sounds (linear, LRU up to 3 MB)
    uint32_t cacheMisses = 0, cacheFails = 0;       // decodes (cumulative), decodes that found no memory / no room
    uint32_t worldPlaying = 0;                      // world-sound channels playing now (of 16)
    uint32_t busyUs = 0;                            // the sound thread's time in its loop (cumulative, wraps)
    uint32_t requests = 0, requestsDropped = 0;     // sound requests (cumulative), dropped because the queue was full
};
void stats(Stats* out);

}  // namespace audio
