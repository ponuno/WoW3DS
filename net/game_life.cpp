// game_life.cpp — death, resurrection, NPC gossip and interaction of GameSession (stage 5.10), plus the
// server-toggled movement states a ghost gets (water walking) and their acknowledgements.
//
// Flow on vMaNGOS (1.12.1): health 0 -> Dead. CMSG_REPOP_REQUEST (no body) -> Player::BuildPlayerRepop: ghost
// flag, corpse object, SMSG_CORPSE_RECLAIM_DELAY, ghost aura (run speed, SMSG_MOVE_WATER_WALK), teleport to the
// graveyard. Back at the body: CMSG_RECLAIM_CORPSE (u64) within 39 yd once the delay is over. Or the spirit
// healer: CMSG_GOSSIP_HELLO -> SMSG_GOSSIP_MESSAGE -> CMSG_GOSSIP_SELECT_OPTION -> spell 17251 ->
// SMSG_SPIRIT_HEALER_CONFIRM (u64) -> CMSG_SPIRIT_HEALER_ACTIVATE (u64). A player's resurrection spell:
// SMSG_RESURRECT_REQUEST -> CMSG_RESURRECT_RESPONSE (u64, u8 accept).
// Layouts: vMaNGOS MiscHandler / NPCHandler / QueryHandler (MSG_CORPSE_QUERY), GossipDef.cpp
// (PlayerMenu::SendGossipMenu), WorldPackets::Spell::ResurrectRequest, WorldPackets::Query::NpcTextUpdate,
// MovementPacketSender::SendMovementFlagChangeToController + WorldPackets::Movement::MoveFlagChangeAck.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    SMSG_MOVE_WATER_WALK = 0x0DE, SMSG_MOVE_LAND_WALK = 0x0DF, SMSG_MOVE_FEATHER_FALL = 0x0F2,
    SMSG_MOVE_NORMAL_FALL = 0x0F3, SMSG_MOVE_SET_HOVER = 0x0F4, SMSG_MOVE_UNSET_HOVER = 0x0F5,
    CMSG_MOVE_HOVER_ACK = 0x0F6, CMSG_MOVE_FEATHER_FALL_ACK = 0x2CF, CMSG_MOVE_WATER_WALK_ACK = 0x2D0,
    CMSG_REPOP_REQUEST = 0x15A, SMSG_RESURRECT_REQUEST = 0x15B, CMSG_RESURRECT_RESPONSE = 0x15C,
    CMSG_GOSSIP_HELLO = 0x17B, CMSG_GOSSIP_SELECT_OPTION = 0x17C, SMSG_GOSSIP_MESSAGE = 0x17D,
    SMSG_GOSSIP_COMPLETE = 0x17E, CMSG_NPC_TEXT_QUERY = 0x17F, SMSG_NPC_TEXT_UPDATE = 0x180,
    SMSG_SHOWTAXINODES = 0x1A9,
SMSG_SHOW_BANK = 0x1B8, SMSG_PETITION_SHOWLIST = 0x1BC, CMSG_RECLAIM_CORPSE = 0x1D2,
    MSG_TABARDVENDOR_ACTIVATE = 0x1F2, MSG_CORPSE_QUERY = 0x216, CMSG_SPIRIT_HEALER_ACTIVATE = 0x21C,
    SMSG_SPIRIT_HEALER_CONFIRM = 0x222, SMSG_RESURRECT_FAILED = 0x252, SMSG_CORPSE_RECLAIM_DELAY = 0x269,
    SMSG_DURABILITY_DAMAGE_DEATH = 0x2BD,
};
constexpr uint32_t kPlayerFlagGhost = 0x10;          // PLAYER_FLAGS_GHOST
constexpr uint32_t kNpcSpiritHealer = 0x20 | 0x40;   // UNIT_NPC_FLAG_SPIRITHEALER | SPIRITGUIDE
constexpr uint32_t kNpcGossip = 0x1, kNpcQuestGiver = 0x2, kNpcVendor = 0x4, kNpcTrainer = 0x10;
constexpr uint32_t kNpcFlightMaster = 0x8, kNpcBanker = 0x100, kNpcAuctioneer = 0x1000;   // UI stage 6
constexpr uint32_t kGoMailbox = 19;
constexpr float kReclaimRadius = 39.0f;              // CORPSE_RECLAIM_RADIUS
constexpr float kInteractRange = 5.5f;               // INTERACTION_DISTANCE 5 + a little for the bounding radius
constexpr float kLootRange = 5.0f;

float dist2d(const float a[3], float x, float y) {
    float dx = a[0] - x, dy = a[1] - y;
    return std::sqrt(dx * dx + dy * dy);
}
}  // namespace

// ------------------------------------------------------------------ state
Life GameSession::life() const {
    const WorldObject* me = selfObject();
    if (!me || me->fields.size() <= UF::UNIT_HEALTH) return Life::Alive;
    if (me->field(UF::PLAYER_FLAGS) & kPlayerFlagGhost) return Life::Ghost;
    return me->health() == 0 ? Life::Dead : Life::Alive;
}

void GameSession::updateLife(uint32_t nowMs) {
    const WorldObject* me = selfObject();
    if (!me || me->fields.size() <= UF::UNIT_HEALTH) return;
    Life l = life();
    if (!lifeSeen_) {   // entering the world: a ghost (or a body) from a previous session
        lifeSeen_ = true;
        lastLife_ = l;
        if (l == Life::Ghost) queryCorpse(nowMs);
        if (l == Life::Dead) addLocal("You are dead.", nowMs);
        if (l == Life::Ghost) addLocal("You are a ghost: return to your corpse or talk to a spirit healer.", nowMs);
        return;
    }
    if (l != lastLife_) {
        if (l == Life::Dead) {
            ++deaths;
            attackWanted_ = attackOn_ = false;
            cast = CastState();
            lootWindow = LootWindow();
            gossip = GossipMenu();
            vendor = VendorWindow();
            questDialog = QuestDialog();
            trainer = TrainerWindow();
            binderConfirm = 0;
            mover.stopAll(nowMs);
            addLocal("You have died. Release your spirit to return as a ghost.", nowMs);
        } else if (l == Life::Ghost) {
            corpse = CorpseInfo();
            const MovementInfo& m = mover.info();   // the body lies where we fell, until the server says otherwise
            corpse.known = true;
            corpse.map = map_;
            corpse.x = m.x; corpse.y = m.y; corpse.z = m.z;
            queryCorpse(nowMs);
            addLocal("You are a ghost: return to your corpse or talk to a spirit healer.", nowMs);
        } else {
            ++resurrections;
            corpse = CorpseInfo();
            resurrectOffer = ResurrectOffer();
            spiritHealerConfirm = 0;
            reclaimAt_ = 0;
            gossip = GossipMenu();
            addLocal("You are alive again.", nowMs);
        }
        lastLife_ = l;
    }
    if (l == Life::Ghost) {
        // The corpse object, when it is in range, is the most exact position.
        for (auto& [g, o] : objects.all()) {
            if (o.type != ObjType::Corpse || !o.hasPos) continue;
            uint64_t owner = (uint64_t)o.field(UF::CORPSE_FIELD_OWNER) | ((uint64_t)o.field(UF::CORPSE_FIELD_OWNER + 1) << 32);
            if (owner != self_) continue;
            corpse.known = true;
            corpse.guid = g;
            corpse.map = map_;
            corpse.x = o.mv.x; corpse.y = o.mv.y; corpse.z = o.mv.z;
        }
        if (!corpse.known && nowMs - corpseAskedMs_ > 10000) queryCorpse(nowMs);
    }
}

void GameSession::releaseSpirit(uint32_t nowMs) {
    if (life() != Life::Dead) return;
    send(CMSG_REPOP_REQUEST, {}, nowMs);
}

void GameSession::queryCorpse(uint32_t nowMs) {
    corpseAskedMs_ = nowMs ? nowMs : 1;
    send(MSG_CORPSE_QUERY, {}, nowMs);
}

float GameSession::corpseDistance(const float pos[3]) const {
    if (!corpse.known || corpse.map != map_) return -1.0f;
    return dist2d(pos, corpse.x, corpse.y);
}

bool GameSession::canReclaim(const float pos[3], uint32_t nowMs) const {
    if (life() != Life::Ghost) return false;
    float d = corpseDistance(pos);
    return d >= 0 && d <= kReclaimRadius && reclaimDelayLeftMs(nowMs) == 0;
}

void GameSession::reclaimCorpse(uint32_t nowMs) {
    Writer w;
    w.u64(corpse.guid ? corpse.guid : self_);   // the server finds the corpse itself
    send(CMSG_RECLAIM_CORPSE, w.b, nowMs);
}

void GameSession::answerResurrect(bool accept, uint32_t nowMs) {
    if (!resurrectOffer.active) return;
    Writer w;
    w.u64(resurrectOffer.from);
    w.u8(accept ? 1 : 0);
    send(CMSG_RESURRECT_RESPONSE, w.b, nowMs);
    resurrectOffer = ResurrectOffer();
}

void GameSession::acceptSpiritHealer(uint32_t nowMs) {
    if (!spiritHealerConfirm) return;
    Writer w;
    w.u64(spiritHealerConfirm);
    send(CMSG_SPIRIT_HEALER_ACTIVATE, w.b, nowMs);
    spiritHealerConfirm = 0;
}

void GameSession::ackFlagChange(uint16_t ackOp, uint32_t counter, uint32_t flag, bool on, uint32_t nowMs) {
    mover.setFlag(flag, on);   // first: the ACK's movement info already carries the new state
    Writer w;
    w.u64(self_);
    w.u32(counter);
    writeMovementInfo(w, mover.snapshot(nowMs));
    w.u32(on ? 1 : 0);
    send(ackOp, w.b, nowMs);
}

// ------------------------------------------------------------------ gossip and interaction
void GameSession::talkTo(uint64_t npc, uint32_t nowMs) {
    if (!npc) return;
    Writer w;
    w.u64(npc);
    send(CMSG_GOSSIP_HELLO, w.b, nowMs);
}

void GameSession::selectGossip(size_t i, uint32_t nowMs) {
    if (!gossip.open || i >= gossip.options.size()) return;
    Writer w;
    w.u64(gossip.npc);
    w.u32(gossip.options[i].index);
    // Coded options (a password box) would add the code string; none of the ones a 3DS player meets need it.
    send(CMSG_GOSSIP_SELECT_OPTION, w.b, nowMs);
}

// ------------------------------------------------------------------ game objects (stage 5.18)
// GAMEOBJECT_TYPE_ID 2 quest giver, 3 chest (quest chests, herbs and ore too: the server checks skills),
// 10 goober; the server flags quest objects usable for us with GO_DYNFLAG_LO_ACTIVATE (vMaNGOS
// Object::BuildValuesUpdate, IsActivateToQuest). CMSG_GAMEOBJ_USE u64 (WorldSession::HandleGameObjectUseOpcode)
// answers with SMSG_LOOT_RESPONSE (chests) or the quest giver's windows.
namespace {
enum : uint16_t { CMSG_GAMEOBJ_USE = 0x0B1 };
constexpr uint32_t kGoQuestGiver = 2, kGoChest = 3, kGoGoober = 10, kGoActivate = 0x1;
}

const char* GameSession::useVerb(const WorldObject& o) const {
    if (o.self || !o.hasPos) return nullptr;
    if (o.type == ObjType::GameObject) return objectAction(o).verb;
    if (o.type != ObjType::Unit) return nullptr;
    const bool ghost = life() == Life::Ghost;
    if (o.dead()) return !ghost && (o.field(UF::UNIT_DYNAMIC_FLAGS) & 1) ? "Loot" : nullptr;
    uint32_t f = o.field(UF::UNIT_NPC_FLAGS);
    if (!f) return nullptr;
    if (factions.loaded() && (uint8_t)reactionOf(o) <= (uint8_t)Rank::Hostile) return nullptr;
    if (ghost) return (f & kNpcSpiritHealer) ? "Talk" : nullptr;
    uint32_t qs = questGiverStatus(o.guid);
    if (qs == QuestMark::Available || qs == QuestMark::RewardRep || qs == QuestMark::Reward2 || qs == QuestMark::RewardOld) return "Quest";
    if ((f & kNpcVendor) && !(f & kNpcGossip)) return "Buy";
    if ((f & kNpcTrainer) && !(f & kNpcGossip)) return "Train";
    if ((f & kNpcBanker) && !(f & kNpcGossip)) return "Bank";
    if ((f & kNpcAuctioneer) && !(f & kNpcGossip)) return "Auction";
    if ((f & kNpcFlightMaster) && !(f & kNpcGossip)) return "Fly";
    return "Talk";
}

GameSession::ObjectAction GameSession::objectAction(const WorldObject& o) const {
    ObjectAction a;
    const uint32_t type = o.field(UF::GAMEOBJECT_TYPE_ID), dyn = o.field(UF::GAMEOBJECT_DYN_FLAGS);
    if (type == kGoMailbox) { a.verb = "Mail"; return a; }
    if (type == kGoQuestGiver) { a.verb = "Quest"; return a; }
    if (type != kGoChest && type != kGoGoober) return a;
    const ObjectTable::GoInfo* gi = objects.goInfo(o.entry());
    const uint32_t lockId = gi ? gi->data[0] : 0;
    if (type == kGoGoober && !(dyn & kGoActivate) && !lockId) return a;
    a.verb = type == kGoGoober ? "Use" : (dyn & kGoActivate) ? "Loot" : "Open";
    const std::vector<LockCase>* lock = lockId ? locks.lock(lockId) : nullptr;
    if (!lock) return a;   // no lock (or no locks.txt): CMSG_GAMEOBJ_USE
    for (const LockCase& c : *lock) {
        if (c.type != 2) continue;   // a skill / spell lock: LockType in index
        const char* verb = c.index == 2 ? "Herb" : c.index == 3 ? "Mine" : c.index == 1 ? "Pick" : a.verb;
        uint32_t best = 0;
        for (const auto& [spell, lt] : locks.openSpells())
            if (lt == c.index && knowsSpell(spell) && spell > best) best = spell;
        if (best) { a.verb = verb; a.spell = best; a.why.clear(); return a; }
        // gathering and lockpicking need their skill; plain "open" locks (5, 10, 17 ...) fall back to CMSG_GAMEOBJ_USE
        if (c.index != 1 && c.index != 2 && c.index != 3) continue;
        a.verb = verb;
        if (a.why.empty())
            a.why = c.index == 2 ? "Requires Herbalism" : c.index == 3 ? "Requires Mining" : "Requires Lockpicking";
    }
    if (!a.why.empty())
        for (const LockCase& c : *lock)
            if (c.type == 1) a.why = "Requires a key";
    return a;
}

void GameSession::useGameObject(uint64_t guid, uint32_t nowMs) {
    if (!guid) return;
    Writer w;
    w.u64(guid);
    send(CMSG_GAMEOBJ_USE, w.b, nowMs);
}

void GameSession::selectGossipQuest(size_t i, uint32_t nowMs) {
    if (!gossip.open || i >= gossip.quests.size()) return;
    chooseQuest(gossip.npc, gossip.quests[i], nowMs);
}

std::string GameSession::interact(uint64_t guid, const float pos[3], uint32_t nowMs) {
    const Life l = life();
    const WorldObject* t = guid ? objects.find(guid) : nullptr;
    const bool healer = t && t->isUnitLike() && (t->field(UF::UNIT_NPC_FLAGS) & kNpcSpiritHealer) && !t->dead();
    if (l == Life::Dead) return "You are dead (release your spirit first)";
    if (l == Life::Ghost && !healer) {
        if (canReclaim(pos, nowMs)) { reclaimCorpse(nowMs); return ""; }
        float d = corpseDistance(pos);
        if (d > kReclaimRadius) return "Your corpse is " + std::to_string((int)d) + " yards away";
        if (d >= 0 && reclaimDelayLeftMs(nowMs))
            return "You can resurrect in " + std::to_string((reclaimDelayLeftMs(nowMs) + 999) / 1000) + " s";
        return "Ghosts can only talk to spirit healers";
    }
    if (!t) return "No target";
    if (!t->hasPos) return "Too far away";
    float p[3];
    t->positionAt(nowMs, p);
    float d = std::sqrt((p[0] - pos[0]) * (p[0] - pos[0]) + (p[1] - pos[1]) * (p[1] - pos[1]) +
                        (p[2] - pos[2]) * (p[2] - pos[2]));
    if (t->isUnitLike() && t->dead()) {
        if (!(t->field(UF::UNIT_DYNAMIC_FLAGS) & 1)) return "Nothing to loot";
        if (d > kLootRange) return "You are too far away to loot that corpse";
        loot(guid, nowMs);
        return "";
    }
    if (t->isUnitLike() && t->type == ObjType::Unit && t->field(UF::UNIT_NPC_FLAGS)) {
        if (d > kInteractRange) return "You are too far away";
        if (lootWindow.open) closeLoot(nowMs);
        if (factions.loaded() && (uint8_t)reactionOf(*t) <= (uint8_t)Rank::Hostile) return "It is hostile to you";
        // like the original client: gossip when the NPC has it, else straight to its quests or its wares
        const uint32_t f = t->field(UF::UNIT_NPC_FLAGS);
        if (!(f & kNpcGossip) && (f & kNpcQuestGiver)) questHello(guid, nowMs);
        else if (!(f & kNpcGossip) && (f & kNpcVendor)) openVendor(guid, nowMs);
        else if (!(f & kNpcGossip) && (f & kNpcTrainer)) openTrainer(guid, nowMs);
        else if (!(f & kNpcGossip) && (f & kNpcBanker)) openBank(guid, nowMs);
        else if (!(f & kNpcGossip) && (f & kNpcAuctioneer)) openAuction(guid, nowMs);
        else if (!(f & kNpcGossip) && (f & kNpcFlightMaster)) openTaxi(guid, nowMs);
        else talkTo(guid, nowMs);
        return "";
    }
    if (t->type == ObjType::GameObject && useVerb(*t)) {
        if (d > kInteractRange) return "You are too far away";
        if (lootWindow.open) closeLoot(nowMs);
        if (t->field(UF::GAMEOBJECT_TYPE_ID) == kGoMailbox) { openMailbox(guid, nowMs); return ""; }   // the client opens it
        ObjectAction act = objectAction(*t);
        if (act.spell) {   // herbs, ore, locked chests: the OPEN_LOCK spell at the object (TARGET_FLAG_GAMEOBJECT)
            Writer w;
            w.u32(act.spell);
            w.u16(0x800);
            w.packedGuid(guid);
            send(0x12E, w.b, nowMs);
            return "";
        }
        if (!act.why.empty()) return act.why;
        useGameObject(guid, nowMs);
        return "";
    }
    return "Nothing to do with " + (t->name.empty() ? std::string("that") : t->name);
}

// ------------------------------------------------------------------ server packets
bool GameSession::handleLife(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_MOVE_WATER_WALK: case SMSG_MOVE_LAND_WALK: case SMSG_MOVE_FEATHER_FALL:
        case SMSG_MOVE_NORMAL_FALL: case SMSG_MOVE_SET_HOVER: case SMSG_MOVE_UNSET_HOVER: {
            uint64_t g = r.left() == 12 ? r.u64() : r.packedGuid();   // packed since 1.9 (vMaNGOS), full u64 before
            uint32_t counter = r.u32();
            if (r.bad || g != self_) return true;
            switch (op) {
                case SMSG_MOVE_WATER_WALK: ackFlagChange(CMSG_MOVE_WATER_WALK_ACK, counter, MoveFlag::WaterWalking, true, nowMs); break;
                case SMSG_MOVE_LAND_WALK: ackFlagChange(CMSG_MOVE_WATER_WALK_ACK, counter, MoveFlag::WaterWalking, false, nowMs); break;
                case SMSG_MOVE_FEATHER_FALL: ackFlagChange(CMSG_MOVE_FEATHER_FALL_ACK, counter, MoveFlag::SafeFall, true, nowMs); break;
                case SMSG_MOVE_NORMAL_FALL: ackFlagChange(CMSG_MOVE_FEATHER_FALL_ACK, counter, MoveFlag::SafeFall, false, nowMs); break;
                case SMSG_MOVE_SET_HOVER: ackFlagChange(CMSG_MOVE_HOVER_ACK, counter, MoveFlag::Hover, true, nowMs); break;
                default: ackFlagChange(CMSG_MOVE_HOVER_ACK, counter, MoveFlag::Hover, false, nowMs); break;
            }
            ++stats.flagAcks;
            return true;
        }
        case MSG_CORPSE_QUERY: {
            uint8_t found = r.u8();
            if (r.bad) return true;
            if (!found) {
                if (life() == Life::Ghost && !corpse.known) addLocal("Your corpse could not be found.", nowMs);
                return true;
            }
            int32_t map = (int32_t)r.u32();
            float x = r.f32(), y = r.f32(), z = r.f32();
            r.u32();   // the map the corpse is really on (the first map is the dungeon entrance when it differs)
            if (r.bad) return true;
            corpse.known = true;
            corpse.map = (uint32_t)map;
            corpse.x = x; corpse.y = y; corpse.z = z;
            return true;
        }
        case SMSG_CORPSE_RECLAIM_DELAY: {
            uint32_t ms = r.u32();
            if (r.bad) return true;
            reclaimAt_ = nowMs + std::min<uint32_t>(ms, 10u * 60u * 1000u) + 250;   // a margin: the server counts from sending
            return true;
        }
        case SMSG_RESURRECT_REQUEST: {
            ResurrectOffer o;
            o.from = r.u64();
            uint32_t n = r.u32();
            if (r.bad || n > 64 || n > r.left()) return true;
            o.name = r.cstr();
            o.sickness = r.u8() != 0;
            if (r.bad) return true;
            if (o.name.empty()) o.name = objects.nameOf(o.from);
            o.active = true;
            resurrectOffer = o;
            addLocal((o.name.empty() ? std::string("Someone") : o.name) + " wants to resurrect you.", nowMs);
            return true;
        }
        case SMSG_RESURRECT_FAILED:
            addLocal("Resurrection failed.", nowMs);
            return true;
        case SMSG_SPIRIT_HEALER_CONFIRM: {
            uint64_t g = r.u64();
            if (r.bad) return true;
            spiritHealerConfirm = g;
            gossip = GossipMenu();
            return true;
        }
        case SMSG_DURABILITY_DAMAGE_DEATH:
            addLocal("Your equipment has lost durability.", nowMs);
            return true;
        case SMSG_GOSSIP_MESSAGE: {
            GossipMenu m;
            m.npc = r.u64();
            m.textId = r.u32();
            uint32_t n = r.u32();
            if (r.bad || n > 32) return true;
            for (uint32_t i = 0; i < n && !r.bad; ++i) {
                GossipOption o;
                o.index = r.u32();
                o.icon = r.u8();
                o.coded = r.u8() != 0;
                o.text = r.cstr();
                m.options.push_back(std::move(o));
            }
            uint32_t q = r.u32();
            if (r.bad || q > 32) return true;
            for (uint32_t i = 0; i < q && !r.bad; ++i) {
                GossipQuest g;
                g.id = r.u32();
                g.icon = r.u32();
                g.level = r.u32();
                g.title = r.cstr();
                if (g.id && !g.title.empty()) questTitles_[g.id] = g.title;
                m.quests.push_back(std::move(g));
            }
            if (r.bad) return true;
            m.open = true;
            if (gossip.open && gossip.npc == m.npc && gossip.textId == m.textId) m.text = gossip.text;
            gossip = std::move(m);
            ++gossipMenus;
            if (gossip.textId) {
                Writer w;
                w.u32(gossip.textId);
                w.u64(gossip.npc);
                send(CMSG_NPC_TEXT_QUERY, w.b, nowMs);
            }
            return true;
        }
        case SMSG_NPC_TEXT_UPDATE: {
            uint32_t id = r.u32();
            if (r.bad) return true;
            std::string best;
            float bestProb = -1;
            for (int i = 0; i < 8 && !r.bad; ++i) {
                float prob = r.f32();
                std::string male = r.cstr(), female = r.cstr();
                r.u32();   // language
                for (int k = 0; k < 6; ++k) r.u32();   // emotes
                const std::string& t = male.empty() ? female : male;
                if (!r.bad && !t.empty() && prob > bestProb) { best = t; bestProb = prob; }
            }
            if (gossip.open && gossip.textId == id) {
                // $N = the player's name, $B = line break, $g/$G male:female; - keep it readable without a parser
                std::string out;
                for (size_t i = 0; i < best.size(); ++i) {
                    if (best[i] == '$' && i + 1 < best.size()) {
                        char c = best[i + 1];
                        if (c == 'B' || c == 'b') { out += ' '; ++i; continue; }
                        if (c == 'N' || c == 'n') { out += !selfName.empty() ? selfName : objects.nameOf(self_).empty() ? "friend" : objects.nameOf(self_); ++i; continue; }
                        if ((c == 'G' || c == 'g') && best.find(';', i) != std::string::npos) {
                            size_t colon = best.find(':', i), semi = best.find(';', i);
                            if (colon != std::string::npos && colon < semi) {
                                out += best.substr(i + 2, colon - i - 2);
                                i = semi;
                                continue;
                            }
                        }
                    }
                    out += best[i];
                }
                gossip.text = out;
            }
            return true;
        }
        case SMSG_GOSSIP_COMPLETE:   // the server closes the NPC's windows (after accepting a quest, ...)
            gossip = GossipMenu();
            questDialog = QuestDialog();
            return true;
        case SMSG_PETITION_SHOWLIST: case MSG_TABARDVENDOR_ACTIVATE: addLocal("(guild window: not supported yet)", nowMs); return true;
        default:
            return false;
    }
}

}  // namespace w3n
