// skybox.cpp — 6.61: the sky models of LightSkybox.dbc (the original's clouds, stars and horizon of a zone: Light.dbc -> LightParams ->
// LightSkybox -> an M2 dome around the viewer) as MODL entries, found by SKYB entries (id = the LightSkybox id).
#include <algorithm>
#include <cstdio>

#include "conv.h"

namespace w3d {

size_t convertSkyboxes(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st) {
    DbcTable sb;
    if (!sb.load(data, "LightSkybox.dbc") || sb.fields < 2) return 0;
    Options so = o;
    so.itemModelTris = 1500;    // a dome is a few hundred triangles; the default of a sword (300) would cut it
    so.itemTex = std::max(o.itemTex, 256);
    size_t ok = 0, failed = 0;
    for (uint32_t r = 0; r < sb.records; ++r) {
        const uint32_t id = sb.u32(r, 0);
        const std::string name = sb.str(r, 1);
        if (!id || name.empty()) continue;
        const uint32_t model = addItemModel(pack, data, name, "", so, st);
        if (!model) { ++failed; if (o.verbose) std::fprintf(stderr, "  sky model missing or empty: %s\n", name.c_str()); continue; }
        W3dSkybox e{};
        e.modelId = model;
        Blob b;
        b.put(e);
        if (pack.add(W3D_ENTRY_SKYBOX, id, std::move(b.data))) ++ok;
    }
    std::printf("  sky models: %zu written (%zu failed)\n", ok, failed);
    return ok;
}

}  // namespace w3d
