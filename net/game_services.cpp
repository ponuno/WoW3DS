// Bank, flight masters and talents (UI stages 4/6). Formats: vMaNGOS Handlers/ItemHandler.cpp (banker, bank slots),
// Server/Packets/Npc.cpp (SMSG_SHOW_BANK), Server/Packets/Taxi.cpp (SMSG_SHOWTAXINODES, ActivateTaxi(Express),
// SMSG_ACTIVATETAXIREPLY), Server/Packets/Skill.cpp + Player::LearnTalent (CMSG_LEARN_TALENT and its rules).
#include <algorithm>
#include <cmath>

#include "game.hpp"
#include "wire.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_TAXINODE_STATUS_QUERY = 0x1AA, SMSG_TAXINODE_STATUS = 0x1AB, SMSG_SHOWTAXINODES = 0x1A9,
    CMSG_TAXIQUERYAVAILABLENODES = 0x1AC, CMSG_ACTIVATETAXI = 0x1AD, SMSG_ACTIVATETAXIREPLY = 0x1AE,
    SMSG_NEW_TAXI_PATH = 0x1AF, CMSG_ACTIVATETAXIEXPRESS = 0x312,
    CMSG_AREATRIGGER = 0x0B4, SMSG_AREA_TRIGGER_MESSAGE = 0x2B8,
    CMSG_BANKER_ACTIVATE = 0x1B7, SMSG_SHOW_BANK = 0x1B8, CMSG_BUY_BANK_SLOT = 0x1B9, SMSG_BUY_BANK_SLOT_RESULT = 0x1BA,
    CMSG_AUTOSTORE_BANK_ITEM = 0x282, CMSG_AUTOBANK_ITEM = 0x283,
    CMSG_LEARN_TALENT = 0x251,
    CMSG_UNLEARN_SKILL = 0x202,
};
constexpr uint16_t kCharacterPoints1 = 1102;   // PLAYER_CHARACTER_POINTS1

const char* taxiError(uint32_t code) {
    switch (code) {
        case 2: return "There is no direct path to that destination";
        case 3: return "You don't have enough money";
        case 4: return "You are too far away";
        case 5: return "There is no flight master nearby";
        case 6: return "You haven't learned that flight path yet";
        case 7: return "You are busy";
        case 8: return "You are already mounted";
        case 9: return "You can't fly while shapeshifted";
        case 10: return "You are moving";
        case 11: return "You are already there";
        case 12: return "You must be standing";
        default: return "The flight master cannot take you there";
    }
}
}  // namespace

// ------------------------------------------------------------------ bank
void GameSession::openBank(uint64_t npc, uint32_t nowMs) {
    Writer w;
    w.u64(npc);
    send(CMSG_BANKER_ACTIVATE, w.b, nowMs);
}

std::vector<InvItem> GameSession::bankItems() const {
    std::vector<InvItem> out;
    const WorldObject* me = selfObject();
    if (!me) return out;
    auto guidAt = [](const WorldObject& o, uint32_t field) {
        return (uint64_t)o.field((uint16_t)field) | ((uint64_t)o.field((uint16_t)(field + 1)) << 32);
    };
    auto add = [&](uint8_t bag, uint8_t slot, uint64_t g) {
        if (!g) return;
        InvItem it;
        it.bag = bag;
        it.slot = slot;
        it.guid = g;
        if (const WorldObject* o = objects.find(g)) {
            it.entry = o->entry();
            it.count = std::max<uint32_t>(1, o->field(UF::ITEM_FIELD_STACK_COUNT));
            it.durability = o->field(UF::ITEM_FIELD_DURABILITY);
            it.maxDurability = o->field(UF::ITEM_FIELD_MAXDURABILITY);
        }
        out.push_back(it);
    };
    for (uint8_t s = kSlotBankFirst; s < kSlotBankBagEnd; ++s) add(kBagSelf, s, guidAt(*me, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * s));
    for (uint8_t b = kSlotBankBagFirst; b < kSlotBankBagEnd; ++b) {
        const WorldObject* bag = objects.find(guidAt(*me, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * b));
        if (!bag || bag->type != ObjType::Container) continue;
        uint32_t n = std::min<uint32_t>(bag->field(UF::CONTAINER_FIELD_NUM_SLOTS), 36);
        for (uint32_t i = 0; i < n; ++i) add(b, (uint8_t)i, guidAt(*bag, UF::CONTAINER_FIELD_SLOT_1 + 2u * i));
    }
    return out;
}

int GameSession::bankBagSlots() const {
    const WorldObject* me = selfObject();
    return me ? (int)((me->field(UF::PLAYER_BYTES_2) >> 16) & 0xFF) : 0;
}

void GameSession::depositItem(const InvItem& it, uint32_t nowMs) {
    Writer w;
    w.u8(it.bag);
    w.u8(it.slot);
    send(CMSG_AUTOBANK_ITEM, w.b, nowMs);
}

void GameSession::withdrawItem(const InvItem& it, uint32_t nowMs) {
    Writer w;
    w.u8(it.bag);
    w.u8(it.slot);
    send(CMSG_AUTOSTORE_BANK_ITEM, w.b, nowMs);
}

void GameSession::buyBankSlot(uint32_t nowMs) {
    if (!bank.open) return;
    Writer w;
    w.u64(bank.npc);
    send(CMSG_BUY_BANK_SLOT, w.b, nowMs);
}

// ------------------------------------------------------------------ flight masters
void GameSession::openTaxi(uint64_t npc, uint32_t nowMs) {
    Writer w;
    w.u64(npc);
    send(CMSG_TAXIQUERYAVAILABLENODES, w.b, nowMs);
}

uint32_t GameSession::taxiCost(uint32_t node) const {
    if (!taxi.open || !taxiTable.loaded() || node == taxi.current) return 0;
    uint32_t cost = 0;
    std::vector<uint32_t> r = taxiTable.route(taxi.current, node, [&](uint32_t n) { return taxi.known(n); }, &cost);
    return r.empty() ? 0 : cost;
}

bool GameSession::flyTo(uint32_t node, uint32_t nowMs) {
    if (!taxi.open) return false;
    if (node == taxi.current) { fail("You are already there.", nowMs); return false; }
    if (!taxi.known(node)) { fail("You haven't learned that flight path yet.", nowMs); return false; }
    Writer w;
    w.u64(taxi.npc);
    if (!taxiTable.loaded() || taxiTable.directCost(taxi.current, node)) {   // without taxi.txt: let the server decide
        w.u32(taxi.current);
        w.u32(node);
        send(CMSG_ACTIVATETAXI, w.b, nowMs);
        return true;
    }
    uint32_t cost = 0;
    std::vector<uint32_t> route = taxiTable.route(taxi.current, node, [&](uint32_t n) { return taxi.known(n); }, &cost);
    if (route.size() < 2) { fail("There is no flight path to that destination.", nowMs); return false; }
    if (cost > money()) { fail("You don't have enough money.", nowMs); return false; }
    w.u32(cost);
    w.u32((uint32_t)route.size());
    for (uint32_t n : route) w.u32(n);
    send(CMSG_ACTIVATETAXIEXPRESS, w.b, nowMs);
    return true;
}

// ------------------------------------------------------------------ talents
uint32_t GameSession::talentPoints() const {
    const WorldObject* me = selfObject();
    return me ? me->field(kCharacterPoints1) : 0;
}

uint32_t GameSession::classId() const {
    const WorldObject* me = selfObject();
    return me ? (me->field(UF::UNIT_BYTES_0) >> 8) & 0xFF : 0;
}

// ------------------------------------------------------------------ skills (stage 6.19)
// vMaNGOS Player.cpp: PLAYER_SKILL_INDEX(x) = PLAYER_SKILL_INFO_1_1 + x*3 holds the skill id in its low 16 bits, +1 the value
// (low) and the maximum (high), +2 the temporary (low) and permanent (high) bonus as int16.
std::vector<GameSession::SkillState> GameSession::playerSkills() const {
    std::vector<SkillState> out;
    const WorldObject* me = selfObject();
    if (!me) return out;
    for (int k = 0; k < 128; ++k) {
        const uint32_t id = me->field((uint16_t)(UF::PLAYER_SKILL_INFO_1_1 + k * 3)) & 0xFFFF;
        if (!id) continue;
        const uint32_t v = me->field((uint16_t)(UF::PLAYER_SKILL_INFO_1_1 + k * 3 + 1));
        const uint32_t b = me->field((uint16_t)(UF::PLAYER_SKILL_INFO_1_1 + k * 3 + 2));
        SkillState s;
        s.id = id;
        s.value = (uint16_t)(v & 0xFFFF);
        s.max = (uint16_t)(v >> 16);
        s.tempBonus = (int16_t)(b & 0xFFFF);
        s.permBonus = (int16_t)(b >> 16);
        out.push_back(s);
    }
    return out;
}

void GameSession::unlearnSkill(uint32_t skill, uint32_t nowMs) {
    if (!skill) return;
    Writer w;
    w.u32(skill);
    send(CMSG_UNLEARN_SKILL, w.b, nowMs);
}

int GameSession::talentRank(const Talent& t) const {
    return talents.rankOf(t, [&](uint32_t s) { return knowsSpell(s); });
}

bool GameSession::learnTalent(uint32_t talentId, uint32_t nowMs) {
    const Talent* t = talents.find(talentId);
    if (!t) return false;
    const int rank = talentRank(*t);
    if (rank >= t->maxRank()) { fail("You already know every rank of that talent.", nowMs); return false; }
    if (!talentPoints()) { fail("You have no talent points.", nowMs); return false; }
    uint32_t spent = 0;
    for (const Talent* o : talents.talentsOf(t->tree)) spent += (uint32_t)talentRank(*o);
    if (spent < t->row * 5) {
        fail("You need " + std::to_string(t->row * 5) + " points in this tree first.", nowMs);
        return false;
    }
    if (t->reqTalent) {
        const Talent* req = talents.find(t->reqTalent);
        if (req && talentRank(*req) < (int)t->reqRank + 1) { fail("You need the talent before it first.", nowMs); return false; }
    }
    Writer w;
    w.u32(talentId);
    w.u32((uint32_t)rank);   // the rank index to learn (0 = the first)
    send(CMSG_LEARN_TALENT, w.b, nowMs);
    return true;
}

// Bank, flight master, auctioneer and mailbox windows close 10 yd away (the original's "too far" closing).
void GameSession::updateServices(uint32_t nowMs) {
    if (!bank.open && !taxi.open && !auction.open && !mail.open && !stable.open) return;
    const MovementInfo& m = mover.info();
    auto far = [&](uint64_t g) {
        const WorldObject* o = objects.find(g);
        if (!o || !o->hasPos) return false;   // not in our table (replays): keep it open
        float p[3];
        o->positionAt(nowMs, p);
        float dx = p[0] - m.x, dy = p[1] - m.y, dz = p[2] - m.z;
        return dx * dx + dy * dy + dz * dz > 10.0f * 10.0f;
    };
    if (bank.open && far(bank.npc)) closeBank();
    if (taxi.open && far(taxi.npc)) closeTaxi();
    if (auction.open && far(auction.npc)) closeAuction();
    if (mail.open && far(mail.box)) closeMail();
    if (stable.open && far(stable.npc)) closeStable();
}

// ------------------------------------------------------------------ server packets
int GameSession::checkAreaTriggers(const float pos[3], uint32_t nowMs) {
    if (!areaTriggers.loaded()) return 0;
    std::vector<uint32_t> entered;
    triggerWatch.update(areaTriggers, map_, pos, entered);
    for (uint32_t id : entered) {
        Writer w;
        w.u32(id);
        send(CMSG_AREATRIGGER, w.b, nowMs);
        ++stats.areaTriggers;
    }
    return (int)entered.size();
}

bool GameSession::handleServices(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_AREA_TRIGGER_MESSAGE: {   // u32 length, string: "You must be level 55 to enter." (a locked door)
            r.u32();
            std::string text = r.cstr();
            if (r.bad || text.empty()) return true;
            addLocal(text, nowMs);
            return true;
        }
        case SMSG_SHOW_BANK: {
            uint64_t npc = r.u64();
            if (r.bad) return true;
            closeGossip();
            bank.open = true;
            bank.npc = npc;
            return true;
        }
        case SMSG_BUY_BANK_SLOT_RESULT: {
            uint32_t res = r.u32();
            if (r.bad) return true;
            if (res == 3) notice("You bought a bank slot.", nowMs);
            else fail(res == 1 ? "You don't have enough money." : res == 0 ? "You can't buy more bank slots." : "You are not at a banker.", nowMs);
            return true;
        }
        case SMSG_SHOWTAXINODES: {
            r.u32();   // 1 = show the map
            TaxiWindow t;
            t.npc = r.u64();
            t.current = r.u32();
            for (uint32_t& m : t.mask) m = r.u32();
            if (r.bad) return true;
            closeGossip();
            t.open = true;
            taxi = t;
            return true;
        }
        case SMSG_ACTIVATETAXIREPLY: {
            uint32_t code = r.u32();
            if (r.bad) return true;
            if (code == 0) { ++flights; closeTaxi(); addLocal("You take a flight.", nowMs); }
            else fail(taxiError(code), nowMs);
            return true;
        }
        case SMSG_TAXINODE_STATUS: case SMSG_NEW_TAXI_PATH:
            if (op == SMSG_NEW_TAXI_PATH) notice("New flight path discovered!", nowMs);
            return true;
        default:
            return false;
    }
}

}  // namespace w3n
