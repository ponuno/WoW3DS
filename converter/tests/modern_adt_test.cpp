// modern_adt_test <dump dir>: a real WoW Forever tile (Azeroth_32_48 root + tex0 + obj0) through the classic ADT reader.
#include <cstdio>
#include <map>
#include <cstring>
#include <string>
#include <vector>
#include "conv.h"
#include "modern_adt.h"
#include "pipeline/adt_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/blp_loader.hpp"

static std::vector<uint8_t> load(const std::string& p) {
    std::vector<uint8_t> b;
    if (FILE* f = fopen(p.c_str(), "rb")) { fseek(f, 0, SEEK_END); b.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET); if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear(); fclose(f); }
    return b;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(int argc, char** argv) {
    const std::string d = argc > 1 ? argv[1] : ".";
    auto root = load(d + "/Azeroth_32_48_root.adt");
    if (root.empty()) { printf("modern ADT: SKIP (no %s/Azeroth_32_48_root.adt)\n", d.c_str()); return 0; }
    auto tex = load(d + "/Azeroth_32_48_tex0.adt"), obj = load(d + "/Azeroth_32_48_obj0.adt");
    std::vector<uint8_t> classic; std::string err;
    CHECK(w3d::modernAdtToClassic(root, tex, obj, classic, err), "convert: %s", err.c_str());
    auto t = wowee::pipeline::ADTLoader::load(classic);
    CHECK(t.isLoaded(), "ADTLoader");
    CHECK(t.textures.size() == 7 && w3d::fdidFromPseudoPath(t.textures[0]) > 0, "textures %zu", t.textures.size());
    CHECK(t.doodadPlacements.size() == 785, "doodads %zu", t.doodadPlacements.size());
    CHECK(t.wmoPlacements.size() == 8 && !t.wmoNames.empty() && w3d::fdidFromPseudoPath(t.wmoNames[t.wmoPlacements[0].nameId]) == 7962574, "buildings %zu", t.wmoPlacements.size());
    if (!t.doodadPlacements.empty()) CHECK(w3d::fdidFromPseudoPath(t.doodadNames[t.doodadPlacements[0].nameId]) == 7549691, "first doodad id");
    const auto& c0 = t.getChunk(0, 0);
    CHECK(c0.hasHeightMap() && c0.areaId == 12, "chunk 0 height %d area %u", (int)c0.hasHeightMap(), c0.areaId);
    CHECK(c0.position[2] > 100.f && c0.position[2] < 400.f, "height base %f", c0.position[2]);
    size_t withLayers = 0, withAlpha = 0, water = 0;
    for (int i = 0; i < 256; ++i) { withLayers += t.chunks[i].hasLayers(); withAlpha += !t.chunks[i].alphaMap.empty(); water += t.waterData[i].hasWater(); }
    CHECK(withLayers == 256, "layers in %zu chunks", withLayers);
    printf("tile: %zu textures, %zu doodads, %zu buildings, layers %zu/256, alpha maps %zu, chunks with water %zu\n",
           t.textures.size(), t.doodadPlacements.size(), t.wmoPlacements.size(), withLayers, withAlpha, water);
    {   // through DataDir: WDT (MAID) -> split files by FileDataID -> classic ADT; ids as in the data probe's report
        w3d::DataDir dd;
        const std::map<uint32_t, std::string> files = {{775971, "Azeroth.wdt"}, {778027, "Azeroth_32_48_root.adt"}, {778028, "Azeroth_32_48_obj0.adt"},
                                                        {778030, "Azeroth_32_48_tex0.adt"}, {7549691, "model_7549691.m2"}, {7549764, "model_7549691_00.skin"}};
        dd.setForever([&](uint32_t id, std::vector<uint8_t>& o) { auto f = files.find(id); if (f == files.end()) return false; o = load(d + "/" + f->second); return !o.empty(); },
                      {{"azeroth", 775971}});
        std::vector<uint8_t> a, w, m, none;
        CHECK(dd.read("World\\Maps\\Azeroth\\Azeroth_32_48.adt", a) && a == classic, "DataDir tile == adapter output");
        CHECK(dd.read("World\\Maps\\Azeroth\\Azeroth.wdt", w) && w.size() == 294988, "DataDir wdt");
        CHECK(dd.read("fdid\\7549691.m2", m) && m.size() > 23056 && !memcmp(m.data(), "MD20", 4), "DataDir fdid model: MD20, %zu bytes", m.size());
        auto mdl = wowee::pipeline::M2Loader::load(m);
        CHECK(mdl.vertices.size() == 440 && mdl.textures.size() == 2 && mdl.textures[0].filename == "fdid\\7743407.blp", "model: %zu vertices, texture '%s'",
              mdl.vertices.size(), mdl.textures.empty() ? "" : mdl.textures[0].filename.c_str());
        std::vector<uint8_t> sk;
        CHECK(dd.read(wowee::pipeline::skinPathForM2("fdid\\7549691.m2"), sk) && wowee::pipeline::M2Loader::loadSkin(sk, mdl) && mdl.batches.size() == 2 && mdl.indices.size() == 366, "skin by the model's path");
        CHECK(!dd.read("World\\Maps\\Azeroth\\Azeroth_0_0.adt", none), "a tile without files is absent");
    }
    {   // a building root: texture / doodad model FileDataIDs become names, GFID lists the groups
        auto wmo = load(d + "/building_7962574.wmo");
        if (!wmo.empty()) {
            std::vector<uint8_t> plain; std::vector<uint32_t> gfid;
            CHECK(w3d::modernWmoRootToClassic(wmo, plain, gfid, err), "wmo: %s", err.c_str());
            CHECK(gfid.size() == 1156 && gfid[0] == 7964530, "GFID %zu", gfid.size());
            auto m = wowee::pipeline::WMOLoader::load(plain);
            CHECK(m.materials.size() == 195 && m.nGroups == 289, "materials %zu groups %u", m.materials.size(), m.nGroups);
            CHECK(!m.textures.empty() && w3d::fdidFromPseudoPath(m.textures[0]) == 8180074, "textures %zu '%s'", m.textures.size(), m.textures.size() > 0 ? m.textures[0].c_str() : "");
            CHECK(m.doodadSets.size() == 1 && !m.doodads.empty(), "doodad sets %zu doodads %zu", m.doodadSets.size(), m.doodads.size());
            std::vector<uint8_t> g, gp;
            const std::string gdir = argc > 2 ? argv[2] : d;
            g = load(gdir + "/building_7962574_g0.wmo");
            if (!g.empty()) {
                CHECK(w3d::modernWmoGroupToClassic(g, gp, err), "group: %s", err.c_str());
                wowee::pipeline::WMOModel mm = m;
                const bool ok = wowee::pipeline::WMOLoader::loadGroup(gp, mm, 0);
                printf("group 0 loaded: %d, %zu vertices\n", (int)ok, mm.groups.empty() ? (size_t)0 : mm.groups[0].vertices.size());
                CHECK(ok && mm.groups.size() > 0 && mm.groups[0].vertices.size() == 302 && mm.groups[0].indices.size() == 1476 && mm.groups[0].batches.size() == 5,
                      "group 0: ok %d vertices %zu indices %zu batches %zu", ok, mm.groups.empty() ? 0 : mm.groups[0].vertices.size(),
                      mm.groups.empty() ? 0 : mm.groups[0].indices.size(), mm.groups.empty() ? 0 : mm.groups[0].batches.size());
            }
            if (!m.doodads.empty()) printf("wmo: %zu materials, %zu textures, %zu doodads, first doodad '%s'\n", m.materials.size(), m.textures.size(), m.doodads.size(), m.doodadNames.empty() ? "" : "(names)");
        }
    }
    {   // a modern BLP2 (DXT5, 256x256) from a building material
        auto b = load((argc > 2 ? std::string(argv[2]) : d) + "/building_tex_8180074.blp");
        if (!b.empty()) {
            auto img = wowee::pipeline::BLPLoader::load(b);
            CHECK(img.width == 256 && img.height == 256 && !img.data.empty(), "BLP2: %dx%d", img.width, img.height);
        }
    }
    printf("modern ADT: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
