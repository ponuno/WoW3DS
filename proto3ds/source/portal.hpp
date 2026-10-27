// portal.hpp — the portal effect of a map change (stage 5.24): dungeon doors, instance portals, continents.
// A swirl of spiral arms made of small flat rectangles (HudList quads, no textures) that closes over the screen
// while the server prepares the new map and the new map pack is opened, and opens again over the new world.
// Pure logic and draw list: no GPU code, tested on the PC (tests/portal_test.cpp).
#pragma once
#include "hud.hpp"

class PortalFx {
public:
    enum class Phase { Idle, Closing, Hold, Opening };

    // SMSG_TRANSFER_PENDING / SMSG_NEW_WORLD: the swirl closes over the screen (no-op while one is already running).
    void start();
    // The new map is ready (or could not be loaded): the swirl opens over the new world. No-op unless it is shown.
    void finish();
    // Already closed (entering the world: the screen is the loading screen from the first frame).
    void startClosed() { phase_ = Phase::Hold; cover_ = 1.0f; finishAfterClose_ = false; }
    // The map's in-game loading screen (ui::Assets::loadingScreen) shown instead of the swirl when it is ok(), and the
    // progress bar under it (0..1; -1 = no bar).
    void setPicture(const ui::Image& pic) { pic_ = pic; }
    void setProgress(float p) { progress_ = p; }
    // Abort at once (e.g. the connection is gone).
    void cancel();
    void update(float dt);

    Phase phase() const { return phase_; }
    bool active() const { return phase_ != Phase::Idle; }
    // How much of the screen the effect covers, 0..1 (1 = the world is hidden behind it).
    float cover() const { return cover_; }
    bool closed() const { return phase_ == Phase::Hold || (phase_ == Phase::Closing && cover_ >= 0.999f); }

    // Adds the effect on top of whatever the list holds. w x h = the screen (400x240 top, 320x240 touch screen).
    // text: a line under the picture / swirl while the screen is closed ("" = none).
    void build(HudList& l, int w, int h, const char* text = "") const;

    static constexpr float kCloseSec = 0.55f, kOpenSec = 0.9f;

private:
    Phase phase_ = Phase::Idle;
    float cover_ = 0;      // 0..1
    float time_ = 0;       // swirl animation clock
    bool finishAfterClose_ = false;
    ui::Image pic_;
    float progress_ = -1.0f;
};
