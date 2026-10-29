// perf_test.cpp — the perf log (stage 6) on the PC: perf_core's pieces, then perflog.cpp over the real streamer (stubbed
// libctru) for a few seconds of simulated frames: one CSV row a second with the same columns each time, a hitch, the
// switch off / on, the session summary. Leaves perf.csv / perf_events.txt in the output folder (tools/perflog/analyze.py
// reads them).
//   perf_test <pack.w3dp> <out dir> [seconds]
#include "perf_core.hpp"
#include "perflog.hpp"
#include "audio.hpp"
#include "ui_render.hpp"
#include "world.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
u32 __ctru_heap_size = 24u << 20;
u32 __ctru_linear_heap_size = 90u << 20;
}
// the sound and the interface are not part of this build
namespace audio { void stats(Stats* o) { *o = Stats(); o->on = true; o->core = 2; o->cacheBytes = 1200000; o->cacheClips = 40; } }
static UiTexStats g_ui;
void pcTestIoDelay(int, uint32_t, uint32_t size) { std::this_thread::sleep_for(std::chrono::microseconds(size / 3)); }   // ~3 MB/s, like an SD card
const UiTexStats& uiTexStats() { return g_ui; }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void coreTests() {
    perf::Acc a;
    CHECK(a.avg() == 0 && a.max() == 0);
    a.add(3); a.add(1); a.add(8);
    CHECK(a.n == 3 && a.min() == 1 && a.max() == 8 && std::fabs(a.avg() - 4) < 1e-9);
    perf::Row r;
    r.begin();
    r.num("a", 12.50); r.num("b", 3.0); r.num("c", -0.001); r.integer("d", -7); r.text("e", "x,y\"z");
    CHECK(r.header() == "a,b,c,d,e\n");
    CHECK(r.line() == "12.5,3,0,-7,x;y;z\n");
    CHECK(r.headerChanged());   // the first row
    r.begin();
    r.num("a", 1); r.num("b", 2); r.num("c", 3); r.integer("d", 4); r.text("e", "");
    CHECK(!r.headerChanged());
    r.begin();
    r.num("a", 1);
    CHECK(r.headerChanged());
    perf::Histogram h;
    for (int i = 1; i <= 100; ++i) h.add(i);   // 1..100 ms
    CHECK(std::fabs(h.percentile(0.5) - 50.5) <= 0.5);
    CHECK(h.percentile(0.99) >= 99 && h.percentile(0.99) <= 100);
    CHECK(h.over(50.0) == 51);
    h.add(1000);
    CHECK(h.percentile(1.0) == 1000);
    std::vector<float> v = {5, 1, 4, 2, 3};
    CHECK(perf::percentile(v, 0.5) == 3 && perf::percentile(v, 1.0) == 5 && perf::percentile(v, 0.0) == 1);
    perf::LowWatch w{100, 50};
    CHECK(!w.check(120) && w.check(90) && !w.check(80) && !w.check(140) && !w.check(160) && w.check(99));
}

static std::vector<std::string> lines(const std::string& path) {
    std::ifstream f(path);
    std::vector<std::string> out;
    for (std::string l; std::getline(f, l);) out.push_back(l);
    return out;
}
static size_t fields(const std::string& l) { return (size_t)std::count(l.begin(), l.end(), ',') + 1; }

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: perf_test <pack.w3dp> <out dir> [seconds]\n"); return 2; }
    coreTests();
    const double seconds = argc > 3 ? std::atof(argv[3]) : 6.0;
    PackIndex index;
    char err[160];
    if (!index.open(argv[1], err, sizeof err) || !streamerInit(&index, err, sizeof err)) { std::printf("init: %s\n", err); return 2; }
    perf::init(argv[2], true);
    float cam[3] = {(32.0f - index.minTileY - 1.5f) * kTileSize, (32.0f - index.minTileX - 1.5f) * kTileSize, 40.0f};
    const float c0[2] = {cam[0], cam[1]};
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    int frame = 0;
    bool toggled = false, hitch = false, stalled = false;
    while (elapsed() < seconds) {
        perf::lap(perf::kInput);   // (the frame's marks started in the previous endFrame)
        std::this_thread::sleep_for(std::chrono::microseconds(1500));   // "the network"
        perf::lap(perf::kNet);
        const float a = (float)frame * 0.004f;
        cam[0] = c0[0] + std::cos(a) * 300.0f;
        cam[1] = c0[1] + std::sin(a) * 300.0f;
        streamerSetView(a);
        streamerUpdate(cam, 400.0f);
        perf::lap(perf::kStream);
        if (!hitch && elapsed() > seconds * 0.4) {   // one long frame: it must be in the events with its breakdown
            hitch = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            perf::lap(perf::kGame);
        }
        { perf::Scope s(perf::kMapLoad); }
        if (std::getenv("PERF_STALL") && !stalled && elapsed() > seconds * 0.5) {   // stage 6.6: a 3.6 s freeze for the watchdog
            stalled = true;
            perf::mark("test: frozen on purpose");
            std::this_thread::sleep_for(std::chrono::milliseconds(3600));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(8000));    // "waiting for the GPU"
        perf::lap(perf::kWait);
        FrameCounters fc;
        fc.drawCalls = 900 + frame % 50; fc.tris = 60000; fc.creatures = 12;
        fc.passMs[FrameCounters::kTerrain] = 1.2; fc.passDraws[FrameCounters::kTerrain] = 300; fc.passTris[FrameCounters::kTerrain] = 30000;
        fc.passMs[FrameCounters::kOpaque] = 3.1; fc.eyes = 1;
        g_ui.quads += 400; g_ui.batches += 30;
        std::this_thread::sleep_for(std::chrono::microseconds(4000));
        perf::lap(perf::kWorld);
        streamerEndFrame();
        perf::lap(perf::kFlush);
        perf::endFrame(fc);
        if (perf::due()) {
            perf::Context c;
            c.phase = elapsed() < 1.5 ? "loading" : "world";
            c.map = 0;
            c.tileX = (int)std::floor(32.0f - cam[1] / kTileSize);
            c.tileY = (int)std::floor(32.0f - cam[0] / kTileSize);
            c.pos[0] = cam[0]; c.pos[1] = cam[1]; c.pos[2] = cam[2];
            c.move = "walk";
            c.drawDist = elapsed() > seconds * 0.6 ? 533 : 400;   // a settings change
            c.online = true;
            c.netBytesIn = (uint64_t)(elapsed() * 9000); c.netPacketsIn = (uint64_t)(elapsed() * 40);
            c.vramTargets = 240 * 400 * 8 + 240 * 320 * 4; c.vramReserve = 240 * 800 * 8; c.vramPortraits = 2 * 64 * 64 * 7;
            c.cmdBufBytes = 2u << 20;
            perf::second(c);
        }
        if (!toggled && elapsed() > seconds * 0.7) {   // the setting: off and on again goes on in the same files
            toggled = true;
            perf::setEnabled(false);
            perf::setEnabled(true);
        }
        ++frame;
    }
    perf::event("test end");
    perf::shutdown();
    streamerShutdown();

    const std::string dir = argv[2];
    const auto csv = lines(dir + "/perf.csv");
    const auto ev = lines(dir + "/perf_events.txt");
    CHECK(csv.size() >= 3);
    size_t rows = 0, cols = 0;
    bool sameCols = true;
    for (const std::string& l : csv) {
        if (l.empty() || l[0] == '#') continue;
        if (l.rfind("t_s,", 0) == 0) {   // a header (again after the switch): every name once
            cols = fields(l);
            std::vector<std::string> names;
            std::stringstream ss(l);
            for (std::string n; std::getline(ss, n, ',');) names.push_back(n);
            std::sort(names.begin(), names.end());
            for (size_t i = 1; i < names.size(); ++i)
                if (names[i] == names[i - 1]) { std::printf("duplicate column %s\n", names[i].c_str()); ++g_fail; }
            continue;
        }
        ++rows;
        if (fields(l) != cols) sameCols = false;
    }
    CHECK(sameCols);
    CHECK(rows + (std::getenv("PERF_STALL") ? 5 : 2) >= (size_t)seconds && rows <= (size_t)seconds + 1);   // (a frozen frame spans seconds)
    std::string all;
    for (const std::string& l : ev) all += l + "\n";
    CHECK(all.find("hitch 7") != std::string::npos || all.find("hitch 8") != std::string::npos);
    CHECK(all.find("game 7") != std::string::npos);   // the hitch's phase
    CHECK(all.find("phase: loading -> world") != std::string::npos);
    CHECK(all.find("settings: draw distance 533") != std::string::npos);
    CHECK(all.find("perf log switched off") != std::string::npos);
    CHECK(all.find("== session summary") != std::string::npos);
    CHECK(all.find("in the world: avg") != std::string::npos);
    if (std::getenv("PERF_STALL")) {   // the watchdog named the spot, and saw the frames come back
        CHECK(all.find("STALL: no frame for") != std::string::npos);
        CHECK(all.find("test: frozen on purpose") != std::string::npos);
        CHECK(all.find("STALL over") != std::string::npos);
        CHECK(!lines(dir + "/perf_stall.txt").empty());
    }
    std::printf("perf log: %zu rows x %zu columns, %zu event lines, %d frames\n", rows, cols, ev.size(), frame);
    std::printf("PERF: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
