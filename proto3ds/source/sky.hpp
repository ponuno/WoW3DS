// sky.hpp — 6.60: the colours of the sky and the fog over the day, from sky.txt (w3dconv: Light.dbc + LightIntBand.dbc).
// Pure maths (no 3DS headers), shared with the PC test.
//
// What the original does (and WoWee after it): Light.dbc lists light volumes (a centre, an inner and an outer radius, a LightParams
// id); each LightParams id has colour curves over the day (LightIntBand: a few keys in half minutes of the day, linear between
// them, the last key wrapping to the first of the next day) for the sky's top, middle and two horizon bands and for the fog. The
// colours at a spot are those of the volumes around it, weighted by how deep inside each the spot is; the map's global volume (no
// radius) fills what the others leave. The time is the SERVER's: SMSG_LOGIN_SETTIMESPEED and the game time packets (net/game.hpp
// GameClock), not the console's clock.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sky {

struct Rgb { float r = 0, g = 0, b = 0; };

struct Colors {
    Rgb top, mid, band1, band2, fog;   // 0..255
};

// A colour curve over the day: n keys (half minutes 0..2879), linear between, wrapping.
struct Curve {
    uint8_t n = 0;
    uint16_t t[16] = {};
    uint8_t rgb[16][3] = {};
    bool sample(float halfMinutes, Rgb* out) const;
};

class SkyTable {
public:
    // Remembers the file; nothing is read until setMap.
    bool open(const char* path);
    bool opened() const { return !path_.empty(); }
    // Reads the light volumes of 'map' and the curves they use (two passes over the file; at a map change only).
    // false: no file / no volume on that map (colours() then says false and the caller keeps its default sky).
    bool setMap(uint32_t map);
    uint32_t map() const { return map_; }
    // 6.61: the LightSkybox id (SKYB entry of the pool) of the place: the smallest volume the spot lies at least half inside, else the
    // map's global light; 0: that light has no sky model (or nothing is known)
    uint32_t skyboxAt(float x, float y) const;
    size_t volumes() const { return vols_.size(); }
    // The colours at world (x, y) (server yards) at 'hour' (0..24, the server's clock). false: nothing known for the spot.
    bool colors(float x, float y, float hour, Colors* out) const;

private:
    struct Vol { float x, y, inner, outer; uint32_t params; };
    struct Set { uint32_t params; Curve c[5]; uint32_t skybox = 0; };   // top, middle, band 1, band 2, fog
    const Set* find(uint32_t params) const;
    std::string path_;
    uint32_t map_ = 0xFFFFFFFFu;
    std::vector<Vol> vols_;
    std::vector<Set> sets_;
};

// The hour of day (0..24) of a game clock: 'baseMinutes' (minutes since midnight) at the packet, + elapsedSec * minutesPerSec.
float clockHour(float baseMinutes, double elapsedSec, float minutesPerSec);

// 6.61: the sun and the moon. dir: unit vector from the viewer (world x = north, y = west, z = up); strength 0..1: 0 below the horizon, 1
// well above it (the moon is also dimmed by daylight). The sun rises in the east (6:00), is highest at noon, tilted a little to the
// south, and sets in the west (18:00); the moon is on the opposite side of the day.
struct Body { float dir[3]; float strength; };
Body sun(float hour);
Body moon(float hour);

// 6.62: the colour of the spirit world: 'amount' 0 (alive) .. 1 (a ghost): towards grey, a little darker and blue.
Rgb ghostGrade(const Rgb& c, float amount);

}  // namespace sky
