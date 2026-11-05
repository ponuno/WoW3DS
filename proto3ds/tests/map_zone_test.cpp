// 6.94: which zone map shows the player (w3dUiPickZoneMap): the zone he stands in decides, not the smallest overlapping rectangle.
#include <cstdio>
#include <cstring>
#include "w3d_ui.h"
static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static W3dUiMap mk(uint32_t id, uint32_t map, uint32_t area, float left, float right, float top, float bottom) {
    W3dUiMap m; memset(&m, 0, sizeof m);
    m.id = id; m.mapId = map; m.areaId = area; m.left = left; m.right = right; m.top = top; m.bottom = bottom;
    return m;
}
int main() {
    // left/right are world y (left > right), top/bottom are world x (top > bottom): two neighbours that overlap, a continent
    const uint32_t WPL = 28, EPL = 139, CONT = 0;
    W3dUiMap maps[] = {
        mk(1, 0, CONT, 18000, -18000, 5000, -15000),
        mk(2, 0, EPL, -1000, -4000, 3000, 0),        // big
        mk(3, 0, WPL, -1500, -4000, 2000, 500),     // smaller, overlaps EPL's west part
        mk(4, 1, 17, 1000, -1000, 1000, -1000),     // other continent, same ids are not mixed up
    };
    const size_t n = sizeof maps / sizeof maps[0];
    // a point of Eastern Plaguelands that also lies in Western's rectangle: x = 1000, y = -2000
    CHECK(w3dUiPickZoneMap(maps, n, 0, 0, 1000, -2000) == 2, "old rule: the smallest rectangle (Western)");
    CHECK(w3dUiPickZoneMap(maps, n, 0, EPL, 1000, -2000) == 1, "the zone we stand in (Eastern) wins over the smaller rectangle");
    CHECK(w3dUiPickZoneMap(maps, n, 0, WPL, 1000, -2000) == 2, "Western when we are in Western");
    // a zone the pack has no map for: the smallest rectangle
    CHECK(w3dUiPickZoneMap(maps, n, 0, 9999, 1000, -2000) == 2, "unknown zone: smallest rectangle");
    // another map: the zone id is only looked up on that map
    CHECK(w3dUiPickZoneMap(maps, n, 1, EPL, 0, 0) == 3, "map 1: EPL does not exist there, rectangle 4 holds the point");
    CHECK(w3dUiPickZoneMap(maps, n, 0, EPL, 100000, 100000) == 1, "the zone wins even for a point outside (the arrow is simply not drawn)");
    CHECK(w3dUiPickZoneMap(maps, n, 0, 0, 100000, 100000) == -1, "outside every zone: none (the continent)");
    CHECK(w3dUiPickZoneMap(maps, n, 0, CONT, 100000, 100000) == -1, "zone 0 / continent id never picks the continent entry as a zone");
    std::printf("map_zone_test: %d checks, %d failed\n", g_ok + g_fail, g_fail);
    return g_fail != 0;
}
