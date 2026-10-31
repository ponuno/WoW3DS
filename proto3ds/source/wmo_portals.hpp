// wmo_portals.hpp — 6.42: which interior groups of a WMO are seen: from the group(s) the camera is in, through every portal that
// faces the camera and is in view, to the group behind it, and on from there. Pure maths (no 3DS headers), shared with the PC test.
#pragma once
#include <stdint.h>

#include <vector>

#include "w3d_format.h"

namespace wmoportal {

// One placed building group (a WMO group instance) of a tile.
struct Node {
    uint32_t modelId = 0;
    const W3dPortal* portals = nullptr;   // the group's portals (null / 0: none)
    uint32_t nPortals = 0;
    bool hasTable = false;                // the model carries a portal table (a pack from 6.42 on)
    bool interior = false;                // interior-only group: drawn only when reached
    float pos[3] = {0, 0, 0};             // the group's centre in the world
    float rot[3] = {0, 0, 0};             // the placement's turn (T * Rz * Ry * Rx)
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};   // model-space bounds around the centre
    float radius = 0;
    uint32_t inst = 0;                    // index of the instance in its tile
    uint32_t stamp = 0;                   // == the frame it was reached in
};

// v turned by the placement's rotation (Rz * Ry * Rx) / by the inverse.
void rotate(const float rot[3], const float v[3], float out[3]);
void unrotate(const float rot[3], const float v[3], float out[3]);

// Is the camera inside the group's box (a margin around it)?
bool cameraInside(const Node& n, const float cam[3], float margin);

// Indices of the nodes sorted by modelId (for the neighbour lookup).
std::vector<uint32_t> buildIndex(const std::vector<Node>& nodes);

typedef bool (*SphereFn)(void* ctx, const float centre[3], float radius);   // is the sphere in view?

// Marks (stamp = frame) every node that is reached. Seeds: the groups whose box holds the camera; none: the exterior groups
// (not 'interior') near and in view. A seed's portals are tested for facing; every portal for view and distance.
// Returns the number of nodes reached.
uint32_t reach(std::vector<Node>& nodes, const std::vector<uint32_t>& byModel, const float cam[3], SphereFn inView, void* ctx,
               float maxDist, uint32_t frame);

}  // namespace wmoportal
