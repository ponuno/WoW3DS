// PC check of the controller schemes (stage 5.11): pad.cpp against the WoW: Forever layout described in
// pad.hpp, and the classic layout of stages 5.1-5.10.
#include <math.h>
#include <cstdio>

#include "movement.hpp"
#include "pad.hpp"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace w3n;

static PadOut press(Pad& p, const PadConfig& c, u32 held, u32 down, float lx = 0, float ly = 0, float rx = 0, float ry = 0,
                    bool combat = false, float camYaw = 0, u32 up = 0) {
    PadInput in;
    in.held = held | down;
    in.down = down;
    in.up = up;
    in.lx = lx; in.ly = ly; in.rx = rx; in.ry = ry;
    return p.update(in, c, combat, camYaw, 1.0f / 30);
}

int main() {
    PadConfig f;   // Forever defaults
    Pad p;
    // fixed face buttons in set 0
    CHECK(press(p, f, 0, KEY_B).jump, "B jumps");
    CHECK(press(p, f, 0, KEY_A).cancel, "A cancels");
    CHECK(press(p, f, 0, KEY_Y).interact, "Y interacts");
    CHECK(press(p, f, 0, KEY_X).context, "X context");
    CHECK(press(p, f, 0, KEY_B).slot < 0, "B is not a slot in set 0");
    // D-pad set 0, triggers select sets 1..3
    CHECK(press(p, f, 0, KEY_DUP).slot == 0, "D-up slot 0");
    CHECK(press(p, f, 0, KEY_DLEFT).slot == 3, "D-left slot 3");
    CHECK(press(p, f, KEY_ZL, KEY_DRIGHT).slot == 8 + 1, "ZL+D-right");
    CHECK(press(p, f, KEY_ZR, KEY_X).slot == 16 + 4, "ZR+X");
    CHECK(press(p, f, KEY_ZR, KEY_B).slot == 16 + 6 && !press(p, f, KEY_ZR, KEY_B).jump, "ZR+B is a slot, not a jump");
    CHECK(press(p, f, KEY_ZL | KEY_ZR, KEY_Y).slot == 24 + 7, "ZL+ZR+Y");
    CHECK(press(p, f, KEY_ZL | KEY_ZR, 0).set == 3, "both triggers: set 3");
    int bindable = 0;
    for (int s = 0; s < kPadSlots; ++s) bindable += padSlotBindable(s);
    CHECK(bindable == 28, "28 bindable slots, got %d", bindable);
    // press-to-toggle modifiers
    PadConfig t = f;
    t.toggleModifiers = true;
    Pad pt;
    press(pt, t, 0, KEY_ZR);
    CHECK(press(pt, t, 0, KEY_A).slot == 16 + 5, "toggled ZR: A is slot 21");
    press(pt, t, 0, KEY_ZR);
    CHECK(press(pt, t, 0, KEY_A).cancel, "toggled off again");
    // targeting modes
    PadOut o = press(p, f, KEY_R, KEY_B);
    CHECK(o.mode == PadOut::Enemy && o.lastHostile && !o.jump, "R+B last hostile");
    CHECK(press(p, f, KEY_R, KEY_A).assist, "R+A assist");
    CHECK(press(p, f, KEY_R, KEY_Y).attack, "R+Y attack");
    CHECK(press(p, f, KEY_R, KEY_X).cycle && press(p, f, KEY_R, KEY_DDOWN).cycle, "R+X / R+D-pad next enemy");
    o = press(p, f, KEY_R, 0, 0, 0, 1.0f, 0.0f, false, 0.5f);
    CHECK(o.aiming && fabsf(remainderf(o.aimYaw - (0.5f - 1.5707963f), 6.2831853f)) < 1e-4f && o.camYaw == 0,
          "C-Stick right aims 90 deg right of the camera (%f)", o.aimYaw);
    o = press(p, f, 0, 0);
    CHECK(o.released == PadOut::Enemy, "releasing R takes the aimed unit");
    CHECK(press(p, f, 0, 0).released == PadOut::None, "release reported once");
    o = press(p, f, KEY_L, KEY_B);
    CHECK(o.mode == PadOut::Friend && o.selfTarget, "L+B yourself");
    o = press(p, f, KEY_L, KEY_DRIGHT);
    CHECK(o.partyPick == 1 && !o.cycle && o.slot < 0, "L+D-right party member 2 (stage 5.17)");
    CHECK(press(p, f, KEY_L, KEY_X).cycle, "L+X next friendly unit");
    press(p, f, 0, 0);
    o = press(p, f, KEY_L | KEY_R, KEY_DLEFT);
    CHECK(o.pageDelta == -1 && o.mode == PadOut::None && o.slot < 0, "L+R+D-left previous page");
    CHECK(press(p, f, KEY_L | KEY_R, KEY_A).centerCamera, "L+R+A centre camera");
    CHECK(press(p, f, KEY_L | KEY_R | KEY_DUP, 0).zoom < 0, "L+R+D-up zoom in");
    o = press(p, f, 0, 0);
    CHECK(o.released == PadOut::None, "L+R is not a targeting release");
    CHECK(press(p, f, 0, KEY_START).menu, "START menu");
    // UI stage 3: SELECT tapped = free camera (on release); SELECT + D-left/right = tabs, no camera, no hotbar slot
    CHECK(!press(p, f, 0, KEY_SELECT).freeCamera && !press(p, f, 0, 0, 0, 0, 0, 0, false, 0, KEY_SELECT).freeCamera,
          "SELECT tap: no free camera any more (stage 5.31)");
    press(p, f, 0, KEY_SELECT);
    o = press(p, f, KEY_SELECT, KEY_DRIGHT);
    CHECK(o.tabDelta == 1 && o.slot < 0, "SELECT + D-right: next tab");
    CHECK(press(p, f, KEY_SELECT, KEY_DLEFT).tabDelta == -1, "SELECT + D-left: previous tab");
    CHECK(!press(p, f, 0, 0, 0, 0, 0, 0, false, 0, KEY_SELECT).freeCamera, "...and no free camera after the combo");
    {
        PadConfig cl;
        cl.scheme = PadScheme::Classic;
        Pad pc;
        press(pc, cl, 0, KEY_SELECT);
        o = press(pc, cl, KEY_SELECT, KEY_DLEFT);
        CHECK(o.tabDelta == -1 && fabsf(o.camYaw) < 1e-6f, "classic: SELECT + D-left is a tab, not a look");
    }
    // movement: face movement out of combat
    o = press(p, f, 0, 0, 0.0f, 1.0f);
    CHECK(o.dir == MoveFlag::Forward && !o.walk && o.faceYaw && fabsf(o.yaw) < 1e-5f, "full up: run forward");
    o = press(p, f, 0, 0, 0.0f, 0.3f);
    CHECK(o.dir == MoveFlag::Forward && o.walk, "partly up: walk");
    o = press(p, f, 0, 0, 1.0f, 0.0f, 0, 0, false, 1.0f);
    CHECK(o.dir == MoveFlag::Forward && fabsf(remainderf(o.yaw - (1.0f - 1.5707963f), 6.2831853f)) < 1e-4f,
          "stick right out of combat: turn right of the camera and run (%f)", o.yaw);
    o = press(p, f, 0, 0, 0.0f, -1.0f, 0, 0, false, 0.0f);
    CHECK(o.dir == MoveFlag::Forward && fabsf(fabsf(o.yaw) - 3.14159265f) < 1e-4f, "stick down out of combat: turn round");
    // combat with face movement off: strafe / backpedal facing the camera
    PadConfig nf = f;
    nf.faceMovement = false;
    o = press(p, nf, 0, 0, 1.0f, 0.0f, 0, 0, true, 0.7f);
    CHECK(o.dir == MoveFlag::StrafeRight && o.faceYaw && fabsf(o.yaw - 0.7f) < 1e-5f, "combat: stick right strafes");
    o = press(p, nf, 0, 0, -0.7f, 0.7f, 0, 0, true);
    CHECK(o.dir == (MoveFlag::Forward | MoveFlag::StrafeLeft), "combat: diagonal");
    o = press(p, nf, 0, 0, 0.0f, -1.0f, 0, 0, true);
    CHECK(o.dir == MoveFlag::Backward, "combat: full back backpedals");
    o = press(p, nf, 0, 0, 0.0f, -0.4f, 0, 0, true);
    CHECK(o.dir == 0, "combat: a slight pull (below the back threshold) does not backpedal");
    // combat with face movement on (default): the character turns to the stick and runs forward, as out of combat
    o = press(p, f, 0, 0, 1.0f, 0.0f, 0, 0, true, 0.7f);
    CHECK(o.dir == MoveFlag::Forward && o.faceYaw && fabsf(remainderf(o.yaw - (0.7f - 1.5707963f), 6.2831853f)) < 1e-4f,
          "combat: stick right turns the character (%f)", o.yaw);
    o = press(p, f, 0, 0, 0.05f, 0.05f, 0, 0, false);
    CHECK(o.dir == 0, "inside the dead zone");
    o = press(p, f, 0, 0, 0, 0, 0, 0, true, 2.0f);
    CHECK(o.faceYaw && o.yaw == 2.0f, "combat, standing: turn with the camera");
    CHECK(press(p, nf, 0, 0, 1.0f, 0.0f).dir == MoveFlag::StrafeRight, "face movement off: strafe out of combat too");
    // camera
    o = press(p, f, 0, 0, 0, 0, 1.0f, 0.0f);
    CHECK(o.camYaw < 0 && !o.aiming, "C-Stick right turns the camera right");
    // classic scheme
    PadConfig c;
    c.scheme = PadScheme::Classic;
    CHECK(press(p, c, 0, KEY_B).jump && press(p, c, 0, KEY_L).nextTarget && press(p, c, 0, KEY_R).toggleAttack &&
          press(p, c, 0, KEY_Y).chat && press(p, c, 0, KEY_A).interact && press(p, c, 0, KEY_X).nextSet, "classic keys");
    o = press(p, c, 0, 0, 0.0f, 1.0f);
    CHECK(o.dir == MoveFlag::Forward && !o.walk, "classic forward runs");
    o = press(p, c, 0, 0, 1.0f, 0.0f);
    CHECK(o.dir == 0 && o.camYaw < 0, "classic: stick right turns");
    CHECK(press(p, c, KEY_DDOWN, 0).zoom > 0, "classic D-down zoom out");
    // config round trip
    PadConfig w = f;
    w.scheme = PadScheme::Classic; w.runThreshold = 0.3f; w.faceMovement = false; w.toggleModifiers = true; w.invertY = true;
    w.save("/tmp/w3d_pad_test.txt");
    PadConfig r;
    r.load("/tmp/w3d_pad_test.txt");
    CHECK(r.scheme == PadScheme::Classic && fabsf(r.runThreshold - 0.3f) < 1e-3f && !r.faceMovement && r.toggleModifiers && r.invertY,
          "pad.txt round trip");
    std::printf("PAD: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
