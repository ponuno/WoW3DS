// stick_cal.hpp — 6.57i: a stick that rests off its centre (a worn C-Stick reads 20-35 units over, past the dead zone) turned the
// camera slowly by itself. The rest position is learned: when the raw reading stays within kQuiet units of one spot for kStillSec
// seconds and that spot is near the middle (<= kMaxCentre units), it becomes the new zero. A deliberate small push does not hold
// that still for a second; a bigger one is never taken as the centre. Pure maths (no 3DS headers), PC-tested in pad_test.
#pragma once
#include <cmath>

struct StickCal {
    static constexpr float kQuiet = 3.0f;       // units of jitter allowed while "still"
    static constexpr float kStillSec = 1.0f;    // how long it must stay still
    static constexpr float kMaxCentre = 40.0f;  // of 156: further out is a push, not a rest
    float cx = 0, cy = 0;                       // the learned centre
    float refX = 0, refY = 0, still = 0;

    // raw reading in, corrected reading out (raw - centre)
    void apply(int dx, int dy, float dt, int& ox, int& oy) {
        const float fx = (float)dx, fy = (float)dy;
        if (std::fabs(fx - refX) > kQuiet || std::fabs(fy - refY) > kQuiet) {
            refX = fx; refY = fy; still = 0;
        } else {
            still += dt;
            if (still >= kStillSec && fx * fx + fy * fy <= kMaxCentre * kMaxCentre) { cx = refX; cy = refY; }
        }
        ox = (int)std::lround(fx - cx);
        oy = (int)std::lround(fy - cy);
    }
};
