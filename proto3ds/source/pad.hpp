// pad.hpp — controller schemes (stage 5.11): turns the 3DS buttons and sticks into game intents. Pure logic (only
// the KEY_* bits of libctru), so the PC test drives it with a stub.
//
// Forever scheme (after WoW: Forever's built-in gamepad UI, positions as on an Xbox pad):
//   Circle Pad   move. Pushed past runThreshold = run, less = walk. Out of combat the character turns to the stick
//                direction (face movement), in combat too; with faceMovement off the character faces the camera and the stick strafes.
//                While L / R is held a ring with an arrow under the character shows the aim (red: enemies, green: friends).
//   C-Stick      camera; while L or R is held it aims the target ring instead.
//   face buttons (no modifier, fixed): B (bottom) jump, A (right) cancel, Y (left) interact / attack in combat,
//                X (top) context menu of the target.
//   D-pad        hotbar slots 0..3 of set 0.
//   ZL / ZR / both   held: hotbar set 1 / 2 / 3 on the D-pad and the face buttons (8 slots each) -> 28 slots.
//   R held       enemy mode: C-Stick aims, B last hostile target, A assist, Y attack, X / D-pad next enemy.
//   L held       friendly mode: C-Stick aims, B yourself, A assist, X next friendly unit, D-pad party members 1-4
//                (outside a party the D-pad also takes the next friendly unit).
//   L+R          D-pad left/right page of the hotbars, up/down camera zoom, A centre the camera.
//   START        menu (bottom screen), SELECT (tap) free camera, SELECT + D-pad left/right the touch screen's tabs.
// Classic scheme (stages 5.1-5.10, Old 3DS without ZL/ZR/C-Stick): Circle Pad up/down walk, left/right turn,
//   C-Stick / D-pad left-right look, D-pad up/down zoom, B jump, L next target, R attack, Y chat, A interact,
//   X shows the next hotbar set on the touch bar.
#pragma once
#include <3ds.h>
#include <stdint.h>

#include <string>

enum class PadScheme : uint8_t { Forever = 0, Classic = 1 };

struct PadConfig {
    PadScheme scheme = PadScheme::Forever;
    float stickDead = 0.12f;          // fraction of the full stick throw
    float runThreshold = 0.5f;        // Forever "Stick Run Threshold"
    float backThreshold = 0.6f;       // Forever "Back Pedal Stick Threshold": pull back this far to backpedal
    bool faceMovement = true;         // out of combat the character faces the stick direction
    bool toggleModifiers = false;     // Forever "HUD Modifier, Press to Toggle": tap ZL/ZR instead of holding
    bool invertY = false;
    float camSpeed = 1.8f;            // radians per second at full C-Stick
    bool questTracker = false;        // stage 5.14: quest objectives on the top screen (UI stage 2: off, the Map tab has them)
    // UI stages 3/6 (the Settings tab)
    int lang = 1;                     // interface language: 0 English, 1 Russian
    int drawDist = 1;                 // index into the draw distances (main.cpp kDrawDists)
    int crowd = 2;                    // creatures drawn at most: 0 = 10, 1 = 20, 2 = 40, 3 = all
    int topMode = 0;                  // top screen: 0 normal 400x240, 1 wide 800x240, 2 stereoscopic 3D (the slider)
    bool doodads = true;              // small world objects (trees, rocks, furniture)
    bool bigText = false;             // chat and lists on the touch screen in the bigger font
    bool dimHud = true;               // the top HUD fades out of combat
    bool autoTabs = true;             // combat -> the Combat tab, after it -> the Map tab (unless a tab is pinned)
    int musicVol = 5, sfxVol = 8, ambVol = 5;   // sound volumes 0..10 (Settings, Graphics & sound page)
    bool load(const char* path);
    bool save(const char* path) const;
};

// Hotbar addressing: slot = set * 8 + button; button 0..3 = D-pad up, right, down, left; 4..7 = face X (top),
// A (right), B (bottom), Y (left). In set 0 the face buttons are the fixed actions (jump, cancel, interact,
// context), so set 0 has only the D-pad slots: 4 + 3 * 8 = 28 bindable slots per page.
constexpr int kPadSets = 4, kPadButtons = 8, kPadSlots = kPadSets * kPadButtons;
inline bool padSlotBindable(int slot) { return slot >= 0 && slot < kPadSlots && !(slot < kPadButtons && slot >= 4); }
const char* padButtonGlyph(int button);   // "Up" "Rt" "Dn" "Lt" "X" "A" "B" "Y"
const char* padSetName(int set);          // "", "ZL", "ZR", "ZL+ZR"

struct PadInput {
    u32 down = 0, held = 0, up = 0;
    float lx = 0, ly = 0;             // Circle Pad, -1..1 (up and right positive)
    float rx = 0, ry = 0;             // C-Stick, -1..1
};

struct PadOut {
    enum Mode : uint8_t { None = 0, Enemy = 1, Friend = 2 };
    // movement (applied by the caller with the server speeds)
    uint32_t dir = 0;                 // w3n::MoveFlag Forward / Backward / StrafeLeft / StrafeRight
    bool walk = false;                // stick only partly pushed
    bool faceYaw = false;             // turn the character to 'yaw' (face movement, or the camera in combat)
    float yaw = 0;
    float turn = 0;                   // classic: turn rate, -1..1 (left positive)
    // camera (deltas for this frame)
    float camYaw = 0, camPitch = 0, zoom = 0;
    bool centerCamera = false;
    // fixed actions
    bool jump = false, cancel = false, interact = false, context = false, menu = false, freeCamera = false;
    bool ascend = false;              // the jump button held: swims up (stage 5.26)
    // classic extras
    bool nextTarget = false, toggleAttack = false, chat = false, nextSet = false;
    // hotbar
    int slot = -1;                    // pressed this frame (see addressing above)
    int set = 0;                      // the set the modifiers select now (bars highlight it)
    int pageDelta = 0;
    // targeting modes
    Mode mode = None;                 // held now
    bool aiming = false;              // the C-Stick points somewhere
    float aimYaw = 0;                 // world yaw it points to
    Mode released = None;             // the mode button went up this frame (take the aimed unit)
    bool lastHostile = false, assist = false, selfTarget = false, cycle = false, attack = false;
    int partyPick = -1;               // friendly mode: D-pad up/right/down/left = party member 0..3 (stage 5.17)
    int tabDelta = 0;                 // SELECT + D-pad left / right: the previous / next tab of the touch screen
};

class Pad {
public:
    // camYaw: the camera's yaw; inCombat switches Forever's face movement to strafing.
    PadOut update(const PadInput& in, const PadConfig& cfg, bool inCombat, float camYaw, float dt);
    int toggledSet() const { return toggled_; }
    void reset() { toggled_ = 0; lastMode_ = PadOut::None; selectCombo_ = false; }

private:
    PadOut forever(const PadInput& in, const PadConfig& cfg, bool inCombat, float camYaw, float dt);
    PadOut classic(const PadInput& in, const PadConfig& cfg, float dt);
    int toggled_ = 0;                 // press-to-toggle modifier state
    bool selectCombo_ = false;        // SELECT was used with the D-pad while held (no free camera on release)
    void selectKeys(const PadInput& in, PadOut& o);
    PadOut::Mode lastMode_ = PadOut::None;
};
