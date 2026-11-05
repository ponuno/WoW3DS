// sky_test — 6.60: sky.txt (w3dconv --texts-only over converter/tests/gen_sky_data.py's Light.dbc) through the client's sky::SkyTable:
//   the map's global light at midnight / dawn / noon / dusk (a curve wrapping through the day), the zone light inside and between its
//   radii (the smoothstep share), outside it, another map, a map without lights; the server's clock (sky::clockHour) across midnight.
//   sky_test <sky.txt>
#include "../source/sky.hpp"

#include <cmath>
#include <cstdio>

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool near3(const sky::Rgb& c, float r, float g, float b, float tol = 1.0f) {
    return std::fabs(c.r - r) <= tol && std::fabs(c.g - g) <= tol && std::fabs(c.b - b) <= tol;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: sky_test <sky.txt>"); return 2; }
    sky::SkyTable t;
    CHECK(!t.opened() && !t.setMap(0), "nothing before open");
    CHECK(t.open(argv[1]), "open");
    CHECK(!sky::SkyTable().open("/nonexistent/sky.txt"), "a missing file");
    CHECK(t.setMap(0) && t.volumes() == 2, "map 0: %zu volumes", t.volumes());
    sky::Colors c;
    // the global light (far from the zone): the top goes midnight (0, 0, 16) -> noon (70, 120, 200) -> midnight again
    CHECK(t.colors(0, 0, 12.0f, &c) && near3(c.top, 70, 120, 200) && near3(c.fog, 190, 210, 230), "noon: top %.0f %.0f %.0f", c.top.r, c.top.g, c.top.b);
    CHECK(t.colors(0, 0, 0.0f, &c) && near3(c.top, 0, 0, 16) && near3(c.mid, 110, 155, 215), "midnight: top %.0f %.0f %.0f", c.top.r, c.top.g, c.top.b);
    CHECK(t.colors(0, 0, 6.0f, &c) && near3(c.top, 35, 60, 108), "dawn (halfway up): %.0f %.0f %.0f", c.top.r, c.top.g, c.top.b);
    CHECK(t.colors(0, 0, 18.0f, &c) && near3(c.top, 35, 60, 108), "dusk (halfway down, through the wrap): %.0f %.0f %.0f", c.top.r, c.top.g, c.top.b);
    CHECK(t.colors(0, 0, 21.0f, &c) && near3(c.top, 17.5f, 30, 62), "21:00: %.1f %.1f %.1f", c.top.r, c.top.g, c.top.b);
    CHECK(t.colors(0, 0, 36.0f, &c) && near3(c.top, 70, 120, 200), "an hour past 24 wraps (36 = noon)");
    // the zone light at (-8800, -100): inner 100, outer 300 (constant red)
    CHECK(t.colors(-8800, -100, 0.0f, &c) && near3(c.top, 200, 40, 40) && near3(c.fog, 120, 80, 80), "inside the zone: %.0f %.0f %.0f", c.top.r, c.top.g, c.top.b);
    CHECK(t.colors(-8800 + 80, -100, 3.0f, &c) && near3(c.top, 200, 40, 40), "inside the inner radius");
    // 200 yd from the centre: halfway between the radii, smoothstep 0.5 -> half the zone, half the global light (at noon 70 120 200)
    CHECK(t.colors(-8800 + 200, -100, 12.0f, &c) && near3(c.top, 135, 80, 120, 1.5f), "between the radii: %.1f %.1f %.1f", c.top.r, c.top.g, c.top.b);
    CHECK(t.colors(-8800 + 400, -100, 12.0f, &c) && near3(c.top, 70, 120, 200), "outside the zone: the global light");
    // 6.61: the sky models: the zone's light has one (LightSkybox 5), the global light of the map none
    CHECK(t.skyboxAt(-8800, -100) == 5, "the zone's sky model: %u", t.skyboxAt(-8800, -100));
    CHECK(t.skyboxAt(-8800 + 100, -100) == 5, "at the inner radius");
    CHECK(t.skyboxAt(-8800 + 400, -100) == 0, "outside the zone: the global light has none");
    CHECK(t.skyboxAt(-8800 + 250, -100) == 0, "less than half inside the zone: the global light");
    // another map
    CHECK(t.setMap(1) && t.volumes() == 1, "map 1");
    CHECK(t.skyboxAt(0, 0) == 6, "map 1: its global light's sky model");
    CHECK(t.colors(-8800, -100, 9.0f, &c) && near3(c.top, 20, 200, 20) && near3(c.fog, 60, 120, 60), "map 1: its own global light");
    CHECK(!t.setMap(5) && !t.colors(0, 0, 12.0f, &c), "a map without lights: no colours (the caller keeps its sky)");
    CHECK(t.setMap(0) && t.colors(0, 0, 12.0f, &c), "back to map 0");
    // the server's clock: 23:30 and the speed of 1 game minute per minute (1/60 per second)
    const float base = 23 * 60 + 30;
    CHECK(std::fabs(sky::clockHour(base, 0, 1.0f / 60) - 23.5f) < 1e-3f, "at the packet");
    CHECK(std::fabs(sky::clockHour(base, 1800, 1.0f / 60) - 0.0f) < 1e-3f, "30 minutes later: midnight (%.3f)", sky::clockHour(base, 1800, 1.0f / 60));
    CHECK(std::fabs(sky::clockHour(base, 7200, 1.0f / 60) - 1.5f) < 1e-3f, "two hours later: 1:30");
    CHECK(std::fabs(sky::clockHour(base, 86400.0 * 3, 1.0f / 60) - 23.5f) < 1e-2f, "three days later: the same time");
    CHECK(std::fabs(sky::clockHour(0, 60, 0.0f) - 1.0f / 60.0f) < 1e-4f, "a nonsense speed falls back to 1/60");
    // 6.61: the sun and the moon
    {
        const sky::Body noon = sky::sun(12.0f), rise = sky::sun(6.0f), set = sky::sun(18.0f), night = sky::sun(0.0f);
        CHECK(noon.dir[2] > 0.9f && noon.strength == 1.0f, "the sun is high at noon (z %.2f)", noon.dir[2]);
        CHECK(rise.dir[1] < -0.95f && std::fabs(rise.dir[2]) < 0.02f && rise.strength < 0.5f, "sunrise in the east (y %.2f, z %.2f, s %.2f)", rise.dir[1], rise.dir[2], rise.strength);
        CHECK(set.dir[1] > 0.95f && std::fabs(set.dir[2]) < 0.02f, "sunset in the west");
        CHECK(night.dir[2] < -0.5f && night.strength == 0.0f, "the sun is under the ground at midnight");
        CHECK(noon.dir[0] < 0.0f, "tilted to the south");
        const sky::Body m0 = sky::moon(0.0f), m12 = sky::moon(12.0f), m6 = sky::moon(6.0f);
        CHECK(m0.dir[2] > 0.9f && m0.strength > 0.99f, "the moon is high at midnight");
        CHECK(m12.strength == 0.0f && m12.dir[2] < -0.5f, "no moon at noon");
        CHECK(m6.strength < 0.2f, "the moon is on the horizon at dawn (%.2f)", m6.strength);
        const sky::Body m5 = sky::moon(5.8f);   // low, the light starting: fainter
        CHECK(m5.strength > 0.0f && m5.strength < 0.8f, "the moon dims in the early light (%.2f)", m5.strength);
        for (float h = 0; h < 24.0f; h += 0.5f) {
            const sky::Body b = sky::sun(h);
            const float l = b.dir[0] * b.dir[0] + b.dir[1] * b.dir[1] + b.dir[2] * b.dir[2];
            CHECK(std::fabs(l - 1.0f) < 1e-4f && b.strength >= 0.0f && b.strength <= 1.0f, "unit direction at %.1f", h);
        }
    }
    // 6.62: the spirit world's grade
    {
        const sky::Rgb blue{70, 120, 200};
        const sky::Rgb alive = sky::ghostGrade(blue, 0.0f), ghost = sky::ghostGrade(blue, 1.0f), half = sky::ghostGrade(blue, 0.5f);
        CHECK(near3(alive, 70, 120, 200, 0.01f), "alive: unchanged");
        const float satAlive = 200.0f - 70.0f, satGhost = ghost.b - ghost.r, satHalf = half.b - half.r;
        CHECK(satGhost < satAlive * 0.25f && satGhost > 0.0f, "a ghost's sky is nearly grey but still cold (%.1f of %.1f)", satGhost, satAlive);
        CHECK(satHalf < satAlive && satHalf > satGhost, "half way: in between");
        CHECK(ghost.r + ghost.g + ghost.b < blue.r + blue.g + blue.b + 40.0f, "not brighter than the living sky");
        const sky::Rgb white = sky::ghostGrade({255, 255, 255}, 1.0f);
        CHECK(white.r <= 255.0f && white.b <= 255.0f && white.b >= white.r, "clamped, cold");
        CHECK(near3(sky::ghostGrade(blue, -3.0f), 70, 120, 200, 0.01f) && near3(sky::ghostGrade(blue, 9.0f), ghost.r, ghost.g, ghost.b, 0.01f), "amount clamped");
    }
    std::printf("SKY: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
