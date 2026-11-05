// autodetail.hpp — 6.72: the draw distance comes down by itself in heavy scenes. A level 0..1 follows the frame's cost against the
// frame time aimed at (the pacer's interval: 16.7 ms at 60, 33.4 at 30; 33.4 with the pacer off): over 95% of it the level rises
// (~1.7 s from 0 to 1 at 60 FPS), under 85% it falls slower (~4 s), in between it holds (no pumping).
// The first half of the level shortens the doodads' distance (to x0.55: they cost the most per yard - 6.51), the second half the
// draw distance itself (to x0.6, never under 120 yd): fog, buildings, ground. Streaming keeps the chosen distance (no tile loads
// back and forth); the PVS stays usable (main.cpp keeps the doodad distance at or over the file's occRange).
// No 3DS code: autodetail_test checks it.
#pragma once

class AutoDetail {
public:
    static constexpr float kUp = 0.01f, kDown = 0.004f, kOver = 0.95f, kUnder = 0.85f;
    static constexpr float kDoodadMin = 0.55f, kDistMin = 0.60f;

    // costMs: this frame's work (the pacer's measure, else the frame time); targetMs: the frame time aimed at
    void update(float costMs, float targetMs, bool on) {
        if (!on) { level_ = 0; ema_ = 0; return; }
        ema_ = ema_ <= 0 ? costMs : ema_ + 0.3f * (costMs - ema_);
        if (ema_ > kOver * targetMs) {   // the further over, the faster (up to 4x)
            float f = (ema_ / targetMs - kOver) * 10.0f;
            level_ += kUp * (f < 1 ? 1 : f > 4 ? 4 : f);
        }
        else if (ema_ < kUnder * targetMs) level_ -= kDown;
        if (level_ < 0) level_ = 0;
        if (level_ > 1) level_ = 1;
    }
    float level() const { return level_; }
    bool canTrim() const { return level_ < 0.999f; }
    float doodadMul() const { const float a = level_ < 0.5f ? level_ * 2 : 1.0f; return 1.0f - (1.0f - kDoodadMin) * a; }
    float distMul() const { const float b = level_ > 0.5f ? (level_ - 0.5f) * 2 : 0.0f; return 1.0f - (1.0f - kDistMin) * b; }
    float cost() const { return ema_; }

private:
    float level_ = 0, ema_ = 0;
};
