// portrait_test — UI stage 8: portraitCalibrate (portrait_calib.hpp) against the test pattern drawn every way the GPU could
// lay it into texture memory (the 8 flips / turns): the fix it returns must make clip space land upright (memory row 0 =
// clip top, column 0 = clip left). Also: an empty or garbled texture is refused.
#include <cstdio>
#include <cstring>
#include <vector>

#include "portrait_calib.hpp"

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}
static size_t at(int x, int y, int w) { return ((size_t)((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)) * 4; }

int main() {
    const int n = 64;
    for (int m = 0; m < 8; ++m) {
        // the GPU's (unknown) map L: memory (c, r) = L (x, y), a signed permutation
        const bool swap = m & 4;
        const int sa = (m & 1) ? -1 : 1, sb = (m & 2) ? -1 : 1;
        auto L = [&](float x, float y, float* c, float* r) { if (swap) { *c = sa * y; *r = sb * x; } else { *c = sa * x; *r = sb * y; } };
        std::vector<uint8_t> mem((size_t)n * n * 4, 0);
        for (int py = 0; py < n; ++py)
            for (int px = 0; px < n; ++px) {   // a memory pixel: which clip point lands there (L is its own kind of inverse)
                const float c = (px + 0.5f) / n * 2 - 1, r = (py + 0.5f) / n * 2 - 1;
                float x = 0, y = 0;
                if (swap) { y = c / sa; x = r / sb; } else { x = c / sa; y = r / sb; }
                uint8_t* o = &mem[at(px, py, n)];
                const int byteRed = m % 4, byteGreen = (m + 1) % 4;   // the channel order is not known either
                if (y > 0) o[byteRed] = 200;
                if (x < 0) o[byteGreen] = (uint8_t)(o[byteGreen] + 100);
            }
        float corr[4];
        const bool ok = portraitCalibrate(mem.data(), n, n, corr);
        CHECK(ok, "orientation %d understood", m);
        if (!ok) continue;
        // drawing with clip' = C clip: the clip top must land in memory rows < n/2, the clip left in columns < n/2
        float c, r;
        const float top[2] = {0, 0.5f}, left[2] = {-0.5f, 0};
        float tx = corr[0] * top[0] + corr[1] * top[1], ty = corr[2] * top[0] + corr[3] * top[1];
        L(tx, ty, &c, &r);
        CHECK(r < -0.2f && c > -0.1f && c < 0.1f, "orientation %d: clip top -> memory top (c %.2f r %.2f)", m, c, r);
        tx = corr[0] * left[0] + corr[1] * left[1]; ty = corr[2] * left[0] + corr[3] * left[1];
        L(tx, ty, &c, &r);
        CHECK(c < -0.2f && r > -0.1f && r < 0.1f, "orientation %d: clip left -> memory left (c %.2f r %.2f)", m, c, r);
    }
    std::vector<uint8_t> empty((size_t)n * n * 4, 0), noise((size_t)n * n * 4);
    for (size_t i = 0; i < noise.size(); ++i) noise[i] = (uint8_t)(i * 7919 % 251);
    float corr[4];
    CHECK(!portraitCalibrate(empty.data(), n, n, corr), "empty texture refused");
    CHECK(!portraitCalibrate(noise.data(), n, n, corr), "noise refused");
    std::printf("PORTRAIT CALIBRATION: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
