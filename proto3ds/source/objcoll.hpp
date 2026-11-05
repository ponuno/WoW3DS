// objcoll.hpp — 6.59: collision of game objects that are buildings (OWMO entries: Turtle WoW's houses, towers, ruins).
// Pure maths (no 3DS headers), shared with the PC test.
//
// The object's triangles (its own space) become a COLL blob of the tiles' format (w3d_format.h W3dCollHeader) whose origin is the
// corner of their box - an object is far smaller than a tile, so the 533-yd grid of the format holds it - and the walker's queries
// go into the object's space (turned back by its yaw, moved, divided by its scale) and their answers come back out:
//   floor: the highest floor at or under the feet + a step (standing on a balcony, walking up its stairs);
//   walls: w3d_collide_move in the object's space (a turn keeps distances; the radius divides by the scale);
//   ceiling: the lowest face over the head.
#pragma once
#include <cstdint>
#include <vector>

namespace objcoll {

// tris: 9 floats each (object space). false: nothing to collide with.
bool build(const float* tris, uint32_t triCount, std::vector<uint8_t>& out);

struct Placed {
    const uint8_t* coll = nullptr;  // build()'s blob
    float pos[3] = {0, 0, 0};
    float yaw = 0, scale = 1;
    float radius = 0;               // in the world (scale applied)
};

// Highest floor of the object at (x, y) not higher than zFeet + step: true and *z (world).
bool floorAt(const Placed& p, float x, float y, float zFeet, float step, float* z);
// Lowest face over (x, y) higher than zFrom: true and *ceil (world).
bool ceilingAbove(const Placed& p, float x, float y, float zFrom, float* ceil);
// Walls: the move from -> *to of a walker (radius r) pushed out of the object's walls; true when it changed.
bool collideMove(const Placed& p, float fromX, float fromY, float zFeet, float r, float* toX, float* toY);

}  // namespace objcoll
