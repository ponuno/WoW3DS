// static_batch.hpp — 6.44: small static doodads of one patch of ground merged into shared buffers (pure maths, PC-tested).
//
// Every instance of every part of a model used to be its own draw call (its matrix, its light, a texture bind per part): ~900 draws a
// frame in Stormwind, ~8 ms of citro3d's CPU time. The small doodads (radius < 12 yd, opaque or cut-out, no effects, no moving
// textures) of a 32 x 32 yd cell, of one size class and one owning building, are put into shared vertex / index buffers once - one per
// texture and part flags - in the cell's frame (the instance's turn, scale and position baked in, normals turned, light in world
// space). The renderer draws such a group with one call per texture while the whole group lies inside the doodads' solid distance
// (no instance fading at its edge); otherwise its members are drawn one by one as before.
#pragma once
#include <stdint.h>

#include <vector>

#include "w3d_format.h"

namespace sbatch {

constexpr float kCell = 32.0f;          // yards
constexpr float kMaxRadius = 12.0f;     // larger doodads stay single (they reach far and have their own LODs)
constexpr uint32_t kMaxModelVerts = 4096;
constexpr int kLevels = 3;              // full + W3D_MODEL_LODS lighter levels

// The size class of a doodad by its radius (members of a group share one, so the group's LOD and distance suit all of them), or -1.
int bucketOf(float radius);

// Can this model take part? (every part opaque or cut-out; a texture or none; a modest mesh)
bool eligibleModel(const uint8_t* blob, uint32_t size);

struct MemberIn {
    const uint8_t* blob;                // the model (W3dModelHeader at the start)
    float pos[3], rot[3], scale;        // the placement (T * Rz * Ry * Rx * S)
    const uintptr_t* texKeys;           // per submesh: the texture it is drawn with (any identity; 0 = untextured)
};

struct BatchOut {
    uintptr_t tex = 0;
    uint8_t blend = 0, flags = 0;       // W3dSubmesh blend / flags of its parts
    std::vector<W3dModelVertex> v;      // positions around 'offset' in units of 'scale', relative to the group origin
    std::vector<uint16_t> idx;          // level k: idx[first[k] .. + count[k]]
    uint32_t first[kLevels] = {0, 0, 0}, count[kLevels] = {0, 0, 0};
    float scale[3] = {1, 1, 1}, offset[3] = {0, 0, 0}, uvScale = W3D_MODEL_UV_SCALE;
};

// Merges the members' parts by (texture, blend, flags); positions relative to 'origin'. A batch never has more than 65535 vertices
// (it is split). Level k of a member is its lightest level up to k that exists (as the single draw picks it).
std::vector<BatchOut> build(const std::vector<MemberIn>& members, const float origin[3]);

// Rotation of a placement (Rz * Ry * Rx) applied to v.
void turn(const float rot[3], const float v[3], float out[3]);

}  // namespace sbatch
