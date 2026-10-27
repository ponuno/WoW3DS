// objects_test — replays a log made by build_objects_log.py and checks the
// object table against positions computed independently in Python; then
// fuzzes the parsers with mutated packets (run under ASan/UBSan).
//   objects_test <log.w3dl> <log.expect>
#include "../game.hpp"
#include "../inflate.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

using namespace w3n;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::vector<LoggedPacket> log;
    if (!readPacketLog(argv[1], log)) { std::puts("bad log"); return 2; }
    struct Check { uint32_t ms; uint64_t g; float p[3]; bool hasO; float o; int present; };
    std::vector<Check> checks;
    std::vector<std::string> finals;
    std::ifstream ex(argv[2]);
    std::string line;
    while (std::getline(ex, line)) {
        std::istringstream ss(line);
        if (!line.empty() && isalpha((unsigned char)line[0])) { finals.push_back(line); continue; }
        Check c{};
        std::string g, o;
        ss >> c.ms >> g >> c.p[0] >> c.p[1] >> c.p[2] >> o >> c.present;
        c.g = std::stoull(g, nullptr, 16);
        c.hasO = o != "-";
        if (c.hasO) c.o = std::stof(o);
        checks.push_back(c);
    }
    std::sort(checks.begin(), checks.end(), [](const Check& a, const Check& b) { return a.ms < b.ms; });

    GameSession s;
    size_t next = 0;
    int bad = 0, done = 0;
    auto runChecks = [&](uint32_t upTo) {
        for (; next < checks.size() && checks[next].ms <= upTo; ++next) {
            const Check& c = checks[next];
            const WorldObject* o = s.objects.find(c.g);
            ++done;
            if (!c.present) {
                if (o) { std::printf("FAIL t=%u %llx should be gone\n", c.ms, (unsigned long long)c.g); ++bad; }
                continue;
            }
            if (!o) { std::printf("FAIL t=%u %llx missing\n", c.ms, (unsigned long long)c.g); ++bad; continue; }
            float p[3], ori;
            o->positionAt(c.ms, p, &ori);
            float d = std::sqrt((p[0] - c.p[0]) * (p[0] - c.p[0]) + (p[1] - c.p[1]) * (p[1] - c.p[1]) + (p[2] - c.p[2]) * (p[2] - c.p[2]));
            float od = c.hasO ? std::fabs(ori - c.o) : 0;
            if (od > 3.14159f) od = 6.28318f - od;
            bool ok = d < 0.02f && od < 0.002f;
            std::printf("%s t=%5u %llx pos (%.3f, %.3f, %.3f) want (%.3f, %.3f, %.3f) d=%.4f%s\n", ok ? "ok  " : "FAIL", c.ms,
                        (unsigned long long)c.g, p[0], p[1], p[2], c.p[0], c.p[1], c.p[2], d,
                        c.hasO ? (" o " + std::to_string(ori) + " want " + std::to_string(c.o)).c_str() : "");
            if (!ok) ++bad;
        }
    };
    for (const LoggedPacket& p : log) {
        if (p.ms) runChecks(p.ms - 1);   // checks strictly before this packet
        s.handle(p.opcode, p.data, p.ms);
        runChecks(p.ms);
    }
    runChecks(0xFFFFFFFF);
    for (const std::string& f : finals) {
        std::istringstream ss(f);
        std::string what, g;
        ss >> what;
        if (what == "FAILED") {
            uint32_t want;
            ss >> want;
            bool ok = s.objects.stats.failedPackets == want;
            std::printf("%s failed packets %u want %u (%s)\n", ok ? "ok  " : "FAIL", s.objects.stats.failedPackets, want,
                        s.objects.stats.lastError.c_str());
            bad += !ok;
            continue;
        }
        ss >> g;
        const WorldObject* o = s.objects.find(std::stoull(g, nullptr, 16));
        uint32_t v = 0;
        ss >> v;
        bool ok = false;
        if (what == "ABSENT") ok = !o;
        else if (what == "HEALTH") ok = o && o->health() == v;
        else if (what == "TYPE") ok = o && (uint32_t)o->type == v;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", f.c_str());
        bad += !ok;
        ++done;
    }

    // ---- fuzz: mutated / truncated copies of every server packet must never crash
    std::mt19937 rng(12345);
    uint64_t parsed = 0;
    for (int round = 0; round < 3000; ++round) {
        GameSession f;
        for (const LoggedPacket& p : log) {
            Bytes d = p.data;
            int mode = rng() % 4;
            if (mode == 1 && !d.empty()) for (int k = 0; k < 4; ++k) d[rng() % d.size()] ^= (uint8_t)(1u << (rng() % 8));
            if (mode == 2 && !d.empty()) d.resize(rng() % d.size());
            if (mode == 3) for (int k = 0; k < 8; ++k) d.insert(d.begin() + (long)(rng() % (d.size() + 1)), (uint8_t)rng());
            f.handle(p.opcode, d, p.ms);
            ++parsed;
            for (auto& [g, o] : f.objects.all()) { float q[3]; o.positionAt(p.ms + 100, q); }
        }
    }
    // random garbage into the inflater
    for (int i = 0; i < 20000; ++i) {
        Bytes junk(rng() % 300);
        for (auto& b : junk) b = (uint8_t)rng();
        if (junk.size() > 2) { junk[0] = 0x78; junk[1] = 0x9C; }
        Bytes out;
        zlibInflate(junk.data(), junk.size(), out, 0, 1 << 16);
    }
    std::printf("%d checks, %d failed; fuzz: %llu mutated packets parsed without crashing\n", done, bad,
                (unsigned long long)parsed);
    return bad ? 1 : 0;
}
