// PortalFx on the PC: phases, coverage and the draw list (flat quads inside the screen). Writes a PPM of three moments.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "portal.hpp"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void raster(const HudList& l, int w, int h, std::vector<uint8_t>& px) {
    px.assign((size_t)w * h * 3, 0);
    for (auto& q : l.q) {
        if (q.tex) continue;   // text: not drawn here
        int a = q.color >> 24, r = q.color & 255, g = (q.color >> 8) & 255, b = (q.color >> 16) & 255;
        for (int y = std::max<int>(0, q.y0); y < std::min<int>(h, q.y1); ++y)
            for (int x = std::max<int>(0, q.x0); x < std::min<int>(w, q.x1); ++x) {
                uint8_t* p = &px[((size_t)y * w + x) * 3];
                p[0] = (uint8_t)((p[0] * (255 - a) + r * a) / 255);
                p[1] = (uint8_t)((p[1] * (255 - a) + g * a) / 255);
                p[2] = (uint8_t)((p[2] * (255 - a) + b * a) / 255);
            }
    }
}

int main(int argc, char** argv) {
    PortalFx fx;
    HudList l;
    CHECK(!fx.active());
    fx.build(l, 400, 240);
    CHECK(l.q.empty());                       // idle: nothing drawn

    fx.start();
    CHECK(fx.phase() == PortalFx::Phase::Closing);
    float t = 0;
    FILE* ppm = argc > 1 ? std::fopen(argv[1], "wb") : nullptr;
    std::vector<std::vector<uint8_t>> shots;
    int shotAt = 0;
    while (!fx.closed() && t < 5) {
        fx.update(1.0f / 60);
        t += 1.0f / 60;
        if (fx.cover() > 0.3f && shotAt == 0) { l.clear(); fx.build(l, 400, 240); shots.emplace_back(); raster(l, 400, 240, shots.back()); shotAt = 1; }
    }
    CHECK(t > 0.4f && t < 0.7f);              // closes in about kCloseSec
    CHECK(fx.cover() >= 0.999f);
    fx.update(2.0f);
    CHECK(fx.phase() == PortalFx::Phase::Hold);   // waits for the map
    l.clear();
    fx.build(l, 400, 240, "Entering...");
    CHECK(l.q.size() > 100 && l.q.size() < 450);
    for (auto& q : l.q) CHECK(q.x1 >= q.x0 && q.y1 >= q.y0);   // sane rectangles
    shots.emplace_back();
    raster(l, 400, 240, shots.back());
    // finishing while closed opens it
    fx.finish();
    CHECK(fx.phase() == PortalFx::Phase::Opening);
    fx.update(0.4f);
    CHECK(fx.cover() > 0.4f && fx.cover() < 0.7f);
    l.clear();
    fx.build(l, 400, 240);
    shots.emplace_back();
    raster(l, 400, 240, shots.back());
    for (int i = 0; i < 100; ++i) fx.update(0.02f);
    CHECK(!fx.active() && fx.cover() == 0.0f);

    // finish() during Closing: it still closes completely first, then opens (never pops)
    fx.start();
    fx.update(0.2f);
    fx.finish();
    CHECK(fx.phase() == PortalFx::Phase::Closing);
    fx.update(0.5f);
    CHECK(fx.phase() == PortalFx::Phase::Opening && fx.cover() >= 0.999f);
    // start() while opening closes again from where it is
    fx.update(0.3f);
    float c = fx.cover();
    fx.start();
    CHECK(fx.phase() == PortalFx::Phase::Closing && fx.cover() == c);
    fx.cancel();
    CHECK(!fx.active() && fx.cover() == 0);
    fx.finish();                               // nothing to finish
    CHECK(!fx.active());
    // small screen
    fx.start();
    fx.update(1.0f);
    l.clear();
    fx.build(l, 320, 240);
    for (auto& q : l.q) CHECK(q.x0 > -400 && q.x1 < 800);

    // the game's loading screen instead of the swirl
    {
        PortalFx lf;
        ui::Image pic; pic.tex = ui::kTexPack0; pic.w = 512; pic.h = 256;
        lf.setPicture(pic);
        lf.startClosed();
        CHECK(lf.closed() && lf.cover() == 1.0f);
        lf.setProgress(0.5f);
        HudList q;
        lf.build(q, 400, 240, "Loading...");
        int pics = 0, bars = 0;
        for (auto& e : q.q) {
            if (e.tex == ui::kTexPack0) { ++pics; CHECK(e.x0 == 0 && e.x1 == 400 && e.y1 == 240); }
            else if (!e.tex && e.y0 >= 200 && e.y1 <= 240) ++bars;
        }
        CHECK(pics == 1 && bars >= 3 && q.q.size() < 60);   // picture + bar frame/fill, no swirl
        lf.finish();
        CHECK(lf.phase() == PortalFx::Phase::Opening);
    }
    if (ppm) {
        const int W = 400, H = 240;
        std::fprintf(ppm, "P6\n%d %d\n255\n", W * (int)shots.size(), H);
        for (int y = 0; y < H; ++y)
            for (auto& s : shots) std::fwrite(&s[(size_t)y * W * 3], 1, (size_t)W * 3, ppm);
        std::fclose(ppm);
    }
    std::printf("PORTAL %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
