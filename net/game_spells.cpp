// game_spells.cpp — spell book, action bar, casting and cooldowns of GameSession (stage 5.8).
//
// Layouts from vMaNGOS / mangos-zero (1.12.1): Player::SendInitialSpells, Spell::SendSpellStart /
// SendSpellGo / SendCastResult / SendInterrupted, Player::SendInitialActionButtons, Spell::
// SendSpellCooldown. No cast counter in CMSG_CAST_SPELL yet (that arrived with TBC).
// Opcodes checked against vMaNGOS Opcodes_1_12_1.h (stage 5q); layouts against WorldPackets::Spell.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_SET_ACTION_BUTTON = 0x128, SMSG_ACTION_BUTTONS = 0x129, SMSG_INITIAL_SPELLS = 0x12A,
    SMSG_LEARNED_SPELL = 0x12B, CMSG_CAST_SPELL = 0x12E, CMSG_CANCEL_CAST = 0x12F,
    SMSG_CAST_FAILED = 0x130,   // SMSG_CAST_RESULT in vMaNGOS
    SMSG_SPELL_FAILED_OTHER = 0x2A6,
    SMSG_SPELL_START = 0x131, SMSG_SPELL_GO = 0x132, SMSG_SPELL_FAILURE = 0x133, SMSG_SPELL_COOLDOWN = 0x134,
    SMSG_COOLDOWN_EVENT = 0x135, SMSG_SPELLHEALLOG = 0x150, SMSG_CLEAR_COOLDOWN = 0x1DE,
    SMSG_REMOVED_SPELL = 0x203, SMSG_SPELLNONMELEEDAMAGELOG = 0x250,
    CMSG_CANCEL_AURA = 0x136, SMSG_UPDATE_AURA_DURATION = 0x137,
};
constexpr uint16_t TARGET_FLAG_UNIT = 0x0002;

// Same trick as the forced-movement packets: a full u64 or a packed GUID, told apart by the size.
uint64_t guidBySize(Reader& r, size_t tail) {
    if (r.left() == 8 + tail) return r.u64();
    return r.packedGuid();
}
}  // namespace

// ------------------------------------------------------------------ names
SpellNames::SpellNames() {
    // The spells a level-1 character is likely to put on the bar before spells.txt is available.
    add(78, "Heroic Strike", "Rank 1");
    add(kSpellAttack, "Attack", "");
    add(2457, "Battle Stance", "");
}

void SpellNames::add(uint32_t id, const std::string& name, const std::string& rank) { names_[id] = {name, rank}; }

bool SpellNames::known(uint32_t id) const { return names_.count(id) != 0; }

std::string SpellNames::name(uint32_t id) const {
    auto it = names_.find(id);
    if (it != names_.end() && !it->second.first.empty()) return it->second.first;
    return "Spell " + std::to_string(id);
}

std::string SpellNames::rank(uint32_t id) const {
    auto it = names_.find(id);
    return it == names_.end() ? std::string() : it->second.second;
}

bool SpellNames::load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char line[512];
    size_t n = 0;
    while (fgets(line, sizeof line, f)) {
        char* tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = 0;
        char* nm = tab1 + 1;
        char* tab2 = strchr(nm, '\t');
        std::string rank;
        if (tab2) {
            *tab2 = 0;
            rank = tab2 + 1;
        }
        while (!rank.empty() && (rank.back() == '\n' || rank.back() == '\r')) rank.pop_back();
        std::string name = nm;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        uint32_t id = (uint32_t)strtoul(line, nullptr, 10);
        if (!id || name.empty()) continue;
        add(id, name, rank);
        ++n;
    }
    fclose(f);
    return n > 0;
}

// ------------------------------------------------------------------ queries
bool GameSession::knowsSpell(uint32_t id) const {
    for (uint32_t s : spells)
        if (s == id) return true;
    return false;
}

std::vector<uint32_t> GameSession::barSpells() const {
    std::vector<uint32_t> out;
    auto add = [&](uint32_t id) {
        if (!id) return;
        for (uint32_t s : out)
            if (s == id) return;
        out.push_back(id);
    };
    for (const ActionButton& b : actionBar)
        if (b.type == 0) add(b.action);
    if (out.empty())   // no server bar: offer what we know by name (real names only: the rest are mostly passives)
        for (uint32_t s : spells)
            if (spellNames.known(s)) add(s);
    return out;
}

uint32_t GameSession::cooldownLeftMs(uint32_t spell, uint32_t nowMs) const {
    auto it = cooldownEnd_.find(spell);
    return it != cooldownEnd_.end() && it->second > nowMs ? it->second - nowMs : 0;
}

CastState GameSession::castOf(uint64_t guid, uint32_t nowMs) const {
    if (guid == self_) return cast;
    auto it = unitCasts_.find(guid);
    if (it == unitCasts_.end() || nowMs - it->second.startMs > it->second.durMs + 300) return CastState();
    return it->second;
}

GameSession::Power GameSession::selfPower() const {
    const WorldObject* me = selfObject();
    Power p{0, 0, 0};
    if (!me) return p;
    p.type = (uint8_t)((me->field(UF::UNIT_BYTES_0) >> 24) & 0xFF);
    if (p.type > 3) p.type = 0;
    p.cur = me->field(UF::UNIT_POWER1 + p.type);
    p.max = me->field(UF::UNIT_MAXPOWER1 + p.type);
    if (p.type == 1) { p.cur /= 10; p.max /= 10; }   // rage is stored x10
    return p;
}

// ------------------------------------------------------------------ casting
bool GameSession::castSpell(uint32_t spell, uint32_t nowMs) {
    if (spell == kSpellAttack) {
        if (attackWanted_) stopAttack(nowMs); else startAttack(nowMs);
        return true;
    }
    if (!spells.empty() && !knowsSpell(spell)) {
        addLocal("You do not know " + spellNames.name(spell), nowMs);
        return false;
    }
    if (cast.active && (cast.durMs == 0 ? nowMs - cast.startMs < 400 : nowMs - cast.startMs < cast.durMs)) {
        addLocal("Already casting", nowMs);
        return false;
    }
    if (uint32_t cd = cooldownLeftMs(spell, nowMs)) {
        char b[64];
        snprintf(b, sizeof b, " is not ready (%.1f s)", cd / 1000.0);
        addLocal(spellNames.name(spell) + b, nowMs);
        return false;
    }
    Writer w;
    w.u32(spell);
    if (target_) {
        w.u16(TARGET_FLAG_UNIT);
        w.packedGuid(target_);
    } else {
        w.u16(0);   // self / no explicit target
    }
    send(CMSG_CAST_SPELL, w.b, nowMs);
    cast.active = true;
    cast.spell = spell;
    cast.startMs = nowMs;
    cast.durMs = 0;
    return true;
}

void GameSession::cancelCast(uint32_t nowMs) {
    if (!cast.active) return;
    Writer w;
    w.u32(cast.spell);
    send(CMSG_CANCEL_CAST, w.b, nowMs);
    cast = CastState();
}

// ------------------------------------------------------------------ server packets
// ------------------------------------------------------------------ auras (stage 5.16)
// vMaNGOS SpellAuraHolder::SetAura / SetAuraFlag / UpdateAuraApplication / UpdateAuraDuration,
// WorldSession::HandleCancelAuraOpcode (u32 spell).
std::vector<AuraView> GameSession::auras(const WorldObject& o, uint32_t nowMs) const {
    std::vector<AuraView> out;
    if (!o.isUnitLike()) return out;
    const bool mine = o.guid == self_ || o.self;
    for (int slot = 0; slot < kAuraSlots; ++slot) {
        uint32_t spell = o.field((uint16_t)(UF::UNIT_FIELD_AURA + slot));
        if (!spell) continue;
        AuraView a;
        a.slot = (uint8_t)slot;
        a.spell = spell;
        a.debuff = slot >= kBuffSlots;
        uint32_t fl = (o.field((uint16_t)(UF::UNIT_FIELD_AURAFLAGS + (slot >> 3))) >> ((slot & 7) * 4)) & 0xF;
        a.cancelable = (fl & 1) != 0 && !a.debuff;
        a.stacks = (uint8_t)(((o.field((uint16_t)(UF::UNIT_FIELD_AURAAPPLICATIONS + slot / 4)) >> ((slot % 4) * 8)) & 0xFF) + 1);
        // the duration packet can come before the field update that puts the aura in the slot (spell 0 then)
        const AuraTime& t = auraTime_[slot];
        if (mine && (t.spell == spell || t.spell == 0) && t.endMs > nowMs) a.leftMs = t.endMs - nowMs;
        out.push_back(a);
    }
    return out;
}

bool GameSession::cancelAura(uint32_t spell, uint32_t nowMs) {
    const WorldObject* me = selfObject();
    if (!me) return false;
    for (const AuraView& a : auras(*me, nowMs))
        if (a.spell == spell) {
            if (!a.cancelable) { addLocal("You can't cancel that.", nowMs); return false; }
            Writer w;
            w.u32(spell);
            send(CMSG_CANCEL_AURA, w.b, nowMs);
            return true;
        }
    return false;
}

bool GameSession::handleSpells(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_INITIAL_SPELLS: {
            r.u8();   // unknown
            uint16_t n = r.u16();
            if (r.bad || (size_t)n * 4 > r.left()) return true;
            spells.clear();
            for (uint16_t i = 0; i < n; ++i) {
                spells.push_back(r.u16());
                r.u16();   // slot
            }
            uint16_t cds = r.u16();
            if (!r.bad && (size_t)cds * 14 <= r.left()) {
                for (uint16_t i = 0; i < cds; ++i) {
                    uint32_t spell = r.u16();
                    r.u16();   // item
                    r.u16();   // category
                    uint32_t rec = r.u32(), cat = r.u32();
                    uint32_t ms = rec ? rec : cat;
                    if (ms) cooldownEnd_[spell] = nowMs + ms;
                }
            }
            return true;
        }
        case SMSG_LEARNED_SPELL: {   // u16 spell, i16 action bar slot (vMaNGOS WorldPackets::Spell::LearnedSpell)
            uint32_t id = r.u16();
            if (r.bad || !id) return true;
            if (!knowsSpell(id)) spells.push_back(id);
            addLocal("You have learned a new spell: " + spellNames.name(id) +
                     (spellNames.rank(id).empty() ? "" : " (" + spellNames.rank(id) + ")"), nowMs);
            return true;
        }
        case SMSG_REMOVED_SPELL: {
            uint32_t id = r.u16();
            if (r.bad) return true;
            for (size_t i = 0; i < spells.size(); ++i)
                if (spells[i] == id) { spells.erase(spells.begin() + (long)i); break; }
            return true;
        }
        case SMSG_ACTION_BUTTONS: {
            size_t n = std::min<size_t>(r.left() / 4, actionBar.size());
            if (n == 0) return true;
            for (size_t i = 0; i < n; ++i) {
                uint32_t v = r.u32();
                actionBar[i].action = v & 0xFFFFFF;
                actionBar[i].type = (uint8_t)(v >> 24);
            }
            actionBarSeen = true;
            return true;
        }
        case SMSG_SPELL_START: {
            r.packedGuid();   // casting item
            uint64_t caster = r.packedGuid();
            uint32_t spell = r.u32();
            r.u16();   // cast flags
            uint32_t ms = r.u32();
            if (r.bad) return true;
            uint64_t tgt = 0;
            {   // SpellCastTargets: u16 mask, then a packed guid for a unit / object / corpse target
                uint16_t mask = r.u16();
                if (!r.bad && (mask & (0x2 | 0x800 | 0x8000 | 0x200))) tgt = r.packedGuid();
                if (r.bad) { tgt = 0; r.bad = false; }
            }
            {
                WorldSound w;
                w.kind = WorldSound::SpellStart;
                w.src = caster;
                w.spell = spell;
                w.castMs = ms;
                worldSound(w);
            }
            if (caster != self_) {   // UI stage 2: the target's cast bar
                if (ms) unitCasts_[caster] = CastState{true, spell, nowMs, ms, tgt};
                if (unitCasts_.size() > 64) unitCasts_.clear();
                return true;
            }
            cast.active = true;
            cast.spell = spell;
            cast.startMs = nowMs;
            cast.durMs = ms;
            cast.target = tgt;
            return true;
        }
        case SMSG_SPELL_GO: {
            r.packedGuid();
            uint64_t caster = r.packedGuid();
            uint32_t spell = r.u32();
            if (r.bad) return true;
            {   // 1.12: u16 cast flags, u8 hits, u64 hit[], u8 misses, (u64, u8 reason)[], targets
                WorldSound w;
                w.kind = WorldSound::SpellGo;
                w.src = caster;
                w.spell = spell;
                r.u16();
                uint8_t n = r.u8();
                for (uint8_t i = 0; i < n && !r.bad; ++i) {
                    uint64_t t = r.u64();
                    if (!r.bad && w.nTargets < 4) w.targets[w.nTargets++] = t;
                }
                worldSound(w);
                r.bad = false;
                if (WorldObject* o = objects.find(caster)) {   // the release animation
                    o->castGoMs = nowMs ? nowMs : 1;
                    o->castGoSpell = spell;
                    o->castGoDirected = false;
                    for (int i = 0; i < w.nTargets; ++i)
                        if (w.targets[i] && w.targets[i] != caster) o->castGoDirected = true;
                }
            }
            if (caster != self_) { unitCasts_.erase(caster); return true; }
            if (cast.spell == spell) cast = CastState();
            ++spellCount;
            gcdEnd_ = nowMs + kGlobalCooldownMs;
            return true;
        }
        case SMSG_CAST_FAILED: {   // vMaNGOS SMSG_CAST_RESULT: u32 spell, u8 status (0 ok, 2 failed), [u8 reason, args]
            uint32_t spell = r.u32();
            uint8_t status = r.u8();
            if (r.bad) return true;
            // The server sends status 0 after every cast that worked: that is not a failure (it must neither
            // clear the cast bar of a spell with a cast time nor print anything).
            if (status == 0) return true;
            uint8_t res = r.u8();
            if (r.bad) return true;
            if (cast.spell == spell) cast = CastState();
            lastCastFail = res;
            lastCastFailMs = nowMs;
            addLocal(spellNames.name(spell) + ": " + castResultText(res), nowMs);
            return true;
        }
        case SMSG_SPELL_FAILURE: {
            uint64_t g = guidBySize(r, 5);
            uint32_t spell = r.u32();
            if (r.bad) return true;
            {
                WorldSound w;
                w.kind = WorldSound::SpellFail;
                w.src = g;
                w.spell = spell;
                worldSound(w);
            }
            if (g != self_) { unitCasts_.erase(g); return true; }
            uint8_t res = r.u8();
            if (cast.active && cast.spell == spell) {
                cast = CastState();
                lastCastFail = res;
                lastCastFailMs = nowMs;
                addLocal(spellNames.name(spell) + ": interrupted", nowMs);
            }
            return true;
        }
        case SMSG_SPELL_FAILED_OTHER: {   // u64 caster, u32 spell: someone else's cast was interrupted
            uint64_t g = r.u64();
            if (!r.bad) unitCasts_.erase(g);
            if (!r.bad) {
                WorldSound w;
                w.kind = WorldSound::SpellFail;
                w.src = g;
                worldSound(w);
            }
            return true;
        }
        case SMSG_SPELL_COOLDOWN: {
            uint64_t g = r.u64();
            if (r.bad || g != self_) return true;
            if (r.left() % 8 == 1) r.u8();
            while (r.left() >= 8) {
                uint32_t spell = r.u32(), ms = r.u32();
                if (spell && ms && ms < 3600u * 1000u) cooldownEnd_[spell] = nowMs + ms;
            }
            return true;
        }
        case SMSG_COOLDOWN_EVENT:
        case SMSG_UPDATE_AURA_DURATION: {   // u8 slot, u32 ms (our own auras)
            uint8_t slot = r.u8();
            uint32_t ms = r.u32();
            if (r.bad || slot >= kAuraSlots) return true;
            const WorldObject* me = selfObject();
            auraTime_[slot].spell = me ? me->field((uint16_t)(UF::UNIT_FIELD_AURA + slot)) : 0;
            auraTime_[slot].endMs = nowMs + std::min<uint32_t>(ms, 24u * 3600u * 1000u);
            return true;
        }
        case SMSG_CLEAR_COOLDOWN: {
            uint32_t spell = r.u32();
            uint64_t g = r.u64();
            if (!r.bad && g == self_) cooldownEnd_.erase(spell);
            return true;
        }
        case SMSG_SPELLNONMELEEDAMAGELOG: {
            uint64_t victim = r.packedGuid(), caster = r.packedGuid();
            uint32_t spell = r.u32(), dmg = r.u32();
            if (r.bad || (caster != self_ && victim != self_)) return true;
            std::string c = caster == self_ ? "Your" : objects.nameOf(caster) + "'s";
            std::string v = victim == self_ ? "you" : objects.nameOf(victim);
            if (c == "'s") c = "?'s";
            if (v.empty()) v = "?";
            addLocal(c + " " + spellNames.name(spell) + " hits " + v + " for " + std::to_string(dmg) + ".", nowMs);
            return true;
        }
        case SMSG_SPELLHEALLOG: {
            uint64_t victim = r.packedGuid(), caster = r.packedGuid();
            uint32_t spell = r.u32(), heal = r.u32();
            if (r.bad || (caster != self_ && victim != self_)) return true;
            std::string c = caster == self_ ? "Your" : objects.nameOf(caster) + "'s";
            std::string v = victim == self_ ? "you" : objects.nameOf(victim);
            if (c == "'s") c = "?'s";
            if (v.empty()) v = "?";
            addLocal(c + " " + spellNames.name(spell) + " heals " + v + " for " + std::to_string(heal) + ".", nowMs);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
