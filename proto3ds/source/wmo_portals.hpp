// wmo_portals.hpp — 6.42: which interior groups of a WMO are seen. Pure maths (no 3DS headers), shared with the PC test.
//
// The rules are WoWee's (wmo_renderer.cpp getVisibleGroupsViaPortals, tuned on Stormwind, Ironforge and Orgrimmar), because a wrong
// answer here does not draw a little too much, it makes a city vanish:
//  * culling only when the camera is surely indoors: the smallest group box holding it is an interior-only group with portals, and
//    no outdoor group's box holds it too (doorways, awnings, streets beside a building whose room box spills out);
//  * then, in that building only (its portal-connected groups), interior groups are drawn when the walk reaches them - seeded from the
//    camera's group, the character's group and every outdoor group of the building - through portals inside the view;
//  * otherwise nothing is culled (the caller keeps its old rule).
#pragma once
#include <stdint.h>

#include <vector>

#include "w3d_format.h"

namespace wmoportal {

constexpr uint32_t kNone = 0xFFFFFFFFu;

// One placed building group (a WMO group instance) of a tile.
struct Node {
    uint32_t modelId = 0;
    const W3dPortal* portals = nullptr;   // the group's portals (null / 0: none)
    uint32_t nPortals = 0;
    bool hasTable = false;                // the model carries a portal table (a pack from 6.42 on)
    bool interior = false;                // interior-only group
    float pos[3] = {0, 0, 0};             // the group's centre in the world
    float rot[3] = {0, 0, 0};             // the placement's turn (T * Rz * Ry * Rx)
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};   // model-space bounds around the centre
    float radius = 0;
    uint32_t inst = 0;                    // index of the instance in its tile
    uint32_t comp = kNone;                // the building: groups joined by portals share it (buildGraph)
    uint32_t stamp = 0;                   // == the frame it was reached in
    std::vector<uint32_t> next;           // the node behind each portal (kNone: not found), buildGraph
};

// v turned by the placement's rotation (Rz * Ry * Rx) / by the inverse.
void rotate(const float rot[3], const float v[3], float out[3]);
void unrotate(const float rot[3], const float v[3], float out[3]);

// Is the point inside the group's box (a margin around it)?
bool inside(const Node& n, const float p[3], float margin);

// Links every portal to the node behind it (the one with that model nearest to where the portal puts it: a tile can hold several
// copies of a WMO) and numbers the buildings (connected groups). Once, when the tile's groups are in.
void buildGraph(std::vector<Node>& nodes);

typedef bool (*SphereFn)(void* ctx, const float centre[3], float radius);   // is the sphere in view?

// The walk of one frame. Returns the building being culled (Node::comp), or kNone: nothing is culled this frame. When a building is
// culled, its nodes reached have stamp == frame; an interior node of that building with another stamp is hidden.
// viewer: the character's position (null: none).
uint32_t reach(std::vector<Node>& nodes, const float cam[3], const float* viewer, SphereFn inView, void* ctx, uint32_t frame);

}  // namespace wmoportal
