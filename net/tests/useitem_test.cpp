// useitem_test — an item whose spell needs a target (quest items "use on ...": Foreman's Blackjack on a Lazy Peon) names the
// selected creature / object in CMSG_USE_ITEM; food, potions and ground spells keep mask 0.
#include <cstdio>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    GameSession s;
    const uint64_t me = 0x77, peon = 0xF130000027AC0001ull, chest = 0xF110000001230001ull;
    auto create = [&](uint64_t guid, uint8_t type, bool self, uint32_t entry) {
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(type);
        if (type == 4 || type == 3) {
            w.u8(self ? 0x71 : 0x70); w.u32(0); w.u32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0); w.f32(0);
            for (int k = 0; k < 6; ++k) w.f32(7);
            w.u32(1);
        } else {
            w.u8(0x10); w.u32(1);
        }
        w.u8(1);
        w.u32(0xF);
        w.u32((uint32_t)guid); w.u32((uint32_t)(guid >> 32)); w.u32(type == 4 ? 0x19u : type == 3 ? 0x9u : 0x21u); w.u32(entry);
        s.handle(0x0A9, w.b, 1);
    };
    create(me, 4, true, 0);
    create(peon, 3, false, 10556);
    create(chest, 5, false, 1234);
    s.objects.setSelf(me);
    s.spellNames.addSelfCast(433);   // food
    auto item = [&](uint32_t entry, uint32_t spell) {
        ItemInfo i;
        i.entry = entry;
        i.spellId[0] = spell;
        i.spellTrigger[0] = 0;
        s.testSetItemInfo(i);
    };
    item(16114, 19938);   // Foreman's Blackjack: Awaken Peon, needs the peon
    item(117, 433);       // bread: self
    InvItem blackjack, bread;
    blackjack.bag = 255; blackjack.slot = 23; blackjack.entry = 16114;
    bread.bag = 255; bread.slot = 24; bread.entry = 117;
    std::vector<Bytes> sent;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1 && op == 0x0AB) sent.push_back(d); };

    CHECK(s.itemTargetSpell(16114) == 19938, "blackjack needs a target");
    CHECK(s.itemTargetSpell(117) == 0, "bread does not");

    s.useItem(blackjack, 10);   // no target: mask 0 (the server answers with its own error)
    CHECK(sent.size() == 1 && sent[0].size() == 5 && sent[0][3] == 0 && sent[0][4] == 0, "no target -> mask 0");

    s.setTarget(peon, 20);
    sent.clear();
    s.useItem(blackjack, 30);
    // u8 bag, u8 slot, u8 spell index, u16 mask 0x0002, packed guid (mask byte + the non-zero bytes)
    CHECK(sent.size() == 1 && sent[0].size() > 5 && sent[0][3] == 0x02 && sent[0][4] == 0x00, "peon -> UNIT mask");
    if (sent.size() == 1 && sent[0].size() > 5) {
        Reader r(sent[0]);
        r.u8(); r.u8(); r.u8(); r.u16();
        CHECK(r.packedGuid() == peon, "the peon's guid");
    }

    sent.clear();
    s.useItem(bread, 40);   // a self spell with a target selected: still mask 0
    CHECK(sent.size() == 1 && sent[0].size() == 5 && sent[0][3] == 0 && sent[0][4] == 0, "bread -> mask 0 with a target");

    s.setTarget(chest, 50);
    sent.clear();
    s.useItem(blackjack, 60);
    CHECK(sent.size() == 1 && sent[0].size() > 5 && sent[0][3] == 0x00 && sent[0][4] == 0x08, "object -> GAMEOBJECT mask 0x800");

    {   // without spell data (spells.txt missing) nothing can be told: the old packet, mask 0
        GameSession bare;
        bare.objects.setSelf(me);
        ItemInfo i;
        i.entry = 16114; i.spellId[0] = 19938; i.spellTrigger[0] = 0;
        bare.testSetItemInfo(i);
        CHECK(bare.itemTargetSpell(16114) == 0, "no spell data -> no target spell");
    }
    std::printf("USE ITEM ON TARGET: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
