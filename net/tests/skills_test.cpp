// 6.19: the skills page's data — skills.txt, the rank titles, and the decoding of the 128 PLAYER_SKILL_INFO slots.
#define private public
#include "game.hpp"
#undef private
#include <cstdio>
using namespace w3n;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    CHECK(skillRankTier(1) == 0 && skillRankTier(75) == 1 && skillRankTier(76) == 2 && skillRankTier(150) == 2 &&
          skillRankTier(225) == 3 && skillRankTier(300) == 4 && skillRankTier(375) == 5, "rank tiers");
    SkillTable t;
    CHECK(!t.load("/nonexistent/skills.txt") && !t.loaded(), "missing file");
    if (argc > 1) {
        CHECK(t.load(argv[1]), "load %s", argv[1]);
        const SkillLine* s = t.find(186);
        CHECK(s && s->category == 11 && s->name == "Mining" && s->description.rfind("Ore and rock", 0) == 0, "Mining");
        CHECK(t.find(43) && t.find(43)->category == 6, "Swords");
    }
    GameSession g;
    CHECK(g.playerSkills().empty(), "skills without a player");
    WorldObject me;
    me.guid = 7; me.type = ObjType::Player; me.self = true;
    me.fields.assign(UF::PLAYER_SKILL_INFO_1_1 + 128 * 3 + 4, 0);
    auto put = [&](int slot, uint32_t id, uint32_t val, uint32_t mx, int tmp, int perm) {
        const int b = UF::PLAYER_SKILL_INFO_1_1 + slot * 3;
        me.fields[b] = id | (1u << 16);
        me.fields[b + 1] = val | (mx << 16);
        me.fields[b + 2] = (uint16_t)(int16_t)tmp | ((uint32_t)(uint16_t)(int16_t)perm << 16);
    };
    put(0, 186, 120, 150, 0, 5);
    put(3, 43, 300, 300, -4, 0);
    put(127, 98, 300, 300, 0, 0);
    g.objects.objs_[7] = me;
    g.self_ = 7;
    auto v = g.playerSkills();
    CHECK(v.size() == 3, "3 skills, got %zu", v.size());
    if (v.size() == 3) {
        CHECK(v[0].id == 186 && v[0].value == 120 && v[0].max == 150 && v[0].permBonus == 5 && v[0].tempBonus == 0, "slot 0");
        CHECK(v[1].id == 43 && v[1].tempBonus == -4, "slot 3 negative bonus (%d)", v[1].tempBonus);
        CHECK(v[2].id == 98, "last slot");
    }
    std::printf(fails ? "skills_test: %d FAILED\n" : "skills_test: ok\n", fails);
    return fails != 0;
}
