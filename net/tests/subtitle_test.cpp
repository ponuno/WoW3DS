// subtitle_test — stage 5.42: the line under a name: <Guild> for players (PLAYER_GUILDID + CMSG_GUILD_QUERY cache), <Subname> for NPCs.
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void fields(Writer& w, std::initializer_list<std::pair<int, uint32_t>> f) {
    uint32_t mask[8] = {};
    int top = 0;
    for (auto& p : f) { mask[p.first / 32] |= 1u << (p.first % 32); top = std::max(top, p.first / 32 + 1); }
    w.u8((uint8_t)top);
    for (int i = 0; i < top; ++i) w.u32(mask[i]);
    for (int b = 0; b < top; ++b)
        for (int bit = 0; bit < 32; ++bit)
            for (auto& p : f) if (p.first == b * 32 + bit) w.u32(p.second);
}

// a CREATE2 block of a living unit (type 3) / player (type 4) at the origin
static void create(GameSession& s, uint64_t guid, uint8_t type, bool self, std::initializer_list<std::pair<int, uint32_t>> f, uint32_t ms) {
    Writer w;
    w.u32(1); w.u8(0); w.u8(3); w.packedGuid(guid); w.u8(type);
    w.u8(self ? 0x61 : 0x60);       // LIVING | HAS_POSITION(0x40)... : LIVING 0x20 + 0x40 + (SELF 0x01)
    w.u32(0); w.u32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0); w.u32(0);
    for (int k = 0; k < 6; ++k) w.f32(7);
    fields(w, f);
    s.handle(0x0A9, w.b, ms);
}


int main() {
    GameSession s;
    std::vector<std::pair<uint16_t, Bytes>> sent;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) sent.push_back({op, d}); };
    const uint64_t me = 0x77, other = 0x1234, npc = 0xF130000000000042ull;
    s.objects.setSelf(me);
    create(s, me, 4, true, {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}}, 100);
    create(s, other, 4, false, {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}, {191, 77}}, 100);   // PLAYER_GUILDID
    create(s, npc, 3, false, {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}, {UF::OBJECT_ENTRY, 1234}}, 100);

    CHECK(s.unitSubtitle(*s.objects.find(me)).empty(), "no guild -> no line");
    CHECK(s.unitSubtitle(*s.objects.find(other)).empty(), "guild name unknown yet -> no line");
    sent.clear();
    s.updateGuildNames(1000);
    CHECK(sent.size() == 1 && sent[0].first == 0x054 && sent[0].second.size() == 4, "one CMSG_GUILD_QUERY for the guild in view");
    if (sent.size() == 1) { Reader r(sent[0].second); CHECK(r.u32() == 77, "asks for guild 77"); }
    sent.clear();
    s.updateGuildNames(3000);
    CHECK(sent.empty(), "no repeat within 10 s");
    s.updateGuildNames(12000);
    CHECK(sent.size() == 1, "asks again after 10 s while unanswered");

    Writer q;
    q.u32(77); q.cstr("Knights of Ni");
    for (int i = 0; i < 10; ++i) q.cstr(i == 0 ? "Master" : "");
    q.u32(0); q.u32(0); q.u32(0); q.u32(0); q.u32(0);
    s.handle(0x055, q.b, 12100);
    CHECK(s.guildNameOf(77) == "Knights of Ni", "guild name cached");
    CHECK(s.unitSubtitle(*s.objects.find(other)) == "<Knights of Ni>", "player line is <Guild>");
    sent.clear();
    s.updateGuildNames(30000);
    CHECK(sent.empty(), "known guild is not asked again");

    CHECK(s.unitSubtitle(*s.objects.find(npc)).empty(), "creature without query answer -> no line");
    Writer c;
    c.u32(1234); c.cstr("Hogger"); c.cstr(""); c.cstr(""); c.cstr(""); c.cstr("Armorer");
    c.u32(0); c.u32(7); c.u32(0); c.u32(0);
    s.handle(0x061, c.b, 13000);
    CHECK(s.unitSubtitle(*s.objects.find(npc)) == "<Armorer>", "NPC line is <Subname>: %s", s.unitSubtitle(*s.objects.find(npc)).c_str());

    // stage 5.45: SMSG_EXPLORATION_EXPERIENCE (u32 area, u32 xp): a sound, the chat line with the area's name, the record
    s.areaName = [](uint32_t id) { return id == 12 ? std::string("Elwynn Forest") : std::string(); };
    s.sounds.clear();
    Writer ex;
    ex.u32(12); ex.u32(140);
    s.handle(0x1F8, ex.b, 14000);
    CHECK(s.sounds.size() == 1 && std::string(s.sounds[0]) == "discover", "a discovered area plays the discover sound");
    CHECK(s.discovery.area == 12 && s.discovery.xp == 140 && s.discovery.seq == 1, "discovery recorded");
    CHECK(!s.chat.empty() && s.chat.back().text == "Discovered Elwynn Forest: 140 experience gained", "chat line: %s",
          s.chat.empty() ? "" : s.chat.back().text.c_str());
    Writer ex2;
    ex2.u32(99); ex2.u32(0);   // unknown name, no experience (max level)
    s.handle(0x1F8, ex2.b, 15000);
    CHECK(!s.chat.empty() && s.chat.back().text == "Discovered a new area" && s.discovery.seq == 2, "no name / no xp: %s",
          s.chat.empty() ? "" : s.chat.back().text.c_str());
    Writer ex3;
    ex3.u32(12);   // truncated: ignored
    s.handle(0x1F8, ex3.b, 16000);
    CHECK(s.discovery.seq == 2, "a truncated packet is ignored");

    std::printf("subtitle_test: %d checks, %d failed\n", g_ok + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
