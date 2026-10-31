// m2_fix.cpp — 6.43: the bone tracks of 1.12 / 2.x M2 files read again with their key ranges as the format has them.
//
// A vanilla (and TBC) track keeps every sequence's keys in one flat array; ranges[i] = {first, last} gives the keys of sequence i,
// BOTH INCLUSIVE (WoW Model Viewer, animated.h: "if (range.first != range.second) ... else return data[range.first]"). WoWee's
// parseAnimTrackVanilla takes them as [start, end): every sequence lost its last key, and a sequence with a single key per bone
// (first == last: a held pose - wind riders and gryphons resting at the flight master, KneelLoop / sleeping / sitting poses) lost all
// of them, so the bone stayed in its bind pose and the model stood in the T-pose. WoWee also shifts each sequence's keys by its first
// key's time instead of the sequence's start on the timeline.
// fixVanillaBoneTracks rebuilds the bones' translation / rotation / scale from the file with inclusive ranges and the sequence start;
// tracks without ranges (global sequences) are left as WoWee read them.
#include "conv.h"

#include "pipeline/m2_loader.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstring>

namespace w3d {

namespace {

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size()) return 0;
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}

enum class Kind { Vec3, Quat, QuatCompressed };

// One track at file offset 'off' (28 bytes: interp u16, globalSeq i16, ranges, timestamps, keys - each count + offset).
bool rebuildTrack(const std::vector<uint8_t>& d, size_t off, Kind kind, const std::vector<uint32_t>& seqStart,
                  wowee::pipeline::M2AnimationTrack& t, size_t* fixed) {
    if (off + 28 > d.size()) return false;
    int16_t globalSeq;
    std::memcpy(&globalSeq, d.data() + off + 2, 2);
    const uint32_t nRanges = rd32(d, off + 4), ofsRanges = rd32(d, off + 8);
    const uint32_t nTs = rd32(d, off + 12), ofsTs = rd32(d, off + 16), nKeys = rd32(d, off + 20), ofsKeys = rd32(d, off + 24);
    if (globalSeq >= 0 || !nRanges || !nTs || !nKeys || nRanges > 4096 || nTs > 100000 || nKeys > 100000) return false;
    const size_t keySize = kind == Kind::Vec3 ? 12 : kind == Kind::Quat ? 16 : 8;
    if ((uint64_t)ofsRanges + nRanges * 8ull > d.size() || (uint64_t)ofsTs + nTs * 4ull > d.size() ||
        (uint64_t)ofsKeys + nKeys * (uint64_t)keySize > d.size())
        return false;
    const uint32_t n = std::min(nTs, nKeys);
    std::vector<wowee::pipeline::M2AnimationTrack::SequenceKeys> seqs(nRanges);
    for (uint32_t i = 0; i < nRanges; ++i) {
        uint32_t first = rd32(d, ofsRanges + i * 8), last = rd32(d, ofsRanges + i * 8 + 4);
        if (first > last || first >= n) continue;   // (no keys for this sequence)
        last = std::min(last, n - 1);
        auto& s = seqs[i];
        const uint32_t base = i < seqStart.size() && seqStart[i] <= rd32(d, ofsTs + first * 4) ? seqStart[i] : rd32(d, ofsTs + first * 4);
        for (uint32_t k = first; k <= last; ++k) {
            s.timestamps.push_back(rd32(d, ofsTs + k * 4) - base);
            const uint8_t* p = d.data() + ofsKeys + k * keySize;
            if (kind == Kind::Vec3) {
                float v[3];
                std::memcpy(v, p, 12);
                s.vec3Values.emplace_back(v[0], v[1], v[2]);
            } else {
                glm::quat q;
                if (kind == Kind::Quat) {   // float x, y, z, w
                    float v[4];
                    std::memcpy(v, p, 16);
                    q = glm::quat(v[3], v[0], v[1], v[2]);
                } else {                    // TBC: int16 x, y, z, w (WoWee's decoding)
                    int16_t c[4];
                    std::memcpy(c, p, 8);
                    auto f = [](int16_t x) { return x < 0 ? (x + 32768) / 32767.0f : (x - 32767) / 32767.0f; };
                    q = glm::quat(f(c[3]), f(c[0]), f(c[1]), f(c[2]));
                }
                const float len = glm::length(q);
                s.quatValues.push_back(len > 0.001f ? q / len : glm::quat(1, 0, 0, 0));
            }
        }
        // what WoWee dropped: a held pose (one key) or the closing key
        const size_t had = i < t.sequences.size() ? t.sequences[i].timestamps.size() : 0;
        if (s.timestamps.size() != had) ++*fixed;
    }
    t.sequences = std::move(seqs);
    return true;
}

}  // namespace

size_t fixVanillaBoneTracks(const std::vector<uint8_t>& d, wowee::pipeline::M2Model& m) {
    if (d.size() < 0x40 || std::memcmp(d.data(), "MD20", 4) != 0) return 0;
    const uint32_t version = rd32(d, 4);
    if (version >= 264) return 0;   // WotLK: per-sequence arrays, no ranges
    const bool tbc = version >= 260;
    const uint32_t nAnim = rd32(d, 0x1C), ofsAnim = rd32(d, 0x20), nBones = rd32(d, 0x34), ofsBones = rd32(d, 0x38);
    if (nBones != m.bones.size() || nAnim > 4096 || (uint64_t)ofsAnim + nAnim * 68ull > d.size()) return 0;
    const size_t boneSize = tbc ? 112 : 108, trackBase = tbc ? 16 : 12;
    if ((uint64_t)ofsBones + nBones * (uint64_t)boneSize > d.size()) return 0;
    std::vector<uint32_t> seqStart(nAnim);
    for (uint32_t i = 0; i < nAnim; ++i) seqStart[i] = rd32(d, ofsAnim + i * 68 + 4);
    size_t fixed = 0;
    for (uint32_t b = 0; b < nBones; ++b) {
        const size_t bo = ofsBones + b * boneSize + trackBase;
        auto& bone = m.bones[b];
        rebuildTrack(d, bo, Kind::Vec3, seqStart, bone.translation, &fixed);
        rebuildTrack(d, bo + 28, tbc ? Kind::QuatCompressed : Kind::Quat, seqStart, bone.rotation, &fixed);
        rebuildTrack(d, bo + 56, Kind::Vec3, seqStart, bone.scale, &fixed);
    }
    return fixed;
}

wowee::pipeline::M2Model loadM2(const std::vector<uint8_t>& bytes) {
    wowee::pipeline::M2Model m = wowee::pipeline::M2Loader::load(bytes);
    g_m2TracksFixed += fixVanillaBoneTracks(bytes, m);
    return m;
}
std::atomic<size_t> g_m2TracksFixed{0};

}  // namespace w3d
