// creature_model.h — M2 (parsed by WoWee) -> CMDL blob: skinned mesh split
// into bone palettes + animation baked at W3D_ANIM_FPS. No game-data access
// here (textures are resolved through a callback), so tests can feed
// synthetic M2Model objects.
#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "pipeline/m2_loader.hpp"
#include "w3d_format.h"

namespace w3d {

struct CreatureBuildInfo {
    size_t tris = 0, verts = 0, palettes = 0, bones = 0, seqs = 0, keyBytes = 0, flippedSubs = 0, droppedSeqs = 0;
    size_t srcVerts = 0, lossyVerts = 0;   // vertices whose 3rd+4th influences carried > 10% of the weight
    std::set<uint16_t> geosets;            // submesh ids present (for per-display geoset selection)
    bool needsAlpha[W3D_CTEX_KINDS] = {};   // per W3dCreatureTexKind 0..6
    std::string error;
};

// fixedTex(filename, needsAlpha) -> TEX id (0 = failed) for type-0 (hard-coded) textures.
std::vector<uint8_t> buildCreatureBlob(const wowee::pipeline::M2Model& m, int maxTris,
                                       const std::function<uint32_t(const std::string&, bool)>& fixedTex,
                                       CreatureBuildInfo& info, size_t animBudgetBytes = 0);

// Animation ids baked (first variation of each), in priority order: stand, death, walk, run, the attacks,
// swimming (always kept); then wounds, ready stances, strafing, parry / dodge, spells, jumping, riding,
// sitting / sleeping / kneeling, emotes. animBudgetBytes (0 = all) drops the non-essential ones that do not fit.
const std::vector<uint16_t>& creatureAnimIds();
bool creatureAnimEssential(uint16_t id);

}  // namespace w3d
