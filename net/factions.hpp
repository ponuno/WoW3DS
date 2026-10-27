// factions.hpp — faction reactions of vanilla 1.12.1 (stage 5.12): who is hostile, neutral or friendly.
//
// Data: factions.txt next to the pack, written by w3dconv from the client's FactionTemplate.dbc and Faction.dbc
// (tab separated; "T" lines = templates, "F" lines = factions with a reputation entry):
//   T id faction flags ourMask friendlyMask hostileMask enemy1..4 friend1..4
//   F id reputationIndex raceMask1..4 classMask1..4 base1..4 flags1..4 name
// The rules are vMaNGOS' (FactionTemplateEntry::IsFriendlyTo / IsHostileTo, WorldObject::GetReactionTo /
// GetFactionReactionTo / IsValidAttackTarget, ReputationMgr::ReputationToRank), which follow the client's
// Unit::UnitReaction / CanAttack. The player's standings come from SMSG_INITIALIZE_FACTIONS and friends.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace w3n {

// Reputation ranks (ReputationRank): the reaction of one unit to another uses the same scale.
enum class Rank : uint8_t { Hated = 0, Hostile = 1, Unfriendly = 2, Neutral = 3, Friendly = 4, Honored = 5,
                            Revered = 6, Exalted = 7 };
const char* rankName(Rank r);

namespace FactionMask { enum : uint32_t { Player = 1, Alliance = 2, Horde = 4, Monster = 8 }; }
namespace FactionTemplateFlag { enum : uint32_t { AssistPlayers = 0x800, AttackPvpActivePlayers = 0x1000 }; }
namespace FactionStateFlag { enum : uint8_t { Visible = 0x01, AtWar = 0x02, Hidden = 0x04, InvisibleForced = 0x08,
                                              PeaceForced = 0x10, Inactive = 0x20 }; }

struct FactionTemplate {
    uint32_t id = 0, faction = 0, flags = 0, ourMask = 0, friendlyMask = 0, hostileMask = 0;
    uint32_t enemies[4] = {0, 0, 0, 0}, friends[4] = {0, 0, 0, 0};
    bool friendlyTo(const FactionTemplate& o) const;
    bool hostileTo(const FactionTemplate& o) const;
};

struct FactionInfo {
    uint32_t id = 0;
    int32_t repIndex = -1;                       // position in the reputation list (SMSG_INITIALIZE_FACTIONS), -1 = none
    uint32_t raceMask[4] = {0, 0, 0, 0}, classMask[4] = {0, 0, 0, 0};
    int32_t base[4] = {0, 0, 0, 0};
    uint32_t repFlags[4] = {0, 0, 0, 0};
    std::string name;
    int indexFor(uint32_t raceMask, uint32_t classMask) const;   // FactionEntry::GetIndexFitTo
};

class FactionTable {
public:
    bool load(const char* path);                 // false if the file is missing or has no template
    bool loaded() const { return !templates_.empty(); }
    size_t templateCount() const { return templates_.size(); }
    size_t factionCount() const { return factions_.size(); }
    const FactionTemplate* tmpl(uint32_t id) const;
    const FactionInfo* faction(uint32_t id) const;
    const FactionInfo* byRepIndex(int32_t idx) const;
    void add(const FactionTemplate& t) { templates_[t.id] = t; }
    void add(const FactionInfo& f);
    bool parseLine(const std::string& line);     // one line of factions.txt (tests feed lines directly)
private:
    std::unordered_map<uint32_t, FactionTemplate> templates_;
    std::unordered_map<uint32_t, FactionInfo> factions_;
    std::unordered_map<int32_t, uint32_t> byRep_;
};

// The player's reputation list (64 entries in 1.12) as the server sent it: flags and standing relative to the
// faction's base value for our race/class.
struct ReputationState {
    struct Entry { bool known = false; uint8_t flags = 0; int32_t standing = 0; };
    std::array<Entry, 64> list{};
    bool initialized = false;                    // SMSG_INITIALIZE_FACTIONS seen
    std::unordered_map<uint32_t, Rank> forced;   // SMSG_SET_FORCED_REACTIONS: faction id -> rank
};

int32_t rankFloor(Rank r);                       // standing where a rank begins (Hated -42000 .. Exalted 42000)
Rank standingToRank(int32_t standing);           // ReputationMgr::ReputationToRank

}  // namespace w3n
