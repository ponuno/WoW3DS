// sound_world_test — replays recorded sessions (net/tests/run_stage5.sh) through SoundWorld: the fight must sound
// like one (impacts, wounds, aggro, the wolf's death), casts give precast / cast / impact and the precast stops,
// walking gives footsteps at the stride.
//   sound_world_test fight.w3dl [more.w3dl ...]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

#include "sound_world.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Run { std::map<std::string, int> what; size_t stops = 0, requests = 0; int unknown = 0, badLoops = 0; };

// One pass over a log; 'tables' maps every display to sound data 1 (or self to 2) and every spell to visual 1.
static Run replay(const std::vector<LoggedPacket>& pk, const std::string& tablesPath, std::set<uint32_t>* displays,
                  std::set<uint32_t>* spells) {
    GameSession s;
    SoundWorld w;
    w.load(tablesPath.c_str());
    Run run;
    std::vector<SoundReq> out;
    float pos[3] = {0, 0, 0};
    bool moving = false;
    uint32_t last = 0;
    for (const LoggedPacket& p : pk) {
        if (p.dir == 1) {
            if (MoveOp::isRelayedMove(p.opcode) || p.opcode == MoveOp::Heartbeat) {
                Reader r(p.data);
                MovementInfo m;
                if (readMovementInfo(r, m)) {
                    if (getenv("SW_DEBUG")) std::printf("move %u op %x flags %x %.2f %.2f\n", p.ms, p.opcode, m.flags, m.x, m.y);
                    moving = (m.flags & MoveFlag::MaskMoving) != 0;
                    pos[0] = m.x; pos[1] = m.y; pos[2] = m.z;
                }
            }
            continue;
        }
        s.handle(p.opcode, p.data, p.ms);
        s.objects.takeCreated();
        s.update(p.ms);
        if (spells)
            for (const GameSession::WorldSound& e : s.worldSounds) if (e.spell) spells->insert(e.spell);
        // frames between packets: 30 per second
        for (uint32_t t = last; t <= p.ms; t += 33) {
            out.clear();
            w.update(s, pos, moving, true, false, t, out);
            for (const SoundReq& q : out) {
                ++run.requests;
                if (q.stop) { ++run.stops; continue; }
                run.what[q.what]++;
                if (!q.sound) ++run.unknown;
                if (std::string(q.what) == "precast" && (!q.tag || !q.loopMs)) ++run.badLoops;   // stage 5.43: a precast repeats for the cast's length
            }
            if (t + 33 > p.ms) break;
        }
        last = p.ms;
        if (displays)
            for (const auto& [g, o] : s.objects.all())
                if (o.field(UF::UNIT_DISPLAYID)) displays->insert(o.field(UF::UNIT_DISPLAYID));
    }
    return run;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: sound_world_test fight.w3dl [...]"); return 2; }
    const std::string tables = "/tmp/sound_world_test_sounds.txt";
    for (int a = 1; a < argc; ++a) {
        std::vector<LoggedPacket> pk;
        readPacketLog(argv[a], pk);
        std::set<uint32_t> displays, spells;
        { FILE* f = fopen(tables.c_str(), "w"); fclose(f); }
        Run silent = replay(pk, tables, &displays, &spells);
        CHECK(silent.what.empty() || (silent.what.size() == 0), "%s: no tables, no sounds", argv[a]);
        FILE* f = fopen(tables.c_str(), "w");
        for (uint32_t d : displays) fprintf(f, "D\t%u\t1\n", d);
        fprintf(f, "K\t1\t300\t301\t302\t303\t304\t305\t7\nF\t7\t330\nW\t7\t310\t311\n");
        for (int sub = 0; sub <= 20; ++sub) fprintf(f, "W\t%d\t310\t311\n", sub);
        fprintf(f, "Q\t1\t320\t321\t322\t0\t0\n");
        for (uint32_t sp : spells) fprintf(f, "P\t%u\t1\n", sp);
        fclose(f);
        Run r = replay(pk, tables, nullptr, nullptr);
        std::string sum;
        for (const auto& [k, n] : r.what) sum += " " + k + "=" + std::to_string(n);
        std::printf("  %s: %zu displays, %zu spells:%s, %zu stops\n", argv[a], displays.size(), spells.size(), sum.c_str(), r.stops);
        CHECK(r.unknown == 0, "every request names a sound");
        const std::string name = argv[a];
        if (name.find("fight") != std::string::npos) {
            CHECK(r.what["impact"] >= 3, "fight: blows land (%d)", r.what["impact"]);
            CHECK(r.what["wound"] >= 1, "fight: the victim cries out (%d)", r.what["wound"]);
            CHECK(r.what["death"] >= 1, "fight: the wolf dies (%d)", r.what["death"]);
            // the log has our position every 0.5 s (8.6 yd walked): one step per position update at most
            CHECK(r.what["step"] >= 2, "fight: footsteps walking to the wolf (%d)", r.what["step"]);
        }
        if (!spells.empty()) {
            CHECK(r.what["cast"] >= 1, "casts sound (%d)", r.what["cast"]);
            CHECK(r.stops >= (size_t)r.what["cast"], "every cast ends its precast (%zu)", r.stops);
            CHECK(r.badLoops == 0, "every precast carries the cast time as its loop length (%d bad)", r.badLoops);
        }
    }
    std::printf("SOUND WORLD: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
