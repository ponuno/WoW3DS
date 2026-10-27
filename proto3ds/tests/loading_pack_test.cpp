// ui.w3dp with loading screens: loadingScreen(map) of the converter test data (run_dungeon_tests.sh makes the pack).
#include <cstdio>
#include "ui_assets.hpp"
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    char err[160];
    ui::Assets a;
    if (!a.open(argv[1], err, sizeof err)) { std::printf("open: %s\n", err); return 1; }
    int fails = 0;
    for (uint32_t m : {36u, 0u, 1u, 13u, 30u, 33u}) {
        ui::Image im = a.loadingScreen(m);
        int w = 0, h = 0; uint8_t f = 0, k = 0;
        bool ok = im.ok() && im.w == 512 && im.h == 256 && a.texInfo(im.tex, &w, &h, &f, &k) && w == 512 && h == 256;
        if (!ok) { std::printf("map %u: no loading screen\n", m); ++fails; }
    }
    if (a.loadingScreen(99).ok()) { std::printf("map 99 must have none\n"); ++fails; }
    std::printf("LOADING PACK %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
