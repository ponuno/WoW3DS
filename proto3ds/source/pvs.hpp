// pvs.hpp — 6.54: the precomputed visibility of the map (map<ID>.pvs next to the map pack, made by `w3docc --pvs`; format in
// common/w3d_pvs.h). Each frame the camera's cell and height level say which doodads and ground batches can be seen from there;
// the rest of the covered tiles' doodads / batches are skipped by the renderer (RenderSettings::pvsOn / pvsStamp).
#pragma once
#include <cstdint>
#include <vector>

struct Tile;
struct RenderSettings;

namespace pvs {

// Opens map<ID>.pvs next to the pack (none, damaged or from another conversion: off). Main thread, with the pack.
bool load(const char* packPath, uint32_t buildId);
void unload();
bool loaded();
float occRange();   // 6.72: the smallest doodad distance (rs.doodadRange) the loaded file holds for; 0 when none is loaded
const char* status();   // a line for the log: what was loaded / why not

struct FrameInfo {
    bool on = false;        // the set is used this frame
    uint8_t why = 0;        // 0 on; 1 no file; 2 switched off; 3 settings outside its range; 4 no cell / level here; 5 tiles around not complete;
                            // 6 the camera's cell still being read (6.54c)
    uint16_t levels = 0;    // levels used
    uint32_t marked = 0;    // doodads + batches marked visible
    bool farHidden = false; // 6.54d: nothing past the draw distance can be seen from here (the far ground / shells skipped)
    bool read = false;      // 6.54c: the camera's cell was not in the cache this frame (asked for: read by the reader thread)
};
// Decides and marks (Instance::pvsStamp, Tile::pvsStamp / pvsTerr) for this frame; fills rs.pvs* . 'enabled': pad.txt / GPU test.
FrameInfo frame(const float cam[3], float drawDist, const std::vector<Tile*>& tiles, bool enabled, RenderSettings& rs);

}  // namespace pvs
