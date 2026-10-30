// hud_game.hpp — the in-game HUD on the top screen, laid out like the original client (stage 5.9, UI stage 8):
// player frame (top left: portrait, level, name, health and power, combat / resting state, party leader), target frame
// (portrait, level in its difficulty colour, elite / rare dragon, name on its reaction colour, auras and cast under it),
// our auras (top right), name plates and floating combat numbers over the units, the red error lines (UIErrorsFrame),
// cast bar, the controller bars in the bottom corners and the experience bar along the bottom edge (rested part,
// 20 segments). No chat frame (stage 5.11): chat and the combat log are on the touch screen; so are the bags and menus.
#pragma once
#include "game.hpp"
#include "hud.hpp"

// The controller bars and prompts of the Forever scheme (stage 5.11), filled by main.cpp from the pad state.
struct HudBarCell {
    std::string name;                 // "" = empty slot
    uint32_t cdMs = 0;                // cooldown left (its own, without the global cooldown)
    uint32_t cdTotalMs = 0;           // UI stage 12: how long that cooldown is (the sweep's 100 %), 0 = unknown
    uint32_t gcdMs = 0;               // UI stage 12: the global cooldown left (a lighter sweep, no numbers)
    bool fixed = false;               // a fixed action (set 0 face buttons)
    bool active = false;              // casting / auto-attack on
    ui::Image icon;                   // UI stage 2: the spell / item icon (none in the stub skin)
    uint32_t cdKey = 0;               // the action (spell id, item entry | 0x80000000) for the cooldown sweep
    uint32_t count = 0;               // items: how many we carry
};
struct HudPadView {
    bool show = false;
    int set = 0, page = 0;            // the set the modifiers select, the hotbar page
    HudBarCell cells[8];              // D-pad up, right, down, left; face X, A, B, Y
    int mode = 0;                     // 1 enemy / 2 friendly targeting mode (R / L held)
    uint64_t aim = 0;                 // unit under the C-Stick ring
    uint64_t soft = 0;                // what the interact button would use now
    const char* softVerb = "";
    bool binding = false;             // the next button press binds a spell / item
};

// UI stage 12: a cooldown running on an action that is not on the crossbars shown now (other sets / pages): the strip
// under the player frame, so a long cooldown is never out of sight.
struct HudCooldown { ui::Image icon; std::string name; uint32_t ms = 0, totalMs = 0; };

struct HudInput {
    const w3n::GameSession* game = nullptr;
    uint32_t nowMs = 0;
    float camPos[3] = {0, 0, 0};
    float camYaw = 0, camPitch = 0;       // the 3D camera (radians, yaw 0 = +x)
    const char* selfName = "";
    bool showSelfPlate = false;           // third person: the character's own name over its head
    const HudPadView* pad = nullptr;      // Forever scheme: bars at the bottom right, aim ring, interact prompt
    bool questTracker = false;            // stage 5.14: objectives of the quest log (UI stage 2: right, under the buffs)
    bool dim = true;                      // UI stage 2: frames fade out of combat
    bool showStats = false;               // 5.35e: FPS and ping at the bottom centre
    float fps = 0;
    uint32_t pingMs = 0;                  // the last pong (0 = none yet)
    int fading = -1;                      // stage 6.12: objects / units drawn fading last frame (shown with the FPS; -1 = hide)
    ui::Image portrait[2];                // UI stage 8: the rendered portraits, ours and the target's (!ok(): a fallback)
    bool charSheet = false;               // UI stage 14: the Equipment tab is open: the character window instead of the HUD
    ui::Image doll;                       // ... its figure, rendered from the model (!ok(): "loading")
    bool xpBar = true;                    // UI stage 8: the experience bar along the bottom edge
    const char* placeHint = nullptr;      // UI stage 10: a ground spell is being aimed (its name): the controls under it
    const char* zoneBig = nullptr;        // stage 5.45: the zone banner (ZoneBanner): the zone / subzone name, the subzone under it
    const char* zoneSmall = nullptr;
    int zoneAlpha = 0;                    // 0..255
    std::vector<HudCooldown> cooldowns;   // UI stage 12: running cooldowns of the actions not on the shown crossbars
};

// Fills 'out' (cleared first) for the current frame.
void hudBuildGame(HudList& out, const HudInput& in);
// World point -> top-screen pixel with the renderer's camera (60 deg vertical FOV, 400:240). False when behind it.
bool hudProject(const HudInput& in, const float world[3], float* sx, float* sy);
