// portrait_calib.hpp — which way up a picture drawn into a texture lands (UI stage 8, portrait.cpp), platform-free so the
// PC test can feed it all eight flips / turns (tests/portrait_test.cpp).
//
// The test pattern (ui_render.cpp uiDrawOrientationTest) is drawn straight in clip space into an RGBA8 texture cleared to
// zero: the top half (clip y > 0) gets a channel of 200, the left half (clip x < 0) a channel of 100 (added). The texture
// memory is the PICA layout: 8x8 tiles in rows from the start of the memory, Morton order inside a tile, 4 bytes a pixel
// in an order we do not need to know. From where the two halves landed we get L: memory (column, row) = L (clip x, y),
// a signed permutation. The interface draws a texture with its memory row 0 at the top and column 0 at the left (the
// converter's convention, tex3ds), so the picture is upright when memory = T (x, y) with T = [[1, 0], [0, -1]]; drawing
// with clip' = C clip, C = L^T T, makes it so.
#pragma once
#include <math.h>
#include <stdint.h>

inline bool portraitCalibrate(const uint8_t* mem, int w, int h, float corr[4]) {
    double top[2] = {0, 0}, left[2] = {0, 0};
    long nTop = 0, nLeft = 0;
    const int tilesX = w / 8;
    for (long i = 0; i < (long)w * h; ++i) {
        const long tile = i >> 6;
        const int m = (int)(i & 63);
        const int mx = (m & 1) | ((m >> 1) & 2) | ((m >> 2) & 4), my = ((m >> 1) & 1) | ((m >> 2) & 2) | ((m >> 3) & 4);
        const int x = (int)(tile % tilesX) * 8 + mx, y = (int)(tile / tilesX) * 8 + my;
        const uint8_t* p = mem + i * 4;
        bool isTop = false, isLeft = false;
        for (int b = 0; b < 4; ++b) {
            if (p[b] >= 150) isTop = true;
            else if (p[b] >= 60 && p[b] <= 140) isLeft = true;
        }
        const double c = (x + 0.5) / w * 2 - 1, r = (y + 0.5) / h * 2 - 1;
        if (isTop) { top[0] += c; top[1] += r; ++nTop; }
        if (isLeft) { left[0] += c; left[1] += r; ++nLeft; }
    }
    const long half = (long)w * h / 2;
    if (nTop < half * 7 / 10 || nTop > half * 13 / 10 || nLeft < half * 7 / 10 || nLeft > half * 13 / 10) return false;
    for (int k = 0; k < 2; ++k) { top[k] /= (double)nTop; left[k] /= (double)nLeft; }
    // which memory axis (0 column, 1 row) a half lies along, and on which side (+1 high, -1 low)
    auto axis = [](const double m[2], int* k, int* sign) {
        if (fabs(m[1]) > 0.3 && fabs(m[0]) < 0.15) { *k = 1; *sign = m[1] > 0 ? 1 : -1; return true; }
        if (fabs(m[0]) > 0.3 && fabs(m[1]) < 0.15) { *k = 0; *sign = m[0] > 0 ? 1 : -1; return true; }
        return false;
    };
    int kTop, sTop, kLeft, sLeft;
    if (!axis(top, &kTop, &sTop) || !axis(left, &kLeft, &sLeft) || kTop == kLeft) return false;
    int L[2][2] = {{0, 0}, {0, 0}};
    L[kTop][1] = sTop;      // clip y > 0 lies on the sTop side of memory axis kTop
    L[kLeft][0] = -sLeft;   // clip x < 0 lies on the sLeft side: x > 0 on the other
    const int T[2][2] = {{1, 0}, {0, -1}};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) corr[i * 2 + j] = (float)(L[0][i] * T[0][j] + L[1][i] * T[1][j]);
    return true;
}
