// Friends, ignore list and guild (UI stage 6). Formats: vMaNGOS Server/Packets/Social.cpp (FriendList, IgnoreList,
// FriendStatus), Handlers/MiscHandler.cpp (CMSG_ADD_FRIEND & co.), Server/Packets/Guild.cpp (GuildRoster,
// GuildInviteNotification, GuildEvent, GuildQueryResponse).
#include <algorithm>

#include "game.hpp"
#include "wire.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_NAME_QUERY = 0x050,
    CMSG_GUILD_QUERY = 0x054, SMSG_GUILD_QUERY_RESPONSE = 0x055,
    CMSG_FRIEND_LIST = 0x066, SMSG_FRIEND_LIST = 0x067, SMSG_FRIEND_STATUS = 0x068, CMSG_ADD_FRIEND = 0x069,
    CMSG_DEL_FRIEND = 0x06A, SMSG_IGNORE_LIST = 0x06B, CMSG_ADD_IGNORE = 0x06C, CMSG_DEL_IGNORE = 0x06D,
    CMSG_GUILD_INVITE = 0x082, SMSG_GUILD_INVITE = 0x083, CMSG_GUILD_ACCEPT = 0x084, CMSG_GUILD_DECLINE = 0x085,
    SMSG_GUILD_DECLINE = 0x086, CMSG_GUILD_ROSTER = 0x089, SMSG_GUILD_ROSTER = 0x08A, CMSG_GUILD_LEAVE = 0x08D,
    SMSG_GUILD_EVENT = 0x092, SMSG_GUILD_COMMAND_RESULT = 0x093,
};
constexpr uint16_t kPlayerGuildId = 191;   // PLAYER_GUILDID (vMaNGOS UpdateFields_1_12_1.h)

const char* friendResultText(uint8_t r) {
    switch (r) {
        case 0x00: return "Friend list error";
        case 0x01: return "Your friend list is full";
        case 0x04: return "Player not found";
        case 0x05: return "Friend removed";
        case 0x06: case 0x07: return "Friend added";
        case 0x08: return "Already on your friend list";
        case 0x09: return "You cannot be your own friend";
        case 0x0A: return "That player is of the other faction";
        case 0x0B: return "Your ignore list is full";
        case 0x0C: return "You cannot ignore yourself";
        case 0x0D: return "Player not found";
        case 0x0E: return "Already ignored";
        case 0x0F: return "Player ignored";
        case 0x10: return "No longer ignored";
        default: return nullptr;
    }
}
}  // namespace

std::string GameSession::playerName(uint64_t guid, uint32_t nowMs) {
    if (!guid) return {};
    std::string n = objects.nameOf(guid);
    if (n.empty() && askedNames_.insert(guid).second) {
        Writer w;
        w.u64(guid);
        send(CMSG_NAME_QUERY, w.b, nowMs);
        ++stats.queries;
    }
    return n;
}

void GameSession::requestFriendList(uint32_t nowMs) { send(CMSG_FRIEND_LIST, {}, nowMs); }

void GameSession::addFriend(const std::string& name, uint32_t nowMs) {
    if (name.empty()) return;
    Writer w;
    w.cstr(name);
    send(CMSG_ADD_FRIEND, w.b, nowMs);
}

void GameSession::removeFriend(uint64_t guid, uint32_t nowMs) {
    Writer w;
    w.u64(guid);
    send(CMSG_DEL_FRIEND, w.b, nowMs);
}

void GameSession::addIgnore(const std::string& name, uint32_t nowMs) {
    if (name.empty()) return;
    Writer w;
    w.cstr(name);
    send(CMSG_ADD_IGNORE, w.b, nowMs);
}

void GameSession::removeIgnore(uint64_t guid, uint32_t nowMs) {
    Writer w;
    w.u64(guid);
    send(CMSG_DEL_IGNORE, w.b, nowMs);
}

uint32_t GameSession::guildId() const {
    const WorldObject* me = selfObject();
    return me ? me->field(kPlayerGuildId) : 0;
}

std::string GameSession::guildNameOf(uint32_t id) const {
    auto it = guildNames_.find(id);
    return it == guildNames_.end() ? std::string() : it->second;
}

std::string GameSession::unitSubtitle(const WorldObject& o) const {
    if (o.type == ObjType::Player) {
        const uint32_t gid = o.field(kPlayerGuildId);
        if (!gid) return {};
        const std::string n = guildNameOf(gid);
        return n.empty() ? std::string() : "<" + n + ">";
    }
    if (o.type == ObjType::Unit && !o.subname.empty()) return "<" + o.subname + ">";
    return {};
}

void GameSession::updateGuildNames(uint32_t nowMs) {
    if (nowMs - guildScanMs_ < 1000 && guildScanMs_) return;
    guildScanMs_ = nowMs ? nowMs : 1;
    int asked = 0;
    for (auto& [guid, o] : objects.all()) {
        if (o.type != ObjType::Player) continue;
        const uint32_t gid = o.field(kPlayerGuildId);
        if (!gid || guildNames_.count(gid)) continue;
        auto it = guildAskMs_.find(gid);
        if (it != guildAskMs_.end() && nowMs - it->second < 10000) continue;
        guildAskMs_[gid] = nowMs ? nowMs : 1;
        Writer w;
        w.u32(gid);
        send(CMSG_GUILD_QUERY, w.b, nowMs);
        if (++asked >= 3) break;
    }
}

void GameSession::requestGuildRoster(uint32_t nowMs) {
    if (!guildId()) return;
    send(CMSG_GUILD_ROSTER, {}, nowMs);
    if (guild.id != guildId() || guild.name.empty()) {
        Writer w;
        w.u32(guildId());
        send(CMSG_GUILD_QUERY, w.b, nowMs);
    }
    guild.rosterMs = nowMs ? nowMs : 1;
}

void GameSession::guildInvite(const std::string& name, uint32_t nowMs) {
    if (name.empty()) return;
    Writer w;
    w.cstr(name);
    send(CMSG_GUILD_INVITE, w.b, nowMs);
}

void GameSession::answerGuildInvite(bool accept, uint32_t nowMs) {
    if (guildInviteFrom.empty()) return;
    send(accept ? CMSG_GUILD_ACCEPT : CMSG_GUILD_DECLINE, {}, nowMs);
    guildInviteFrom.clear();
    guildInviteName.clear();
}

void GameSession::leaveGuild(uint32_t nowMs) {
    if (!guildId()) return;
    send(CMSG_GUILD_LEAVE, {}, nowMs);
}

bool GameSession::handleFriends(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_FRIEND_LIST: {
            uint8_t n = r.u8();
            std::vector<FriendEntry> list;
            for (uint8_t i = 0; i < n && !r.bad; ++i) {
                FriendEntry f;
                f.guid = r.u64();
                f.status = r.u8();
                if (f.status) { f.area = r.u32(); f.level = r.u32(); f.cls = r.u32(); }
                if (!r.bad) list.push_back(f);
            }
            friends = list;
            friendsSeen = true;
            for (const FriendEntry& f : friends) playerName(f.guid, nowMs);
            return true;
        }
        case SMSG_IGNORE_LIST: {
            uint8_t n = r.u8();
            std::vector<uint64_t> list;
            for (uint8_t i = 0; i < n && !r.bad; ++i) {
                uint64_t g = r.u64();
                if (!r.bad) list.push_back(g);
            }
            ignored = list;
            for (uint64_t g : ignored) playerName(g, nowMs);
            return true;
        }
        case SMSG_FRIEND_STATUS: {
            uint8_t res = r.u8();
            uint64_t g = r.u64();
            if (r.bad) return true;
            auto it = std::find_if(friends.begin(), friends.end(), [&](const FriendEntry& f) { return f.guid == g; });
            FriendEntry info;
            info.guid = g;
            if (res == 0x02 || res == 0x06) {   // online / added online: status, area, level, class
                info.status = r.u8();
                info.area = r.u32();
                info.level = r.u32();
                info.cls = r.u32();
                if (r.bad) info.status = 1;
                if (!info.status) info.status = 1;
            }
            switch (res) {
                case 0x02: case 0x03:   // a friend came online / went offline
                    if (it != friends.end()) {
                        std::string n = playerName(g, nowMs);
                        if (!n.empty()) addLocal(n + (res == 0x02 ? " has come online." : " has gone offline."), nowMs);
                        *it = info;
                    }
                    break;
                case 0x06: case 0x07:
                    if (it == friends.end()) friends.push_back(info);
                    else *it = info;
                    playerName(g, nowMs);
                    break;
                case 0x05:
                    if (it != friends.end()) friends.erase(it);
                    break;
                case 0x0F:
                    if (std::find(ignored.begin(), ignored.end(), g) == ignored.end()) ignored.push_back(g);
                    playerName(g, nowMs);
                    break;
                case 0x10:
                    ignored.erase(std::remove(ignored.begin(), ignored.end(), g), ignored.end());
                    break;
                default: break;
            }
            if (res != 0x02 && res != 0x03)
                if (const char* t = friendResultText(res)) addLocal(t, nowMs);
            return true;
        }
        case SMSG_GUILD_INVITE: {
            std::string from = r.cstr(), name = r.cstr();
            if (r.bad) return true;
            guildInviteFrom = from.empty() ? "Someone" : from;
            guildInviteName = name;
            addLocal(guildInviteFrom + " invites you to join " + name + ".", nowMs);
            return true;
        }
        case SMSG_GUILD_DECLINE: {
            std::string who = r.cstr();
            addLocal(who + " declines your guild invitation.", nowMs);
            return true;
        }
        case SMSG_GUILD_COMMAND_RESULT: {
            uint32_t cmd = r.u32();
            std::string s = r.cstr();
            uint32_t res = r.u32();
            if (r.bad || res == 0) return true;   // ERR_PLAYER_NO_MORE_IN_GUILD == 0 means "ok"
            static const char* kErr[] = {"", "Internal guild error", "You are already in a guild", "That player is already in a guild",
                                         "You have been invited to a guild already", "That player has already been invited",
                                         "Invalid guild name", "That name is taken", "You lack the permission", "You are not in a guild",
                                         "That player is not in your guild", "Player not found", "Not allied", "Rank too high",
                                         "Rank too low"};
            (void)cmd;
            addLocal(res < sizeof(kErr) / sizeof(kErr[0]) ? std::string(kErr[res]) + (s.empty() ? "" : ": " + s) : "Guild error " + std::to_string(res), nowMs);
            return true;
        }
        case SMSG_GUILD_EVENT: {
            uint8_t ev = r.u8(), n = r.u8();
            std::vector<std::string> args;
            for (uint8_t i = 0; i < n && !r.bad; ++i) args.push_back(r.cstr());
            if (r.bad) return true;
            auto a = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
            switch (ev) {   // GuildEvents: 0 promotion, 1 demotion, 2 MOTD, 3 joined, 4 left, 5 removed, 6/7 leader, 8 disbanded, 11 roster, 12/13 on/off
                case 2: guild.motd = a(0); addLocal("Guild Message of the Day: " + a(0), nowMs); break;
                case 3: addLocal(a(0) + " has joined the guild.", nowMs); break;
                case 4: addLocal(a(0) + " has left the guild.", nowMs); break;
                case 5: addLocal(a(1) + " has been kicked out of the guild by " + a(0) + ".", nowMs); break;
                case 8: addLocal("The guild has been disbanded.", nowMs); guild = GuildState(); break;
                case 12: addLocal(a(0) + " has come online.", nowMs); break;
                case 13: addLocal(a(0) + " has gone offline.", nowMs); break;
                default: break;
            }
            if (ev == 3 || ev == 4 || ev == 5 || ev == 11 || ev == 12 || ev == 13) guild.rosterSeen = false;   // the Social tab asks again
            return true;
        }
        case SMSG_GUILD_ROSTER: {
            uint32_t n = r.u32();
            GuildState g = guild;
            g.motd = r.cstr();
            g.info = r.cstr();
            uint32_t ranks = r.u32();
            for (uint32_t i = 0; i < ranks && !r.bad; ++i) r.u32();
            g.members.clear();
            for (uint32_t i = 0; i < n && i < 1000 && !r.bad; ++i) {
                GuildMember m;
                m.guid = r.u64();
                m.online = r.u8();
                m.name = r.cstr();
                m.rank = r.u32();
                m.level = r.u8();
                m.cls = r.u8();
                m.zone = r.u32();
                if (!m.online) m.offlineDays = r.f32();
                m.note = r.cstr();
                r.cstr();   // officer note
                if (!r.bad) g.members.push_back(m);
            }
            if (r.bad && g.members.empty()) return true;
            std::stable_sort(g.members.begin(), g.members.end(), [](const GuildMember& a, const GuildMember& b) {
                if ((a.online != 0) != (b.online != 0)) return a.online != 0;
                return a.name < b.name;
            });
            g.rosterSeen = true;
            g.id = guildId();
            guild = g;
            return true;
        }
        case SMSG_GUILD_QUERY_RESPONSE: {
            uint32_t id = r.u32();
            std::string name = r.cstr();
            std::vector<std::string> ranks;
            for (int i = 0; i < 10 && !r.bad; ++i) ranks.push_back(r.cstr());
            if (r.bad && name.empty()) return true;
            if (id && !name.empty()) guildNames_[id] = name;
            if (id == guildId() || !guild.id) {
                guild.id = id;
                guild.name = name;
                guild.ranks = ranks;
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
