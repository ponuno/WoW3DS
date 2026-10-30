// creature_valid_test — 6.35: w3d_creature_valid accepts every CMDL the converter writes and refuses a damaged one.
//   creature_valid_test <pack.w3dp>...
#include "w3d_format.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    int models = 0, bad = 0, refusedDamaged = 0;
    for (int a = 1; a < argc; ++a) {
        std::ifstream f(argv[a], std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
        if (b.size() < sizeof(W3dPackHeader)) continue;
        W3dPackHeader ph;
        std::memcpy(&ph, b.data(), sizeof ph);
        const W3dEntry* e = (const W3dEntry*)(b.data() + ph.indexOffset);
        for (uint32_t i = 0; i < ph.entryCount; ++i) {
            if (e[i].type != W3D_ENTRY_CREATURE) continue;
            std::vector<uint8_t> blob(b.begin() + e[i].offset, b.begin() + e[i].offset + e[i].size);
            ++models;
            if (!w3d_creature_valid(blob.data(), (uint32_t)blob.size())) { ++bad; std::printf("refused: entry %08x\n", e[i].id); continue; }
            const W3dCreatureHeader* h = (const W3dCreatureHeader*)blob.data();
            if (h->paletteCount) {   // a damaged palette: 25 bones
                W3dBonePalette* p = (W3dBonePalette*)(blob.data() + h->offPalettes);
                p[0].count = W3D_PALETTE_BONES + 1;
                refusedDamaged += !w3d_creature_valid(blob.data(), (uint32_t)blob.size());
                p[0].count = 0xFFFF;
                refusedDamaged += !w3d_creature_valid(blob.data(), (uint32_t)blob.size());
                refusedDamaged -= 1;   // (two checks per model count once)
            }
        }
    }
    const bool ok = models > 0 && bad == 0 && refusedDamaged == models;
    std::printf("creature models: %d, refused %d; damaged palettes refused in %d\nCREATURE VALID: %s\n", models, bad, refusedDamaged, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
