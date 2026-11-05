// pacer_test — 6.71: the frame pacer's decisions (pacer.hpp). A frame of 'work' ms started on a blank is ready after the first
// blank past its work: k = ceil(work / period) blanks (at least the interval).
#include <cmath>
#include <cstdio>
#include "pacer.hpp"

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static int run(FramePacer& p, float workMs, int frames, int* switches = nullptr) {
    for (int i = 0; i < frames; ++i) {
        int k = (int)std::ceil(workMs / FramePacer::kPeriodMs - 1e-4f);
        if (k < p.interval()) k = p.interval();
        if (p.frameDone(k, workMs) && switches) ++*switches;
    }
    return p.blanks();
}

int main() {
    const float P = FramePacer::kPeriodMs;
    {
        FramePacer p;
        p.setMode(2);
        CHECK(run(p, 10, 200) == 1, "light frames: 60");
        CHECK(std::fabs(p.shownMs() - P) < 0.01f, "time step one period (%f)", p.shownMs());
        int sw = 0;
        CHECK(run(p, 24, 10, &sw) == 2 && sw == 1, "heavy frames (24 ms): down to 30 within a few frames");
        CHECK(std::fabs(p.shownMs() - 2 * P) < 0.01f, "time step two periods");
        sw = 0;
        CHECK(run(p, 24, 600, &sw) == 2 && sw == 0, "stays at 30 while heavy (no flapping)");
        CHECK(run(p, 15, 300) == 2, "15 ms of work (over 80%% of a period): still 30");
        CHECK(run(p, 11, 89) == 2 && run(p, 11, 1) == 1, "light again for 90 frames: back to 60");
    }
    {   // occasional spikes do not drop to 30
        FramePacer p;
        p.setMode(2);
        int sw = 0;
        for (int i = 0; i < 600; ++i) run(p, i % 20 == 0 ? 22.0f : 12.0f, 1, &sw);
        CHECK(p.blanks() == 1 && sw == 0, "one slow frame in 20: stays at 60");
        for (int i = 0; i < 600; ++i) run(p, i % 4 == 0 ? 22.0f : 12.0f, 1, &sw);
        CHECK(p.blanks() == 2, "one slow frame in 4: goes to 30");
    }
    {   // GPU-bound: the CPU part looks light (10 ms) but the frames miss at 60 - no endless flapping
        FramePacer p;
        p.setMode(2);
        int sw = 0;
        for (int i = 0; i < 20000; ++i) {
            const int k = p.interval() == 1 ? 2 : 2;   // the GPU (25 ms) makes every frame two blanks long
            if (p.frameDone(k, 10.0f)) ++sw;
        }
        CHECK(sw < 40, "GPU-bound with a light CPU: %d switches in 20000 frames (11 min: a try of 4 frames every ~48 s)", sw);
        CHECK(p.upNeed() == FramePacer::kUpMax, "the wait to go up grew to the cap (%d)", p.upNeed());
    }
    {   // 6.72: while the draw distance can still come down, the pacer waits longer before dropping to 30
        FramePacer p;
        p.setMode(2);
        int k = 0;
        while (p.blanks() == 1 && k < 100) { p.frameDone(k % 2 ? 2 : 1, 15.0f, true); ++k; }
        CHECK(k >= 100, "half the frames missed, trimming possible: no 30 (%d frames)", k);
        FramePacer q;
        q.setMode(2);
        k = 0;
        while (q.blanks() == 1 && k < 100) { q.frameDone(k % 2 ? 2 : 1, 15.0f, false); ++k; }
        CHECK(k < 10, "nothing left to trim: %d frames", k);
    }
    {
        FramePacer p;
        p.setMode(1);
        CHECK(run(p, 5, 100) == 2 && p.interval() == 2, "mode 30: always two blanks");
        p.setMode(0);
        CHECK(!p.active() && p.interval() == 1, "off");
    }
    std::printf("PACER: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
