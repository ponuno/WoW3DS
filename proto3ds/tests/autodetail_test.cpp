// autodetail_test — 6.72: the automatic draw distance (autodetail.hpp) against a scene whose cost falls with the distance.
#include <cmath>
#include <cstdio>
#include "autodetail.hpp"
#include "pacer.hpp"

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// a scene: 'fixed' ms whatever the distance, 'doodads' ms at full doodad distance, 'far' ms at full draw distance
static float cost(const AutoDetail& a, float fixed, float doodads, float far) {
    return fixed + doodads * a.doodadMul() + far * a.distMul() * a.distMul();
}

int main() {
    const float T60 = FramePacer::kPeriodMs, T30 = 2 * FramePacer::kPeriodMs;
    {   // light: nothing trimmed
        AutoDetail a;
        for (int i = 0; i < 600; ++i) a.update(cost(a, 6, 2, 3), T60, true);
        CHECK(a.level() == 0 && a.doodadMul() == 1 && a.distMul() == 1, "light scene: full detail");
    }
    {   // a bit too heavy for 60: the doodads alone bring it back, the draw distance stays
        AutoDetail a;
        for (int i = 0; i < 600; ++i) a.update(cost(a, 9, 6, 3), T60, true);
        CHECK(a.distMul() == 1.0f && a.doodadMul() < 1.0f, "18 ms: doodads shortened (x%.2f), draw distance kept", a.doodadMul());
        CHECK(a.cost() < 0.97f * T60, "and the frame fits 60 (%.1f ms)", a.cost());
        float lo = 1, hi = 0;
        for (int i = 0; i < 600; ++i) { a.update(cost(a, 9, 6, 3), T60, true); lo = fminf(lo, a.level()); hi = fmaxf(hi, a.level()); }
        CHECK(hi - lo < 0.1f, "steady: the level holds (%.2f..%.2f)", lo, hi);
    }
    {   // much too heavy: everything to the limits, then it is the pacer's turn (canTrim false)
        AutoDetail a;
        for (int i = 0; i < 200; ++i) a.update(cost(a, 18, 6, 6), T60, true);
        CHECK(!a.canTrim() && fabsf(a.distMul() - AutoDetail::kDistMin) < 1e-4f, "30 ms at 60: trimmed to the limits");
        for (int i = 0; i < 3000; ++i) a.update(cost(a, 18, 6, 6), T30, true);
        CHECK(a.distMul() == 1.0f && a.level() < 0.5f, "at 30 the same scene fits: the draw distance back in full (level %.2f)", a.level());
    }
    {   // a heavy spot passed: back to full detail within seconds, not at once
        AutoDetail a;
        for (int i = 0; i < 200; ++i) a.update(cost(a, 12, 6, 4), T60, true);
        const float l0 = a.level();
        a.update(5, T60, true);
        CHECK(a.level() > l0 - 0.05f, "no jump back on one light frame");
        int n = 0;
        while (a.level() > 0 && n < 2000) { a.update(5, T60, true); ++n; }
        CHECK(n > 100 && n < 400, "back to full detail in %d frames", n);
    }
    // with the pacer (Auto 60/30): a scene a bit too heavy for 60 stays at 60 with the detail trimmed; a far too heavy one goes to
    // 30 and gets its detail back
    auto sim = [&](float fixed, float dood, float far, int frames, FramePacer& p, AutoDetail& a, int* switches) {
        for (int i = 0; i < frames; ++i) {
            const float c = cost(a, fixed, dood, far);
            int k = (int)std::ceil(c / FramePacer::kPeriodMs - 1e-4f);
            if (k < p.interval()) k = p.interval();
            if (p.frameDone(k, c, a.canTrim())) ++*switches;
            a.update(c, p.interval() * FramePacer::kPeriodMs, true);
        }
    };
    {
        FramePacer p; p.setMode(2);
        AutoDetail a;
        int sw = 0;
        sim(9, 6, 4, 3000, p, a, &sw);
        CHECK(p.blanks() == 1 && a.level() > 0 && sw == 0, "19 ms scene: kept at 60 by trimming (level %.2f, %d switches)", a.level(), sw);
        sw = 0;
        sim(20, 4, 4, 3000, p, a, &sw);
        CHECK(p.blanks() == 2 && a.distMul() == 1.0f && sw == 1, "28 ms scene: 30 FPS with the full draw distance (%d switches)", sw);
    }
    {
        AutoDetail a;
        for (int i = 0; i < 100; ++i) a.update(40, T60, true);
        a.update(40, T60, false);
        CHECK(a.level() == 0 && a.distMul() == 1, "off: full detail at once");
    }
    std::printf("AUTODETAIL: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
