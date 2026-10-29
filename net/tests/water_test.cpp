// Stage 5.26: replays build_water_log.py's log (mirror timers, environmental damage, fishing) through GameSession and
// checks the state the HUD and the interact button read.   water_test <log.w3dl> <log.expect>
#include "../game.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace w3n;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::vector<LoggedPacket> log;
    if (!readPacketLog(argv[1], log)) { std::puts("bad log"); return 2; }
    struct Line { uint32_t ms; std::string kind, rest; };
    std::vector<Line> lines, finals;
    std::ifstream ex(argv[2]);
    std::string l;
    while (std::getline(ex, l)) {
        std::istringstream s(l);
        Line x;
        s >> x.kind;
        if (x.kind == "C" || x.kind == "H") { x.ms = 0; std::getline(s, x.rest); if (!x.rest.empty()) x.rest.erase(0, 1); finals.push_back(x); continue; }
        s >> x.ms;
        std::getline(s, x.rest);
        lines.push_back(x);
    }
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.ms < b.ms; });
    GameSession g;
    size_t next = 0;
    int bad = 0, done = 0;
    auto run = [&](uint32_t upTo) {
        for (; next < lines.size() && lines[next].ms <= upTo; ++next) {
            const Line& x = lines[next];
            std::istringstream s(x.rest);
            ++done;
            if (x.kind == "M") {
                unsigned type, active, left;
                s >> type >> active >> left;
                const MirrorTimer& m = g.mirror[type];
                const unsigned got = m.valueAt(x.ms);
                if ((unsigned)m.active != active || (active && (got + 1 < left || got > left + 1))) {
                    std::printf("FAIL t=%u mirror %u: active %d left %u, want %u %u\n", x.ms, type, m.active, got, active, left);
                    ++bad;
                }
            } else if (x.kind == "B") {
                int want;
                s >> want;
                if (g.fishBiting(x.ms) != (want != 0)) { std::printf("FAIL t=%u biting %d want %d\n", x.ms, g.fishBiting(x.ms), want); ++bad; }
            } else if (x.kind == "S") {
                float p[3];
                std::string verb;
                s >> p[0] >> p[1] >> p[2];
                std::getline(s, verb);
                if (!verb.empty()) verb.erase(0, 1);
                const char* v = "";
                uint64_t guid = g.softInteract(p, 0.0f, x.ms, &v);
                std::string got = guid ? v : "-";
                if (got != verb) { std::printf("FAIL t=%u soft interact '%s' want '%s'\n", x.ms, got.c_str(), verb.c_str()); ++bad; }
            }
        }
    };
    for (const LoggedPacket& p : log) {
        if (p.dir != 0) continue;
        run(p.ms ? p.ms - 1 : 0);
        g.handle(p.opcode, p.data, p.ms);
    }
    run(0xFFFFFFFFu);
    for (const Line& x : finals) {
        ++done;
        if (x.kind == "C") {
            bool found = false;
            for (const ChatLine& c : g.chat) found |= c.text == x.rest;
            if (!found) { std::printf("FAIL chat line missing: %s\n", x.rest.c_str()); ++bad; }
        } else {
            bool found = false;
            for (const MeleeHit& h : g.hits) found |= h.victim == g.selfGuid() && h.damage == std::atoi(x.rest.c_str());
            if (!found) { std::printf("FAIL no floating number %s on us\n", x.rest.c_str()); ++bad; }
        }
    }
    for (const ChatLine& c : g.chat)
        if (c.text.find("drowning and loses") != std::string::npos) { std::printf("FAIL a stranger's drowning in the chat\n"); ++bad; }
    std::printf("WATER: %s (%d checks, %d failed)\n", bad ? "FAIL" : "PASS", done, bad);
    return bad ? 1 : 0;
}
