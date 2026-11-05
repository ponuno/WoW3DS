// cinematic_test.cpp — 7.04: cinematics.txt of the synthetic client (run_sound_tests.sh) read by the 3DS side.
//   cinematic_test <sound dir>
#include <cmath>
#include <cstdio>
#include <string>

#include "cinematic.hpp"

static int fails = 0, checks = 0;
#define CHECK(c, what) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s\n", what); } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 0.02f; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    cine::Table t;
    CHECK(t.load(std::string(argv[1]) + "/cinematics.txt"), "loads");
    CHECK(t.count() == 1 && !t.find(8), "the sequence without a camera is left out");
    const cine::Sequence* s = t.find(7);
    CHECK(s && s->totalMs == 2000 && s->keys.size() == 21, "sequence 7: 2 s, a key every 100 ms");
    CHECK(s && s->sounds.size() == 1 && s->sounds[0].startMs == 0 && s->sounds[0].file.rfind("cin7_0.", 0) == 0, "its narration");
    if (s) {
        float p[3], g[3];
        s->at(500, p, g);   // halfway along the first leg: local (6,0,10) -> world (100, 206, 60)
        CHECK(near(p[0], 100) && near(p[1], 206) && near(p[2], 60), "position at 0.5 s");
        s->at(1550, p, g);  // (11, 5.5, 10) -> (94.5, 211, 60)
        CHECK(near(p[0], 94.5f) && near(p[1], 211) && near(p[2], 60), "position at 1.55 s (between two keys)");
        s->at(99999, p, g);
        CHECK(near(p[0], 90) && near(p[1], 211) && near(g[0], 95) && near(g[1], 200) && near(g[2], 50), "held at the end");
        float yaw, pitch;
        const float a[3] = {0, 0, 0}, b[3] = {0, 1, 1};
        cine::lookAngles(a, b, &yaw, &pitch);
        CHECK(near(yaw, 1.5708f) && near(pitch, 0.7854f), "look angles");
    }
    std::printf("CINEMATIC: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
