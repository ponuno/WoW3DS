// Modern (split) ADT -> classic ADT image, so the existing terrain pipeline (WoWee's ADTLoader) reads WoW Forever tiles.
// A modern tile is several files: <map>_X_Y.adt (heights, normals, holes, water), _tex0.adt (texture layers + alpha, texture
// FileDataIDs in MDID), _obj0.adt (doodad / building placements; model ids are FileDataIDs).  Everything here is referenced by
// FileDataID, so the textures / models / buildings come out as pseudo paths "fdid\<n>.blp|.m2|.wmo" that a FileDataID-aware
// DataDir resolves.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace w3d {

// obj0 / tex0 may be empty (a tile without placements or layers).  Returns false (with err) when the root is not a modern ADT.
bool modernAdtToClassic(const std::vector<uint8_t>& root, const std::vector<uint8_t>& tex0, const std::vector<uint8_t>& obj0,
                        std::vector<uint8_t>& out, std::string& err);

// Chunked model file (MD21 + SFID / TXID ...) -> the plain MD20 image the model loader reads; textures with a TXID entry get
// the file name "fdid\\<n>.blp" so they resolve through the FileDataID reader.  skinId = SFID[0] (LOD 0), 0 when absent.
bool modernM2ToClassic(const std::vector<uint8_t>& file, std::vector<uint8_t>& out, uint32_t& skinId, std::string& err);

// Modern building root (no MOTX / MODN: materials hold texture FileDataIDs, MODI lists the doodad models) -> a root with MOTX / MODN
// carrying pseudo paths.  gfid = the GFID list (group file ids) for the caller.
bool modernWmoRootToClassic(const std::vector<uint8_t>& file, std::vector<uint8_t>& out, std::vector<uint32_t>& gfid, std::string& err);

// Modern building group file: MPY2 (u16 flags, u16 material per face) -> MOPY (u8, u8); the other sub-chunks are as in the old format.
bool modernWmoGroupToClassic(const std::vector<uint8_t>& file, std::vector<uint8_t>& out, std::string& err);

// "fdid\\123.m2" -> 123 (0 when the path is not a FileDataID pseudo path)
uint32_t fdidFromPseudoPath(const std::string& path);
std::string fdidPseudoPath(uint32_t id, const char* ext);

}  // namespace w3d
