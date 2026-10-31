// game_quests.cpp — quests of GameSession (stage 5.14): quest giver marks, the quest giver's windows (list,
// details, items wanted, reward), the quest log from our update fields with its objectives, progress lines.
//
// Layouts from vMaNGOS (1.12.1) WorldPackets::Quest (Quest.cpp) and QuestHandler.cpp:
//   CMSG_QUESTGIVER_STATUS_QUERY u64 -> SMSG_QUESTGIVER_STATUS u64, u32 DialogStatus
//   CMSG_QUESTGIVER_HELLO u64 -> SMSG_QUESTGIVER_QUEST_LIST: u64, greeting, u32 emote delay, u32 emote, u8 n,
//     n x {u32 id, u32 icon, u32 level, title}
//   CMSG_QUESTGIVER_QUERY_QUEST u64 npc, u32 quest -> SMSG_QUESTGIVER_QUEST_DETAILS: u64, u32 id, title, details,
//     objectives, u32 auto finish, u32 n choice {item, count, display}, u32 n reward {...}, u32 money, u32 spell,
//     u32 n emotes {emote, delay}  -> CMSG_QUESTGIVER_ACCEPT_QUEST u64, u32 (the server closes the window)
//   CMSG_QUESTGIVER_COMPLETE_QUEST u64, u32 -> SMSG_QUESTGIVER_REQUEST_ITEMS: u64, u32, title, text, u32 emote delay,
//     u32 emote, u32 close on cancel, u32 money, u32 n {item, count, display}, 4 x u32 flags (all set = completable)
//     (skipped by the server straight to the reward when nothing is wanted)
//   CMSG_QUESTGIVER_REQUEST_REWARD u64, u32 -> SMSG_QUESTGIVER_OFFER_REWARD: u64, u32, title, text, u32 auto finish,
//     u32 n emotes {delay, emote}, u32 n choice {...}, u32 n reward {...}, u32 money, u32 flags, u32 spell
//   CMSG_QUESTGIVER_CHOOSE_REWARD u64, u32 quest, u32 choice -> SMSG_QUESTGIVER_QUEST_COMPLETE: u32 quest, u32, u32 xp,
//     u32 money, u32 n {item, count}
//   CMSG_QUEST_QUERY u32 -> SMSG_QUEST_QUERY_RESPONSE (QuestQueryResponse::AppendBodyTo, with the 1.10+ money field)
//   SMSG_QUESTUPDATE_ADD_KILL u32 quest, u32 entry, u32 count, u32 required, u64 guid; _ADD_ITEM u32 item, u32 count;
//   _COMPLETE u32 quest; _FAILED / _FAILEDTIMER u32 quest; SMSG_QUESTGIVER_QUEST_INVALID u32 reason;
//   SMSG_QUESTGIVER_QUEST_FAILED u32 quest, u32 reason; SMSG_QUESTLOG_FULL; CMSG_QUESTLOG_REMOVE_QUEST u8 slot;
//   CMSG_QUESTGIVER_CANCEL (no body).
// Quest log: PLAYER_QUEST_LOG_1_1 (198) + 3 * slot: quest id; counters (6 bits per creature objective) with the
// state in byte 3 (1 complete, 2 failed); timer (Player::SetQuestSlot / SetQuestSlotCounter / SetQuestSlotState).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_QUEST_QUERY = 0x05C, SMSG_QUEST_QUERY_RESPONSE = 0x05D,
    CMSG_QUESTGIVER_STATUS_QUERY = 0x182, SMSG_QUESTGIVER_STATUS = 0x183, CMSG_QUESTGIVER_HELLO = 0x184,
    SMSG_QUESTGIVER_QUEST_LIST = 0x185, CMSG_QUESTGIVER_QUERY_QUEST = 0x186, SMSG_QUESTGIVER_QUEST_DETAILS = 0x188,
    CMSG_QUESTGIVER_ACCEPT_QUEST = 0x189, CMSG_QUESTGIVER_COMPLETE_QUEST = 0x18A, SMSG_QUESTGIVER_REQUEST_ITEMS = 0x18B,
    CMSG_QUESTGIVER_REQUEST_REWARD = 0x18C, SMSG_QUESTGIVER_OFFER_REWARD = 0x18D, CMSG_QUESTGIVER_CHOOSE_REWARD = 0x18E,
    SMSG_QUESTGIVER_QUEST_INVALID = 0x18F, CMSG_QUESTGIVER_CANCEL = 0x190, SMSG_QUESTGIVER_QUEST_COMPLETE = 0x191,
    SMSG_QUESTGIVER_QUEST_FAILED = 0x192, CMSG_QUESTLOG_REMOVE_QUEST = 0x194, SMSG_QUESTLOG_FULL = 0x195,
    SMSG_QUESTUPDATE_FAILED = 0x196, SMSG_QUESTUPDATE_FAILEDTIMER = 0x197, SMSG_QUESTUPDATE_COMPLETE = 0x198,
    SMSG_QUESTUPDATE_ADD_KILL = 0x199, SMSG_QUESTUPDATE_ADD_ITEM = 0x19A,
};
constexpr uint16_t kQuestLog1 = 198;            // PLAYER_QUEST_LOG_1_1
constexpr uint32_t kNpcQuestGiver = 0x2;
constexpr float kStatusRange = 60.0f;          // ask quest givers this close for their mark
constexpr uint32_t kStatusPerUpdate = 4, kQuestQueriesPerUpdate = 4;
constexpr uint32_t kStatusRefreshMs = 60000;

const char* invalidText(uint32_t r) {
    switch (r) {
        case 1: return "You are not high enough level for that quest.";
        case 4: return "Inventory is full.";
        case 6: return "That quest is not available to your race.";
        case 12: return "You can only be on one timed quest at a time.";
        case 13: return "You are already on that quest.";
        case 17: return "Duplicate item found.";
        case 20: return "You don't have the required items with you. Check storage.";
        case 22: return "You don't have enough money for that quest.";
        case 2: default: return "You don't meet the requirements for that quest.";
    }
}

void readItems(Reader& r, std::vector<QuestItemLine>& out, uint32_t max, bool display = true) {
    uint32_t n = r.u32();
    if (r.bad || n > max || (size_t)n * (display ? 12 : 8) > r.left()) { r.bad = true; return; }
    for (uint32_t i = 0; i < n; ++i) {
        QuestItemLine l;
        l.item = r.u32();
        l.count = r.u32();
        if (display) l.displayId = r.u32();
        out.push_back(l);
    }
}

// $N / $B / $G male:female; / $C (class) / $R (race) in quest texts.
std::string expandText(const std::string& s, const std::string& name) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '$' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'B' || c == 'b') { out += '\n'; ++i; continue; }
            if (c == 'N' || c == 'n') { out += name.empty() ? "friend" : name; ++i; continue; }
            if (c == 'C' || c == 'c') { out += "adventurer"; ++i; continue; }
            if (c == 'R' || c == 'r') { out += "traveller"; ++i; continue; }
            if (c == 'G' || c == 'g') {
                size_t colon = s.find(':', i), semi = s.find(';', i);
                if (colon != std::string::npos && semi != std::string::npos && colon < semi) {
                    size_t a = i + 2;
                    while (a < colon && s[a] == ' ') ++a;
                    out += s.substr(a, colon - a);
                    i = semi;
                    continue;
                }
            }
        }
        if (s[i] == '\r') continue;
        out += s[i];
    }
    return out;
}
}  // namespace

const char* questMarkGlyph(uint32_t st) {
    switch (st) {
        case QuestMark::Available: case QuestMark::Unavailable: return "!";
        case QuestMark::Incomplete: case QuestMark::RewardRep: case QuestMark::RewardOld: case QuestMark::Reward2: return "?";
        default: return "";
    }
}

bool questMarkGrey(uint32_t st) { return st == QuestMark::Unavailable || st == QuestMark::Incomplete; }

uint32_t GameSession::questGiverStatus(uint64_t guid) const {
    auto it = questStatus_.find(guid);
    return it == questStatus_.end() ? 0 : it->second;
}

// ------------------------------------------------------------------ the quest log
std::vector<QuestLogEntry> GameSession::questLog() const {
    std::vector<QuestLogEntry> out;
    const WorldObject* me = selfObject();
    if (!me) return out;
    for (int s = 0; s < kQuestLogSize; ++s) {
        uint32_t id = me->field((uint16_t)(kQuestLog1 + 3 * s));
        if (!id) continue;
        QuestLogEntry e;
        e.slot = (uint8_t)s;
        e.id = id;
        uint32_t v = me->field((uint16_t)(kQuestLog1 + 3 * s + 1));
        for (int k = 0; k < 4; ++k) e.kills[k] = (uint8_t)((v >> (6 * k)) & 0x3F);
        uint8_t state = (uint8_t)(v >> 24);
        e.complete = (state & 1) != 0;
        e.failed = (state & 2) != 0;
        e.timer = me->field((uint16_t)(kQuestLog1 + 3 * s + 2));
        out.push_back(e);
    }
    return out;
}

const QuestInfo* GameSession::questInfoIfKnown(uint32_t id) const {
    auto it = quests_.find(id);
    return it == quests_.end() ? nullptr : &it->second;
}

const QuestInfo* GameSession::questInfo(uint32_t id) {
    if (!id) return nullptr;
    if (const QuestInfo* q = questInfoIfKnown(id)) return q;
    questWanted_.insert(id);
    return nullptr;
}

std::string GameSession::questTitle(uint32_t id) {
    const QuestInfo* q = questInfo(id);
    if (q && !q->title.empty()) return q->title;
    auto it = questTitles_.find(id);   // seen in a quest giver's window before the quest data arrived
    if (it != questTitles_.end()) return it->second;
    return "Quest " + std::to_string(id);
}

static uint32_t countItem(const std::vector<InvItem>& inv, uint32_t entry) {
    uint32_t n = 0;
    for (const InvItem& it : inv)
        if (it.entry == entry) n += it.count;
    return n;
}

std::vector<std::string> GameSession::questProgress(const QuestLogEntry& e) {
    // ask for what is missing (quest data, names of wanted items), then the same text as the const version
    if (const QuestInfo* q = questInfo(e.id))
        for (const QuestInfo::Objective& o : q->obj)
            if (o.item) itemInfo(o.item);
    return questProgressKnown(e);
}

std::vector<std::string> GameSession::questProgressKnown(const QuestLogEntry& e) const {
    std::vector<std::string> out;
    const QuestInfo* q = questInfoIfKnown(e.id);
    if (!q) return out;
    std::vector<InvItem> inv = inventory();
    for (int k = 0; k < 4; ++k) {
        const QuestInfo::Objective& o = q->obj[k];
        if (o.creatureOrGo && o.count) {
            std::string what = o.text;
            if (what.empty()) {
                if (!(o.creatureOrGo & 0x80000000u))
                    for (auto& [g, ob] : objects.all())
                        if (ob.type == ObjType::Unit && ob.entry() == o.creatureOrGo && !ob.name.empty()) { what = ob.name + " slain"; break; }
                if (what.empty()) what = (o.creatureOrGo & 0x80000000u) ? "Object used" : "Creature slain";
            }
            uint32_t have = std::min<uint32_t>(e.kills[k], o.count);
            out.push_back(what + ": " + std::to_string(have) + "/" + std::to_string(o.count));
        }
    }
    for (int k = 0; k < 4; ++k) {
        const QuestInfo::Objective& o = q->obj[k];
        if (o.item && o.itemCount) {
            uint32_t have = std::min<uint32_t>(countItem(inv, o.item), o.itemCount);
            const ItemInfo* ii = itemInfoIfKnown(o.item);
            std::string nm = ii && !ii->name.empty() ? ii->name : "Item " + std::to_string(o.item);
            out.push_back(nm + ": " + std::to_string(have) + "/" + std::to_string(o.itemCount));
        }
    }
    if (e.failed) out.push_back("(failed)");
    else if (e.complete) out.push_back("(complete)");
    return out;
}

std::string GameSession::questTitleKnown(uint32_t id) const {
    if (const QuestInfo* q = questInfoIfKnown(id)) if (!q->title.empty()) return q->title;
    auto it = questTitles_.find(id);
    return it != questTitles_.end() ? it->second : "Quest " + std::to_string(id);
}

void GameSession::abandonQuest(uint8_t slot, uint32_t nowMs) {
    if (slot >= kQuestLogSize) return;
    Writer w;
    w.u8(slot);
    send(CMSG_QUESTLOG_REMOVE_QUEST, w.b, nowMs);
}

// ------------------------------------------------------------------ the quest giver
void GameSession::questHello(uint64_t npc, uint32_t nowMs) {
    if (!npc) return;
    Writer w;
    w.u64(npc);
    send(CMSG_QUESTGIVER_HELLO, w.b, nowMs);
}

void GameSession::chooseQuest(uint64_t npc, const GossipQuest& q, uint32_t nowMs) {
    if (!npc || !q.id) return;
    bool inLog = false;
    for (const QuestLogEntry& e : questLog()) inLog = inLog || e.id == q.id;
    Writer w;
    w.u64(npc);
    w.u32(q.id);
    send(inLog ? CMSG_QUESTGIVER_COMPLETE_QUEST : CMSG_QUESTGIVER_QUERY_QUEST, w.b, nowMs);
}

void GameSession::acceptQuest(uint32_t nowMs) {
    if (questDialog.kind != QuestDialog::Kind::Details) return;
    Writer w;
    w.u64(questDialog.npc);
    w.u32(questDialog.quest);
    send(CMSG_QUESTGIVER_ACCEPT_QUEST, w.b, nowMs);
    questDialog = QuestDialog();
}

void GameSession::continueQuest(uint32_t nowMs) {
    if (questDialog.kind != QuestDialog::Kind::RequestItems) return;
    if (!questDialog.completable) { fail("You haven't finished that quest yet.", nowMs); return; }
    Writer w;
    w.u64(questDialog.npc);
    w.u32(questDialog.quest);
    send(CMSG_QUESTGIVER_REQUEST_REWARD, w.b, nowMs);
}

void GameSession::completeQuest(size_t choice, uint32_t nowMs) {
    if (questDialog.kind != QuestDialog::Kind::OfferReward) return;
    if (!questDialog.choices.empty() && choice >= questDialog.choices.size()) {
        fail("Choose a reward first.", nowMs);
        return;
    }
    Writer w;
    w.u64(questDialog.npc);
    w.u32(questDialog.quest);
    w.u32(questDialog.choices.empty() ? 0 : (uint32_t)choice);
    send(CMSG_QUESTGIVER_CHOOSE_REWARD, w.b, nowMs);
    questDialog = QuestDialog();
}

void GameSession::closeQuestDialog(uint32_t nowMs) {
    if (!questDialog.open()) return;
    questDialog = QuestDialog();
    send(CMSG_QUESTGIVER_CANCEL, {}, nowMs);
}

// ------------------------------------------------------------------ per frame
void GameSession::updateQuests(uint32_t nowMs) {
    const WorldObject* me = selfObject();
    const MovementInfo& m = mover.info();
    // a changed quest log or level changes the marks: ask again
    std::vector<QuestLogEntry> log = questLog();
    uint32_t h = 2166136261u;
    for (const QuestLogEntry& e : log) h = (h ^ e.id ^ ((uint32_t)e.complete << 31) ^ ((uint32_t)e.failed << 30)) * 16777619u;
    uint32_t lvl = me ? me->level() : 0;
    if (h != questLogHash_ || lvl != questLevel_ || nowMs - statusRefreshMs_ > kStatusRefreshMs) {
        if (me && (h != questLogHash_) && questLogSeen_) {   // accepted / dropped quests
            for (const QuestLogEntry& e : log)
                if (!questLogIds_.count(e.id)) {
                    ++questsAccepted;
                    notice("Quest accepted: " + questTitle(e.id), nowMs);
                    sound("questaccept");
                }
        }
        if (me) {
            questLogIds_.clear();
            for (const QuestLogEntry& e : log) questLogIds_.insert(e.id);
            questLogSeen_ = true;
        }
        questLogHash_ = h;
        questLevel_ = lvl;
        statusRefreshMs_ = nowMs ? nowMs : 1;
        statusAskedMs_.clear();
    }
    for (const QuestLogEntry& e : log) questInfo(e.id);   // titles and objectives for the log
    // quest giver marks of the NPCs around us
    uint32_t sent = 0;
    for (auto& [g, o] : objects.all()) {
        if (sent >= kStatusPerUpdate) break;
        const bool giver = o.type == ObjType::Unit ? (o.field(UF::UNIT_NPC_FLAGS) & kNpcQuestGiver) && !o.dead()
                         : o.type == ObjType::GameObject && o.field(UF::GAMEOBJECT_TYPE_ID) == 2;   // quest giver objects
        if (!giver || !o.hasPos) continue;
        if (statusAskedMs_.count(g)) continue;
        float p[3];
        o.positionAt(nowMs, p);
        float dx = p[0] - m.x, dy = p[1] - m.y;
        if (dx * dx + dy * dy > kStatusRange * kStatusRange) continue;
        statusAskedMs_[g] = nowMs ? nowMs : 1;
        Writer w;
        w.u64(g);
        send(CMSG_QUESTGIVER_STATUS_QUERY, w.b, nowMs);
        ++sent;
    }
    // quest data
    uint32_t asked = 0;
    for (auto it = questWanted_.begin(); it != questWanted_.end() && asked < kQuestQueriesPerUpdate;) {
        uint32_t id = *it;
        it = questWanted_.erase(it);
        if (quests_.count(id) || !questAsked_.insert(id).second) continue;
        Writer w;
        w.u32(id);
        send(CMSG_QUEST_QUERY, w.b, nowMs);
        ++questQueries;
        ++asked;
    }
    // item objectives: the quest packet comes before the item itself, so the "x/y" line waits for the bags
    if (!pendingItemLines_.empty()) {
        std::vector<InvItem> inv = inventory();
        for (auto it = pendingItemLines_.begin(); it != pendingItemLines_.end();) {
            uint32_t have = countItem(inv, it->item);
            if (have == it->before && nowMs - it->ms < 1500) { ++it; continue; }
            if (it->need) notice(itemName(it->item) + ": " + std::to_string(std::min(have, it->need)) + "/" + std::to_string(it->need), nowMs);
            it = pendingItemLines_.erase(it);
        }
    }
    // windows close when we walk away from the NPC (the original does the same)
    auto tooFar = [&](uint64_t npc) {
        const WorldObject* o = objects.find(npc);
        if (!o || !o->hasPos) return false;
        float p[3];
        o->positionAt(nowMs, p);
        float dx = p[0] - m.x, dy = p[1] - m.y, dz = p[2] - m.z;
        return dx * dx + dy * dy + dz * dz > 10.0f * 10.0f;
    };
    if (questDialog.open() && tooFar(questDialog.npc)) questDialog = QuestDialog();
    if (vendor.open && tooFar(vendor.npc)) vendor = VendorWindow();
    if (gossip.open && tooFar(gossip.npc)) gossip = GossipMenu();
    if (trainer.open && tooFar(trainer.npc)) trainer = TrainerWindow();
    if (binderConfirm && tooFar(binderConfirm)) binderConfirm = 0;
}

// ------------------------------------------------------------------ server packets
bool GameSession::handleQuests(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    const std::string me = !selfName.empty() ? selfName : objects.nameOf(self_);
    switch (op) {
        case SMSG_QUESTGIVER_STATUS: {
            uint64_t g = r.u64();
            uint32_t st = r.u32();
            if (r.bad) return true;
            questStatus_[g] = st;
            return true;
        }
        case SMSG_QUESTGIVER_QUEST_LIST: {
            QuestDialog d;
            d.kind = QuestDialog::Kind::List;
            d.npc = r.u64();
            d.text = expandText(r.cstr(), me);
            r.u32();
            r.u32();
            uint8_t n = r.u8();
            if (r.bad || n > 32) return true;
            for (uint8_t i = 0; i < n && !r.bad; ++i) {
                GossipQuest q;
                q.id = r.u32();
                q.icon = r.u32();
                q.level = r.u32();
                q.title = r.cstr();
                if (q.id && !q.title.empty()) questTitles_[q.id] = q.title;
                d.list.push_back(std::move(q));
            }
            if (r.bad) return true;
            questDialog = std::move(d);
            gossip = GossipMenu();
            vendor = VendorWindow();
            trainer = TrainerWindow();
            return true;
        }
        case SMSG_QUESTGIVER_QUEST_DETAILS: {
            QuestDialog d;
            d.kind = QuestDialog::Kind::Details;
            d.npc = r.u64();
            d.quest = r.u32();
            d.title = r.cstr();
            d.text = expandText(r.cstr(), me);
            d.objectives = expandText(r.cstr(), me);
            r.u32();   // auto finish
            readItems(r, d.choices, 6);
            readItems(r, d.rewards, 4);
            d.money = r.u32();
            d.rewardSpell = r.u32();   // the emotes after it are not needed
            if (r.bad) return true;
            if (d.quest && !d.title.empty()) questTitles_[d.quest] = d.title;
            for (const QuestItemLine& l : d.choices) itemInfo(l.item);
            for (const QuestItemLine& l : d.rewards) itemInfo(l.item);
            questDialog = std::move(d);
            gossip = GossipMenu();
            vendor = VendorWindow();
            trainer = TrainerWindow();
            return true;
        }
        case SMSG_QUESTGIVER_REQUEST_ITEMS: {
            QuestDialog d;
            d.kind = QuestDialog::Kind::RequestItems;
            d.npc = r.u64();
            d.quest = r.u32();
            d.title = r.cstr();
            d.text = expandText(r.cstr(), me);
            r.u32();   // emote delay
            r.u32();   // emote
            r.u32();   // close on cancel
            d.requiredMoney = r.u32();
            readItems(r, d.required, 6);
            uint32_t f1 = r.u32(), f2 = r.u32(), f3 = r.u32(), f4 = r.u32();
            if (r.bad) return true;
            d.completable = f1 && f2 && f3 && f4;
            for (const QuestItemLine& l : d.required) itemInfo(l.item);
            questDialog = std::move(d);
            gossip = GossipMenu();
            vendor = VendorWindow();
            trainer = TrainerWindow();
            return true;
        }
        case SMSG_QUESTGIVER_OFFER_REWARD: {
            QuestDialog d;
            d.kind = QuestDialog::Kind::OfferReward;
            d.npc = r.u64();
            d.quest = r.u32();
            d.title = r.cstr();
            d.text = expandText(r.cstr(), me);
            r.u32();   // auto finish
            uint32_t ne = r.u32();
            if (r.bad || ne > 16) return true;
            r.skip((size_t)ne * 8);
            readItems(r, d.choices, 6);
            readItems(r, d.rewards, 4);
            d.money = r.u32();
            r.u32();   // quest flags
            d.rewardSpell = r.u32();
            if (r.bad) return true;
            for (const QuestItemLine& l : d.choices) itemInfo(l.item);
            for (const QuestItemLine& l : d.rewards) itemInfo(l.item);
            questDialog = std::move(d);
            gossip = GossipMenu();
            vendor = VendorWindow();
            trainer = TrainerWindow();
            return true;
        }
        case SMSG_QUESTGIVER_QUEST_INVALID: {
            uint32_t why = r.u32();
            if (r.bad) return true;
            lastQuestInvalid = why;
            fail(invalidText(why), nowMs);
            return true;
        }
        case SMSG_QUESTGIVER_QUEST_FAILED: {
            uint32_t q = r.u32(), why = r.u32();
            if (r.bad) return true;
            fail(questTitle(q) + " failed: " + invalidText(why), nowMs);
            return true;
        }
        case SMSG_QUESTGIVER_QUEST_COMPLETE: {
            uint32_t q = r.u32();
            r.u32();
            uint32_t xp = r.u32(), money = r.u32();
            uint32_t n = r.u32();
            if (r.bad || n > 8) return true;
            ++questsCompleted;
            sound("questcomplete");
            std::string t = questTitle(q) + " completed.";
            if (xp) t += " " + std::to_string(xp) + " xp.";
            if (money) t += " " + moneyText(money) + ".";
            addLocal(t, nowMs);
            if (xp) xpGained += xp;
            questDialog = QuestDialog();
            statusAskedMs_.clear();
            return true;
        }
        case SMSG_QUESTLOG_FULL:
            fail("Your quest log is full.", nowMs);
            return true;
        case SMSG_QUESTUPDATE_FAILED: case SMSG_QUESTUPDATE_FAILEDTIMER: {
            uint32_t q = r.u32();
            if (r.bad) return true;
            fail(questTitle(q) + (op == SMSG_QUESTUPDATE_FAILED ? " failed." : " failed: out of time."), nowMs);
            return true;
        }
        case SMSG_QUESTUPDATE_COMPLETE: {
            uint32_t q = r.u32();
            if (r.bad) return true;
            ++questUpdates;
            notice(questTitle(q) + " (Complete)", nowMs);
            statusAskedMs_.clear();
            return true;
        }
        case SMSG_QUESTUPDATE_ADD_KILL: {
            uint32_t q = r.u32(), entry = r.u32(), count = r.u32(), need = r.u32();
            r.u64();
            if (r.bad) return true;
            ++questUpdates;
            sound("questupdate");
            std::string what;
            if (const QuestInfo* qi = questInfoIfKnown(q))
                for (const QuestInfo::Objective& o : qi->obj)
                    if (o.creatureOrGo == entry && !o.text.empty()) what = o.text;
            if (what.empty()) {
                for (auto& [g, ob] : objects.all())
                    if (ob.type == ObjType::Unit && ob.entry() == entry && !ob.name.empty()) { what = ob.name + " slain"; break; }
                if (what.empty()) what = "Creature slain";
            }
            notice(what + ": " + std::to_string(count) + "/" + std::to_string(need), nowMs);
            return true;
        }
        case SMSG_QUESTUPDATE_ADD_ITEM: {
            uint32_t item = r.u32();
            r.u32();
            if (r.bad) return true;
            ++questUpdates;
            itemInfo(item);
            PendingItemLine pl;
            pl.item = item;
            pl.ms = nowMs ? nowMs : 1;
            pl.before = countItem(inventory(), item);
            for (const QuestLogEntry& e : questLog())   // what the quests in the log want of it (now: the log may change)
                if (const QuestInfo* q = questInfoIfKnown(e.id))
                    for (const QuestInfo::Objective& o : q->obj)
                        if (o.item == item && o.itemCount > pl.need) pl.need = o.itemCount;
            if (pendingItemLines_.size() < 16) pendingItemLines_.push_back(pl);
            return true;
        }
        case SMSG_QUEST_QUERY_RESPONSE: {
            QuestInfo q;
            q.id = r.u32();
            q.method = r.u32();
            q.level = r.u32();
            q.zoneOrSort = r.u32();
            q.type = r.u32();
            r.u32(); r.u32(); r.u32(); r.u32();   // reputation objective, opposite reputation
            q.nextInChain = r.u32();
            q.money = r.u32();
            r.u32();   // money at max level (1.10+)
            q.rewardSpell = r.u32();
            q.sourceItem = r.u32();
            q.flags = r.u32();
            for (int i = 0; i < 4; ++i) { q.rewardItem[i] = r.u32(); q.rewardCount[i] = r.u32(); }
            for (int i = 0; i < 6; ++i) { q.choiceItem[i] = r.u32(); q.choiceCount[i] = r.u32(); }
            r.u32(); r.f32(); r.f32(); r.u32();   // point of interest
            q.title = r.cstr();
            q.objectives = expandText(r.cstr(), me);
            q.details = expandText(r.cstr(), me);
            q.endText = r.cstr();
            for (int i = 0; i < 4; ++i) {
                q.obj[i].creatureOrGo = r.u32();
                q.obj[i].count = r.u32();
                q.obj[i].item = r.u32();
                q.obj[i].itemCount = r.u32();
            }
            for (int i = 0; i < 4; ++i) q.obj[i].text = r.cstr();
            if (r.bad || !q.id) return true;
            for (int i = 0; i < 4; ++i)
                if (q.obj[i].item) itemInfo(q.obj[i].item);
            quests_[q.id] = std::move(q);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
