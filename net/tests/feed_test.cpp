// feed_test — 7.13: the Feed Pet list. The 1.12 server sends subclass 0 for EVERY consumable (vMaNGOS
// HandleItemQuerySingleOpcode: Class == CONSUMABLE ? 0 : SubClass), so Food & Drink cannot be found by subclass 5:
// every consumable is offered and the server checks the pet's diet. Trade Goods / Meat is offered too.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    GameSession s;
    std::vector<std::pair<uint16_t, Bytes>> out;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) out.push_back({op, d}); };
    auto fl = [](float v) { uint32_t u; memcpy(&u, &v, 4); return u; };
    auto values = [&](Writer& w, const std::map<uint16_t, uint32_t>& f) {
        uint16_t top = 0;
        for (auto& kv : f) top = std::max<uint16_t>(top, kv.first);
        const uint8_t blocks = (uint8_t)(top / 32 + 1);
        w.u8(blocks);
        std::vector<uint32_t> mask(blocks, 0);
        for (auto& kv : f) mask[kv.first / 32] |= 1u << (kv.first % 32);
        for (uint32_t m : mask) w.u32(m);
        for (auto& kv : f) w.u32(kv.second);
    };
    auto create = [&](uint64_t guid, uint8_t type, bool self, float x, std::map<uint16_t, uint32_t> f) {
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(type);
        w.u8(self ? 0x71 : 0x70); w.u32(0); w.u32(0); w.f32(x); w.f32(0); w.f32(0); w.f32(0); w.f32(0);
        for (int k = 0; k < 6; ++k) w.f32(7);
        w.u32(1);
        f[0] = (uint32_t)guid; f[1] = (uint32_t)(guid >> 32); f[2] = type == 4 ? 0x19u : 0x9u;
        f[UF::UNIT_BOUNDINGRADIUS] = fl(0.5f);
        values(w, f);
        s.handle(0x0A9, w.b, 1);
    };
    const uint64_t me = 0x77, cooked = 0x400000000001ull, meat = 0x400000000002ull, ore = 0x400000000003ull,
                   unknown = 0x400000000004ull;
    std::map<uint16_t, uint32_t> self = {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}};
    uint64_t bagItems[] = {cooked, meat, ore, unknown};
    for (size_t i = 0; i < 4; ++i) {   // backpack slots 23..26
        const uint16_t slot = (uint16_t)(w3n::kSlotBackpackFirst + i);
        self[UF::PLAYER_FIELD_INV_SLOT_HEAD + 2 * slot] = (uint32_t)bagItems[i];
        self[UF::PLAYER_FIELD_INV_SLOT_HEAD + 2 * slot + 1] = (uint32_t)(bagItems[i] >> 32);
    }
    create(me, 4, true, 0, self);
    s.objects.setSelf(me);
    create(cooked, 1, false, 0, {{UF::OBJECT_ENTRY, 10}});
    create(meat, 1, false, 0, {{UF::OBJECT_ENTRY, 11}});
    create(ore, 1, false, 0, {{UF::OBJECT_ENTRY, 12}});
    create(unknown, 1, false, 0, {{UF::OBJECT_ENTRY, 13}});

    ItemInfo food;
    food.entry = 10; food.cls = 0; food.subclass = 0; food.name = "Cooked Meat"; food.maxStack = 10;   // server sends 0
    ItemInfo flesh;
    flesh.entry = 11; flesh.cls = 7; flesh.subclass = 8; flesh.name = "Raw Meat"; flesh.maxStack = 10;
    ItemInfo metal;
    metal.entry = 12; metal.cls = 7; metal.subclass = 7; metal.name = "Copper Ore"; metal.maxStack = 10;
    s.testSetItemInfo(food);
    s.testSetItemInfo(flesh);
    s.testSetItemInfo(metal);

    std::vector<InvItem> got = s.petFoodCandidates();
    CHECK(got.size() == 2, "cooked food (subclass 0) and meat are offered, ore is not (%zu)", got.size());
    bool haveFood = false, haveMeat = false;
    for (const InvItem& it : got) {
        haveFood |= it.entry == 10;
        haveMeat |= it.entry == 11;
    }
    CHECK(haveFood, "Food & Drink with subclass 0 is in the list");
    CHECK(haveMeat, "Trade Goods / Meat is in the list");
    CHECK(!s.itemInfoIfKnown(13), "the unknown bag item is not known yet");

    out.clear();
    s.update(1000);
    bool asked = false;
    for (auto& p : out) if (p.first == 0x056 && p.second.size() >= 4) asked = true;
    CHECK(asked, "an unknown bag item is asked for by ITEM_QUERY");

    s.pet.active = true;
    s.pet.guid = 0xF130000045000201ull;
    InvItem src;
    src.entry = 10; src.guid = cooked; src.count = 1;
    out.clear();
    CHECK(s.feedPet(src, 2000), "Feed Pet sends the cast");
    CHECK(out.size() == 1 && out[0].first == 0x12E, "CMSG_CAST_SPELL");
    if (out.size() == 1 && out[0].second.size() >= 6) {
        Reader r(out[0].second);
        const uint32_t spell = r.u32();
        const uint16_t mask = r.u16();
        CHECK(spell == GameSession::kSpellFeedPet, "spell 6991 (%u)", spell);
        CHECK(mask == 0x10, "TARGET_FLAG_ITEM (0x%x)", mask);
    }

    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
