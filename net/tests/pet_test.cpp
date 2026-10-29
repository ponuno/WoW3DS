// pet_test — UI stage 13: SMSG_PET_SPELLS / PET_MODE / feedback / stable lists through GameSession::handle with
// hand-built packets, and the bytes of CMSG_PET_ACTION, PET_SPELL_AUTOCAST, PET_ABANDON, PET_RENAME, STABLE_*.
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

static Bytes petSpells(uint64_t guid, uint8_t react, uint8_t command, uint8_t flags, const uint32_t bar[10], const std::vector<uint32_t>& book,
                       const std::vector<std::array<uint32_t, 4>>& cds) {
    Writer w;
    w.u64(guid); w.u32(0); w.u8(react); w.u8(command); w.u8(0); w.u8(flags);
    for (int i = 0; i < 10; ++i) w.u32(bar[i]);
    w.u8((uint8_t)book.size());
    for (uint32_t b : book) w.u32(b);
    w.u16((uint16_t)cds.size());
    for (auto& c : cds) { w.u32(c[0]); w.u16((uint16_t)c[1]); w.u32(c[2]); w.u32(c[3]); }
    return w.b;
}

int main() {
    GameSession s;
    std::vector<std::pair<uint16_t, Bytes>> sent;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) sent.push_back({op, d}); };
    const uint64_t me = 0x77, petG = 0xF130000000000005ull | (0x1234ull << 24);
    s.objects.setSelf(me);
    const uint32_t kClassHunter = 3;
    create(s, me, 4, true, {{UF::UNIT_FIELD_SUMMON, (uint32_t)petG}, {UF::UNIT_FIELD_SUMMON + 1, (uint32_t)(petG >> 32)},
                            {UF::UNIT_BYTES_0, (kClassHunter << 8) | 1}, {UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}}, 100);
    create(s, petG, 3, false, {{UF::UNIT_HEALTH, 50}, {UF::UNIT_MAXHEALTH, 80}, {UF::UNIT_LEVEL, 12}, {UF::UNIT_FIELD_PETNUMBER, 7},
                               {UF::UNIT_POWER1 + 4, 500000}, {UF::UNIT_MAXPOWER1 + 4, 1050000}, {UF::UNIT_BYTES_1, 4u << 8},
                               {UF::UNIT_FLAGS, 0x10}}, 100);
    const uint64_t foe = 0xF130000000000099ull;
    create(s, foe, 3, false, {{UF::UNIT_HEALTH, 10}, {UF::UNIT_MAXHEALTH, 10}}, 100);
    CHECK(s.objects.find(petG) && s.objects.find(me), "objects created");
    CHECK(s.petGuid() == petG, "the pet's guid comes from UNIT_FIELD_SUMMON before any bar (%llx)", (unsigned long long)s.petGuid());
    CHECK(s.petHappiness() == 2, "happiness: 500000 = content (%d)", s.petHappiness());
    CHECK(s.petLoyalty() == 4, "loyalty from BYTES_1 byte 1 (%d)", s.petLoyalty());
    CHECK(s.petCanRename() && s.petCanAbandon(), "rename flag and hunter's pet");
    CHECK(!s.pet.active && !s.petCommand(kPetAttack, 200), "no bar yet: no command");

    // ---- the bar
    const uint32_t bar[10] = {2 | 7u << 24, 1 | 7u << 24, 0 | 7u << 24, 16827 | 0xC1u << 24, 17253 | 0x81u << 24, 24604 | 0x01u << 24, 0 | 0x81u << 24,
                              2 | 6u << 24, 1 | 6u << 24, 0 | 6u << 24};
    s.handle(0x179, petSpells(petG, 1, 1, 0, bar, {16827 | 0xC1u << 24, 17253 | 0x81u << 24, 24604 | 0x01u << 24}, {{17253, 0, 5000, 0}}), 1000);
    CHECK(s.pet.active && s.pet.guid == petG && s.pet.react == 1 && s.pet.command == 1 && s.pet.enabled, "bar header");
    CHECK(s.pet.spells.size() == 3 && s.petKnowsSpell(17253) && s.petKnowsSpell(24604) && !s.petKnowsSpell(1), "book");
    CHECK(s.petAutocast(16827) && !s.petAutocast(17253) && !s.petCanAutocast(24604) && s.petCanAutocast(17253), "auto-cast states");
    CHECK(s.petCooldownLeftMs(17253, 2000) == 4000 && s.petCooldownTotalMs(17253) == 5000 && s.petCooldownLeftMs(17253, 6001) == 0, "cooldown from the bar packet");
    CHECK(PetState::spellOf(s.pet.bar[3]) == 16827 && PetState::typeOf(s.pet.bar[9]) == PetAct::Reaction, "bar slots");

    // ---- commands
    sent.clear();
    CHECK(!s.petCommand(kPetAttack, 1100) && sent.empty(), "attack without a target is refused");
    s.setTarget(foe, 1100);
    sent.clear();
    CHECK(s.petCommand(kPetAttack, 1200), "attack with a target");
    CHECK(sent.size() == 1 && sent[0].first == 0x175 && sent[0].second.size() == 20, "CMSG_PET_ACTION size");
    {
        Reader r(sent[0].second);
        CHECK(r.u64() == petG && r.u32() == (2u | 7u << 24) && r.u64() == foe, "attack: pet, 0x07000002, target");
    }
    sent.clear();
    CHECK(s.petCommand(kPetStay, 1300) && s.pet.command == kPetStay, "stay");
    { Reader r(sent[0].second); r.u64(); CHECK(r.u32() == (0u | 7u << 24) && r.u64() == 0, "stay: 0x07000000, no target"); }
    sent.clear();
    CHECK(s.petReaction(kPetPassive, 1400) && s.pet.react == kPetPassive, "passive");
    { Reader r(sent[0].second); r.u64(); CHECK(r.u32() == (0u | 6u << 24), "passive: 0x06000000"); }
    CHECK(!s.petReaction(5, 1400), "bad reaction refused");

    // ---- spells
    sent.clear();
    CHECK(!s.petCastSpell(17253, 2000), "Dash... cooling down");   // 4 s left
    CHECK(sent.empty(), "nothing sent while cooling");
    CHECK(s.petCastSpell(17253, 7000), "castable after the cooldown");
    { Reader r(sent[0].second); r.u64(); CHECK(r.u32() == (17253u | 0x81u << 24) && r.u64() == foe, "spell: 0x81 type, target"); }
    sent.clear();
    CHECK(s.petCastSpell(16827, 7000), "an auto-cast-on spell");
    { Reader r(sent[0].second); r.u64(); CHECK(r.u32() == (16827u | 0xC1u << 24), "auto-cast spell is sent with 0xC1"); }
    sent.clear();
    CHECK(!s.petCastSpell(24604, 7000) && !s.petCastSpell(999, 7000) && sent.empty(), "passive / unknown spells are not cast");

    // ---- auto-cast toggle
    sent.clear();
    s.petSetAutocast(17253, true, 7100);
    CHECK(sent.size() == 1 && sent[0].first == 0x2F3 && sent[0].second.size() == 13, "CMSG_PET_SPELL_AUTOCAST size");
    { Reader r(sent[0].second); CHECK(r.u64() == petG && r.u32() == 17253 && r.u8() == 1, "autocast: pet, spell, on"); }
    CHECK(s.petAutocast(17253) && PetState::typeOf(s.pet.bar[4]) == PetAct::Enabled, "state follows at once, bar too");
    sent.clear();
    s.petSetAutocast(24604, true, 7100);
    CHECK(sent.empty(), "a passive spell has no auto-cast");

    // ---- mode, feedback, name
    {
        Writer w; w.u64(petG); w.u8(2); w.u8(2); w.u8(0); w.u8(0x8);
        s.handle(0x17A, w.b, 8000);
        CHECK(s.pet.react == 2 && s.pet.command == 2 && !s.pet.enabled, "SMSG_PET_MODE");
        CHECK(!s.petCastSpell(17253, 9000), "a disabled bar casts nothing");
        Writer w2; w2.u64(petG); w2.u8(1); w2.u8(1); w2.u8(0); w2.u8(0);
        s.handle(0x17A, w2.b, 8100);
        CHECK(s.pet.enabled, "enabled again");
    }
    {
        Writer w; w.u8(2);
        const size_t before = s.chat.size();
        s.handle(0x2C6, w.b, 8200);
        CHECK(s.petFeedback == 1 && s.chat.size() > before, "action feedback");
    }
    {
        Writer w; w.u32(7); w.cstr("Fang"); w.u32(1234);
        s.handle(0x053, w.b, 8300);
        CHECK(s.petName() == "Fang", "pet name from the query (%s)", s.petName().c_str());
    }
    {   // update() asks for the name when the pet's number is known and the name is not
        GameSession t;
        std::vector<uint16_t> ops;
        t.tap = [&](uint32_t, int dir, uint16_t op, const Bytes&) { if (dir == 1) ops.push_back(op); };
        t.objects.setSelf(me);
        create(t, me, 4, true, {{UF::UNIT_FIELD_SUMMON, (uint32_t)petG}, {UF::UNIT_FIELD_SUMMON + 1, (uint32_t)(petG >> 32)}}, 100);
        create(t, petG, 3, false, {{UF::UNIT_FIELD_PETNUMBER, 9}}, 100);
        t.handle(0x179, petSpells(petG, 1, 1, 0, bar, {}, {}), 200);
        t.update(300);
        bool asked = false;
        for (uint16_t o : ops) asked = asked || o == 0x052;
        CHECK(asked, "CMSG_PET_NAME_QUERY sent for pet number 9");
    }

    // ---- dismiss, rename
    sent.clear();
    s.petDismiss(9000);
    CHECK(sent.size() == 1 && sent[0].first == 0x176 && sent[0].second.size() == 8, "CMSG_PET_ABANDON");
    sent.clear();
    CHECK(!s.petRename("X", 9000) && !s.petRename("Bad1", 9000) && !s.petRename("WayTooLongName", 9000) && sent.empty(), "bad names are not sent");
    CHECK(s.petRename("Rex", 9000), "good name");
    { Reader r(sent[0].second); CHECK(sent[0].first == 0x177 && r.u64() == petG && r.cstr() == "Rex" && !r.bad, "rename bytes"); }

    // ---- stop attack
    sent.clear();
    s.petStopAttack(9100);
    CHECK(sent.size() == 1 && sent[0].first == 0x2EA, "CMSG_PET_STOP_ATTACK");

    // ---- cast failure and broken
    {
        Writer w; w.u32(17253); w.u8(2); w.u8(0x28);
        const size_t before = s.chat.size();
        s.handle(0x138, w.b, 9200);
        CHECK(s.chat.size() > before, "pet cast failed prints a line");
        Writer b0; s.handle(0x2AF, b0.b, 9300);
    }

    // ---- stable
    {
        Writer w; w.u64(0x1111); w.u8(2); w.u8(1);
        w.u32(7); w.u32(1234); w.u32(12); w.cstr("Fang"); w.u32(4); w.u8(1);
        w.u32(8); w.u32(1235); w.u32(9); w.cstr("Sly"); w.u32(2); w.u8(2);
        s.handle(0x26F, w.b, 10000);
        CHECK(s.stable.open && s.stable.npc == 0x1111 && s.stable.slots == 1 && s.stable.pets.size() == 2, "stable list");
        CHECK(s.stable.pets[1].name == "Sly" && s.stable.pets[1].slot == 2 && s.stable.pets[0].loyalty == 4, "stable pets");
        sent.clear();
        s.stableCurrentPet(10100);
        CHECK(sent.size() == 1 && sent[0].first == 0x270 && sent[0].second.size() == 8, "CMSG_STABLE_PET");
        sent.clear();
        s.unstablePet(s.stable.pets[1], 10200);
        { Reader r(sent[0].second); CHECK(sent[0].first == 0x275 && r.u64() == 0x1111 && r.u32() == 8, "swap with a current pet"); }
        sent.clear();
        s.buyStableSlot(10300);
        CHECK(sent.size() == 1 && sent[0].first == 0x272, "CMSG_BUY_STABLE_SLOT");
        sent.clear();
        Writer res; res.u8(0x0A);
        s.handle(0x273, res.b, 10400);
        CHECK(sent.size() == 1 && sent[0].first == 0x26F, "a successful result asks for the list again");
        // no current pet: plain unstable
        Writer w2; w2.u64(0x1111); w2.u8(1); w2.u8(2); w2.u32(8); w2.u32(1235); w2.u32(9); w2.cstr("Sly"); w2.u32(2); w2.u8(2);
        s.handle(0x26F, w2.b, 10500);
        sent.clear();
        s.unstablePet(s.stable.pets[0], 10600);
        CHECK(sent.size() == 1 && sent[0].first == 0x271, "CMSG_UNSTABLE_PET when we have no pet");
        s.closeStable();
        CHECK(!s.stable.open, "closed");
    }

    // ---- the bar goes away
    {
        Writer w; w.u64(0);
        s.handle(0x179, w.b, 11000);
        CHECK(!s.pet.active && s.pet.spells.empty(), "empty SMSG_PET_SPELLS removes the bar");
        CHECK(s.petGuid() == petG, "the summon field still names the pet");
    }

    // ---- truncated / hostile packets never crash
    {
        Bytes full = petSpells(petG, 1, 1, 0, bar, {16827 | 0xC1u << 24}, {{1, 0, 2, 3}});
        for (size_t n = 0; n < full.size(); ++n) { Bytes cut(full.begin(), full.begin() + n); s.handle(0x179, cut, 12000); }
        Writer lst; lst.u64(1); lst.u8(200); lst.u8(1);
        s.handle(0x26F, lst.b, 12000);
        for (uint16_t op : {0x17A, 0x053, 0x178, 0x2C6, 0x138, 0x273}) { Bytes e; s.handle(op, e, 12000); Bytes one(1, 0xFF); s.handle(op, one, 12000); }
        CHECK(true, "fuzz");
    }
    std::printf("pet_test: %d checks, %d failed\n", g_ok + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
