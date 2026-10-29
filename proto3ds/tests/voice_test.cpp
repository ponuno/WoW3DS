// voice_test — UI stage 8: replays recorded sessions (net/tests/run_stage5.sh) through VoiceDirector with tables that give
// every display a voice set and every race / gender every error line: NPCs greet when selected (CMSG_SET_SELECTION of the
// log) or when their window opens, say goodbye when it closes, get annoyed when clicked again and again; our failed
// attacks / casts / purchases are spoken; nothing is said without tables; text emotes are heard in the doer's voice.
//   voice_test fight.w3dl life.w3dl trade.w3dl [...]
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

#include "voice.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Run { std::map<std::string, int> what; std::set<uint32_t> sounds; int selfLines = 0; };

static Run replay(const std::vector<LoggedPacket>& pk, const snd::SoundTables& t, std::set<uint32_t>* displays, GameSession* keep = nullptr) {
    GameSession local;
    GameSession& s = keep ? *keep : local;
    VoiceDirector v;
    Run run;
    std::vector<SoundReq> out;
    uint32_t last = 0;
    for (const LoggedPacket& p : pk) {
        if (p.dir == 1) {
            if (p.opcode == 0x13D && p.data.size() >= 8) {   // CMSG_SET_SELECTION: what the player selected
                Reader r(p.data);
                s.setTarget(r.u64(), p.ms);
            }
            continue;
        }
        s.handle(p.opcode, p.data, p.ms);
        s.objects.takeCreated();
        s.update(p.ms);
        out.clear();
        v.update(s, t, p.ms, out);
        for (const SoundReq& q : out) {
            run.what[q.what]++;
            run.sounds.insert(q.sound);
            if (q.self) ++run.selfLines;
        }
        last = p.ms;
        if (displays)
            for (const auto& [g, o] : s.objects.all())
                if (o.type == ObjType::Unit && o.field(UF::UNIT_DISPLAYID)) displays->insert(o.field(UF::UNIT_DISPLAYID));
    }
    (void)last;
    return run;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: voice_test fight.w3dl [...]"); return 2; }
    const std::string path = "/tmp/voice_test_sounds.txt";
    std::map<std::string, Run> runs;
    for (int a = 1; a < argc; ++a) {
        std::vector<LoggedPacket> pk;
        readPacketLog(argv[a], pk);
        std::set<uint32_t> displays;
        snd::SoundTables none;
        { FILE* f = fopen(path.c_str(), "w"); fclose(f); }
        none.load(path.c_str());
        Run silent = replay(pk, none, &displays);
        CHECK(silent.what.empty(), "%s: without voices nothing is said", argv[a]);
        FILE* f = fopen(path.c_str(), "w");
        for (uint32_t d : displays) fprintf(f, "N\t%u\t%u\t%u\t%u\n", d, 9001u, 9002u, 9003u);
        for (int race = 1; race <= 8; ++race)
            for (int g = 0; g < 2; ++g)
                for (int k = 1; k < (int)Vocal::Count; ++k) fprintf(f, "V\t%d\t%d\t%s\t%d\n", race, g, vocalToken((Vocal)k), 7000 + k);
        for (int race = 1; race <= 8; ++race)
            for (int g = 0; g < 2; ++g)
                for (int te = 1; te < 500; ++te) fprintf(f, "T\t%d\t%d\t%d\t%d\n", te, race, g, 8000 + te);
        fclose(f);
        snd::SoundTables t;
        CHECK(t.load(path.c_str()) && t.voiceCount() > 0, "tables load");
        Run r = replay(pk, t, nullptr);
        std::printf("%s: %zu displays;", argv[a], displays.size());
        for (const auto& [k, n] : r.what) std::printf(" %s %d", k.c_str(), n);
        std::printf("\n");
        const std::string name = argv[a];
        runs[name.substr(name.rfind('/') + 1)] = r;
    }
    if (runs.count("fight.w3dl")) {   // out of range / facing / casts that failed: spoken, in our own voice
        const Run& r = runs["fight.w3dl"];
        CHECK(r.what.count("error") && r.what.at("error") >= 1, "fight: our error lines");
        CHECK(r.selfLines >= 1 && r.sounds.count(7000 + (int)Vocal::OutOfRange), "fight: 'out of range' spoken");
    }
    if (runs.count("trade.w3dl")) {   // talking to NPCs: greetings (selection or window) and goodbyes
        const Run& r = runs["trade.w3dl"];
        CHECK(r.what.count("greeting") && r.what.at("greeting") >= 2, "trade: NPCs greet");
    }
    {
        int bye = 0;
        for (const auto& [n, r] : runs) bye += r.what.count("farewell") ? r.what.at("farewell") : 0;
        CHECK(bye >= 1, "NPCs say goodbye when their window closes");
    }
    {   // clicking one NPC again and again: a greeting, then (fifth click) an annoyed line; greetings not closer than 2 s
        GameSession s;
        snd::SoundTables t;
        FILE* f = fopen(path.c_str(), "w");
        fprintf(f, "N\t55\t1\t2\t3\nV\t1\t0\tnorage\t40\nV\t1\t0\toutofrange\t41\nT\t21\t1\t0\t50\n");
        fclose(f);
        t.load(path.c_str());
        // objects: an NPC (display 55) and ourselves (human male, a warrior) through SMSG_UPDATE_OBJECT
        auto create = [&](uint64_t guid, uint8_t type, bool self, uint32_t display, uint32_t bytes0) {
            Writer w;
            w.u32(1); w.u8(0);                  // one block, no transport
            w.u8(2);                            // CREATE_OBJECT
            w.packedGuid(guid);
            w.u8(type);
            w.u8(self ? 0x71 : 0x70);           // (SELF) ALL LIVING HAS_POSITION
            w.u32(0); w.u32(0);                 // move flags, time
            w.f32(1); w.f32(2); w.f32(3); w.f32(0);
            w.f32(0);                           // fall time
            for (int k = 0; k < 6; ++k) w.f32(7);
            w.u32(1);                           // ALL: high guid
            std::map<int, uint32_t> fields = {{0, (uint32_t)guid}, {1, (uint32_t)(guid >> 32)}, {2, type == 4 ? 0x19u : 0x9u},
                                              {UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}, {UF::UNIT_BYTES_0, bytes0},
                                              {UF::UNIT_DISPLAYID, display}, {UF::UNIT_NPC_FLAGS, type == 3 ? 1u : 0u}};
            const int blocks = (UF::UNIT_NPC_FLAGS / 32) + 1;
            w.u8((uint8_t)blocks);
            for (int b = 0; b < blocks; ++b) {
                uint32_t m = 0;
                for (const auto& [k, v] : fields) if (k / 32 == b) m |= 1u << (k % 32);
                w.u32(m);
            }
            for (const auto& [k, v] : fields) w.u32(v);
            s.handle(0x0A9, w.b, 1);
        };
        create(0x55, 3, false, 55, 0);
        create(0x77, 4, true, 49, 0x01000101u);   // race 1, class 1 (warrior), gender 0, rage
        s.objects.setSelf(0x77);
        CHECK(s.objects.find(0x55) && s.objects.find(0x55)->displayId() == 55, "test NPC created (display %u)",
              s.objects.find(0x55) ? s.objects.find(0x55)->displayId() : 0);
        VoiceDirector v;
        std::vector<SoundReq> out;
        std::map<std::string, int> what;
        auto frame = [&](uint32_t ms) { out.clear(); v.update(s, t, ms, out); for (auto& q : out) what[q.what]++; };
        frame(100);
        uint32_t ms = 1000;
        for (int k = 0; k < 6; ++k) {   // select / deselect every 0.6 s
            s.setTarget(0x55, ms); frame(ms); ms += 300;
            s.setTarget(0, ms); frame(ms); ms += 300;
        }
        CHECK(what["greeting"] == 1 && what["pissed"] == 1, "greetings %d (want 1: then 2 s of quiet, then the fifth click is annoyed), pissed %d (want 1)",
              what["greeting"], what["pissed"]);
        // our error lines: the warrior has no rage; the same line again at once is not repeated
        what.clear();
        ms += 5000;
        s.error("Not enough rage", Vocal::NoRage, ms); frame(ms);
        s.error("Not enough rage", Vocal::NoRage, ms + 1000); frame(ms + 1000);
        s.error("Out of range", Vocal::OutOfRange, ms + 2000); frame(ms + 2000);
        CHECK(what["error"] == 2 && s.uiErrors.size() == 2, "error lines spoken %d (want 2), shown %zu (want 2)", what["error"],
              s.uiErrors.size());
        // a text emote: the doer's race / gender
        Writer w;
        w.u64(0x77); w.u32(21); w.u32(0); w.u32(1); w.u8(0);
        s.handle(0x105, w.b, ms + 3000);
        what.clear();
        out.clear();
        v.update(s, t, ms + 3000, out);
        CHECK(out.size() == 1 && out[0].sound == 50 && out[0].self, "emote voice (%zu requests)", out.size());
    }
    std::printf("VOICES: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
