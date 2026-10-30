// game_social.cpp — chat, target and melee of GameSession (stages 5.6/5.7).
//
// Layouts from vMaNGOS (1.12.1): ChatHandler::BuildChatPacket, WorldPackets::Chat::ChatMessage,
// WorldPackets::Combat (AttackStart/AttackStop/MeleeAttackingStateUpdate), Player::SendLogXPGain,
// Channel::MakeNotifyPacket. (Loot moved to game_items.cpp in stage 5.10.)
#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_LEAVE_CHANNEL = 0x098, CMSG_CHANNEL_LIST = 0x09A, SMSG_CHANNEL_LIST = 0x09B, CMSG_CHANNEL_PASSWORD = 0x09C,
    CMSG_CHANNEL_SET_OWNER = 0x09D, CMSG_CHANNEL_OWNER = 0x09E, CMSG_CHANNEL_MODERATOR = 0x09F, CMSG_CHANNEL_UNMODERATOR = 0x0A0,
    CMSG_CHANNEL_MUTE = 0x0A1, CMSG_CHANNEL_UNMUTE = 0x0A2, CMSG_CHANNEL_INVITE = 0x0A3, CMSG_CHANNEL_KICK = 0x0A4,
    CMSG_CHANNEL_BAN = 0x0A5, CMSG_CHANNEL_UNBAN = 0x0A6, CMSG_CHANNEL_ANNOUNCEMENTS = 0x0A7, CMSG_CHANNEL_MODERATE = 0x0A8,
    CMSG_WHO = 0x062, SMSG_WHO = 0x063, MSG_RANDOM_ROLL = 0x1FB, SMSG_SERVER_MESSAGE = 0x291, SMSG_CHAT_WRONG_FACTION = 0x219,
    SMSG_ZONE_UNDER_ATTACK = 0x254, SMSG_RAID_INSTANCE_MESSAGE = 0x2FA, SMSG_CHAT_RESTRICTED = 0x2FD,
    SMSG_CHAT_PLAYER_AMBIGUOUS = 0x32D,
    CMSG_JOIN_CHANNEL = 0x097, CMSG_MESSAGECHAT = 0x095, SMSG_MESSAGECHAT = 0x096, SMSG_CHANNEL_NOTIFY = 0x099,
    CMSG_SET_SELECTION = 0x13D, CMSG_ATTACKSWING = 0x141, CMSG_ATTACKSTOP = 0x142,
    SMSG_ATTACKSTART = 0x143, SMSG_ATTACKSTOP = 0x144, SMSG_ATTACKSWING_NOTINRANGE = 0x145,
    SMSG_ATTACKSWING_BADFACING = 0x146, SMSG_ATTACKSWING_NOTSTANDING = 0x147, SMSG_ATTACKSWING_DEADTARGET = 0x148,
    SMSG_ATTACKSWING_CANT_ATTACK = 0x149, SMSG_ATTACKERSTATEUPDATE = 0x14A, SMSG_CANCEL_COMBAT = 0x14E,
    SMSG_NOTIFICATION = 0x1CB, CMSG_STANDSTATECHANGE = 0x101, SMSG_EMOTE = 0x103, CMSG_TEXT_EMOTE = 0x104,
    SMSG_TEXT_EMOTE = 0x105, SMSG_LOG_XPGAIN = 0x1D0, SMSG_EXPLORATION_EXPERIENCE = 0x1F8, SMSG_CHAT_PLAYER_NOT_FOUND = 0x2A9,
    // UI stage 9 (vMaNGOS Opcodes_1_12_1.h, Packets/Duel.cpp, MiscHandler.cpp)
    CMSG_INSPECT = 0x114, SMSG_INSPECT = 0x115, SMSG_DUEL_REQUESTED = 0x167, SMSG_DUEL_OUTOFBOUNDS = 0x168,
    SMSG_DUEL_INBOUNDS = 0x169, SMSG_DUEL_COMPLETE = 0x16A, SMSG_DUEL_WINNER = 0x16B, CMSG_DUEL_ACCEPTED = 0x16C,
    CMSG_DUEL_CANCELLED = 0x16D, SMSG_DUEL_COUNTDOWN = 0x2B7, MSG_INSPECT_HONOR_STATS = 0x2D6, CMSG_CAST_SPELL_ = 0x12E,
};
constexpr uint32_t LANG_UNIVERSAL = 0, LANG_ORCISH = 1, LANG_COMMON = 7;
constexpr size_t kHitsKeep = 32;

// Length-prefixed name as the server writes it: u32 (strlen + 1) then the C string.
bool readSizedName(Reader& r, std::string& out) {
    uint32_t n = r.u32();
    if (r.bad || n > 256 || n > r.left()) return false;
    out = r.cstr();
    return !r.bad;
}

}  // namespace

const char* Chat::typeName(uint8_t t) {
    switch (t) {
        case Say: return "say"; case Party: return "party"; case Raid: return "raid"; case Guild: return "guild";
        case Officer: return "officer"; case Yell: return "yell"; case Whisper: return "whisper";
        case WhisperInform: return "to"; case Emote: case TextEmote: return "emote"; case System: return "system";
        case MonsterSay: return "npc say"; case MonsterYell: return "npc yell"; case MonsterEmote: return "npc emote";
        case Channel: return "channel"; case MonsterWhisper: return "npc whisper"; case RaidLeader: return "raid leader";
        case RaidWarning: return "raid warning"; case Battleground: return "bg"; case BattlegroundLeader: return "bg leader";
        case ChannelNotice: return "channel notice"; case Afk: return "afk"; case Dnd: return "dnd"; case Ignored: return "ignored";
        case Loot: return "loot"; case Skill: return "skill"; case Local: return "client";
        default: return "?";
    }
}

void GameSession::addLine(ChatLine l) {
    chat.push_back(std::move(l));
    while (chat.size() > kChatKeep) chat.pop_front();
    ++chatCount;
}

std::string GameSession::channelNoticeText(uint8_t k, const std::string& ch, const std::string& a, const std::string& b) {
    const std::string c = "[" + ch + "] ";
    switch (k) {
        case 0x00: return c + a + " joined the channel.";
        case 0x01: return c + a + " left the channel.";
        case 0x02: return "Joined Channel: [" + a + ". " + ch + "]";
        case 0x03: return "Left Channel: [" + (a == "0" ? std::string() : a + ". ") + ch + "]";
        case 0x04: return "Wrong password for " + ch + ".";
        case 0x05: return "Not on channel " + ch + ".";
        case 0x06: return "Not a moderator of " + ch + ".";
        case 0x07: return c + "Password changed by " + a + ".";
        case 0x08: return c + "Owner changed to " + a + ".";
        case 0x09: return c + "Player " + a + " was not found.";
        case 0x0A: return c + "You are not the channel owner.";
        case 0x0B: return c + "Channel owner is " + a + ".";
        case 0x0C: return c + a + " " + b + ".";
        case 0x0D: return c + "Channel announcements enabled by " + a + ".";
        case 0x0E: return c + "Channel announcements disabled by " + a + ".";
        case 0x0F: return c + "Channel moderation enabled by " + a + ".";
        case 0x10: return c + "Channel moderation disabled by " + a + ".";
        case 0x11: return c + "You do not have permission to speak.";
        case 0x12: return c + "Player " + a + " kicked by " + b + ".";
        case 0x13: return c + "You are banned from that channel.";
        case 0x14: return c + "Player " + a + " banned by " + b + ".";
        case 0x15: return c + "Player " + a + " unbanned by " + b + ".";
        case 0x16: return c + "Player " + a + " is not banned.";
        case 0x17: return c + "Player " + a + " is already on the channel.";
        case 0x18: return a + " has invited you to join the channel '" + ch + "'.";
        case 0x19: return "Target is in the wrong alliance for " + ch + ".";
        case 0x1A: return "Wrong alliance for " + ch + ".";
        case 0x1B: return "Invalid channel name.";
        case 0x1C: return ch + " is not moderated.";
        case 0x1D: return c + "You invited " + a + " to join the channel.";
        case 0x1E: return c + a + " has been banned.";
        case 0x1F: return c + "The number of messages that can be sent to this channel is limited, please wait to send another message.";
        default: { char h[24]; std::snprintf(h, sizeof h, "%02X", k); return c + "Channel notice 0x" + h + "."; }
    }
}

void GameSession::addLocal(const std::string& text, uint32_t nowMs) {
    ChatLine l;
    l.ms = nowMs;
    l.type = Chat::Local;
    l.text = text;
    addLine(std::move(l));
}

std::string GameSession::lineText(const ChatLine& l) const {
    std::string who = l.name;
    if (who.empty() && l.sender) who = objects.nameOf(l.sender);
    if (who.empty() && l.sender) who = l.sender == self_ ? "You" : "?";
    switch (l.type) {
        case Chat::Say: case Chat::MonsterSay: return "[" + who + "] says: " + l.text;
        case Chat::Yell: case Chat::MonsterYell: return "[" + who + "] yells: " + l.text;
        case Chat::Whisper: case Chat::MonsterWhisper: return "[" + who + "] whispers: " + l.text;
        case Chat::WhisperInform: return "To [" + (l.channel.empty() ? who : l.channel) + "]: " + l.text;
        case Chat::Emote: case Chat::MonsterEmote: return who + " " + l.text;
        case Chat::TextEmote: return l.text;
        case Chat::Party: return "[Party] [" + who + "]: " + l.text;
        case Chat::Raid: case Chat::RaidLeader: return "[Raid] [" + who + "]: " + l.text;
        case Chat::RaidWarning: return "[Raid Warning] [" + who + "]: " + l.text;
        case Chat::Guild: return "[Guild] [" + who + "]: " + l.text;
        case Chat::Officer: return "[Officer] [" + who + "]: " + l.text;
        case Chat::Channel: {
            int n = channelNumber(l.channel);
            return "[" + (n ? std::to_string(n) + ". " : std::string()) + l.channel + "] [" + who + "]: " + l.text;
        }
        case Chat::ChannelNotice: return l.text;
        case Chat::Afk: return who + " is Away from Keyboard" + (l.text.empty() ? std::string() : ": " + l.text);
        case Chat::Dnd: return who + " is Do Not Disturb" + (l.text.empty() ? std::string() : ": " + l.text);
        case Chat::Ignored: return who + " is ignoring you.";
        case Chat::Loot:
            if (!l.channel.empty()) {   // an item push: put in the name once the item query has answered
                uint32_t entry = (uint32_t)std::strtoul(l.channel.c_str(), nullptr, 10);
                const ItemInfo* i = itemInfoIfKnown(entry);
                std::string placeholder = "[Item " + l.channel + "]";
                size_t at = l.text.find(placeholder);
                if (i && !i->name.empty() && at != std::string::npos)
                    return l.text.substr(0, at) + "[" + i->name + "]" + l.text.substr(at + placeholder.size());
            }
            return l.text;
        case Chat::System: case Chat::Local: case Chat::Skill: return l.text;
        default: return (who.empty() ? std::string() : "[" + who + "]: ") + l.text;
    }
}

uint32_t GameSession::chatLanguage() const {
    const WorldObject* me = selfObject();
    uint32_t race = me ? (me->field(UF::UNIT_BYTES_0) & 0xFF) : 0;
    switch (race) {
        case 1: case 3: case 4: case 7: return LANG_COMMON;   // human, dwarf, night elf, gnome
        case 2: case 5: case 6: case 8: return LANG_ORCISH;   // orc, undead, tauren, troll
        default: return LANG_UNIVERSAL;
    }
}

void GameSession::sendChat(uint8_t type, const std::string& text, const std::string& to, uint32_t nowMs) {
    Writer w;
    w.u32(type);
    w.u32(chatLanguage());
    if (type == Chat::Whisper || type == Chat::Channel) w.cstr(to);
    w.cstr(text.substr(0, 255));
    send(CMSG_MESSAGECHAT, w.b, nowMs);
}

void GameSession::addSystem(const std::string& text, uint32_t nowMs) {
    ChatLine l;
    l.ms = nowMs;
    l.type = Chat::System;
    l.text = text;
    addLine(std::move(l));
}

int GameSession::channelNumber(const std::string& name) const {
    for (size_t i = 0; i < channels_.size(); ++i)
        if (!channels_[i].empty() && channels_[i] == name) return (int)i + 1;
    return 0;
}

void GameSession::joinChannel(const std::string& name, const std::string& password, uint32_t nowMs) {
    Writer w;
    w.cstr(name);
    w.cstr(password);
    send(CMSG_JOIN_CHANNEL, w.b, nowMs);
}

void GameSession::updateDefaultChannels(uint32_t zoneId, const std::string& zoneName, uint32_t nowMs) {
    if (!autoChannels || !zoneId || zoneName.empty() || zoneId == autoZone_ || !wc_) return;
    // the capitals (AreaTable): Stormwind City, Ironforge, Darnassus, Orgrimmar, Thunder Bluff, Undercity
    const bool city = zoneId == 1519 || zoneId == 1537 || zoneId == 1657 || zoneId == 1637 || zoneId == 1638 || zoneId == 1497;
    std::vector<std::string> want;
    want.push_back("General - " + zoneName);
    if (city) want.push_back("Trade - " + zoneName);
    want.push_back("LocalDefense - " + zoneName);
    for (const std::string& old : autoJoined_) leaveChannel(old, nowMs);   // first the old ones: their numbers are free again
    autoJoined_ = want;
    for (const std::string& n : want) joinChannel(n, "", nowMs);
    if (!autoLfg_) { autoLfg_ = true; joinChannel("LookingForGroup", "", nowMs); }
    autoZone_ = zoneId;
}

void GameSession::leaveChannel(const std::string& name, uint32_t nowMs) {
    Writer w;
    w.cstr(name);
    send(CMSG_LEAVE_CHANNEL, w.b, nowMs);
}

void GameSession::listChannel(const std::string& name, uint32_t nowMs) {
    Writer w;
    w.cstr(name);
    send(CMSG_CHANNEL_LIST, w.b, nowMs);
}

void GameSession::channelCommand(uint16_t opcode, const std::string& channel, const std::string* arg, uint32_t nowMs) {
    Writer w;
    w.cstr(channel);
    if (arg) w.cstr(*arg);
    send(opcode, w.b, nowMs);
}

void GameSession::roll(uint32_t lo, uint32_t hi, uint32_t nowMs) {
    Writer w;
    w.u32(lo);
    w.u32(hi);
    send(MSG_RANDOM_ROLL, w.b, nowMs);
}

void GameSession::who(const std::string& query, uint32_t nowMs) {
    Writer w;
    w.u32(0);
    w.u32(100);
    w.cstr("");   // player name and guild name stay empty: the search term matches names, guilds and zones
    w.cstr("");
    w.u32(0xFFFFFFFF);   // race mask
    w.u32(0xFFFFFFFF);   // class mask
    w.u32(0);            // zones
    w.u32(query.empty() ? 0 : 1);
    if (!query.empty()) w.cstr(query.substr(0, 48));
    send(CMSG_WHO, w.b, nowMs);
}

std::vector<std::string> GameSession::commandHelp() {
    return {
        "Chat: /s /y /p /g /o /raid /rw /e text, /w Name text, /r text, /1 .. /9 text",
        "Channels: /join Name [password], /leave Name, /chatlist [N], /cinvite N Name, /ckick N Name, /cban N Name, /cmod N Name",
        "Info: /who [text], /roll [max], /emotes, /afk [text], /dnd [text]",
        "Social: /friend Name, /unfriend Name, /ignore Name, /unignore Name, /invite Name, /uninvite Name, /leaveparty, /promote Name",
        "Guild: /ginvite Name, /gquit, /groster. Other: /target Name, /duel, /sit /stand /sleep",
    };
}

namespace {
std::string lowered(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string firstWord(std::string& rest) {   // splits "a b c" into "a" and rest = "b c"
    size_t i = rest.find_first_not_of(' ');
    if (i == std::string::npos) { rest.clear(); return {}; }
    size_t e = rest.find(' ', i);
    std::string w = rest.substr(i, e == std::string::npos ? std::string::npos : e - i);
    rest = e == std::string::npos ? std::string() : rest.substr(e + 1);
    size_t j = rest.find_first_not_of(' ');
    rest = j == std::string::npos ? std::string() : rest.substr(j);
    return w;
}
}  // namespace

bool GameSession::sendTyped(const std::string& lineIn, uint32_t nowMs) {
    std::string line = lineIn;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\n' || line.back() == '\r')) line.pop_back();
    size_t lead = line.find_first_not_of(' ');
    if (lead == std::string::npos) return false;
    line = line.substr(lead);
    if (line[0] != '/') { sendChat(Chat::Say, line, "", nowMs); return true; }
    size_t sp = line.find(' ');
    std::string cmd = lowered(line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1));
    std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    {
        size_t j = rest.find_first_not_of(' ');
        rest = j == std::string::npos ? std::string() : rest.substr(j);
    }
    auto need = [&](bool ok, const char* what) {
        if (!ok) addLocal(std::string("Usage: ") + what, nowMs);
        return ok;
    };
    // chat channels by type
    struct Say { const char* names; uint8_t type; };
    static const Say kSay[] = {
        {" s say ", Chat::Say}, {" y yell sh shout ", Chat::Yell}, {" p party ", Chat::Party},
        {" g guild ", Chat::Guild}, {" o officer ", Chat::Officer}, {" raid ra rsay ", Chat::Raid},
        {" rw raidwarning ", Chat::RaidWarning}, {" e me emote ", Chat::Emote},
    };
    for (const Say& c : kSay)
        if (std::string(c.names).find(" " + cmd + " ") != std::string::npos) {
            if (!need(!rest.empty(), ("/" + cmd + " text").c_str())) return false;
            sendChat(c.type, rest, "", nowMs);
            return true;
        }
    if (cmd == "w" || cmd == "whisper" || cmd == "t" || cmd == "tell" || cmd == "send") {
        std::string name = firstWord(rest);
        if (!need(!name.empty() && !rest.empty(), "/w Name text")) return false;
        lastWhisperTo = name;
        sendChat(Chat::Whisper, rest, name, nowMs);
        return true;
    }
    if (cmd == "r" || cmd == "reply") {
        std::string name = lastWhisperGuid ? objects.nameOf(lastWhisperGuid) : std::string();
        if (name.empty()) name = lastWhisperTo;
        if (name.empty()) { fail("There is no one to reply to.", nowMs); return false; }
        if (!need(!rest.empty(), "/r text")) return false;
        lastWhisperTo = name;
        sendChat(Chat::Whisper, rest, name, nowMs);
        return true;
    }
    if (cmd == "afk" || cmd == "dnd") { sendChat(cmd == "afk" ? Chat::Afk : Chat::Dnd, rest, "", nowMs); return true; }
    // channels
    auto resolve = [&](const std::string& tok) -> std::string {   // number or name (prefix) of a joined channel
        if (!tok.empty() && tok.find_first_not_of("0123456789") == std::string::npos) {
            size_t n = (size_t)std::atoi(tok.c_str());
            return n >= 1 && n <= channels_.size() ? channels_[n - 1] : std::string();
        }
        std::string t = lowered(tok);
        for (const std::string& c : channels_)
            if (!c.empty() && lowered(c).compare(0, t.size(), t) == 0) return c;
        return {};
    };
    if (cmd == "join" || cmd == "j" || cmd == "chatjoin") {
        std::string name = firstWord(rest);
        if (!need(!name.empty(), "/join Channel [password]")) return false;
        joinChannel(name, rest, nowMs);
        return true;
    }
    if (cmd == "leave" || cmd == "chatleave" || cmd == "l") {
        std::string tok = firstWord(rest);
        if (!need(!tok.empty(), "/leave Channel")) return false;
        std::string ch = resolve(tok);
        leaveChannel(ch.empty() ? tok : ch, nowMs);
        return true;
    }
    if (cmd == "chatlist" || cmd == "chatinfo" || cmd == "clist") {
        std::string tok = firstWord(rest);
        std::string ch = tok.empty() ? std::string() : resolve(tok);
        if (ch.empty()) for (const std::string& c : channels_) if (!c.empty()) { ch = c; break; }   // the first one
        if (ch.empty()) { addLocal("You are not in any channel (/join Name).", nowMs); return false; }
        listChannel(ch, nowMs);
        return true;
    }
    if (cmd == "chat") {   // /chat: the joined channels
        bool any = false;
        for (size_t i = 0; i < channels_.size(); ++i)
            if (!channels_[i].empty()) { addLocal(std::to_string(i + 1) + ". " + channels_[i], nowMs); any = true; }
        if (!any) addLocal("You are not in any channel (/join Name).", nowMs);
        return true;
    }
    {
        struct Cc { const char* name; uint16_t op; bool player; const char* usage; };
        static const Cc kCc[] = {
            {"cinvite", CMSG_CHANNEL_INVITE, true, "/cinvite N Name"}, {"ckick", CMSG_CHANNEL_KICK, true, "/ckick N Name"},
            {"cban", CMSG_CHANNEL_BAN, true, "/cban N Name"}, {"cunban", CMSG_CHANNEL_UNBAN, true, "/cunban N Name"},
            {"cmod", CMSG_CHANNEL_MODERATOR, true, "/cmod N Name"}, {"cunmod", CMSG_CHANNEL_UNMODERATOR, true, "/cunmod N Name"},
            {"cmute", CMSG_CHANNEL_MUTE, true, "/cmute N Name"}, {"cunmute", CMSG_CHANNEL_UNMUTE, true, "/cunmute N Name"},
            {"cowner", CMSG_CHANNEL_SET_OWNER, true, "/cowner N Name"}, {"cpass", CMSG_CHANNEL_PASSWORD, true, "/cpass N password"},
            {"cwho", CMSG_CHANNEL_OWNER, false, "/cwho N"}, {"cannounce", CMSG_CHANNEL_ANNOUNCEMENTS, false, "/cannounce N"},
            {"cmoderate", CMSG_CHANNEL_MODERATE, false, "/cmoderate N"},
        };
        for (const Cc& c : kCc) {
            if (cmd != c.name) continue;
            std::string tok = firstWord(rest);
            std::string ch = tok.empty() ? std::string() : resolve(tok);
            if (ch.empty()) { addLocal(tok.empty() ? std::string("Usage: ") + c.usage : "Not in channel " + tok + ".", nowMs); return false; }
            if (c.player) {
                if (!need(!rest.empty(), c.usage)) return false;
                channelCommand(c.op, ch, &rest, nowMs);
            } else {
                channelCommand(c.op, ch, nullptr, nowMs);
            }
            return true;
        }
    }
    if (!cmd.empty() && cmd.find_first_not_of("0123456789") == std::string::npos) {   // /1 text
        size_t n = (size_t)std::atoi(cmd.c_str());
        if (n < 1 || n > channels_.size() || channels_[n - 1].empty()) { addLocal("Not in channel " + cmd + " (/join Name first)", nowMs); return false; }
        if (!need(!rest.empty(), "/1 text")) return false;
        sendChat(Chat::Channel, rest, channels_[n - 1], nowMs);
        return true;
    }
    // information
    if (cmd == "who" || cmd == "w3") { who(rest, nowMs); return true; }
    if (cmd == "roll" || cmd == "random" || cmd == "rnd") {
        uint32_t lo = 1, hi = 100;
        std::string a = firstWord(rest), b = firstWord(rest);
        auto num = [](const std::string& t, uint32_t* out) {
            if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos || t.size() > 7) return false;
            *out = (uint32_t)std::atoi(t.c_str());
            return true;
        };
        if (!a.empty() && !b.empty()) { if (!num(a, &lo) || !num(b, &hi)) { addLocal("Usage: /roll [min] max", nowMs); return false; } }
        else if (!a.empty()) { if (!num(a, &hi)) { addLocal("Usage: /roll [min] max", nowMs); return false; } }
        if (lo > hi || hi > 1000000) { addLocal("Usage: /roll [min] max (1 .. 1000000)", nowMs); return false; }
        roll(lo, hi, nowMs);
        return true;
    }
    if (cmd == "emotes" || cmd == "emotelist") {
        std::string row;
        for (const std::string& e : emoteNames()) {
            if (row.size() + e.size() + 1 > 56) { addLocal(row, nowMs); row.clear(); }
            row += (row.empty() ? "" : " ") + e;
        }
        if (!row.empty()) addLocal(row, nowMs);
        return true;
    }
    if (cmd == "help" || cmd == "?") { for (const std::string& h : commandHelp()) addLocal(h, nowMs); return true; }
    // social lists
    auto byName = [&](const std::string& name, const std::vector<uint64_t>& guids) -> uint64_t {
        for (uint64_t g : guids)
            if (lowered(objects.nameOf(g)) == lowered(name)) return g;
        return 0;
    };
    if (cmd == "friend" || cmd == "addfriend") { if (!need(!rest.empty(), "/friend Name")) return false; addFriend(rest, nowMs); return true; }
    if (cmd == "unfriend" || cmd == "removefriend") {
        if (!need(!rest.empty(), "/unfriend Name")) return false;
        std::vector<uint64_t> g;
        for (const FriendEntry& f : friends) g.push_back(f.guid);
        if (uint64_t id = byName(rest, g)) { removeFriend(id, nowMs); return true; }
        addLocal(rest + " is not on your friends list.", nowMs);
        return false;
    }
    if (cmd == "ignore") { if (!need(!rest.empty(), "/ignore Name")) return false; addIgnore(rest, nowMs); return true; }
    if (cmd == "unignore") {
        if (!need(!rest.empty(), "/unignore Name")) return false;
        if (uint64_t id = byName(rest, ignored)) { removeIgnore(id, nowMs); return true; }
        addLocal(rest + " is not on your ignore list.", nowMs);
        return false;
    }
    // stage 5.17: the party
    if (cmd == "inv" || cmd == "invite") { if (!need(!rest.empty(), "/invite Name")) return false; inviteToGroup(rest, nowMs); return true; }
    if (cmd == "kick" || cmd == "uninvite") { if (!need(!rest.empty(), "/uninvite Name")) return false; uninvite(rest, nowMs); return true; }
    if (cmd == "leaveparty" || cmd == "leavegroup") { leaveGroup(nowMs); return true; }
    if (cmd == "promote") {
        for (const PartyMember& m : party.members)
            if (m.name == rest) { promoteLeader(m.guid, nowMs); return true; }
        addLocal(rest.empty() ? "Usage: /promote Name" : rest + " is not in your party.", nowMs);
        return false;
    }
    // guild
    if (cmd == "ginvite") { if (!need(!rest.empty(), "/ginvite Name")) return false; guildInvite(rest, nowMs); return true; }
    if (cmd == "gquit" || cmd == "guildquit" || cmd == "gleave") { leaveGuild(nowMs); return true; }
    if (cmd == "groster" || cmd == "guildroster") { requestGuildRoster(nowMs); return true; }
    // target by name, duel
    if (cmd == "target") {
        if (!need(!rest.empty(), "/target Name")) return false;
        const std::string want = lowered(rest);
        uint64_t best = 0;
        for (const auto& [g, o] : objects.all()) {
            if (o.self || !o.isUnitLike()) continue;
            std::string n = lowered(o.name.empty() ? objects.nameOf(g) : o.name);
            if (n == want) { best = g; break; }
            if (!best && !n.empty() && n.compare(0, want.size(), want) == 0) best = g;
        }
        if (!best) { addLocal("No unit named '" + rest + "' in view.", nowMs); return false; }
        setTarget(best, nowMs);
        return true;
    }
    if (cmd == "duel") return challengeDuel(nowMs);
    // the animation stage: /sit /stand /sleep (stand state) and the text emotes that animate (TextEmotes.dbc ids)
    if (cmd == "sit" || cmd == "stand" || cmd == "sleep") {
        setStandState(cmd == "sit" ? 1 : cmd == "sleep" ? 3 : 0, nowMs);
        return true;
    }
    if (uint32_t te = textEmoteId(cmd)) { textEmote(te, nowMs); return true; }
    addLocal("Unknown command /" + cmd + " (/help lists the commands, /emotes the emotes)", nowMs);
    return false;
}

// TextEmotes.dbc ids of the emotes with an animation (vMaNGOS TextEmotes enum)
namespace {
const std::pair<const char*, uint32_t> kEmotes[] = {
    {"applaud", 5}, {"beg", 8}, {"bow", 17}, {"bye", 19}, {"cheer", 21}, {"chicken", 22}, {"clap", 24}, {"cry", 31},
    {"curtsey", 33}, {"dance", 34}, {"eat", 37}, {"flex", 41}, {"greet", 48}, {"hail", 53}, {"hello", 55}, {"kiss", 58},
    {"kneel", 59}, {"laugh", 60}, {"lol", 60}, {"no", 66}, {"nod", 67}, {"yes", 67}, {"point", 72}, {"roar", 75},
    {"rude", 77}, {"salute", 78}, {"shout", 82}, {"shy", 84}, {"talk", 93}, {"talkex", 94}, {"talkq", 95},
    {"thank", 97}, {"victory", 100}, {"wave", 101}, {"welcome", 102}, {"work", 105},
};
}

uint32_t GameSession::textEmoteId(const std::string& cmd) {
    for (const auto& [n, id] : kEmotes)
        if (cmd == n) return id;
    return 0;
}

std::vector<std::string> GameSession::emoteNames() {
    std::vector<std::string> v;
    for (const auto& e : kEmotes) v.push_back(std::string("/") + e.first);
    v.push_back("/sit"); v.push_back("/stand"); v.push_back("/sleep");
    return v;
}

void GameSession::textEmote(uint32_t id, uint32_t nowMs) {   // CMSG_TEXT_EMOTE: u32 text emote, u32 emote num, u64 target
    Writer w;
    w.u32(id);
    w.u32(0xFFFFFFFF);
    w.u64(target_);
    send(CMSG_TEXT_EMOTE, w.b, nowMs);
}

void GameSession::setStandState(uint8_t state, uint32_t nowMs) {   // CMSG_STANDSTATECHANGE: u8 (0 stand, 1 sit, 3 sleep)
    Writer w;
    w.u8(state);
    send(CMSG_STANDSTATECHANGE, w.b, nowMs);
}

// ------------------------------------------------------------------ target and melee
void GameSession::setTarget(uint64_t guid, uint32_t nowMs) {
    if (guid == target_) return;
    if (attackWanted_ && guid != target_) stopAttack(nowMs);
    target_ = guid;
    Writer w;
    w.u64(guid);
    send(CMSG_SET_SELECTION, w.b, nowMs);
}

uint64_t GameSession::pickTarget(const float pos[3], float yaw, float range, uint64_t current, uint32_t nowMs) const {
    struct Cand { int group; float angle, dist; uint64_t guid; };
    std::vector<Cand> c;
    for (auto& [g, o] : objects.all()) {
        if (o.self || !o.hasPos || !o.isUnitLike()) continue;
        float p[3];
        o.positionAt(nowMs, p);
        float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > range) continue;
        float a = std::fabs(std::remainder(std::atan2(dy, dx) - yaw, 6.2831853f));
        // units we can attack first (stage 5.12: by faction; without factions.txt creatures without NPC functions),
        // then NPCs (vendors, quest givers, guards), then players, then the dead; in front (90 degrees either side)
        // before behind
        int group = o.dead() ? 6 : canAttack(o) ? 0 : o.type == ObjType::Player ? 4 : 2;
        group += a > 1.5708f ? 1 : 0;
        c.push_back({group, a, d, g});
    }
    if (c.empty()) return 0;
    std::sort(c.begin(), c.end(), [](const Cand& x, const Cand& y) {
        if (x.group != y.group) return x.group < y.group;
        float kx = x.angle * 8.0f + x.dist, ky = y.angle * 8.0f + y.dist;   // mostly ahead, then near
        return kx != ky ? kx < ky : x.guid < y.guid;
    });
    for (size_t i = 0; i < c.size(); ++i)
        if (c[i].guid == current) return c[(i + 1) % c.size()].guid;
    return c[0].guid;
}

// ------------------------------------------------------------------ other players (UI stage 9)
const char* pvpRankName(uint32_t rank, bool horde) {
    static const char* kAlliance[14] = {"Private", "Corporal", "Sergeant", "Master Sergeant", "Sergeant Major", "Knight",
        "Knight-Lieutenant", "Knight-Captain", "Knight-Champion", "Lieutenant Commander", "Commander", "Marshal", "Field Marshal",
        "Grand Marshal"};
    static const char* kHorde[14] = {"Scout", "Grunt", "Sergeant", "Senior Sergeant", "First Sergeant", "Stone Guard",
        "Blood Guard", "Legionnaire", "Centurion", "Champion", "Lieutenant General", "General", "Warlord", "High Warlord"};
    if (rank < 5 || rank > 18) return "";
    return horde ? kHorde[rank - 5] : kAlliance[rank - 5];
}

bool GameSession::challengeDuel(uint32_t nowMs) {
    const WorldObject* t = objects.find(target_);
    if (!t || t->type != ObjType::Player || t->self) { error("You have no target", Vocal::NoTarget, nowMs); return false; }
    if (duel.active()) { error("You are already in a duel", Vocal::None, nowMs); return false; }
    Writer w;   // the Duel spell at the player (the server answers with SMSG_DUEL_REQUESTED, or a cast failure)
    w.u32(kSpellDuel);
    w.u16(0x2);   // TARGET_FLAG_UNIT
    w.packedGuid(target_);
    send(CMSG_CAST_SPELL_, w.b, nowMs);
    duel.opponent = target_;
    duel.opponentName = t->name.empty() ? objects.nameOf(target_) : t->name;
    return true;
}

void GameSession::answerDuel(bool accept, uint32_t nowMs) {
    if (!duel.arbiter) return;
    Writer w;
    w.u64(duel.arbiter);
    if (accept && duel.challenged) {
        send(CMSG_DUEL_ACCEPTED, w.b, nowMs);
        duel.challenged = false;
        duel.fighting = true;   // (the countdown comes next)
    } else {
        send(CMSG_DUEL_CANCELLED, w.b, nowMs);
        if (duel.challenged) addLocal("You decline the duel.", nowMs);
        duel.challenged = duel.asked = false;
    }
}

bool GameSession::startFollow(uint64_t guid, const float pos[3], uint32_t nowMs) {
    const WorldObject* o = objects.find(guid);
    if (!o || !o->isUnitLike() || o->self || !o->hasPos) { error("You have no target", Vocal::NoTarget, nowMs); return false; }
    float p[3];
    o->positionAt(nowMs, p);
    const float dx = p[0] - pos[0], dy = p[1] - pos[1];
    const std::string nm = o->name.empty() ? objects.nameOf(guid) : o->name;
    if (dx * dx + dy * dy > kFollowStartRange * kFollowStartRange) { error(nm + " is too far away to follow.", Vocal::OutOfRange, nowMs); return false; }
    following = guid;
    followingName = nm;
    followMoving_ = false;
    addLocal("Following " + nm + ".", nowMs);
    return true;
}

void GameSession::stopFollow(uint32_t nowMs, bool tell) {
    if (!following) return;
    if (tell) addLocal("You are no longer following " + followingName + ".", nowMs);
    following = 0;
    followingName.clear();
    followMoving_ = false;
}

bool GameSession::followStep(const float pos[3], float* yaw, uint32_t nowMs) {
    if (!following) return false;
    const WorldObject* o = objects.find(following);
    if (!o || !o->hasPos || o->dead()) { stopFollow(nowMs); return false; }
    float p[3];
    o->positionAt(nowMs, p);
    const float dx = p[0] - pos[0], dy = p[1] - pos[1], d = std::sqrt(dx * dx + dy * dy);
    if (d > kFollowLoseRange) { stopFollow(nowMs); return false; }
    // run until about 2 yd behind, start again past 3.5 yd (no stutter at the edge)
    if (followMoving_ ? d < 2.2f : d > 3.5f) followMoving_ = !followMoving_;
    if (d > 0.3f) *yaw = std::atan2(dy, dx);
    return followMoving_;
}

bool GameSession::inspect(uint64_t guid, const float pos[3], uint32_t nowMs) {
    const WorldObject* o = objects.find(guid);
    if (!o || o->type != ObjType::Player || !o->hasPos) { error("Target a player first", Vocal::NoTarget, nowMs); return false; }
    float p[3];
    o->positionAt(nowMs, p);
    const float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
    if (!o->self && dx * dx + dy * dy + dz * dz > 28.0f * 28.0f) { error("Out of range", Vocal::OutOfRange, nowMs); return false; }
    Writer w;
    w.u64(guid);
    if (!o->self) send(CMSG_INSPECT, w.b, nowMs);
    send(MSG_INSPECT_HONOR_STATS, w.b, nowMs);
    inspected = guid;
    if (honor.guid != guid) honor = HonorView{guid};
    for (int k = 0; k < 19; ++k) if (uint32_t e = visibleItem(*o, k)) itemInfo(e);   // ask for the names now
    return true;
}

uint32_t GameSession::visibleItem(const WorldObject& o, int slot) const {
    if (slot < 0 || slot > 18) return 0;
    return o.field((uint16_t)(UF::PLAYER_VISIBLE_ITEM_1_0 + UF::PLAYER_VISIBLE_ITEM_STRIDE * slot));
}

bool GameSession::enemyLike(const WorldObject& o) const {
    // stage 5.12: whatever we may attack (hostile and attackable neutral creatures, flagged enemy players); without
    // factions.txt the guess of stage 5.11 (a creature without NPC functions)
    if (!factions.loaded()) return o.type == ObjType::Unit && o.hasPos && !o.dead() && !o.field(UF::UNIT_NPC_FLAGS);
    return o.hasPos && canAttack(o);
}

uint64_t GameSession::softEnemy(const float pos[3], float yaw, float range, float halfAngle, uint32_t nowMs) const {
    uint64_t best = 0;
    float bestKey = 1e9f;
    for (auto& [g, o] : objects.all()) {
        if (o.self || !enemyLike(o)) continue;
        float p[3];
        o.positionAt(nowMs, p);
        float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > range) continue;
        float a = d < 0.5f ? 0.0f : std::fabs(std::remainder(std::atan2(dy, dx) - yaw, 6.2831853f));
        if (a > halfAngle) continue;
        float key = a * 8.0f + d;
        if (key < bestKey) { bestKey = key; best = g; }
    }
    return best;
}

uint64_t GameSession::softInteract(const float pos[3], float yaw, uint32_t nowMs, const char** verb) const {
    // stage 5.18: one rule for units and game objects (useVerb): lootable corpses 5 yd, NPCs and objects 5.5 yd
    uint64_t best = 0;
    float bestKey = 1e9f;
    const char* bestVerb = "";
    for (auto& [g, o] : objects.all()) {
        if (o.self || !o.hasPos || (o.type != ObjType::Unit && o.type != ObjType::GameObject)) continue;
        const char* v = useVerb(o);
        if (!v) continue;
        const bool bobber = o.type == ObjType::GameObject && o.field(UF::GAMEOBJECT_TYPE_ID) == 17;   // ours (useVerb)
        float range = bobber ? (float)kFishingRange : o.type == ObjType::Unit && o.dead() ? 5.0f : 5.5f;
        float p[3];
        o.positionAt(nowMs, p);
        float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > range) continue;
        float a = d < 0.5f ? 0.0f : std::fabs(std::remainder(std::atan2(dy, dx) - yaw, 6.2831853f));
        float key = d + (a > 1.5708f ? 4.0f : 0.0f) + (o.type == ObjType::GameObject ? 0.5f : 0.0f);   // in front, units first
        if (bobber) {   // a bite beats everything; else anything near comes first
            key = fishBiting(nowMs) ? -1.0f : 50.0f;
            v = fishBiting(nowMs) ? "Catch" : "Reel in";
        }
        if (key < bestKey) { bestKey = key; best = g; bestVerb = v; }
    }
    if (verb) *verb = bestVerb;
    return best;
}

bool GameSession::inCombat() const {
    const WorldObject* me = selfObject();
    return attackWanted_ || attackOn_ || (me && (me->field(UF::UNIT_FLAGS) & 0x80000));   // UNIT_FLAG_IN_COMBAT
}

uint64_t GameSession::targetOf(uint64_t guid) const {
    const WorldObject* o = objects.find(guid);
    if (!o) return 0;
    return (uint64_t)o->field(UF::UNIT_TARGET) | ((uint64_t)o->field(UF::UNIT_TARGET + 1) << 32);
}

void GameSession::startAttack(uint32_t nowMs) {
    if (!target_) { error("You have no target", Vocal::NoTarget, nowMs); return; }
    const WorldObject* t = objects.find(target_);
    if (t && t->dead()) { error("Your target is dead", Vocal::InvalidTarget, nowMs); return; }
    // with the faction data we know what the server would refuse (SMSG_ATTACKSWING_CANT_ATTACK) - say it at once
    if (t && factions.loaded() && !canAttack(*t)) { error("You cannot attack that target", Vocal::InvalidTarget, nowMs); return; }
    attackWanted_ = true;
    attackError_ = 0;
    Writer w;
    w.u64(target_);
    send(CMSG_ATTACKSWING, w.b, nowMs);
}

void GameSession::stopAttack(uint32_t nowMs) {
    if (!attackWanted_ && !attackOn_) return;
    attackWanted_ = false;
    send(CMSG_ATTACKSTOP, {}, nowMs);
}

bool GameSession::takeFacingRequest(float* yaw) {
    if (!faceRequest_) return false;
    faceRequest_ = false;
    const WorldObject* t = objects.find(target_);
    if (!t || !t->hasPos) return false;
    float p[3];
    t->positionAt(now_, p);
    const MovementInfo& me = mover.info();
    *yaw = normalizeAngle(std::atan2(p[1] - me.y, p[0] - me.x));
    return true;
}

// ------------------------------------------------------------------ server packets
bool GameSession::handleSocial(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_MESSAGECHAT: {
            ChatLine l;
            l.ms = nowMs;
            l.type = r.u8();
            l.lang = r.u32();
            switch (l.type) {
                case Chat::MonsterWhisper: case Chat::MonsterEmote:
                    if (!readSizedName(r, l.name)) return true;
                    r.u64();   // target
                    break;
                case Chat::Say: case Chat::Party: case Chat::Yell:
                    l.sender = r.u64();
                    r.u64();   // written twice
                    break;
                case Chat::MonsterSay: case Chat::MonsterYell:
                    l.sender = r.u64();
                    if (!readSizedName(r, l.name)) return true;
                    r.u64();
                    break;
                case Chat::Channel:
                    l.channel = r.cstr();
                    r.u32();   // player rank
                    l.sender = r.u64();
                    break;
                default:
                    l.sender = r.u64();
                    break;
            }
            uint32_t len = r.u32();
            if (r.bad || len > 4096 || len > r.left()) return true;
            l.text.assign((const char*)data.data() + r.pos, len);
            r.pos += len;
            while (!l.text.empty() && l.text.back() == '\0') l.text.pop_back();
            r.u8();   // chat tag (AFK/DND/GM)
            if (l.sender && l.name.empty() && objects.nameOf(l.sender).empty() && l.sender != self_ &&
                (l.sender >> 48) == 0 && askedNames_.insert(l.sender).second) {   // a player not in range: ask
                Writer w;
                w.u64(l.sender);
                send(ObjOp::CMSG_NAME_QUERY, w.b, nowMs);
                ++stats.queries;
            }
            if (l.type == Chat::Whisper) { sound("whisper"); if (l.sender) lastWhisperGuid = l.sender; }
            addLine(std::move(l));
            return true;
        }
        case SMSG_CHANNEL_NOTIFY: {
            uint8_t kind = r.u8();
            std::string name = r.cstr();
            if (r.bad) return true;
            auto pn = [&](uint64_t g) { std::string n = playerName(g, nowMs); return n.empty() ? std::string("Someone") : n; };
            std::string a, b;
            switch (kind) {
                case 0x00: case 0x01: case 0x07: case 0x08: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x17: case 0x18:
                    a = pn(r.u64());
                    break;
                case 0x12: case 0x14: case 0x15:
                    a = pn(r.u64());
                    b = pn(r.u64());
                    break;
                case 0x09: case 0x0B: case 0x16: case 0x1D: case 0x1E:
                    a = r.cstr();
                    break;
                case 0x0C: {
                    a = pn(r.u64());
                    uint8_t before = r.u8(), after = r.u8();
                    if ((after & 2) != (before & 2)) b = (after & 2) ? "is now a moderator" : "is no longer a moderator";
                    else if ((after & 8) != (before & 8)) b = (after & 8) ? "has been muted" : "can speak again";
                    else b = "changed mode";
                    break;
                }
                default: break;
            }
            if (r.bad) return true;
            if (kind == 0x02) {
                size_t slot = 0;
                while (slot < channels_.size() && !channels_[slot].empty() && channels_[slot] != name) ++slot;
                if (slot == channels_.size()) channels_.push_back(name);
                else channels_[slot] = name;
                a = std::to_string(slot + 1);
            } else if (kind == 0x03) {
                a = std::to_string(channelNumber(name));
                for (std::string& c : channels_) if (c == name) c.clear();
                while (!channels_.empty() && channels_.back().empty()) channels_.pop_back();
            }
            ChatLine l;
            l.ms = nowMs;
            l.type = Chat::ChannelNotice;
            l.channel = name;
            l.text = channelNoticeText(kind, name, a, b);
            addLine(std::move(l));
            return true;
        }
        case SMSG_CHANNEL_LIST: {   // cstr name, u8 flags, u32 count, count x (u64 guid, u8 member flags)
            std::string name = r.cstr();
            r.u8();
            uint32_t n = r.u32();
            if (r.bad || n > 512) return true;
            std::string text;
            uint32_t shown = 0;
            for (uint32_t i = 0; i < n && !r.bad; ++i) {
                uint64_t g = r.u64();
                uint8_t f = r.u8();
                if (r.bad) break;
                std::string pn = playerName(g, nowMs);
                text += (shown++ ? ", " : "") + (pn.empty() ? std::string("?") : pn) + ((f & 1) ? " (owner)" : (f & 2) ? " (moderator)" : "");
            }
            int num = channelNumber(name);
            ChatLine l;
            l.ms = nowMs;
            l.type = Chat::ChannelNotice;
            l.channel = name;
            l.text = "Players in [" + (num ? std::to_string(num) + ". " : std::string()) + name + "] (" + std::to_string(n) + "): " + text;
            addLine(std::move(l));
            return true;
        }
        case SMSG_SERVER_MESSAGE: {   // u32 kind (1 shutdown in, 2 restart in, 3 custom, 4/5 cancelled), cstr text
            uint32_t kind = r.u32();
            std::string t = r.cstr();
            if (r.bad) return true;
            switch (kind) {
                case 1: addSystem("Server shutdown in " + t + ".", nowMs); break;
                case 2: addSystem("Server restart in " + t + ".", nowMs); break;
                case 4: addSystem("Server shutdown cancelled.", nowMs); break;
                case 5: addSystem("Server restart cancelled.", nowMs); break;
                default: if (!t.empty()) addSystem("[Server] " + t, nowMs); break;
            }
            return true;
        }
        case SMSG_CHAT_WRONG_FACTION: addSystem("You cannot speak to members of the opposing faction.", nowMs); return true;
        case SMSG_CHAT_RESTRICTED: addSystem("You cannot chat right now: your chat is restricted.", nowMs); return true;
        case SMSG_CHAT_PLAYER_AMBIGUOUS: {
            std::string who = r.cstr();
            addSystem(who.empty() ? std::string("Ambiguous player name.") : "Ambiguous player name '" + who + "'.", nowMs);
            return true;
        }
        case SMSG_ZONE_UNDER_ATTACK: {
            uint32_t area = r.u32();
            if (r.bad) return true;
            std::string nm = areaName ? areaName(area) : std::string();
            addSystem((nm.empty() ? "Area " + std::to_string(area) : nm) + " is under attack!", nowMs);
            sound("whisper");
            return true;
        }
        case SMSG_RAID_INSTANCE_MESSAGE: {   // u32 kind (1 hours, 2 minutes, 3 welcome), u32 map, u32 time
            uint32_t kind = r.u32();
            r.u32();
            uint32_t t = r.u32();
            if (r.bad) return true;
            if (kind == 1 || kind == 2) {
                uint32_t mins = kind == 1 ? t * 60 : t;
                addSystem("The instance you are in will reset in " +
                          (mins >= 60 ? std::to_string(mins / 60) + " hour(s)" : std::to_string(mins) + " minute(s)") + ".", nowMs);
            } else {
                addSystem("You are saved to this instance.", nowMs);
            }
            return true;
        }
        case SMSG_WHO: {   // u32 listed, u32 online, then name, guild, u32 level, class, race, zone
            uint32_t listed = r.u32(), online = r.u32();
            if (r.bad || listed > 100) return true;
            addSystem(std::to_string(online) + " player" + (online == 1 ? "" : "s") + " total" +
                      (listed < online ? ", " + std::to_string(listed) + " shown" : std::string()) + ".", nowMs);
            for (uint32_t i = 0; i < listed; ++i) {
                std::string nm = r.cstr(), gd = r.cstr();
                uint32_t lvl = r.u32(), cls = r.u32(), race = r.u32(), zone = r.u32();
                if (r.bad) break;
                std::string z = areaName ? areaName(zone) : std::string();
                addSystem("[" + nm + "]" + (gd.empty() ? std::string() : " <" + gd + ">") + " - Level " + std::to_string(lvl) + " " +
                          raceName((uint8_t)race) + " " + className((uint8_t)cls) + (z.empty() ? std::string() : " - " + z), nowMs);
            }
            return true;
        }
        case MSG_RANDOM_ROLL: {   // u32 min, u32 max, u32 roll, u64 guid
            uint32_t lo = r.u32(), hi = r.u32(), v = r.u32();
            uint64_t g = r.u64();
            if (r.bad) return true;
            std::string nm = g == self_ ? std::string("You") : playerName(g, nowMs);
            if (nm.empty()) nm = "Someone";
            addSystem(nm + (g == self_ ? " roll " : " rolls ") + std::to_string(v) + " (" + std::to_string(lo) + "-" + std::to_string(hi) + ")", nowMs);
            return true;
        }
        case SMSG_CHAT_PLAYER_NOT_FOUND: {
            std::string who = r.cstr();
            if (!r.bad) fail("No player named '" + who + "' is currently playing.", nowMs);
            return true;
        }
        case SMSG_NOTIFICATION: {
            std::string t = r.cstr();
            if (!r.bad) addLocal(t, nowMs);
            return true;
        }
        case SMSG_ATTACKSTART: {
            uint64_t a = r.u64(), v = r.u64();
            if (r.bad) return true;
            if (WorldObject* o = objects.find(a)) o->meleeVictim = v;
            if (a == self_) { attackOn_ = true; attackError_ = 0; }
            return true;
        }
        case SMSG_ATTACKSTOP: {
            uint64_t a = r.packedGuid(), v = r.packedGuid();
            (void)v;
            if (r.bad) return true;
            if (WorldObject* o = objects.find(a)) o->meleeVictim = 0;
            if (a == self_) {
                attackOn_ = false;
                const WorldObject* t = objects.find(target_);
                if (!t || t->dead()) attackWanted_ = false;   // the target died: auto-attack ends
            }
            return true;
        }
        case SMSG_CANCEL_COMBAT:
            attackOn_ = attackWanted_ = false;
            return true;
        case SMSG_ATTACKSWING_NOTINRANGE:
        case SMSG_ATTACKSWING_BADFACING:
        case SMSG_ATTACKSWING_NOTSTANDING:
        case SMSG_ATTACKSWING_DEADTARGET:
        case SMSG_ATTACKSWING_CANT_ATTACK: {
            bool repeat = attackError_ == op && nowMs - attackErrorMs_ < 3000;
            attackError_ = op;
            attackErrorMs_ = nowMs;
            if (op == SMSG_ATTACKSWING_BADFACING) faceRequest_ = true;
            if (op == SMSG_ATTACKSWING_DEADTARGET || op == SMSG_ATTACKSWING_CANT_ATTACK) attackWanted_ = attackOn_ = false;
            if (!repeat) {
                static const char* msg[] = {"Out of range", "You are facing the wrong way", "You must be standing",
                                            "Your target is dead", "You cannot attack that target"};
                static const Vocal voc[] = {Vocal::OutOfRange, Vocal::None, Vocal::None, Vocal::InvalidTarget, Vocal::InvalidTarget};
                error(msg[op - SMSG_ATTACKSWING_NOTINRANGE], voc[op - SMSG_ATTACKSWING_NOTINRANGE], nowMs);
            }
            return true;
        }
        case SMSG_ATTACKERSTATEUPDATE: {
            MeleeHit h;
            h.ms = nowMs;
            h.hitInfo = r.u32();
            h.attacker = r.packedGuid();
            h.victim = r.packedGuid();
            h.damage = (int32_t)r.u32();
            uint8_t n = r.u8();
            if (r.bad || (size_t)n * 20 > r.left()) return true;
            r.pos += (size_t)n * 20;   // per school: u32 school, f32 damage, u32 damage, u32 absorb, u32 resist
            h.victimState = r.u32();
            if (r.bad) return true;
            if (WorldObject* o = objects.find(h.attacker)) {
                o->swingMs = nowMs ? nowMs : 1;
                o->meleeVictim = h.victim;
                o->swingOff = (h.hitInfo & 0x4) != 0;
            }
            if (WorldObject* v = objects.find(h.victim)) {   // how the victim takes it (its animation)
                uint8_t react = h.victimState == 2 ? 3 : h.victimState == 3 ? 4 : h.victimState == 5 ? 5
                              : h.miss() ? 0 : h.crit() ? 2 : 1;
                if (react) { v->hitMs = nowMs ? nowMs : 1; v->hitReact = react; }
            }
            if (h.attacker == self_ || h.victim == self_) {
                std::string a = h.attacker == self_ ? "You" : objects.nameOf(h.attacker);
                std::string v = h.victim == self_ ? "you" : objects.nameOf(h.victim);
                if (a.empty()) a = "?";
                if (v.empty()) v = "?";
                static const char* states[] = {"", "", "dodges", "parries", "is interrupted", "blocks", "evades",
                                               "is immune", "deflects"};
                std::string t;
                if (h.miss()) t = a + (h.attacker == self_ ? " miss " : " misses ") + v + ".";
                else if (h.victimState >= 2 && h.victimState <= 8) t = (h.victim == self_ ? "You" : v) + " " + states[h.victimState] + ".";
                else t = a + (h.attacker == self_ ? (h.crit() ? " crit " : " hit ") : (h.crit() ? " crits " : " hits ")) + v +
                         " for " + std::to_string(h.damage) + ".";
                addLocal(t, nowMs);
            }
            {
                WorldSound w;
                w.kind = WorldSound::Melee;
                w.src = h.attacker;
                w.dst = h.victim;
                w.hitInfo = h.hitInfo;
                w.victimState = h.victimState;
                worldSound(w);
            }
            hits.push_back(h);
            while (hits.size() > kHitsKeep) hits.pop_front();
            return true;
        }
        case SMSG_EMOTE: {   // u32 emote (Emotes.dbc), u64 guid: a one-shot animation (talk, bow, wave ...)
            uint32_t e = r.u32();
            uint64_t g = r.u64();
            if (r.bad) return true;
            if (WorldObject* o = objects.find(g)) { o->emote = e; o->emoteMs = nowMs ? nowMs : 1; }
            return true;
        }
        // ---- duels (UI stage 9)
        case SMSG_DUEL_REQUESTED: {   // u64 the flag, u64 who challenged
            const uint64_t flag = r.u64(), from = r.u64();
            if (r.bad) return true;
            duel.arbiter = flag;
            duel.result.clear();
            if (from == self_) {
                duel.asked = true;
                addLocal("You have requested a duel with " + (duel.opponentName.empty() ? std::string("your target") : duel.opponentName) + ".", nowMs);
            } else {
                duel.challenged = true;
                duel.opponent = from;
                duel.opponentName = objects.nameOf(from);
                if (duel.opponentName.empty()) duel.opponentName = playerName(from, nowMs);
                addLocal((duel.opponentName.empty() ? std::string("Someone") : duel.opponentName) + " has challenged you to a duel.", nowMs);
                sound("invite");
            }
            return true;
        }
        case SMSG_DUEL_COUNTDOWN: {   // u32 ms until the fight
            const uint32_t ms = r.u32();
            if (r.bad) return true;
            duel.fighting = true;
            duel.asked = duel.challenged = false;
            duel.startMs = nowMs + std::min<uint32_t>(ms, 10000);
            return true;
        }
        case SMSG_DUEL_OUTOFBOUNDS:
            duel.outOfBounds = true;
            duel.outMs = nowMs;
            return true;
        case SMSG_DUEL_INBOUNDS:
            duel.outOfBounds = false;
            return true;
        case SMSG_DUEL_COMPLETE: {   // u8: 1 it was fought, 0 cancelled before it began
            const uint8_t started = r.u8();
            if (!r.bad && !started) addLocal("The duel was cancelled.", nowMs);
            const std::string res = duel.result;
            const uint32_t resMs = duel.resultMs;
            duel = DuelState();
            duel.result = res;
            duel.resultMs = resMs;
            return true;
        }
        case SMSG_DUEL_WINNER: {   // u8 fled, winner, loser
            const uint8_t fled = r.u8();
            const std::string winner = r.cstr(), loser = r.cstr();
            if (r.bad) return true;
            duel.result = fled ? loser + " has fled from " + winner + " in a duel" : winner + " has defeated " + loser + " in a duel";
            duel.resultMs = nowMs ? nowMs : 1;
            addLocal(duel.result + ".", nowMs);
            return true;
        }
        case SMSG_INSPECT:   // u64: the server lets us look (the gear is in the visible item fields)
            return true;
        case MSG_INSPECT_HONOR_STATS: {
            HonorView h;
            h.guid = r.u64();
            h.highestRank = r.u8();
            h.todayKills = r.u32();
            r.u16(); r.u16(); r.u16(); r.u16(); r.u16(); r.u16();   // yesterday, last week, this week (honorable / not)
            h.lifetimeHK = r.u32();
            h.lifetimeDHK = r.u32();
            r.u32();   // yesterday's honor
            h.lastWeekHonor = r.u32();
            r.u32();   // this week's honor
            h.lastWeekRank = r.u32();
            if (r.bad) return true;
            h.known = true;
            if (h.guid == inspected) honor = h;
            return true;
        }
        case SMSG_TEXT_EMOTE: {   // u64 who, u32 text emote, u32 emote number, u32 name length, name: its voice (UI stage 8)
            uint64_t who = r.u64();
            uint32_t te = r.u32();
            if (!r.bad && te && heardEmotes.size() < 8) heardEmotes.push_back({who, te});
            return true;   // (the text itself - EmotesText.dbc - is not shown yet)
        }
        case SMSG_LOG_XPGAIN: {
            uint64_t victim = r.u64();
            uint32_t xp = r.u32();
            uint8_t kind = r.u8();
            if (r.bad) return true;
            xpGained += xp;
            xpGains.push_back({xp, nowMs, kind == 0});
            while (xpGains.size() > 4) xpGains.pop_front();
            if (kind == 0) {
                ++kills;
                std::string v = objects.nameOf(victim);
                addLocal((v.empty() ? std::string("Your victim") : v) + " dies, you gain " + std::to_string(xp) + " experience.", nowMs);
            } else {
                addLocal("You gain " + std::to_string(xp) + " experience.", nowMs);
            }
            return true;
        }
        case SMSG_EXPLORATION_EXPERIENCE: {   // u32 area, u32 experience: an area explored for the first time (stage 5.45)
            const uint32_t area = r.u32(), xp = r.u32();
            if (r.bad) return true;
            discovery.area = area;
            discovery.xp = xp;
            ++discovery.seq;
            std::string nm = areaName ? areaName(area) : std::string();
            if (nm.empty()) nm = "a new area";
            addLocal("Discovered " + nm + (xp ? ": " + std::to_string(xp) + " experience gained" : std::string()), nowMs);
            sound("discover");
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
