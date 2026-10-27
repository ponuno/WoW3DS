// game_group.cpp — the party of GameSession (stage 5.17): invitations, the member list, member stats, leaving,
// kicking and promoting.
//
// Layouts from vMaNGOS (1.12.1) WorldPackets::Group (Group.cpp) and GroupHandler.cpp:
//   CMSG_GROUP_INVITE name -> the other side gets SMSG_GROUP_INVITE inviter -> CMSG_GROUP_ACCEPT / CMSG_GROUP_DECLINE
//   (no body); SMSG_PARTY_COMMAND_RESULT u32 operation, name, u32 result; SMSG_GROUP_DECLINE name;
//   SMSG_GROUP_LIST u8 type, u8 own subgroup|0x80 assistant, u32 n, n x {name, u64 guid, u8 status, u8 subgroup|0x80},
//   u64 leader, [when n > 0: u8 loot method, u64 master looter, u8 threshold, u8 difficulty (1.11+)];
//   SMSG_GROUP_SET_LEADER name; SMSG_GROUP_UNINVITE / SMSG_GROUP_DESTROYED (no body);
//   CMSG_GROUP_DISBAND (no body) leaves; CMSG_GROUP_UNINVITE name; CMSG_GROUP_SET_LEADER u64;
//   SMSG_PARTY_MEMBER_STATS(_FULL): packed guid, u32 mask, then by mask bit: u8 status, u16 hp, u16 max hp, u8 power
//   type, u16 power, u16 max power, u16 level, u16 zone, i16 x + i16 y, u32 mask + u16 per buff, u16 mask + u16 per
//   debuff, pet: u64 guid, name, u16 model, u16 hp, u16 max hp, u8 power type, u16 power, u16 max power, u32 mask + u16s,
//   u16 mask + u16s (WorldSession::BuildPartyMemberStatsPacket); CMSG_REQUEST_PARTY_MEMBER_STATS u64.
#include <algorithm>

#include "game.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_GROUP_INVITE = 0x06E, SMSG_GROUP_INVITE = 0x06F, CMSG_GROUP_ACCEPT = 0x072, CMSG_GROUP_DECLINE = 0x073,
    SMSG_GROUP_DECLINE = 0x074, CMSG_GROUP_UNINVITE = 0x075, SMSG_GROUP_UNINVITE = 0x077, CMSG_GROUP_SET_LEADER = 0x078,
    SMSG_GROUP_SET_LEADER = 0x079, CMSG_GROUP_DISBAND = 0x07B, SMSG_GROUP_DESTROYED = 0x07C, SMSG_GROUP_LIST = 0x07D,
    SMSG_PARTY_MEMBER_STATS = 0x07E, SMSG_PARTY_COMMAND_RESULT = 0x07F, CMSG_REQUEST_PARTY_MEMBER_STATS = 0x27F,
    SMSG_PARTY_MEMBER_STATS_FULL = 0x2F2,
};

std::string partyResultText(uint32_t op, uint32_t res, const std::string& name) {
    switch (res) {
        case 1: return "Cannot find '" + name + "'.";
        case 2: return name + " is not in your party.";
        case 3: return "Your party is full.";
        case 4: return name + " is already in a group.";
        case 5: return "You aren't in a party.";
        case 6: return "You are not the party leader.";
        case 7: return "Target is not part of your alliance.";
        case 8: return name + " is ignoring you.";
        default: return op == 0 ? "You have invited " + name + " to join your group." : "";
    }
}
}  // namespace

bool GameSession::inParty(uint64_t guid) const {
    for (const PartyMember& m : party.members)
        if (m.guid == guid) return true;
    return false;
}

PartyMember GameSession::memberView(const PartyMember& m) const {
    PartyMember v = m;
    if (const WorldObject* o = objects.find(m.guid)) {
        if (o->fields.size() > UF::UNIT_MAXHEALTH) {
            v.hp = o->health();
            v.maxHp = o->maxHealth();
            v.level = o->level();
            v.powerType = (uint8_t)((o->field(UF::UNIT_BYTES_0) >> 24) & 0xFF);
            if (v.powerType > 3) v.powerType = 0;
            v.power = o->field((uint16_t)(UF::UNIT_POWER1 + v.powerType));
            v.maxPower = o->field((uint16_t)(UF::UNIT_MAXPOWER1 + v.powerType));
            v.statsKnown = true;
        }
    }
    return v;
}

void GameSession::inviteToGroup(const std::string& name, uint32_t nowMs) {
    if (name.empty()) return;
    Writer w;
    w.cstr(name);
    send(CMSG_GROUP_INVITE, w.b, nowMs);
}

void GameSession::answerGroupInvite(bool accept, uint32_t nowMs) {
    if (groupInviteFrom.empty()) return;
    send(accept ? CMSG_GROUP_ACCEPT : CMSG_GROUP_DECLINE, {}, nowMs);
    groupInviteFrom.clear();
}

void GameSession::leaveGroup(uint32_t nowMs) {
    if (!party.inGroup) { addLocal("You aren't in a party.", nowMs); return; }
    send(CMSG_GROUP_DISBAND, {}, nowMs);
}

void GameSession::uninvite(const std::string& name, uint32_t nowMs) {
    Writer w;
    w.cstr(name);
    send(CMSG_GROUP_UNINVITE, w.b, nowMs);
}

void GameSession::promoteLeader(uint64_t guid, uint32_t nowMs) {
    Writer w;
    w.u64(guid);
    send(CMSG_GROUP_SET_LEADER, w.b, nowMs);
}

bool GameSession::handleGroup(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_GROUP_INVITE: {
            std::string who = r.cstr();
            if (r.bad) return true;
            groupInviteFrom = who.empty() ? "Someone" : who;
            addLocal(groupInviteFrom + " invites you to a group.", nowMs);
            sound("invite");
            return true;
        }
        case SMSG_GROUP_DECLINE: {
            std::string who = r.cstr();
            if (!r.bad) addLocal(who + " declines your group invitation.", nowMs);
            return true;
        }
        case SMSG_PARTY_COMMAND_RESULT: {
            uint32_t operation = r.u32();
            std::string name = r.cstr();
            uint32_t res = r.u32();
            if (r.bad) return true;
            std::string t = partyResultText(operation, res, name);
            if (!t.empty()) addLocal(t, nowMs);
            return true;
        }
        case SMSG_GROUP_LIST: {
            Party p;
            p.raid = r.u8() == 1;
            r.u8();   // our own subgroup | assistant
            uint32_t n = r.u32();
            if (r.bad || n > 40) return true;
            for (uint32_t i = 0; i < n && !r.bad; ++i) {
                PartyMember m;
                m.name = r.cstr();
                m.guid = r.u64();
                m.status = r.u8();
                uint8_t f = r.u8();
                m.subgroup = f & 0x7F;
                m.assistant = (f & 0x80) != 0;
                // keep what we knew of an old member
                for (const PartyMember& o : party.members)
                    if (o.guid == m.guid && o.statsKnown) {
                        m.statsKnown = true; m.hp = o.hp; m.maxHp = o.maxHp; m.power = o.power; m.maxPower = o.maxPower;
                        m.powerType = o.powerType; m.level = o.level; m.zone = o.zone; m.x = o.x; m.y = o.y;
                    }
                p.members.push_back(std::move(m));
            }
            p.leader = r.u64();
            if (r.bad) return true;
            p.inGroup = n > 0;
            bool joined = p.inGroup && !party.inGroup;
            for (const PartyMember& m : p.members) {
                bool known = false;
                for (const PartyMember& o : party.members) known = known || o.guid == m.guid;
                if (party.inGroup && !known) addLocal(m.name + " joins the party.", nowMs);
                if (!m.statsKnown) {   // ask for its health etc. (members out of range get no objects)
                    Writer w;
                    w.u64(m.guid);
                    send(CMSG_REQUEST_PARTY_MEMBER_STATS, w.b, nowMs);
                }
            }
            for (const PartyMember& o : party.members) {
                bool still = false;
                for (const PartyMember& m : p.members) still = still || o.guid == m.guid;
                if (!still && p.inGroup) addLocal(o.name + " leaves the party.", nowMs);
            }
            if (!p.inGroup && party.inGroup) addLocal("You leave the group.", nowMs);
            party = std::move(p);
            if (party.inGroup) groupInviteFrom.clear();   // in a group now: the invitation is answered
            if (joined) addLocal("You joined a party.", nowMs);
            ++partyLists;
            return true;
        }
        case SMSG_GROUP_SET_LEADER: {
            std::string who = r.cstr();
            if (r.bad) return true;
            addLocal(who + " is now the group leader.", nowMs);
            return true;
        }
        case SMSG_GROUP_UNINVITE:
            party = Party();
            addLocal("You have been removed from the group.", nowMs);
            return true;
        case SMSG_GROUP_DESTROYED:
            party = Party();
            addLocal("Your group has been disbanded.", nowMs);
            return true;
        case SMSG_PARTY_MEMBER_STATS: case SMSG_PARTY_MEMBER_STATS_FULL: {
            uint64_t g = r.packedGuid();
            uint32_t mask = r.u32();
            if (r.bad) return true;
            PartyMember* m = nullptr;
            for (PartyMember& x : party.members)
                if (x.guid == g) m = &x;
            PartyMember tmp;
            PartyMember& d = m ? *m : tmp;
            if (mask & 0x1) d.status = r.u8();
            if (mask & 0x2) d.hp = r.u16();
            if (mask & 0x4) d.maxHp = r.u16();
            if (mask & 0x8) d.powerType = r.u8();
            if (mask & 0x10) d.power = r.u16();
            if (mask & 0x20) d.maxPower = r.u16();
            if (mask & 0x40) d.level = r.u16();
            if (mask & 0x80) d.zone = r.u16();
            if (mask & 0x100) { d.x = (int16_t)r.u16(); d.y = (int16_t)r.u16(); }
            // auras and the pet are not shown on the 3DS (yet): parsing stops here
            if (r.bad) return true;
            if (d.powerType > 3) d.powerType = 0;
            if (mask & 0x6) d.statsKnown = true;
            ++partyStats;
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
