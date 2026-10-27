// swim.hpp — swimming (stage 5.26): how a character moves under / on a liquid surface, the way the stock client does
// it, kept free of the 3DS so the PC tests drive it (tests/swim_test.cpp) with the same code main.cpp runs.
//
// Rules (yards, feet height):
//   * a walker or faller whose feet are more than W3D_SWIM_DEPTH under the surface starts swimming (MSG_MOVE_START_SWIM
//     ends a fall: no MSG_MOVE_FALL_LAND, no fall damage);
//   * a swimmer floats with the feet W3D_SWIM_DEPTH under the surface and cannot rise above that;
//   * swimming forward / backward goes where the camera looks (a dead zone keeps level strokes level), strafing stays
//     level; the jump button held swims up; nothing pulls a swimmer down (one who fell in comes back up by itself);
//   * the horizontal speed never passes the swim speed (the server's speed check is horizontal);
//   * touching a bottom shallower than W3D_SWIM_EXIT stands up; leaving the liquid's cells stops swimming too;
//   * the jump button pressed at the surface jumps out of the water.
#pragma once
#include <stdint.h>

#include <functional>

#include "w3d_liquid.h"

namespace swim {

struct World {
    // ground for feet at zFeet: the highest surface at most a step above them (terrain or a floor), false = not loaded
    std::function<bool(float x, float y, float zFeet, float* ground)> floor;
    // the liquid over (x, y) for feet at zRef (level, W3D_LIQ_* flags)
    std::function<bool(float x, float y, float zRef, float* level, uint8_t* flags)> liquid;
    // walls: moves (fromX, fromY) towards (*toX, *toY) at feet height z, sliding along walls
    std::function<void(float fromX, float fromY, float z, float* toX, float* toY)> walls;
};

struct State {
    float vz = 0;            // momentum of the fall that ended in the water (negative: still going down)
    bool surfacing = false;  // fell in: floats back up to the surface when nothing else moves it vertically
};

struct Input {
    float dt = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;           // facing
    uint32_t dir = 0;        // w3n::MoveFlag Forward / Backward / StrafeLeft / StrafeRight
    float speed = 0;         // the swim speed for 'dir' (Mover::speeds.forFlags with the swimming flag)
    float camPitch = 0;
    bool ascend = false, jump = false, rooted = false;
};

struct Result {
    float pos[3] = {0, 0, 0};
    float pitch = 0;         // swim pitch for the movement packets
    bool stop = false;       // stood up on a shallow bottom, or swam out of the liquid: MSG_MOVE_STOP_SWIM
    bool jumpOut = false;    // jump pressed at the surface: stop swimming, then jump (pos unchanged)
    bool blocked = false;    // a wall or a steep bottom stopped the horizontal stroke
};

// Movement keys bits used here (the same values as w3n::MoveFlag).
constexpr uint32_t kForward = 0x1, kBackward = 0x2, kStrafeLeft = 0x4, kStrafeRight = 0x8;

// Pitch of the stroke from the camera pitch: level within +-kDead, then the rest of the range up to +-1.4 rad.
float strokePitch(float camPitch);
// Feet at feetZ in a liquid with this surface: deep enough to swim?
inline bool deepEnough(float level, float feetZ) { return level - feetZ > W3D_SWIM_DEPTH; }
// One frame of swimming.
Result step(State& s, const World& w, const Input& in);

}  // namespace swim
