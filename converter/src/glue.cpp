// glue.cpp — the 3D scenes of the login and character screens (stage 5.29): Interface\Glues\Models\UI_*.m2, the Dark
// Portal of the login screen and one racial home per race for character selection and creation, each converted as one
// static MODL plus a GLUE entry with the camera and the character's mark the artist authored in the model.
//
// As WoWee's CharacterPreview does: camera 0 gives the view (position and target), attachment 0 the spot the character
// stands on; Gnome and Troll have no scene of their own in the game and use the Dwarf and Orc ones.
#include "conv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pipeline/m2_loader.hpp"

namespace w3d {

namespace {
struct SceneSrc { uint32_t id; const char* name; };
// id: W3D_GLUE_MAINMENU or a race (ChrRaces id)
const SceneSrc kScenes[] = {
    {W3D_GLUE_MAINMENU, "UI_MainMenu"},
    {1, "UI_Human"}, {2, "UI_Orc"}, {3, "UI_Dwarf"}, {4, "UI_NightElf"}, {5, "UI_Scourge"}, {6, "UI_Tauren"},
    {7, "UI_Dwarf"}, {8, "UI_Orc"},
};
}  // namespace

bool convertGlueScenes(const Options& oIn, const DataDir& data, PackWriter& pack, ConvStats& st) {
    Options o = oIn;
    o.maxModelTris = std::max(o.maxModelTris, 12000);   // a whole backdrop, seen up close: far more than a doodad
    o.modelTex = std::max(o.modelTex, 256);
    o.noDoodadColl = true;
    size_t ok = 0, missing = 0;
    std::map<std::string, W3dGlueScene> done;   // a scene shared by two races is converted once
    for (const SceneSrc& sc : kScenes) {
        const std::string path = std::string("Interface\\Glues\\Models\\") + sc.name + "\\" + sc.name + ".m2";
        auto it = done.find(path);
        W3dGlueScene g{};
        if (it != done.end()) {
            g = it->second;
        } else {
            std::vector<uint8_t> bytes;
            if (!data.read(path, bytes)) {
                ++missing;
                if (o.verbose) std::fprintf(stderr, "  glue scene missing: %s\n", path.c_str());
                done[path] = g;
                continue;
            }
            wowee::pipeline::M2Model mdl = wowee::pipeline::M2Loader::load(bytes);
            g.modelId = addModelFromGame(pack, data, path, o, st);
            if (!g.modelId) {
                std::fprintf(stderr, "  glue scene %s: the model did not convert\n", path.c_str());
                done[path] = g;
                continue;
            }
            float r2 = 0;
            for (const auto& v : mdl.vertices) r2 = std::max(r2, v.position.x * v.position.x + v.position.y * v.position.y +
                                                                    v.position.z * v.position.z);
            g.radius = std::sqrt(r2);
            if (!mdl.cameras.empty()) {
                const auto& c = mdl.cameras[0];
                for (int k = 0; k < 3; ++k) { g.camPos[k] = c.positionBase[k]; g.camTarget[k] = c.targetBase[k]; }
                g.fov = c.fov;
                g.nearClip = c.nearClip;
                g.farClip = c.farClip;
                g.flags |= W3D_GLUE_HAS_CAMERA;
            }
            for (const auto& a : mdl.attachments)
                if (a.id == 0) {
                    g.stand[0] = a.position.x; g.stand[1] = a.position.y; g.stand[2] = a.position.z;
                    g.flags |= W3D_GLUE_HAS_STAND;
                    break;
                }
            if (!(g.flags & W3D_GLUE_HAS_STAND) && (g.flags & W3D_GLUE_HAS_CAMERA)) {   // no mark: on the camera's target
                for (int k = 0; k < 3; ++k) g.stand[k] = g.camTarget[k];
                g.flags |= W3D_GLUE_HAS_STAND;
            }
            done[path] = g;
            ++ok;
            std::printf("  glue scene %-48s radius %6.1f, camera %s (fov %.2f) at (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), stand %s"
                        " (%.1f %.1f %.1f)\n", path.c_str(), g.radius, (g.flags & W3D_GLUE_HAS_CAMERA) ? "yes" : "no", g.fov,
                        g.camPos[0], g.camPos[1], g.camPos[2], g.camTarget[0], g.camTarget[1], g.camTarget[2],
                        (g.flags & W3D_GLUE_HAS_STAND) ? "yes" : "no", g.stand[0], g.stand[1], g.stand[2]);
        }
        if (!g.modelId) continue;
        std::vector<uint8_t> blob(sizeof g);
        std::memcpy(blob.data(), &g, sizeof g);
        pack.add(W3D_ENTRY_GLUE, sc.id, std::move(blob));
    }
    std::printf("  glue scenes: %zu converted, %zu missing (login / character screens%s)\n", ok, missing,
                ok ? "" : ": the 3DS draws plain backgrounds");
    return ok > 0;
}

}  // namespace w3d
