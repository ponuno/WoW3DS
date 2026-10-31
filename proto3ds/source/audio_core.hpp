// audio_core.hpp — the platform-free half of the 3DS sound (audio.cpp is the NDSP half): the files w3dconv --sound
// writes (common/w3d_sound.h), the zone of a position, which music / ambience should play, and the decoders of the
// streamed files (MP3 via minimp3, IMA ADPCM). Builds on the PC for tests/sound_test.cpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace snd {

struct Clip { const int16_t* pcm = nullptr; uint32_t samples = 0, rate = 22050; float volume = 1; };

// sfx.w3ds: event sounds, PCM16 mono. alloc lets the 3DS put the samples in linear (DSP-visible) memory.
class SfxBank {
public:
    using Alloc = void* (*)(size_t);
    using Free = void (*)(void*);
    ~SfxBank();
    bool load(const char* path, Alloc a = nullptr, Free f = nullptr);
    bool loaded() const { return !clips_.empty(); }
    // One variant of the event (random), null when the bank has none.
    const Clip* pick(const char* event);
    size_t count() const { return clips_.size(); }
    size_t bytes() const { return size_; }
    // The built-in sounds of the stub (no client sounds converted): short synthesized blips per event.
    bool synthesize(Alloc a = nullptr, Free f = nullptr);
    // 6.39: after load(): the built-in sound for every event the pack does not carry (a name that this client does not have),
    // so an event is never silent. Returns how many were added.
    size_t synthesizeMissing(Alloc a = nullptr, Free f = nullptr);
private:
    bool synthInto(Alloc a, Free f, bool missingOnly, size_t* added);
    void release();
    std::unordered_map<uint32_t, std::vector<Clip>> clips_;
    uint8_t* blob_ = nullptr;
    size_t size_ = 0;
    Free free_ = nullptr;
    uint8_t* extra_ = nullptr;   // the built-in sounds that fill the holes of a loaded pack
    Free extraFree_ = nullptr;
    std::minstd_rand rng_{12345};
};

struct SoundSet { float volume = 1; std::vector<std::string> files; };   // a SoundEntries row: variants
struct ZoneSound {
    uint32_t music = 0, ambience = 0;            // ZoneMusic / SoundAmbience ids (0 none)
    uint32_t silMinMs = 0, silMaxMs = 0;         // pause between tracks
    const SoundSet* day = nullptr; const SoundSet* night = nullptr;
    const SoundSet* ambDay = nullptr; const SoundSet* ambNight = nullptr;
};

// music.txt + areas_<map>.bin
class MusicTable {
public:
    bool load(const char* dir);                  // the sound directory (music.txt inside)
    bool loaded() const { return !areas_.empty(); }
    // Area id of a world position (0 when unknown); the grid of the map is read on first use.
    uint32_t areaAt(uint32_t map, float x, float y);
    // Music and ambience of an area (its parent's when it has none of its own).
    ZoneSound zone(uint32_t area) const;
    std::string path(const std::string& file) const { return dir_ + "/music/" + file; }
    // The login / character screens' music ("G" in music.txt, stage 5.31); null when the converter found none.
    const SoundSet* glue() const { return glue_.files.empty() ? nullptr : &glue_; }
private:
    struct Area { uint32_t parent = 0, music = 0, ambience = 0; };
    struct Zm { uint32_t silMin = 0, silMax = 0, day = 0, night = 0; };
    struct Grid { bool tried = false; std::vector<uint16_t> slots, areas; };
    std::string dir_;
    std::unordered_map<uint32_t, Area> areas_;
    std::unordered_map<uint32_t, Zm> zm_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> amb_;
    std::unordered_map<uint32_t, SoundSet> sounds_;
    std::unordered_map<uint32_t, uint32_t> mapArea_;
    std::unordered_map<uint32_t, Grid> grids_;
    SoundSet glue_;
};

// What should play: zone music with pauses between tracks, ambience looping.
class Director {
public:
    explicit Director(uint32_t seed = 1) : rng_(seed) {}
    // Every frame (cheap): the current zone and whether it is night.
    void update(const MusicTable& t, uint32_t area, bool night, uint32_t nowMs);
    const std::string& music() const { return music_; }       // file to play now ("" = silence)
    const std::string& ambience() const { return ambience_; }
    float musicVolume() const { return musicVol_; }
    float ambienceVolume() const { return ambVol_; }
    void musicEnded(uint32_t nowMs);                           // the stream ran out: pause, then another track
    uint32_t area() const { return area_; }
private:
    std::string pickFrom(const MusicTable& t, const SoundSet* s);
    std::minstd_rand rng_;
    uint32_t area_ = ~0u, musicId_ = ~0u, ambId_ = ~0u;
    bool night_ = false;
    ZoneSound zone_;
    std::string music_, ambience_;
    float musicVol_ = 1, ambVol_ = 1;
    uint32_t silenceUntil_ = 0;
    bool pendingTrack_ = false;
    uint32_t cand_ = ~0u, candMs_ = 0;   // a new area must hold for 1 s before the music follows it
    std::string lastTrack_;               // not repeated at once when the set has another file
};

// world.w3ds: positioned sounds by SoundEntries id, IMA ADPCM, decoded on demand (only the index stays in memory).
struct WorldClip { uint32_t offset = 0, samples = 0, rate = 22050; float volume = 1, minDist = 8, cutoff = 40; };
class WorldBank {
public:
    ~WorldBank();
    bool load(const char* path);
    bool loaded() const { return f_ != nullptr; }
    int variants(uint32_t id) const;
    const WorldClip* clip(uint32_t id, int k) const;
    // Decodes variant k into out (clip->samples samples); false on a read error.
    bool decode(const WorldClip& c, int16_t* out);
    size_t count() const { return clips_.size(); }
private:
    FILE* f_ = nullptr;
    std::unordered_map<uint32_t, std::vector<WorldClip>> clips_;
};

// sounds.txt: which SoundEntries id a creature / weapon / spell / footstep makes.
struct CreatureSounds { uint32_t aggro = 0, death = 0, wound = 0, woundCrit = 0, attack = 0, attackCrit = 0, footstep = 0; };
struct SpellSounds { uint32_t precast = 0, cast = 0, impact = 0, missile = 0, channel = 0; };
struct NpcVoice { uint32_t greeting = 0, farewell = 0, pissed = 0; };   // UI stage 8 (NPCSounds.dbc / the race's voices)
class SoundTables {
public:
    bool load(const char* path);
    bool loaded() const { return loaded_; }
    const CreatureSounds* creature(uint32_t displayId) const;
    std::pair<uint32_t, uint32_t> weapon(uint32_t subclass) const;   // impact, crit (0 when unknown)
    uint32_t footstep(uint32_t footstepId) const;                    // footstep id 0: the generic one
    const SpellSounds* spell(uint32_t spellId) const;
    uint32_t liquid(const char* kind) const;                         // stage 5.26: "splash", "swim", "wade", "fishbite" (0: none)
    // UI stage 8: voices. An NPC's greeting / farewell / pissed lines by its display (nullptr: it has no voice); our
    // character's error line by race, gender (0 male, 1 female) and token ("nomana" ...); a text emote's voice. 0: none.
    const NpcVoice* npcVoice(uint32_t displayId) const;
    uint32_t errorVoice(uint32_t race, uint32_t gender, const char* token) const;
    uint32_t emoteVoice(uint32_t textEmote, uint32_t race, uint32_t gender) const;
    size_t voiceCount() const { return npcVoice_.size() + errorVoice_.size() + emoteVoice_.size(); }
private:
    bool loaded_ = false;
    std::unordered_map<uint32_t, NpcVoice> npcVoice_;
    std::unordered_map<std::string, uint32_t> errorVoice_;    // "race gender token"
    std::unordered_map<uint64_t, uint32_t> emoteVoice_;       // textEmote << 16 | race << 8 | gender
    std::unordered_map<std::string, uint32_t> liquid_;
    std::unordered_map<uint32_t, uint32_t> display_, footstep_, spellVisual_;
    std::unordered_map<uint32_t, CreatureSounds> csd_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> weapon_;
    std::unordered_map<uint32_t, SpellSounds> visual_;
};

// 3D: gains for a sound at (x, y) heard at the listener (lx, ly) facing 'yaw' (radians, world X axis = 0):
// distance falloff between minDist and cutoff, constant-power pan. false when out of range.
bool spatialize(float x, float y, float lx, float ly, float yaw, float minDist, float cutoff, float* left, float* right);

// A streamed file: MP3 (any rate, mono / stereo) or IMA ADPCM mono (.ima). read() gives interleaved PCM16.
class Stream {
public:
    Stream();
    ~Stream();
    bool open(const std::string& path);
    void close();
    bool isOpen() const { return f_ != nullptr; }
    int channels() const { return channels_; }
    uint32_t rate() const { return rate_; }
    // Up to 'frames' frames into out (frames * channels() samples); 0 at the end of the file.
    int read(int16_t* out, int frames);
    bool rewind();
    // How far the stream got (frames returned since open) and why read() last returned 0: 0 not yet, 1 end of file,
    // 2 the decoder gave up (stage 5.43: used by the music log).
    uint64_t framesRead() const { return framesRead_; }
    int endReason() const { return endReason_; }
private:
    bool fillMp3();
    FILE* f_ = nullptr;
    bool mp3_ = false;
    int channels_ = 1;
    uint32_t rate_ = 22050;
    // mp3
    struct Mp3;
    std::unique_ptr<Mp3> m_;
    // ima
    uint32_t imaLeft_ = 0;
    std::vector<int16_t> pcm_;        // decoded, not yet returned (interleaved)
    size_t pcmPos_ = 0;
    std::string path_;
    uint64_t framesRead_ = 0;
    int endReason_ = 0;
};

}  // namespace snd
