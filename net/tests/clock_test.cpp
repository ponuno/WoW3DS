// clock_test — 6.60: the server's game clock (the sky's time of day): SMSG_LOGIN_SETTIMESPEED (packed time + speed), GAMETIME_UPDATE /
// GAMETIME_SET (packed time) and GAMESPEED_SET, into GameSession::gameHour; -1 until the server has said; nonsense refused.
#include <cmath>
#include <cstdio>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// minute 0-5, hour 6-10, weekday 11-13, day 14-19, month 20-23, year 24-28
static uint32_t packed(int hour, int minute) { return (uint32_t)minute | ((uint32_t)hour << 6) | (3u << 11) | (14u << 14) | (10u << 20) | (26u << 24); }

int main() {
    GameSession s;
    CHECK(s.gameHour(1000) < 0, "no clock before the server says");
    {   // 21:15, speed 1/60 per second
        Writer w;
        w.u32(packed(21, 15));
        w.f32(0.01666667f);
        s.handle(0x042, w.b, 5000);
        CHECK(std::fabs(s.gameHour(5000) - 21.25f) < 1e-3f, "at the packet: %.3f", s.gameHour(5000));
        CHECK(std::fabs(s.gameHour(5000 + 45 * 60 * 1000) - 22.0f) < 1e-2f, "45 minutes later: %.3f", s.gameHour(5000 + 45 * 60 * 1000));
        CHECK(std::fabs(s.gameHour(5000 + 3 * 3600 * 1000) - 0.25f) < 1e-2f, "three hours later: past midnight %.3f", s.gameHour(5000 + 3 * 3600 * 1000));
    }
    {   // a plain time update: the hour moves, the speed stays
        Writer w;
        w.u32(packed(6, 30));
        s.handle(0x043, w.b, 100000);
        CHECK(std::fabs(s.gameHour(100000) - 6.5f) < 1e-3f, "GAMETIME_UPDATE: %.3f", s.gameHour(100000));
        CHECK(std::fabs(s.gameHour(100000 + 30 * 60 * 1000) - 7.0f) < 1e-2f, "the speed kept: %.3f", s.gameHour(100000 + 30 * 60 * 1000));
        Writer w2;
        w2.u32(packed(12, 0));
        s.handle(0x045, w2.b, 200000);
        CHECK(std::fabs(s.gameHour(200000) - 12.0f) < 1e-3f, "GAMETIME_SET: %.3f", s.gameHour(200000));
    }
    {   // GAMESPEED_SET: time and a faster clock (2 game minutes per second)
        Writer w;
        w.u32(packed(10, 0));
        w.f32(2.0f);
        s.handle(0x047, w.b, 300000);
        CHECK(std::fabs(s.gameHour(300000 + 30 * 1000) - 11.0f) < 1e-2f, "30 s at 2 min/s: %.3f", s.gameHour(300000 + 30 * 1000));
    }
    {   // nonsense: hour 31 / a short packet change nothing
        const uint32_t updates = s.clock.updates;
        Writer w;
        w.u32(packed(31, 0));
        s.handle(0x043, w.b, 400000);
        Writer shortPkt;
        shortPkt.u8(1);
        s.handle(0x043, shortPkt.b, 400001);
        CHECK(s.clock.updates == updates && std::fabs(s.gameHour(300000) - 10.0f) < 1e-3f, "nonsense refused (%u updates)", s.clock.updates);
    }
    std::printf("GAME CLOCK: %d checks, %d failed\n", g_ok + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
