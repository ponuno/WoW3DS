// game_pet.cpp — hunter and warlock pets of GameSession (UI stage 13): the action bar (SMSG_PET_SPELLS), commands,
// the pet's spells with auto-cast, rename, dismiss, feeding, and the stable master.
//
// Formats from vMaNGOS (1.12.1): Player::PetSpellInitialize / CharmInfo::BuildActionBar / Unit::WritePetSpellsCooldown
// (SMSG_PET_SPELLS), WorldPackets::Pet (PetAction: u64 pet, u32 action | type << 24, u64 target; PetSpellAutocast:
// u64 pet, u32 spell, u8 state; PetRename: u64 pet, string; PetAbandon / PetStopAttack: u64 pet; PetNameQuery: u32 number,
// u64 pet; PetNameQueryResponse: u32 number, string, u32 timestamp; PetActionFeedback: u8; PetCastFailed: u32 spell,
// u8 status, u8 reason; PetMode: u64 pet, u8 react, u8 command, u8 0, u8 flags), WorldSession::SendStablePet
// (MSG_LIST_STABLED_PETS: u64 npc, u8 n, u8 slots, n x {u32 number, u32 entry, u32 level, string name, u32 loyalty, u8 slot}),
// SMSG_STABLE_RESULT u8 (1 money, 6 error, 8 stabled, 9 unstabled / swapped, 0xA slot bought).
#include <algorithm>
#include <cstdio>

#include "game.hpp"
#include "wire.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_PET_NAME_QUERY = 0x052, SMSG_PET_NAME_QUERY_RESPONSE = 0x053, SMSG_PET_CAST_FAILED = 0x138,
    CMSG_PET_ACTION = 0x175, CMSG_PET_ABANDON = 0x176, CMSG_PET_RENAME = 0x177, SMSG_PET_NAME_INVALID = 0x178,
    SMSG_PET_SPELLS = 0x179, SMSG_PET_MODE = 0x17A,
    MSG_LIST_STABLED_PETS = 0x26F, CMSG_STABLE_PET = 0x270, CMSG_UNSTABLE_PET = 0x271, CMSG_BUY_STABLE_SLOT = 0x272,
    SMSG_STABLE_RESULT = 0x273, CMSG_STABLE_SWAP_PET = 0x275, SMSG_PET_BROKEN = 0x2AF, SMSG_PET_ACTION_FEEDBACK = 0x2C6,
    CMSG_PET_STOP_ATTACK = 0x2EA, CMSG_PET_SPELL_AUTOCAST = 0x2F3, SMSG_PET_ACTION_SOUND = 0x324,
    CMSG_CAST_SPELL_PET = 0x12E,   // CMSG_CAST_SPELL
};
constexpr uint32_t kHappinessStep = 333000;
constexpr uint32_t kUnitFlagPetRename = 0x10;
constexpr uint32_t kNoCooldown = 0;

const char* feedbackText(uint8_t m) {
    switch (m) {
        case 1: return "Your pet is dead";
        case 2: return "Your pet has nothing to attack";
        case 3: return "Your pet can't attack that target";
        case 4: return "Your pet can't find a path to the target";
        default: return "";
    }
}

const char* stableText(uint8_t r) {
    switch (r) {
        case 0x01: return "You don't have enough money";
        case 0x06: return "The stable master can't do that now";
        case 0x08: return "Your pet has been stabled";
        case 0x09: return "Your pet has been taken out of the stable";
        case 0x0A: return "You bought a stable slot";
        default: return "";
    }
}
}  // namespace

uint64_t GameSession::petGuid() const {
    if (pet.active && pet.guid) return pet.guid;
    const WorldObject* me = selfObject();
    if (!me) return 0;
    return (uint64_t)me->field(UF::UNIT_FIELD_SUMMON) | ((uint64_t)me->field(UF::UNIT_FIELD_SUMMON + 1) << 32);
}

const WorldObject* GameSession::petObject() const {
    const uint64_t g = petGuid();
    return g ? objects.find(g) : nullptr;
}

int GameSession::petHappiness() const {
    const WorldObject* o = petObject();
    if (!o || o->field((uint16_t)(UF::UNIT_POWER1 + 4)) == 0xFFFFFFFFu) return 0;
    if (o->field((uint16_t)(UF::UNIT_MAXPOWER1 + 4)) == 0) return 0;   // warlock pets have no happiness
    const uint32_t v = o->field((uint16_t)(UF::UNIT_POWER1 + 4));
    return v < kHappinessStep ? 1 : v < 2 * kHappinessStep ? 2 : 3;
}

int GameSession::petLoyalty() const {
    const WorldObject* o = petObject();
    if (!o || o->field((uint16_t)(UF::UNIT_MAXPOWER1 + 4)) == 0) return 0;
    return (int)((o->field(UF::UNIT_BYTES_1) >> 8) & 0xFF);
}

bool GameSession::petCanRename() const {
    const WorldObject* o = petObject();
    return o && (o->field(UF::UNIT_FLAGS) & kUnitFlagPetRename) != 0;
}

bool GameSession::petCanAbandon() const {
    const WorldObject* o = petObject();
    return o && classId() == 3 && o->field((uint16_t)(UF::UNIT_MAXPOWER1 + 4)) != 0;
}

std::string GameSession::petName() const {
    if (!pet.name.empty()) return pet.name;
    const WorldObject* o = petObject();
    return o ? o->name : std::string();
}

bool GameSession::petKnowsSpell(uint32_t spell) const {
    for (uint32_t p : pet.spells)
        if (PetState::spellOf(p) == spell) return true;
    return false;
}

bool GameSession::petCanAutocast(uint32_t spell) const {
    for (uint32_t p : pet.spells)
        if (PetState::spellOf(p) == spell) return PetState::typeOf(p) != PetAct::Passive;
    return false;
}

bool GameSession::petAutocast(uint32_t spell) const {
    for (uint32_t p : pet.spells)
        if (PetState::spellOf(p) == spell) return PetState::typeOf(p) == PetAct::Enabled;
    return false;
}

uint32_t GameSession::petCooldownLeftMs(uint32_t spell, uint32_t nowMs) const {
    auto it = petCdEnd_.find(spell);
    return it != petCdEnd_.end() && it->second > nowMs ? it->second - nowMs : 0;
}

uint32_t GameSession::petCooldownTotalMs(uint32_t spell) const {
    auto it = petCdLen_.find(spell);
    return it != petCdLen_.end() ? it->second : kNoCooldown;
}

// ------------------------------------------------------------------ commands
bool GameSession::petCommand(uint8_t command, uint32_t nowMs) {
    const uint64_t g = petGuid();
    if (!g || !pet.active) { error("You have no pet", Vocal::None, nowMs); return false; }
    uint64_t tgt = 0;
    if (command == kPetAttack) {
        tgt = target_;
        if (!tgt || tgt == g || tgt == self_) { error("Your pet has nothing to attack", Vocal::None, nowMs); return false; }
    }
    Writer w;
    w.u64(g);
    w.u32((uint32_t)command | ((uint32_t)PetAct::Command << 24));
    w.u64(tgt);
    send(CMSG_PET_ACTION, w.b, nowMs);
    // the server confirms with SMSG_PET_MODE only for reactions: show the command at once
    if (command != kPetAttack) pet.command = command;
    return true;
}

bool GameSession::petReaction(uint8_t react, uint32_t nowMs) {
    const uint64_t g = petGuid();
    if (!g || !pet.active) { error("You have no pet", Vocal::None, nowMs); return false; }
    if (react > kPetAggressive) return false;
    Writer w;
    w.u64(g);
    w.u32((uint32_t)react | ((uint32_t)PetAct::Reaction << 24));
    w.u64(0);
    send(CMSG_PET_ACTION, w.b, nowMs);
    pet.react = react;
    return true;
}

bool GameSession::petCastSpell(uint32_t spell, uint32_t nowMs) {
    const uint64_t g = petGuid();
    if (!g || !pet.active) { error("You have no pet", Vocal::None, nowMs); return false; }
    if (!pet.enabled) { error("Your pet cannot do that now", Vocal::None, nowMs); return false; }
    uint8_t type = 0;
    bool found = false;
    for (uint32_t p : pet.spells)
        if (PetState::spellOf(p) == spell) { type = PetState::typeOf(p); found = true; }
    if (!found) {
        for (uint32_t b : pet.bar)
            if (PetState::spellOf(b) == spell && PetState::typeOf(b) != PetAct::Command && PetState::typeOf(b) != PetAct::Reaction) {
                type = PetState::typeOf(b);
                found = true;
            }
    }
    if (!found || type == PetAct::Passive) return false;
    if (uint32_t cd = petCooldownLeftMs(spell, nowMs)) {
        char b[64];
        snprintf(b, sizeof b, " is not ready (%.1f s)", cd / 1000.0);
        error("Spell is not ready yet", Vocal::None, nowMs, spellNames.name(spell) + b);
        return false;
    }
    Writer w;
    w.u64(g);
    w.u32(spell | ((uint32_t)(type == PetAct::Enabled ? PetAct::Enabled : PetAct::Disabled) << 24));
    w.u64(target_);
    send(CMSG_PET_ACTION, w.b, nowMs);
    return true;
}

void GameSession::petStopAttack(uint32_t nowMs) {
    const uint64_t g = petGuid();
    if (!g) return;
    Writer w;
    w.u64(g);
    send(CMSG_PET_STOP_ATTACK, w.b, nowMs);
}

void GameSession::petSetAutocast(uint32_t spell, bool on, uint32_t nowMs) {
    const uint64_t g = petGuid();
    if (!g || !petCanAutocast(spell)) return;
    Writer w;
    w.u64(g);
    w.u32(spell);
    w.u8(on ? 1 : 0);
    send(CMSG_PET_SPELL_AUTOCAST, w.b, nowMs);
    for (uint32_t& p : pet.spells)   // the server sends no answer: show it at once
        if (PetState::spellOf(p) == spell) p = spell | ((uint32_t)(on ? PetAct::Enabled : PetAct::Disabled) << 24);
    for (uint32_t& b : pet.bar)
        if (PetState::spellOf(b) == spell && PetState::typeOf(b) != PetAct::Command && PetState::typeOf(b) != PetAct::Reaction)
            b = spell | ((uint32_t)(on ? PetAct::Enabled : PetAct::Disabled) << 24);
}

void GameSession::petDismiss(uint32_t nowMs) {
    const uint64_t g = petGuid();
    if (!g) return;
    Writer w;
    w.u64(g);
    send(CMSG_PET_ABANDON, w.b, nowMs);
}

bool GameSession::petRename(const std::string& name, uint32_t nowMs) {
    const uint64_t g = petGuid();
    if (!g || !petCanRename()) { error("You cannot rename this pet", Vocal::None, nowMs); return false; }
    if (name.size() < 2 || name.size() > 12) { fail("A pet's name has 2 to 12 letters.", nowMs); return false; }
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { fail("A pet's name has letters only.", nowMs); return false; }
    Writer w;
    w.u64(g);
    w.cstr(name);
    send(CMSG_PET_RENAME, w.b, nowMs);
    pet.name.clear();       // ask again when the object shows the new timestamp
    pet.nameAskedMs = 0;
    return true;
}

// ------------------------------------------------------------------ feeding
std::vector<InvItem> GameSession::petFoodCandidates() {
    std::vector<InvItem> out;
    for (const InvItem& it : inventory()) {
        if (it.equipped() || it.isBagSlot()) continue;
        const ItemInfo* info = itemInfoIfKnown(it.entry);
        if (!info || info->missing) continue;
        // Consumable / Food & Drink, or Trade Goods / Meat: the pet's diet (family) is checked by the server
        if ((info->cls == 0 && info->subclass == 5) || (info->cls == 7 && info->subclass == 8)) out.push_back(it);
    }
    return out;
}

bool GameSession::feedPet(const InvItem& food, uint32_t nowMs) {
    if (!petGuid()) { error("You have no pet", Vocal::None, nowMs); return false; }
    if (!knowsSpell(kSpellFeedPet)) { fail("You do not know Feed Pet.", nowMs); return false; }
    if (cast.active && nowMs - cast.startMs < 400) return false;
    Writer w;
    w.u32(kSpellFeedPet);
    w.u16(0x10);                 // TARGET_FLAG_ITEM
    w.packedGuid(food.guid);
    send(CMSG_CAST_SPELL_PET, w.b, nowMs);
    cast.active = true;
    cast.spell = kSpellFeedPet;
    cast.startMs = nowMs;
    cast.durMs = 0;
    return true;
}

// ------------------------------------------------------------------ stable master
void GameSession::openStable(uint64_t npc, uint32_t nowMs) {
    Writer w;
    w.u64(npc);
    send(MSG_LIST_STABLED_PETS, w.b, nowMs);
}

void GameSession::stableCurrentPet(uint32_t nowMs) {
    if (!stable.open) return;
    Writer w;
    w.u64(stable.npc);
    send(CMSG_STABLE_PET, w.b, nowMs);
}

void GameSession::unstablePet(const StabledPet& p, uint32_t nowMs) {
    if (!stable.open) return;
    bool haveCurrent = false;
    for (const StabledPet& x : stable.pets) haveCurrent = haveCurrent || x.slot == 1;
    Writer w;
    w.u64(stable.npc);
    w.u32(p.number);
    send(haveCurrent ? CMSG_STABLE_SWAP_PET : CMSG_UNSTABLE_PET, w.b, nowMs);
}

void GameSession::buyStableSlot(uint32_t nowMs) {
    if (!stable.open) return;
    Writer w;
    w.u64(stable.npc);
    send(CMSG_BUY_STABLE_SLOT, w.b, nowMs);
}

// ------------------------------------------------------------------ updates
void GameSession::updatePet(uint32_t nowMs) {
    const WorldObject* o = petObject();
    if (pet.active && !o) {
        // the unit left our view (died and despawned, went out of range): the bar stays for a few seconds
        if (!petGoneMs_) petGoneMs_ = nowMs ? nowMs : 1;
        else if (nowMs - petGoneMs_ > 8000) { pet = PetState(); petGoneMs_ = 0; }
    } else {
        petGoneMs_ = 0;
    }
    if (o && pet.active) {
        const uint32_t num = o->field(UF::UNIT_FIELD_PETNUMBER);
        if (num && num != pet.number) { pet.number = num; pet.name.clear(); pet.nameAskedMs = 0; }
        if (num && pet.name.empty() && (!pet.nameAskedMs || nowMs - pet.nameAskedMs > 10000)) {
            pet.nameAskedMs = nowMs ? nowMs : 1;
            Writer w;
            w.u32(num);
            w.u64(pet.guid);
            send(CMSG_PET_NAME_QUERY, w.b, nowMs);
        }
    }
}

// ------------------------------------------------------------------ server packets
bool GameSession::handlePet(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_PET_SPELLS: {
            const uint64_t g = r.u64();
            if (r.bad) return true;
            if (!g) { pet = PetState(); petCdEnd_.clear(); petCdLen_.clear(); return true; }
            PetState p;
            p.active = true;
            p.guid = g;
            r.u32();   // duration (charm / possess)
            p.react = r.u8();
            p.command = r.u8();
            r.u8();
            p.enabled = (r.u8() & 0x8) == 0;
            for (int i = 0; i < kPetBarSlots; ++i) p.bar[i] = r.u32();
            const uint8_t n = r.u8();
            for (uint8_t i = 0; i < n && !r.bad; ++i) p.spells.push_back(r.u32());
            if (r.bad) return true;
            const uint16_t cdN = r.u16();
            std::unordered_map<uint32_t, uint32_t> cdEnd, cdLen;
            for (uint16_t i = 0; i < cdN && !r.bad; ++i) {
                const uint32_t spell = r.u32();
                r.u16();
                const uint32_t cd = r.u32(), cat = r.u32() & 0x7FFFFFF;
                const uint32_t ms = std::max(cd, cat);
                if (ms) { cdEnd[spell] = nowMs + ms; cdLen[spell] = ms; }
            }
            if (g == pet.guid) { p.name = pet.name; p.number = pet.number; p.nameAskedMs = pet.nameAskedMs; }
            p.bars = pet.bars + 1;
            pet = std::move(p);
            petCdEnd_ = std::move(cdEnd);
            petCdLen_ = std::move(cdLen);
            petGoneMs_ = 0;
            return true;
        }
        case SMSG_PET_MODE: {
            const uint64_t g = r.u64();
            const uint8_t react = r.u8(), command = r.u8();
            r.u8();
            const uint8_t flags = r.u8();
            if (r.bad || !pet.active || g != pet.guid) return true;
            pet.react = react;
            pet.command = command;
            pet.enabled = (flags & 0x8) == 0;
            return true;
        }
        case SMSG_PET_NAME_QUERY_RESPONSE: {
            const uint32_t num = r.u32();
            std::string name = r.cstr();
            if (r.bad) return true;
            if (pet.active && (num == pet.number || !pet.number) && !name.empty()) { pet.name = name; if (!pet.number) pet.number = num; }
            return true;
        }
        case SMSG_PET_NAME_INVALID: {
            r.u32();
            lastPetNameInvalid = r.cstr();
            fail("That pet name is not allowed.", nowMs);
            return true;
        }
        case SMSG_PET_ACTION_FEEDBACK: {
            const uint8_t m = r.u8();
            if (r.bad) return true;
            ++petFeedback;
            const char* t = feedbackText(m);
            if (*t) error(t, Vocal::None, nowMs);
            return true;
        }
        case SMSG_PET_CAST_FAILED: {
            const uint32_t spell = r.u32();
            const uint8_t status = r.u8();
            const uint8_t reason = r.u8();
            if (r.bad || status == 0) return true;
            std::string why = castResultText(reason);
            error(why, Vocal::None, nowMs, "Pet: " + spellNames.name(spell) + ": " + why);
            return true;
        }
        case SMSG_PET_BROKEN:
            addLocal("Your pet's loyalty is gone: it has run away.", nowMs);
            return true;
        case SMSG_PET_ACTION_SOUND:
            return true;
        case MSG_LIST_STABLED_PETS: {
            const uint64_t npc = r.u64();
            const uint8_t n = r.u8();
            const uint8_t slots = r.u8();
            if (r.bad) return true;
            StableWindow w;
            w.open = true;
            w.npc = npc;
            w.slots = slots;
            w.result = stable.result;
            for (uint8_t i = 0; i < n && !r.bad; ++i) {
                StabledPet p;
                p.number = r.u32();
                p.entry = r.u32();
                p.level = r.u32();
                p.name = r.cstr();
                p.loyalty = r.u32();
                p.slot = r.u8();
                w.pets.push_back(std::move(p));
            }
            if (r.bad) return true;
            stable = std::move(w);
            return true;
        }
        case SMSG_STABLE_RESULT: {
            const uint8_t res = r.u8();
            if (r.bad) return true;
            stable.result = res;
            const char* t = stableText(res);
            if (*t) addLocal(t, nowMs);
            if (stable.open && (res == 0x08 || res == 0x09 || res == 0x0A)) openStable(stable.npc, nowMs);   // the list changed
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
