// 6.43: vanilla key ranges are [first, last] inclusive. WoWee's loader reads them as [start, end): a held pose (one key) vanishes and the
// last key of each sequence is lost. fixVanillaBoneTracks (m2_fix.cpp) reads them again.   m2_ranges_test <RangeTest.m2>
#include "conv.h"

#include "pipeline/m2_loader.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>

static int bad = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++bad; } }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    wowee::pipeline::M2Model w = wowee::pipeline::M2Loader::load(b);
    check(w.bones.size() == 1 && w.sequences.size() == 3, "the model loads (1 bone, 3 sequences)");
    if (w.bones.size() != 1 || w.sequences.size() != 3) { std::printf("M2 RANGES: FAIL\n"); return 1; }
    const auto& wr = w.bones[0].rotation;
    const size_t wowKneel = wr.sequences.size() > 1 ? wr.sequences[1].quatValues.size() : 0;
    std::printf("WoWee's reading: Stand %zu rotation keys, KneelLoop %zu, Walk translation %zu\n", wr.sequences.empty() ? 0 : wr.sequences[0].quatValues.size(),
                wowKneel, w.bones[0].translation.sequences.size() > 2 ? w.bones[0].translation.sequences[2].vec3Values.size() : 0);

    wowee::pipeline::M2Model m = w3d::loadM2(b);
    const auto& r = m.bones[0].rotation;
    const auto& t = m.bones[0].translation;
    check(r.sequences.size() == 3 && t.sequences.size() == 3, "a key list per sequence");
    check(r.sequences[0].quatValues.size() == 2 && r.sequences[0].timestamps.size() == 2 && r.sequences[0].timestamps[1] == 1000,
          "Stand keeps both keys (0 and 1000 ms)");
    check(r.sequences[1].quatValues.size() == 1 && std::fabs(r.sequences[1].quatValues[0].z - 0.70710678f) < 1e-4f,
          "KneelLoop keeps its single key: the held pose (90 degrees)");
    check(t.sequences[1].vec3Values.size() == 1 && std::fabs(t.sequences[1].vec3Values[0].z + 0.5f) < 1e-5f, "the held pose's translation");
    check(t.sequences[2].vec3Values.size() == 3 && t.sequences[2].timestamps[0] == 100 && t.sequences[2].timestamps[2] == 1000,
          "Walk: three keys, times from the sequence start (100, .., 1000 ms)");
    check(wowKneel == 0, "(WoWee alone drops the held pose - the reason for the fix)");
    std::printf("M2 RANGES: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
