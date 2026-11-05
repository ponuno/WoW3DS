// cinematic.hpp — 7.04: intro cinematics (SMSG_TRIGGER_CINEMATIC). The flight of the camera, baked by w3dconv --sound into
// sound/cinematics.txt (CinematicSequences -> CinematicCamera -> the M2 camera's position / target tracks, in world
// coordinates every 100 ms), and the narration files (sound/music/). Platform-free: tests/cinematic_test.cpp reads it on the PC.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cine {

struct Sound { uint32_t startMs = 0; std::string file; int volume = 100; };
struct Key { uint32_t t = 0; float pos[3] = {0, 0, 0}, target[3] = {0, 0, 0}; };

struct Sequence {
    uint32_t id = 0, totalMs = 0;
    std::vector<Sound> sounds;   // by startMs
    std::vector<Key> keys;       // by t
    // Where the camera is and what it looks at, t ms into the flight (clamped to the ends).
    void at(uint32_t t, float pos[3], float target[3]) const;
};

class Table {
public:
    bool load(const std::string& path);   // false: no file (the intro is then answered at once, as before 7.04)
    bool loaded() const { return !seqs_.empty(); }
    const Sequence* find(uint32_t id) const;
    size_t count() const { return seqs_.size(); }
private:
    std::unordered_map<uint32_t, Sequence> seqs_;
};

// The camera looking from pos to target -> yaw (around z, from +x) and pitch (up positive), radians.
void lookAngles(const float pos[3], const float target[3], float* yaw, float* pitch);

}  // namespace cine
