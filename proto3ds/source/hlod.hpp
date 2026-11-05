// hlod.hpp — 6.57: the horizon (map<ID>.hlod next to the map pack, made by `w3docc --hlod`; format in common/w3d_hlod.h).
// A block (133 yd) of a tile farther than the switch distance is drawn as one coarse mesh of its buildings and large doodads, and its
// members (the placements it stands for) are skipped by the renderer (Instance::hblock, Tile::hlodFar); past the draw distance, up to
// the horizon, the blocks of tiles that are not loaded at all are drawn too. Tiles' meshes are read by a thread of their own.
#pragma once
#include <cstdint>
#include <vector>

#include "w3d_format.h"

struct Tile;
struct RenderSettings;
struct GpuTex;

namespace hlod {

// Opens map<ID>.hlod next to the pack (none / damaged / another conversion: off). Main thread, with the pack.
bool load(const char* packPath, uint32_t buildId);
void unload();
bool loaded();
const char* status();   // a line for the log

struct Draw {
    const W3dTerrainVertex* verts;
    const uint16_t* idx;
    uint32_t count;       // indices
    float origin[3];      // the vertices are relative to it, in W3D_HLOD_POS_UNIT
    float center[3], radius;
};
struct FrameInfo {
    uint16_t tiles = 0;     // tiles whose meshes are in memory
    uint16_t blocks = 0;    // blocks drawn as their mesh this frame
    uint16_t hidden = 0;    // far blocks the PVS says cannot be seen (neither mesh nor members drawn)
    uint32_t tris = 0;      // triangles of the meshes drawn
    uint32_t bytes = 0;     // linear memory of the meshes
    bool read = false;      // a tile was asked for this frame
};
// Decides which blocks are far this frame (after pvs::frame: it uses the PVS stamps): marks Tile::hlodFar for the loaded tiles' far
// blocks and fills rs.hlodStamp; switchDist 0 = off (nothing far, no meshes). 'reach' = the farthest anything is drawn (the
// horizon or the draw distance).
FrameInfo frame(const float cam[3], float reach, float switchDist, const std::vector<Tile*>& tiles, RenderSettings& rs);
const std::vector<Draw>& draws();   // this frame's meshes (frame() fills it)
GpuTex* palette();                  // the colours (null until loaded)
bool coversTile(int tx, int ty);    // the tile's meshes are in: its far building shells (FARO) are not needed

}  // namespace hlod
