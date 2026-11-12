// useitem_test — an item whose spell needs a target (quest items "use on ...": Foreman's Blackjack on a Lazy Peon) names the
// selected creature / object in CMSG_USE_ITEM; food, potions and ground spells keep mask 0. 7.16: an item used on another
// item (Shiny Bauble on the fishing pole) names its GUID with TARGET_FLAG_ITEM, taking the main hand for a held-item spell.
#include <algorithm>
#include <cstdio>
#include <vector>

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
    {   // 6.59b: a quest item used with no target selected goes on the nearest usable object within 10 yd (Samuel's Remains on Marla's
        // Grave, quest 6395: the original client asks for a click on the grave); a far one, a plain item or no object nearby: mask 0
        GameSession q;
        q.objects.setSelf(me);
        q.spellNames.addSelfCast(433);
        auto goAt = [&](uint64_t guid, uint32_t entry, float x, uint32_t goType, uint32_t dyn) {
            std::vector<std::pair<uint32_t, uint32_t>> f = {{0, (uint32_t)guid}, {1, (uint32_t)(guid >> 32)}, {2, 0x21u}, {3, entry},
                                                            {UF::GAMEOBJECT_TYPE_ID, goType}, {UF::GAMEOBJECT_DYN_FLAGS, dyn}};
            uint32_t maxIdx = 0;
            for (auto& kv : f) maxIdx = std::max(maxIdx, kv.first);
            const uint32_t blocks = maxIdx / 32 + 1;
            std::vector<uint32_t> mask(blocks, 0);
            for (auto& kv : f) mask[kv.first / 32] |= 1u << (kv.first % 32);
            std::sort(f.begin(), f.end());
            Writer w;
            w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(5);
            w.u8(0x40); w.f32(x); w.f32(0); w.f32(0); w.f32(0);   // HAS_POSITION
            w.u8((uint8_t)blocks);
            for (uint32_t m : mask) w.u32(m);
            for (auto& kv : f) w.u32(kv.second);
            q.handle(0x0A9, w.b, 1);
        };
        goAt(0xF110000002B7A001ull, 178090, 3.0f, 10, 1);    // Marla's Grave, 3 yd away, usable for the quest
        goAt(0xF110000000000002ull, 1234, 2.0f, 5, 0);       // a plain decoration nearer: not a usable kind
        goAt(0xF110000000000003ull, 999, 40.0f, 10, 1);      // a goober far away
        ItemInfo rem;
        rem.entry = 16333; rem.cls = 12; rem.spellId[0] = 20804; rem.spellTrigger[0] = 0;
        q.testSetItemInfo(rem);
        ItemInfo tool;
        tool.entry = 5956; tool.cls = 7; tool.spellId[0] = 20805; tool.spellTrigger[0] = 0;
        q.testSetItemInfo(tool);
        std::vector<Bytes> out;
        q.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1 && op == 0x0AB) out.push_back(d); };
        InvItem r1; r1.bag = 255; r1.slot = 25; r1.entry = 16333;
        q.useItem(r1, 100);
        CHECK(out.size() == 1 && out[0].size() > 5 && out[0][3] == 0x00 && out[0][4] == 0x08, "quest item, no target -> the grave (0x800)");
        if (out.size() == 1 && out[0].size() > 5) {
            Reader r(out[0]);
            r.u8(); r.u8(); r.u8(); r.u16();
            CHECK(r.packedGuid() == 0xF110000002B7A001ull, "the grave's guid");
        }
        out.clear();
        InvItem t1; t1.bag = 255; t1.slot = 26; t1.entry = 5956;
        q.useItem(t1, 110);   // not a quest item: as before
        CHECK(out.size() == 1 && out[0].size() == 5 && out[0][3] == 0 && out[0][4] == 0, "a trade good -> mask 0");
    }
    {   // 6.73: quest objects of every kind answer the use key; the ones the server flagged for us come first
        GameSession q;
        q.objects.setSelf(me);
        auto goAt = [&](uint64_t guid, uint32_t entry, float x, uint32_t goType, uint32_t dyn) {
            std::vector<std::pair<uint32_t, uint32_t>> f = {{0, (uint32_t)guid}, {1, (uint32_t)(guid >> 32)}, {2, 0x21u}, {3, entry},
                                                            {UF::GAMEOBJECT_TYPE_ID, goType}, {UF::GAMEOBJECT_DYN_FLAGS, dyn}};
            uint32_t maxIdx = 0;
            for (auto& kv : f) maxIdx = std::max(maxIdx, kv.first);
            const uint32_t blocks = maxIdx / 32 + 1;
            std::vector<uint32_t> mask(blocks, 0);
            for (auto& kv : f) mask[kv.first / 32] |= 1u << (kv.first % 32);
            std::sort(f.begin(), f.end());
            Writer w;
            w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(5);
            w.u8(0x40); w.f32(x); w.f32(0); w.f32(0); w.f32(0);
            w.u8((uint8_t)blocks);
            for (uint32_t m : mask) w.u32(m);
            for (auto& kv : f) w.u32(kv.second);
            q.handle(0x0A9, w.b, 1);
        };
        goAt(0xF110000000000011ull, 11, 2.0f, 10, 0);   // a goober with no flag yet
        goAt(0xF110000000000012ull, 12, 2.5f, 1, 0);    // a button
        goAt(0xF110000000000013ull, 13, 3.0f, 9, 0);    // a readable text
        goAt(0xF110000000000014ull, 14, 3.5f, 22, 0);   // a spell caster
        goAt(0xF110000000000015ull, 15, 4.0f, 0, 0);    // a door: not offered
        const char* v = nullptr;
        const float pos[3] = {0, 0, 0};
        CHECK(q.useVerb(*q.objects.find(0xF110000000000011ull)) && !std::strcmp(q.useVerb(*q.objects.find(0xF110000000000011ull)), "Use"), "unflagged goober: Use");
        CHECK(q.useVerb(*q.objects.find(0xF110000000000012ull)) != nullptr, "button: offered");
        CHECK(q.useVerb(*q.objects.find(0xF110000000000013ull)) && !std::strcmp(q.useVerb(*q.objects.find(0xF110000000000013ull)), "Read"), "text: Read");
        CHECK(q.useVerb(*q.objects.find(0xF110000000000014ull)) != nullptr, "spell caster: offered");
        CHECK(q.useVerb(*q.objects.find(0xF110000000000015ull)) == nullptr, "door: not offered");
        goAt(0xF110000000000016ull, 16, 5.0f, 10, 1);   // flagged, farther than the unflagged one
        CHECK(q.softInteract(pos, 0.0f, 100, &v) == 0xF110000000000016ull, "the flagged goober beats the nearer unflagged ones");
        std::vector<Bytes> out;
        q.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1 && op == 0x0B1) out.push_back(d); };
        const float at[3] = {0, 0, 0};
        CHECK(q.interact(0xF110000000000012ull, at, 200).empty() && out.size() == 1, "using the button sends CMSG_GAMEOBJ_USE");
    }
    {   // 7.07: an item that begins a quest and has no spell asks the item for its quest (CMSG_QUESTGIVER_QUERY_QUEST u64 item,
        // u32 quest); CMSG_USE_ITEM would get "Item not found". An item with a spell and a quest keeps CMSG_USE_ITEM.
        GameSession q;
        q.objects.setSelf(me);
        ItemInfo note;
        note.entry = 1972; note.cls = 12; note.startQuest = 783;
        q.testSetItemInfo(note);
        ItemInfo both;
        both.entry = 2000; both.startQuest = 5; both.spellId[0] = 433; both.spellTrigger[0] = 0;
        q.testSetItemInfo(both);
        std::vector<std::pair<uint16_t, Bytes>> out;
        q.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) out.push_back({op, d}); };
        InvItem n; n.bag = 255; n.slot = 27; n.entry = 1972; n.guid = 0x4000000000001234ull;
        q.useItem(n, 10);
        CHECK(out.size() == 1 && out[0].first == 0x186 && out[0].second.size() == 12, "quest item -> CMSG_QUESTGIVER_QUERY_QUEST");
        if (out.size() == 1 && out[0].second.size() == 12) {
            Reader r(out[0].second);
            const uint64_t g = r.u64();
            const uint32_t quest = r.u32();
            CHECK(g == n.guid && quest == 783, "the item's guid and its quest");
        }
        CHECK(note.usable(), "a quest item counts as usable");
        out.clear();
        InvItem b; b.bag = 255; b.slot = 28; b.entry = 2000; b.guid = 0x4000000000001235ull;
        q.useItem(b, 20);
        CHECK(out.size() == 1 && out[0].first == 0x0AB, "quest item with an on-use spell -> CMSG_USE_ITEM");
    }
    {   // 7.16: the converter's @item line: the requirement masks and the main-hand flag
        const char* path = "useitem_spells_tmp.txt";
        FILE* f = fopen(path, "wb");
        if (f) {
            fputs("@item\t8087\t2\t1048576\t0\t1\n@enchant\t263\tFishing Lure +25\n8087\tShiny Bauble\t\n", f);
            fclose(f);
        }
        SpellNames names;
        CHECK(f && names.load(path), "spells.txt with @item loads");
        const SpellNames::ItemTarget* t = names.itemTarget(8087);
        CHECK(t && t->mainHand, "main-hand flag");
        CHECK(t && t->fits(2, 20, 17) && !t->fits(2, 7, 21) && !t->fits(4, 1, 5), "class / subclass masks");
        CHECK(names.enchantName(263) == "Fishing Lure +25", "enchant name");
        if (f) remove(path);
    }
    {   // 7.16: an item used on another item - Shiny Bauble on the fishing pole in the main hand. The packet names the
        // pole (TARGET_FLAG_ITEM 0x10 and its GUID); a "held item" spell always takes the main hand, as the original does.
        auto setF = [](WorldObject* o, uint16_t f, uint32_t v) { if (o->fields.size() <= f) o->fields.resize(f + 1u, 0); o->fields[f] = v; };
        auto itemObj = [&](uint64_t guid, uint32_t entry) {
            Writer w;
            w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(1);
            w.u8(0x10); w.u32(1);
            w.u8(1); w.u32(0xF);
            w.u32((uint32_t)guid); w.u32((uint32_t)(guid >> 32)); w.u32(0x3u); w.u32(entry);
            s.handle(0x0A9, w.b, 1);
        };
        const uint64_t pole = 0x400000000000BEEFull, lureGuid = 0x400000000000BEE1ull;
        itemObj(pole, 6366);
        itemObj(lureGuid, 6529);
        WorldObject* p = s.objects.find(me);
        setF(p, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * 15, (uint32_t)pole);
        setF(p, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * 15 + 1, (uint32_t)(pole >> 32));
        setF(p, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * 23, (uint32_t)lureGuid);
        setF(p, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * 23 + 1, (uint32_t)(lureGuid >> 32));
        ItemInfo lure;
        lure.entry = 6529; lure.cls = 0; lure.spellId[0] = 8087; lure.spellTrigger[0] = 0;
        s.testSetItemInfo(lure);
        ItemInfo poleInfo;
        poleInfo.entry = 6366; poleInfo.cls = 2; poleInfo.subclass = 20; poleInfo.inventoryType = 17; poleInfo.name = "Fishing Pole";
        s.testSetItemInfo(poleInfo);
        ItemInfo sword;   // a weapon the bait does not fit: never chosen
        sword.entry = 1000; sword.cls = 2; sword.subclass = 7; sword.inventoryType = 21;
        s.testSetItemInfo(sword);
        s.spellNames.addItemTarget(8087, SpellNames::ItemTarget{2, 1u << 20, 0, true});
        s.spellNames.addEnchant(263, "Fishing Lure +25");
        setF(s.objects.find(pole), UF::ITEM_FIELD_ENCHANTMENT, 263);
        setF(s.objects.find(pole), UF::ITEM_FIELD_ENCHANTMENT + 1, 600000);   // 10 minutes at apply time
        CHECK(s.itemOnItemSpell(6529) == 8087, "Shiny Bauble is an item-on-item spell");
        CHECK(s.itemTargetSpell(6529) == 0, "it is not a unit / object target spell");
        InvItem bait;
        bait.bag = 255; bait.slot = 23; bait.entry = 6529; bait.guid = lureGuid;
        std::vector<InvItem> targets = s.itemOnItemTargets(bait);
        CHECK(targets.size() == 1 && targets[0].guid == pole, "only the fishing pole fits");
        CHECK(targets.size() == 1 && targets[0].enchant[0].id == 263 && targets[0].enchant[0].leftMs == 600000,
              "the pole carries enchant 263 for 10 min");
        std::vector<std::string> tip = itemTooltip(poleInfo, targets.empty() ? nullptr : &targets[0], s.spellNames);
        bool hasEnchant = false;
        for (const std::string& l : tip) hasEnchant = hasEnchant || l == "Enchant: Fishing Lure +25 (10m 0s left)";
        CHECK(hasEnchant, "the item card shows the enchant and its time");
        {   // SMSG_ITEM_ENCHANT_TIME_UPDATE: 120 s left; the card counts down from there
            Writer e;
            e.u64(pole); e.u8(0); e.u32(120); e.u64(me);
            s.handle(0x1EB, e.b, 1000);
            s.update(31000);
            InvItem fresh;
            fresh.guid = pole;
            CHECK(s.refreshItem(fresh), "refresh the carried item");
            CHECK(fresh.enchant[0].leftMs == 90000, "90 s left after 30 s");
            tip = itemTooltip(poleInfo, &fresh, s.spellNames);
            hasEnchant = false;
            for (const std::string& l : tip) hasEnchant = hasEnchant || l == "Enchant: Fishing Lure +25 (1m 30s left)";
            CHECK(hasEnchant, "the card counts down");
        }
        sent.clear();
        s.useItem(bait, 200);
        CHECK(sent.size() == 1 && sent[0].size() > 5 && sent[0][3] == 0x10 && sent[0][4] == 0x00, "use -> TARGET_FLAG_ITEM 0x10");
        if (sent.size() == 1) {
            Reader r(sent[0]);
            r.u8(); r.u8(); r.u8();
            const uint16_t mask = r.u16();
            CHECK(mask == 0x10 && r.packedGuid() == pole, "the pole's guid");
        }
        sent.clear();
        s.useItemOnItem(bait, pole, 210);   // the picked target (the touch-screen item list)
        CHECK(sent.size() == 1 && sent[0].size() > 5 && sent[0][3] == 0x10 && sent[0][4] == 0x00, "useItemOnItem -> 0x10");
    }
    std::printf("USE ITEM ON TARGET: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
