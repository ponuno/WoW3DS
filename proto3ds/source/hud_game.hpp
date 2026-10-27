// hud_game.hpp — the in-game HUD on the top screen, laid out like the original client (stage 5.9):
// player frame (top left), target frame, minimap (top right), name plates and floating combat numbers
// over the units, cast bar. No chat frame (stage 5.11): chat and the combat log are on the touch screen. The bottom row of the original (action bars,
// experience bar, bags, menu) is not drawn here: on the 3DS it lives on the touch screen.
#pragma once
#include "game.hpp"
#include "hud.hpp"

// The controller bars and prompts of the Forever scheme (stage 5.11), filled by main.cpp from the pad state.
struct HudBarCell {
    std::string name;                 // "" = empty slot
    uint32_t cdMs = 0;                // cooldown left
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
};

// Fills 'out' (cleared first) for the current frame.
void hudBuildGame(HudList& out, const HudInput& in);
// World point -> top-screen pixel with the renderer's camera (60 deg vertical FOV, 400:240). False when behind it.
bool hudProject(const HudInput& in, const float world[3], float* sx, float* sy);
