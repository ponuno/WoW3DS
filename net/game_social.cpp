// game_social.cpp — chat, target and melee of GameSession (stages 5.6/5.7).
//
// Layouts from vMaNGOS (1.12.1): ChatHandler::BuildChatPacket, WorldPackets::Chat::ChatMessage,
// WorldPackets::Combat (AttackStart/AttackStop/MeleeAttackingStateUpdate), Player::SendLogXPGain,
// Channel::MakeNotifyPacket. (Loot moved to game_items.cpp in stage 5.10.)
#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_JOIN_CHANNEL = 0x097, CMSG_MESSAGECHAT = 0x095, SMSG_MESSAGECHAT = 0x096, SMSG_CHANNEL_NOTIFY = 0x099,
    CMSG_SET_SELECTION = 0x13D, CMSG_ATTACKSWING = 0x141, CMSG_ATTACKSTOP = 0x142,
    SMSG_ATTACKSTART = 0x143, SMSG_ATTACKSTOP = 0x144, SMSG_ATTACKSWING_NOTINRANGE = 0x145,
    SMSG_ATTACKSWING_BADFACING = 0x146, SMSG_ATTACKSWING_NOTSTANDING = 0x147, SMSG_ATTACKSWING_DEADTARGET = 0x148,
    SMSG_ATTACKSWING_CANT_ATTACK = 0x149, SMSG_ATTACKERSTATEUPDATE = 0x14A, SMSG_CANCEL_COMBAT = 0x14E,
    SMSG_NOTIFICATION = 0x1CB, CMSG_STANDSTATECHANGE = 0x101, SMSG_EMOTE = 0x103, CMSG_TEXT_EMOTE = 0x104,
    SMSG_TEXT_EMOTE = 0x105, SMSG_LOG_XPGAIN = 0x1D0, SMSG_CHAT_PLAYER_NOT_FOUND = 0x2A9,
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
        case Loot: return "loot"; case Skill: return "skill"; case Local: return "client";
        default: return "?";
    }
}

void GameSession::addLine(ChatLine l) {
    chat.push_back(std::move(l));
    while (chat.size() > kChatKeep) chat.pop_front();
    ++chatCount;
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
        case Chat::Channel: return "[" + l.channel + "] [" + who + "]: " + l.text;
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

bool GameSession::sendTyped(const std::string& lineIn, uint32_t nowMs) {
    std::string line = lineIn;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return false;
    if (line[0] != '/') { sendChat(Chat::Say, line, "", nowMs); return true; }
    size_t sp = line.find(' ');
    std::string cmd = line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
    std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    for (char& c : cmd) c = (char)tolower((unsigned char)c);
    auto need = [&](bool ok, const char* what) {
        if (!ok) addLocal(std::string("Usage: ") + what, nowMs);
        return ok;
    };
    if (cmd == "s" || cmd == "say") { if (!need(!rest.empty(), "/s text")) return false; sendChat(Chat::Say, rest, "", nowMs); return true; }
    if (cmd == "y" || cmd == "yell") { if (!need(!rest.empty(), "/y text")) return false; sendChat(Chat::Yell, rest, "", nowMs); return true; }
    if (cmd == "p" || cmd == "party") { if (!need(!rest.empty(), "/p text")) return false; sendChat(Chat::Party, rest, "", nowMs); return true; }
    if (cmd == "g" || cmd == "guild") { if (!need(!rest.empty(), "/g text")) return false; sendChat(Chat::Guild, rest, "", nowMs); return true; }
    if (cmd == "e" || cmd == "me" || cmd == "emote") { if (!need(!rest.empty(), "/e text")) return false; sendChat(Chat::Emote, rest, "", nowMs); return true; }
    if (cmd == "w" || cmd == "whisper" || cmd == "t" || cmd == "tell") {
        size_t s2 = rest.find(' ');
        if (!need(s2 != std::string::npos && s2 > 0 && s2 + 1 < rest.size(), "/w Name text")) return false;
        sendChat(Chat::Whisper, rest.substr(s2 + 1), rest.substr(0, s2), nowMs);
        return true;
    }
    if (cmd == "join") {
        if (!need(!rest.empty(), "/join Channel")) return false;
        Writer w;
        w.cstr(rest);
        w.cstr("");
        send(CMSG_JOIN_CHANNEL, w.b, nowMs);
        return true;
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
    // the animation stage: /sit /stand /sleep (stand state) and the text emotes that animate (TextEmotes.dbc ids)
    if (cmd == "sit" || cmd == "stand" || cmd == "sleep") {
        setStandState(cmd == "sit" ? 1 : cmd == "sleep" ? 3 : 0, nowMs);
        return true;
    }
    if (uint32_t te = textEmoteId(cmd)) { textEmote(te, nowMs); return true; }
    if (!cmd.empty() && cmd.find_first_not_of("0123456789") == std::string::npos) {   // /1 text
        size_t n = (size_t)std::atoi(cmd.c_str());
        if (n < 1 || n > channels_.size()) { addLocal("Not in channel " + cmd + " (/join Name first)", nowMs); return false; }
        if (!need(!rest.empty(), "/1 text")) return false;
        sendChat(Chat::Channel, rest, channels_[n - 1], nowMs);
        return true;
    }
    addLocal("Unknown command /" + cmd + " (/s /y /p /g /w /e /join /1 /invite /leaveparty /sit /dance /wave ...)", nowMs);
    return false;
}

// TextEmotes.dbc ids of the emotes with an animation (vMaNGOS TextEmotes enum)
uint32_t GameSession::textEmoteId(const std::string& cmd) {
    static const std::pair<const char*, uint32_t> k[] = {
        {"applaud", 5}, {"beg", 8}, {"bow", 17}, {"bye", 19}, {"cheer", 21}, {"chicken", 22}, {"clap", 24}, {"cry", 31},
        {"curtsey", 33}, {"dance", 34}, {"eat", 37}, {"flex", 41}, {"greet", 48}, {"hail", 53}, {"hello", 55}, {"kiss", 58},
        {"kneel", 59}, {"laugh", 60}, {"lol", 60}, {"no", 66}, {"nod", 67}, {"yes", 67}, {"point", 72}, {"roar", 75},
        {"rude", 77}, {"salute", 78}, {"shout", 82}, {"shy", 84}, {"talk", 93}, {"talkex", 94}, {"talkq", 95},
        {"thank", 97}, {"victory", 100}, {"wave", 101}, {"welcome", 102}, {"work", 105},
    };
    for (const auto& [n, id] : k)
        if (cmd == n) return id;
    return 0;
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
        float range = o.type == ObjType::Unit && o.dead() ? 5.0f : 5.5f;
        float p[3];
        o.positionAt(nowMs, p);
        float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > range) continue;
        float a = d < 0.5f ? 0.0f : std::fabs(std::remainder(std::atan2(dy, dx) - yaw, 6.2831853f));
        float key = d + (a > 1.5708f ? 4.0f : 0.0f) + (o.type == ObjType::GameObject ? 0.5f : 0.0f);   // in front, units first
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
    if (!target_) { addLocal("No target", nowMs); return; }
    const WorldObject* t = objects.find(target_);
    if (t && t->dead()) { addLocal("Your target is dead", nowMs); return; }
    // with the faction data we know what the server would refuse (SMSG_ATTACKSWING_CANT_ATTACK) - say it at once
    if (t && factions.loaded() && !canAttack(*t)) { addLocal("You cannot attack that target", nowMs); return; }
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
            if (l.type == Chat::Whisper) sound("whisper");
            addLine(std::move(l));
            return true;
        }
        case SMSG_CHANNEL_NOTIFY: {
            uint8_t kind = r.u8();
            std::string name = r.cstr();
            if (r.bad) return true;
            if (kind == 0x02 && std::find(channels_.begin(), channels_.end(), name) == channels_.end()) {
                channels_.push_back(name);
                addLocal("Joined channel " + std::to_string(channels_.size()) + ". " + name, nowMs);
            } else if (kind == 0x03) {
                channels_.erase(std::remove(channels_.begin(), channels_.end(), name), channels_.end());
                addLocal("Left channel " + name, nowMs);
            }
            return true;
        }
        case SMSG_CHAT_PLAYER_NOT_FOUND: {
            std::string who = r.cstr();
            if (!r.bad) addLocal("No player named '" + who + "' is currently playing.", nowMs);
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
                addLocal(msg[op - SMSG_ATTACKSWING_NOTINRANGE], nowMs);
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
        case SMSG_TEXT_EMOTE: return true;   // the text of /dance etc. (EmotesText.dbc): not shown yet
        case SMSG_LOG_XPGAIN: {
            uint64_t victim = r.u64();
            uint32_t xp = r.u32();
            uint8_t kind = r.u8();
            if (r.bad) return true;
            xpGained += xp;
            if (kind == 0) {
                ++kills;
                std::string v = objects.nameOf(victim);
                addLocal((v.empty() ? std::string("Your victim") : v) + " dies, you gain " + std::to_string(xp) + " experience.", nowMs);
            } else {
                addLocal("You gain " + std::to_string(xp) + " experience.", nowMs);
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
