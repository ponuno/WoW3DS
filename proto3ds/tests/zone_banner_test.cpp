// zone_banner_test — stage 5.45: when the zone name shows, which lines, and how it fades.
#include <cstdio>
#include <map>
#include <string>

#include "../source/zone_banner.hpp"

static int g_ok = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    // Elwynn Forest (12) with Goldshire (87) and Northshire (9); Stormwind City (1519) with the Trade District (1520)
    const std::map<uint32_t, std::string> names = {{12, "Elwynn Forest"}, {87, "Goldshire"}, {9, "Northshire Valley"}, {1519, "Stormwind City"}, {1520, "Trade District"}};
    const std::map<uint32_t, uint32_t> parent = {{87, 12}, {9, 12}, {1520, 1519}};
    ZoneBanner::NameFn name = [&](uint32_t id) { auto i = names.find(id); return i == names.end() ? std::string() : i->second; };
    ZoneBanner::ParentFn par = [&](uint32_t id) { auto i = parent.find(id); return i == parent.end() ? 0u : i->second; };

    ZoneBanner b;
    b.update(0, 0, 100, name, par);
    CHECK(b.alpha(100) == 0, "no area: nothing");
    b.update(0, 87, 1000, name, par);   // Goldshire, first seen
    CHECK(b.alpha(1000) == 0 && b.big().empty(), "a new area is held for 1 s first");
    b.update(0, 87, 1900, name, par);
    CHECK(b.big().empty(), "0.9 s: not yet");
    b.update(0, 87, 2000, name, par);
    CHECK(b.big() == "Elwynn Forest" && b.small() == "Goldshire", "new zone: the zone large, the subzone under it (%s / %s)", b.big().c_str(), b.small().c_str());
    CHECK(b.alpha(2000) == 0 && b.alpha(2000 + 350) > 100 && b.alpha(2000 + 350) < 160, "fading in (%d)", b.alpha(2000 + 350));
    CHECK(b.alpha(2000 + 700) == 255 && b.alpha(2000 + 2900) == 255, "held");
    CHECK(b.alpha(2000 + 700 + 2300 + 750) > 100 && b.alpha(2000 + 700 + 2300 + 750) < 160, "fading out");
    CHECK(b.alpha(2000 + 4500) == 0 && b.alpha(2000 + 9000) == 0, "gone after 4.5 s");

    b.update(0, 87, 10000, name, par);   // still Goldshire: nothing new
    CHECK(b.alpha(10000) == 0, "the same area does not show again");
    b.update(0, 9, 11000, name, par);    // Northshire: same zone, only the subzone
    b.update(0, 9, 12000, name, par);
    CHECK(b.big() == "Northshire Valley" && b.small().empty(), "same zone: the subzone alone (%s / %s)", b.big().c_str(), b.small().c_str());
    CHECK(b.alpha(12000 + 1000) == 255, "visible");

    // a border: flickering between two areas never reaches 1 s
    ZoneBanner c;
    c.update(0, 87, 0, name, par);
    c.update(0, 9, 500, name, par);
    c.update(0, 87, 900, name, par);
    c.update(0, 9, 1400, name, par);
    c.update(0, 87, 1800, name, par);
    CHECK(c.big().empty(), "flicker on a border announces nothing");
    c.update(0, 87, 2900, name, par);
    CHECK(c.big() == "Elwynn Forest", "...until one holds");

    // a zone itself (no parent) and a city's subzone
    ZoneBanner d;
    d.update(0, 1519, 0, name, par);
    d.update(0, 1519, 1000, name, par);
    CHECK(d.big() == "Stormwind City" && d.small().empty(), "a zone alone: one line");
    d.update(0, 1520, 5000, name, par);
    d.update(0, 1520, 6000, name, par);
    CHECK(d.big() == "Trade District" && d.small().empty(), "its subzone alone");
    d.update(0, 12, 9000, name, par);
    d.update(0, 12, 10000, name, par);
    CHECK(d.big() == "Elwynn Forest" && d.small().empty(), "another zone");

    // the same area id on another map shows again; reset shows it again too; unknown names show nothing
    d.update(1, 12, 20000, name, par);
    d.update(1, 12, 21000, name, par);
    CHECK(d.alpha(21500) > 0, "another map: shown");
    d.reset();
    d.update(1, 12, 30000, name, par);
    d.update(1, 12, 31000, name, par);
    CHECK(d.big() == "Elwynn Forest", "after reset");
    ZoneBanner e;
    e.update(0, 777, 0, name, par);
    e.update(0, 777, 1500, name, par);
    CHECK(e.alpha(2000) == 0, "an unnamed area shows nothing");
    ZoneBanner f;   // glue.txt without parents: the area's own name
    f.update(0, 87, 0, name, nullptr);
    f.update(0, 87, 1000, name, nullptr);
    CHECK(f.big() == "Goldshire" && f.small().empty(), "no parent data: one line");

    std::printf("ZONE BANNER: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
