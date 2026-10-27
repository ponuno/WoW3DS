// game_factions.cpp — reactions and reputation of GameSession (stage 5.12).
//
// vMaNGOS (1.12.1) as the reference, which itself follows the client's Unit::UnitReaction / Unit::CanAttack:
//   WorldObject::GetReactionTo (us -> unit), WorldObject::GetFactionReactionTo (unit -> us),
//   WorldObject::IsValidAttackTarget + Unit::IsTargetableBy (can we attack it),
//   ReputationMgr (standing = base for our race/class + the server's delta; at war -> hostile).
// Packets: WorldPackets::Misc::InitializeFactions (u32 count, {u8 flags, i32 standing}), SetFactionStanding
// (u32 count, {u32 reputation index, i32 standing}), SetForcedReactions (u32 count, {u32 faction, u32 rank}),
// FactionAtWarChange (u32 reputation index, u8 flags), SMSG_SET_FACTION_VISIBLE (u32 reputation index).
//
// Without factions.txt the stage-5.11 guesses stay: NPCs with functions are friendly, other creatures neutral
// and attackable, players by team (race).
#include <algorithm>
#include <cstdio>

#include "game.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    SMSG_INITIALIZE_FACTIONS = 0x122, SMSG_SET_FACTION_VISIBLE = 0x123, SMSG_SET_FACTION_STANDING = 0x124,
    SMSG_SET_FORCED_REACTIONS = 0x2A5, SMSG_SET_FACTION_ATWAR = 0x313,
};
namespace UnitFlag {
enum : uint32_t { Spawning = 0x2, PlayerControlled = 0x8, NotAttackable1 = 0x80, ImmuneToPlayer = 0x100, Pvp = 0x1000,
                  NonAttackable2 = 0x10000, TaxiFlight = 0x100000, NotSelectable = 0x02000000 };
}
constexpr uint32_t kPlayerFlagContestedPvp = 0x100;

// The server keeps standings within [-42000, 42999] (Reputation_Bottom / _Cap) around the base; anything else is noise.
int32_t clampStanding(uint32_t raw) {
    int64_t v = (int32_t)raw;
    return (int32_t)std::max<int64_t>(-90000, std::min<int64_t>(90000, v));
}

uint32_t raceOf(const WorldObject& o) { return o.field(UF::UNIT_BYTES_0) & 0xFF; }
uint32_t classOf(const WorldObject& o) { return (o.field(UF::UNIT_BYTES_0) >> 8) & 0xFF; }
uint32_t bitOf(uint32_t v) { return v >= 1 && v <= 32 ? 1u << (v - 1) : 0; }   // race / class -> mask
bool hordeRace(uint32_t r) { return r == 2 || r == 5 || r == 6 || r == 8; }
Rank minRank(Rank a, Rank b) { return (uint8_t)a < (uint8_t)b ? a : b; }
}  // namespace

// ------------------------------------------------------------------ standings
Rank GameSession::standing(const FactionInfo& f, int32_t* value) const {
    int32_t v = 0;
    const WorldObject* me = selfObject();
    uint32_t race = me ? raceOf(*me) : 0, cls = me ? classOf(*me) : 0;
    int idx = f.indexFor(bitOf(race), bitOf(cls));
    if (idx >= 0) v = f.base[idx];
    if (f.repIndex >= 0 && f.repIndex < 64 && rep.list[f.repIndex].known) v += rep.list[f.repIndex].standing;
    if (value) *value = v;
    return standingToRank(v);
}

bool GameSession::atWarWith(const FactionInfo& f) const {
    if (f.repIndex >= 0 && f.repIndex < 64 && rep.list[f.repIndex].known)
        return (rep.list[f.repIndex].flags & FactionStateFlag::AtWar) != 0;
    // no server state yet (replays): the default flags of Faction.dbc for our race/class
    const WorldObject* me = selfObject();
    uint32_t race = me ? raceOf(*me) : 0, cls = me ? classOf(*me) : 0;
    int idx = f.indexFor(bitOf(race), bitOf(cls));
    if (idx >= 0) return (f.repFlags[idx] & FactionStateFlag::AtWar) != 0 || standing(f) <= Rank::Hostile;
    return standing(f) <= Rank::Hostile;
}

std::vector<GameSession::RepLine> GameSession::reputations() const {
    std::vector<RepLine> out;
    if (!rep.initialized) return out;
    for (int i = 0; i < 64; ++i) {
        const FactionInfo* f = factions.byRepIndex(i);
        if (!f) continue;
        const ReputationState::Entry& e = rep.list[i];
        if (!e.known || !(e.flags & FactionStateFlag::Visible)) continue;
        if (e.flags & (FactionStateFlag::Hidden | FactionStateFlag::InvisibleForced)) continue;
        RepLine l;
        l.faction = f->id;
        l.name = f->name;
        l.rank = standing(*f, &l.value);
        l.atWar = (e.flags & FactionStateFlag::AtWar) != 0;
        out.push_back(l);
    }
    std::sort(out.begin(), out.end(), [](const RepLine& a, const RepLine& b) { return a.name < b.name; });
    return out;
}

// ------------------------------------------------------------------ reactions
namespace {
// FactionTemplate vs FactionTemplate (GetFactionReactionTo's common part).
Rank templateReaction(const FactionTemplate& self, const FactionTemplate& target) {
    if (self.hostileTo(target)) return Rank::Hostile;
    if (self.friendlyTo(target)) return Rank::Friendly;
    if (target.friendlyTo(self)) return Rank::Friendly;
    return Rank::Neutral;
}
}  // namespace

// The guesses without factions.txt (or for a unit whose template the file does not have).
static Rank guessReaction(const GameSession& g, const WorldObject& o) {
    if (o.self) return Rank::Friendly;
    if (o.type == ObjType::Player) {
        const WorldObject* me = g.selfObject();
        bool sameTeam = !me || hordeRace(raceOf(*me)) == hordeRace(raceOf(o));
        return sameTeam ? Rank::Friendly : Rank::Hostile;
    }
    return o.field(UF::UNIT_NPC_FLAGS) ? Rank::Friendly : Rank::Neutral;
}

Rank GameSession::reactionOf(const WorldObject& o) const {
    if (o.self || o.guid == self_) return Rank::Friendly;
    const WorldObject* me = selfObject();
    const FactionTemplate* theirs = factions.tmpl(o.field(UF::UNIT_FACTIONTEMPLATE));
    const FactionTemplate* ours = me ? factions.tmpl(me->field(UF::UNIT_FACTIONTEMPLATE)) : nullptr;
    if (!factions.loaded() || !theirs || !ours) return guessReaction(*this, o);
    if (o.type == ObjType::Unit && !(o.field(UF::UNIT_FLAGS) & UnitFlag::PlayerControlled)) {
        if ((theirs->flags & FactionTemplateFlag::AttackPvpActivePlayers) && (me->field(UF::PLAYER_FLAGS) & kPlayerFlagContestedPvp))
            return Rank::Hostile;
        auto forced = rep.forced.find(theirs->faction);
        if (forced != rep.forced.end()) return forced->second;
        if (const FactionInfo* f = factions.faction(theirs->faction))
            if (f->repIndex >= 0) {
                Rank r = standing(*f);
                if (atWarWith(*f)) r = minRank(Rank::Neutral, r);
                return r;
            }
    }
    return templateReaction(*theirs, *ours);
}

Rank GameSession::reactionTo(const WorldObject& o) const {
    if (o.self || o.guid == self_) return Rank::Friendly;
    const WorldObject* me = selfObject();
    const FactionTemplate* theirs = factions.tmpl(o.field(UF::UNIT_FACTIONTEMPLATE));
    const FactionTemplate* ours = me ? factions.tmpl(me->field(UF::UNIT_FACTIONTEMPLATE)) : nullptr;
    if (!factions.loaded() || !theirs || !ours) return guessReaction(*this, o);
    auto forced = rep.forced.find(theirs->faction);
    if (forced != rep.forced.end()) return forced->second;
    if (const FactionInfo* f = factions.faction(theirs->faction))
        if (f->repIndex >= 0) {
            if ((theirs->flags & FactionTemplateFlag::AttackPvpActivePlayers) && (me->field(UF::PLAYER_FLAGS) & kPlayerFlagContestedPvp))
                return Rank::Hostile;
            return atWarWith(*f) ? Rank::Hostile : Rank::Friendly;
        }
    return templateReaction(*ours, *theirs);
}

Rank GameSession::shownReaction(const WorldObject& o) const {
    return minRank(reactionTo(o), reactionOf(o));
}

bool GameSession::canAttack(const WorldObject& o) const {
    if (o.self || o.guid == self_ || !o.isUnitLike() || o.dead()) return false;
    const uint32_t fl = o.field(UF::UNIT_FLAGS);
    if (fl & (UnitFlag::NonAttackable2 | UnitFlag::Spawning | UnitFlag::TaxiFlight | UnitFlag::NotAttackable1 |
              UnitFlag::NotSelectable | UnitFlag::ImmuneToPlayer))
        return false;
    const Rank us = reactionTo(o), them = reactionOf(o);
    if ((uint8_t)us > (uint8_t)Rank::Neutral || (uint8_t)them > (uint8_t)Rank::Neutral) return false;
    const bool theyPlayer = o.type == ObjType::Player || (fl & UnitFlag::PlayerControlled);
    if (us == Rank::Neutral && them == Rank::Neutral && !theyPlayer && factions.loaded()) {
        // not every neutral creature can be attacked: one with a reputation needs us to be at war with it
        if (const FactionTemplate* t = factions.tmpl(o.field(UF::UNIT_FACTIONTEMPLATE)))
            if (rep.forced.find(t->faction) == rep.forced.end())
                if (const FactionInfo* f = factions.faction(t->faction))
                    if (f->repIndex >= 0 && !atWarWith(*f)) return false;
    }
    if (theyPlayer) return (fl & UnitFlag::Pvp) != 0;   // other players: only when they are flagged (no duels yet)
    return true;
}

// ------------------------------------------------------------------ server packets
bool GameSession::handleFactions(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_INITIALIZE_FACTIONS: {
            uint32_t n = r.u32();
            if (r.bad || n > 128 || n * 5 > r.left()) return true;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t flags = r.u8();
                int32_t st = clampStanding(r.u32());
                if (i < 64) rep.list[i] = {true, flags, st};
            }
            rep.initialized = true;
            return true;
        }
        case SMSG_SET_FACTION_STANDING: {
            uint32_t n = r.u32();
            if (r.bad || n > 128 || n * 8 > r.left()) return true;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t idx = r.u32();
                int32_t st = clampStanding(r.u32());
                if (idx >= 64) continue;
                ReputationState::Entry& e = rep.list[idx];
                const FactionInfo* f = factions.byRepIndex((int32_t)idx);
                int32_t before = 0;
                Rank oldRank = f ? standing(*f, &before) : Rank::Neutral;
                int32_t delta = e.known ? st - e.standing : 0;
                e.known = true;
                e.standing = st;
                e.flags |= FactionStateFlag::Visible;
                ++repChanges;
                if (f && delta) {
                    Rank nr = standing(*f);
                    char b[160];
                    snprintf(b, sizeof b, "Reputation with %s %s by %d.", f->name.c_str(), delta > 0 ? "increased" : "decreased",
                             delta > 0 ? (int)delta : (int)-delta);
                    addLocal(b, nowMs);
                    if (nr != oldRank) addLocal(std::string("You are now ") + rankName(nr) + " with " + f->name + ".", nowMs);
                }
                (void)before;
            }
            return true;
        }
        case SMSG_SET_FORCED_REACTIONS: {
            uint32_t n = r.u32();
            if (r.bad || n > 64 || n * 8 > r.left()) return true;
            rep.forced.clear();
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t f = r.u32(), rank = r.u32();
                rep.forced[f] = (Rank)std::min<uint32_t>(rank, 7);
            }
            return true;
        }
        case SMSG_SET_FACTION_ATWAR: {
            uint32_t idx = r.u32();
            uint8_t flags = r.u8();
            if (r.bad || idx >= 64) return true;
            ReputationState::Entry& e = rep.list[idx];
            e.known = true;
            if (flags & FactionStateFlag::AtWar) e.flags |= FactionStateFlag::AtWar;
            else e.flags &= (uint8_t)~FactionStateFlag::AtWar;
            return true;
        }
        case SMSG_SET_FACTION_VISIBLE: {
            uint32_t idx = r.u32();
            if (!r.bad && idx < 64) { rep.list[idx].known = true; rep.list[idx].flags |= FactionStateFlag::Visible; }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
