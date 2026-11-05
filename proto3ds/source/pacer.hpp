// pacer.hpp — 6.71: the frame pacer. Without VSync (6.4) a frame is shown at the first vertical blank after it is done, so frames of
// 20-28 ms reach the screen 16.7 or 33.4 ms apart in no order - the picture judders though the FPS counter says 40+. The pacer
// starts every frame on a vertical blank and holds a fixed number of blanks per frame:
//   mode 1 "30": two blanks a frame (29.9 FPS), always;
//   mode 2 "Auto": one blank (59.8 FPS) while frames fit; after kMissDown frames of the last kWindow missed their blank it goes to
//                  two; back to one when kUpFrames frames in a row were done (the work before the wait) within kUpBusy of a blank
//                  (a GPU-bound frame can look light to the CPU: a fall back to 30 soon after going up doubles the frames asked
//                  for next time, up to 16x; a minute at 60 forgives it);
//   mode 0: off (the 6.4 behaviour: no wait but the 60 FPS cap).
// The game's time step is the shown interval (blanks x period), so movement is even on the screen.
// No 3DS code here: main.cpp waits for the blanks and reports; pacer_test checks the decisions.
#pragma once
#include <stdint.h>

class FramePacer {
public:
    static constexpr float kPeriodMs = 1000.0f / 59.8261f;   // the 3DS top screen
    static constexpr int kWindow = 30, kMissDown = 4, kUpFrames = 90, kUpMax = 90 * 16, kSoon = 300, kForgive = 3600;
    static constexpr float kUpBusy = 0.80f;                   // of one period

    int mode = 2;
    // Blanks to wait for since the last paced frame began (1 or 2).
    int interval() const { return mode == 1 ? 2 : mode == 2 ? n_ : 1; }
    bool active() const { return mode == 1 || mode == 2; }
    // After the wait: k = blanks since the previous paced frame began, busyMs = time from then to the start of this wait (the work).
    // Returns true when the interval changed (for the log).
    // canTrim (6.72): the automatic draw distance can still lighten the frames - it goes first (no 30 while it can).
    bool frameDone(int k, float busyMs, bool canTrim = false) {
        shownMs_ = (float)(k < 1 ? 1 : k) * kPeriodMs;
        busyMs_ = busyMs;
        if (mode != 2) { n_ = mode == 1 ? 2 : 1; return false; }
        const int was = n_;
        // the miss history of the last kWindow frames
        const bool missed = k > n_;
        hist_[pos_] = missed;
        pos_ = (pos_ + 1) % kWindow;
        if (filled_ < kWindow) ++filled_;
        int misses = 0;
        for (int i = 0; i < filled_; ++i) misses += hist_[i] ? 1 : 0;
        ++sinceSwitch_;
        if (n_ == 1) {
            if (!canTrim && misses >= kMissDown) {
                if (cameUp_ && sinceSwitch_ < kSoon) upNeed_ = upNeed_ * 2 > kUpMax ? kUpMax : upNeed_ * 2;   // it did not hold: wait longer
                n_ = 2; reset();
            } else if (sinceSwitch_ >= kForgive) {
                upNeed_ = kUpFrames;
            }
        } else {
            if (busyMs <= kUpBusy * kPeriodMs) ++fits_; else fits_ = 0;
            if (fits_ >= upNeed_) { n_ = 1; reset(); cameUp_ = true; }
        }
        return n_ != was;
    }
    // The time step of the game: what the screen showed between the last two frames (ms); 0 before the first.
    float shownMs() const { return shownMs_; }
    float busyMs() const { return busyMs_; }   // 6.72: the last frame's work (the automatic draw distance reads it)
    int blanks() const { return n_; }
    int upNeed() const { return upNeed_; }
    void setMode(int m) { if (m != mode) { mode = m; n_ = m == 1 ? 2 : 1; reset(); } }

private:
    void reset() { for (bool& h : hist_) h = false; pos_ = filled_ = fits_ = sinceSwitch_ = 0; }
    int n_ = 1;
    bool hist_[kWindow] = {};
    int pos_ = 0, filled_ = 0, fits_ = 0, sinceSwitch_ = 0, upNeed_ = kUpFrames;
    bool cameUp_ = false;
    float shownMs_ = 0, busyMs_ = 0;
};
