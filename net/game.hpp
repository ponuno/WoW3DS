// game.hpp — stage 5 in-world session on top of WorldClient: object table,
// own movement (Mover), server-forced movement (teleport, speed, root) with the
// acknowledgements vanilla servers expect, name/creature/gameobject queries.
//
// Usage: once WorldClient is InWorld, call attach(); then every frame
// mover.update(...) with the local player's state and session.update(nowMs).
#pragma once
#include <array>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "client.hpp"
#include "factions.hpp"
#include "objects.hpp"
#include "tables.hpp"

namespace w3n {

// ---------------------------------------------------------------- chat, target, melee, loot (stage 5.6/5.7)
// Chat types of 1.12 (vMaNGOS SharedDefines.h ChatMsg; WoWee's enum is the WotLK numbering).
namespace Chat {
enum : uint8_t {
    Say = 0x00, Party = 0x01, Raid = 0x02, Guild = 0x03, Officer = 0x04, Yell = 0x05, Whisper = 0x06,
    WhisperInform = 0x07, Emote = 0x08, TextEmote = 0x09, System = 0x0A, MonsterSay = 0x0B, MonsterYell = 0x0C,
    MonsterEmote = 0x0D, Channel = 0x0E, ChannelJoin = 0x0F, ChannelLeave = 0x10, ChannelNotice = 0x12, Afk = 0x14,
    Dnd = 0x15, Ignored = 0x16, Skill = 0x17, Loot = 0x18, MonsterWhisper = 0x1A, RaidLeader = 0x57,
    RaidWarning = 0x58, Battleground = 0x5C, BattlegroundLeader = 0x5D,
    // not from the server: lines the session writes itself (combat, XP, loot, errors)
    Local = 0xFE,
};
const char* typeName(uint8_t type);
}

struct ChatLine {
    uint32_t ms = 0;
    uint8_t type = 0;
    uint32_t lang = 0;
    uint64_t sender = 0;
    std::string name;      // sender (player names arrive later through the name cache: see GameSession::lineText)
    std::string channel;   // Channel: its name; Whisper/WhisperInform: the other side's name when known
    std::string text;
};

// One melee swing (SMSG_ATTACKERSTATEUPDATE).
struct MeleeHit {
    uint32_t ms = 0;
    uint64_t attacker = 0, victim = 0;
    int32_t damage = 0;
    uint32_t hitInfo = 0, victimState = 0;
    bool miss() const { return (hitInfo & 0x10) != 0; }
    bool crit() const { return (hitInfo & 0x80) != 0; }
};

struct LootSlot { uint8_t slot; uint32_t itemId, count; };

// ---------------------------------------------------------------- spells (stage 5.8)
// Spell names: optional text file written by w3dconv (one "id<TAB>name<TAB>rank" line per spell);
// without it a few level-1 spells are known by name and the rest show as "Spell <id>".
class SpellNames {
public:
    bool load(const char* path);                  // false if the file is missing
    std::string name(uint32_t id) const;          // "Heroic Strike" or "Spell 78"
    std::string rank(uint32_t id) const;          // "Rank 1" or ""
    bool known(uint32_t id) const;
    size_t size() const { return names_.size(); }
    void add(uint32_t id, const std::string& name, const std::string& rank);
    SpellNames();
private:
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> names_;
};

struct ActionButton { uint32_t action = 0; uint8_t type = 0; };   // type 0 = spell (1.12: 0x40 macro, 0x80 item)

// A buff or debuff on a unit (stage 5.16): UNIT_FIELD_AURA (48 spell ids: slots 0-31 buffs, 32-47 debuffs),
// AURAFLAGS (4 bits per slot, 1 = cancelable), AURAAPPLICATIONS (a byte per slot: stacks - 1); the time left is
// known for our own auras only (SMSG_UPDATE_AURA_DURATION).
struct AuraView {
    uint8_t slot = 0;
    uint32_t spell = 0;
    bool debuff = false, cancelable = false;
    uint8_t stacks = 1;
    uint32_t leftMs = 0;                // 0 = unknown / permanent
};
constexpr int kAuraSlots = 48, kBuffSlots = 32;

struct CastState {
    bool active = false;
    uint32_t spell = 0, startMs = 0, durMs = 0;   // durMs 0 = instant / unknown
    uint64_t target = 0;                           // SpellCastTargets unit / object (0 = itself / the ground)
};

// ---------------------------------------------------------------- items (stage 5.10)
// Static item data from SMSG_ITEM_QUERY_SINGLE_RESPONSE (vMaNGOS WorldSession::HandleItemQuerySingleOpcode).
struct ItemInfo {
    uint32_t entry = 0, cls = 0, subclass = 0, displayId = 0, quality = 0, flags = 0, buyPrice = 0, sellPrice = 0;
    uint32_t inventoryType = 0, itemLevel = 0, requiredLevel = 0, maxStack = 1, containerSlots = 0;
    uint32_t armor = 0, delayMs = 0, bonding = 0, maxDurability = 0;
    float dmgMin = 0, dmgMax = 0;
    uint32_t spellId[5] = {0, 0, 0, 0, 0}, spellTrigger[5] = {0, 0, 0, 0, 0};   // trigger 0 = on use
    std::string name, description;
    int32_t statType[10] = {0}, statValue[10] = {0};   // ItemModType (3 agility, 4 strength, 5 intellect, 6 spirit, 7 stamina)
    int32_t resist[6] = {0};            // holy, fire, nature, frost, shadow, arcane
    uint32_t requiredSkill = 0, requiredSkillRank = 0, maxCount = 0, startQuest = 0, lockId = 0;
    bool openable() const { return (flags & 0x4) != 0 || lockId != 0; }   // ITEM_FLAG_LOOTABLE / locked: "Right click to open"
    bool missing = false;               // the server does not know the entry
    bool usable() const;                // has an on-use spell (food, potions, hearthstone ...)
    bool equippable() const { return inventoryType != 0; }   // bags (18) go into the bag slots
};
const char* itemQualityName(uint32_t q);
const char* itemSubclassName(uint32_t cls, uint32_t subclass);   // "Sword", "Leather", "" ...
// The original's tooltip as text lines (stage 5.15): binding, slot / type, damage and speed, armor, stats,
// resistances, durability ('it' for the current value, may be nullptr), level, Use: / Equip: spells, description,
// sell price. Spell names come from 'spells'.
struct InvItem;
std::vector<std::string> itemTooltip(const ItemInfo& i, const InvItem* it, const class SpellNames& spells);
const char* inventoryTypeName(uint32_t t);   // "Head", "Main Hand", "Bag" ...

// Where an item is, in the addressing the item packets use: bag 255 = the character itself (slots 0..18
// equipped, 19..22 bag slots, 23..38 backpack), bag 19..22 = the bag in that slot (slot = index in the bag).
constexpr uint8_t kBagSelf = 255;
constexpr uint8_t kSlotBagFirst = 19, kSlotBackpackFirst = 23, kSlotBackpackEnd = 39;
struct InvItem {
    uint8_t bag = kBagSelf, slot = 0;
    uint64_t guid = 0;
    uint32_t entry = 0, count = 1, durability = 0, maxDurability = 0;
    bool equipped() const { return bag == kBagSelf && slot < kSlotBagFirst; }
    bool isBagSlot() const { return bag == kBagSelf && slot >= kSlotBagFirst && slot < kSlotBackpackFirst; }
};
const char* equipSlotName(uint8_t slot);     // "Head" .. "Tabard" for slots 0..18

struct LootWindow {
    bool open = false;
    uint64_t guid = 0;
    uint32_t money = 0;
    std::vector<LootSlot> items;        // what is still there (removed slots disappear)
};

// ---------------------------------------------------------------- gossip (stage 5.10)
struct GossipOption { uint32_t index = 0; uint8_t icon = 0; bool coded = false; std::string text; };
struct GossipQuest { uint32_t id = 0, icon = 0, level = 0; std::string title; };
struct GossipMenu {
    bool open = false;
    uint64_t npc = 0;
    uint32_t textId = 0;
    std::string text;                   // from SMSG_NPC_TEXT_UPDATE (asked for automatically)
    std::vector<GossipOption> options;
    std::vector<GossipQuest> quests;    // icon = QuestMark (5 available, 3 incomplete, 4 completable ...)
};

// ---------------------------------------------------------------- vendors (stage 5.13)
// SMSG_LIST_INVENTORY (vMaNGOS WorldSession::SendListInventory): slot numbers count only the visible rows, from 1.
struct VendorItem {
    uint32_t slot = 0, entry = 0, displayId = 0, stock = 0, price = 0, maxDurability = 0, buyCount = 1;
    bool unlimited() const { return stock == 0xFFFFFFFFu; }
};
struct VendorWindow {
    bool open = false;
    uint64_t npc = 0;
    std::vector<VendorItem> items;
    bool noInventory = false;           // the server said "this vendor has nothing" (count 0)
    bool canRepair = false;             // UNIT_NPC_FLAG_REPAIR
};
// A class / profession trainer (SMSG_TRAINER_LIST, vMaNGOS WorldSession::SendTrainerList + SendTrainerSpellHelper).
struct TrainerSpell {
    uint32_t spell = 0;                 // the teaching spell (its name in spells.txt is the spell it teaches)
    uint8_t state = 0;                  // 0 can learn (green), 1 not yet (red), 2 known (grey)
    uint32_t cost = 0, reqLevel = 0, reqSkill = 0, reqSkillValue = 0, reqSpell = 0, reqSpell2 = 0;
};
struct TrainerWindow {
    bool open = false;
    uint64_t npc = 0;
    uint32_t type = 0;                  // 0 class, 2 profession (TrainerType)
    std::vector<TrainerSpell> spells;
    std::string greeting;
};

// One of the 12 buyback slots (PLAYER_FIELD_VENDORBUYBACK_SLOT_1, PLAYER_FIELD_BUYBACK_PRICE_1).
struct BuybackItem { uint32_t slot = 0; uint64_t guid = 0; uint32_t entry = 0, count = 1, price = 0; };

// ---------------------------------------------------------------- quests (stage 5.14)
// Quest giver marks (SMSG_QUESTGIVER_STATUS, DialogStatus of 1.12).
namespace QuestMark {
enum : uint8_t { None = 0, Unavailable = 1, Chat = 2, Incomplete = 3, RewardRep = 4, Available = 5, RewardOld = 6,
                 Reward2 = 7 };
}
const char* questMarkGlyph(uint32_t status);     // "!" "?" or ""; grey (unavailable / incomplete) is up to the caller
bool questMarkGrey(uint32_t status);
struct QuestItemLine { uint32_t item = 0, count = 0, displayId = 0; };
// The quest giver's window: a list of quests, the details of one (accept), what it wants (continue) or the reward.
struct QuestDialog {
    enum class Kind : uint8_t { None, List, Details, RequestItems, OfferReward };
    Kind kind = Kind::None;
    uint64_t npc = 0;
    uint32_t quest = 0;
    std::string title, text, objectives;          // List: text = greeting
    std::vector<GossipQuest> list;
    std::vector<QuestItemLine> choices, rewards, required;
    uint32_t money = 0, requiredMoney = 0, rewardSpell = 0;
    bool completable = false;                     // RequestItems: the server enabled "Continue"
    bool open() const { return kind != Kind::None; }
};
// What SMSG_QUEST_QUERY_RESPONSE says about a quest (for the log).
struct QuestInfo {
    uint32_t id = 0, method = 0, level = 0, zoneOrSort = 0, type = 0, flags = 0, nextInChain = 0, money = 0;
    uint32_t sourceItem = 0, rewardSpell = 0;
    std::string title, objectives, details, endText;
    struct Objective { uint32_t creatureOrGo = 0, count = 0, item = 0, itemCount = 0; std::string text; };
    Objective obj[4];
    uint32_t rewardItem[4] = {0, 0, 0, 0}, rewardCount[4] = {0, 0, 0, 0};
    uint32_t choiceItem[6] = {0, 0, 0, 0, 0, 0}, choiceCount[6] = {0, 0, 0, 0, 0, 0};
};
// A quest log slot (PLAYER_QUEST_LOG_1_1 + 3 * slot: id, 4 x 6-bit kill counters + state byte, timer).
struct QuestLogEntry {
    uint8_t slot = 0;
    uint32_t id = 0;
    uint8_t kills[4] = {0, 0, 0, 0};
    bool complete = false, failed = false;
    uint32_t timer = 0;
};
constexpr int kQuestLogSize = 20;

// ---------------------------------------------------------------- party (stage 5.17)
// SMSG_GROUP_LIST (vMaNGOS WorldPackets::Group::GroupList) lists the others; SMSG_PARTY_MEMBER_STATS(_FULL)
// (WorldSession::BuildPartyMemberStatsPacket) keeps their health / power / level / zone when they are out of range.
struct PartyMember {
    uint64_t guid = 0;
    std::string name;
    uint8_t status = 0;                 // 1 online, 2 PvP, 4 dead, 8 ghost (MEMBER_STATUS_*)
    uint8_t subgroup = 0;
    bool assistant = false;
    bool statsKnown = false;
    uint32_t hp = 0, maxHp = 0, power = 0, maxPower = 0, level = 0, zone = 0;
    uint8_t powerType = 0;
    int16_t x = 0, y = 0;
    bool online() const { return status & 1; }
};
struct Party {
    bool inGroup = false, raid = false;
    uint64_t leader = 0;
    std::vector<PartyMember> members;   // without us
};

// ---------------------------------------------------------------- friends, ignore, guild (UI stage 6)
// SMSG_FRIEND_LIST / SMSG_IGNORE_LIST at login and on request, SMSG_FRIEND_STATUS for every change
// (vMaNGOS PlayerSocial / SocialMgr); the guild roster from SMSG_GUILD_ROSTER (WorldPackets::Guild::GuildRoster).
struct FriendEntry {
    uint64_t guid = 0;
    uint8_t status = 0;                 // 0 offline, 1 online, 2 AFK, 4 DND
    uint32_t area = 0, level = 0, cls = 0;
    bool online() const { return status != 0; }
};
struct GuildMember {
    uint64_t guid = 0;
    uint8_t online = 0;                 // presence flags: 0 offline
    std::string name, note;
    uint32_t rank = 0, zone = 0;
    uint8_t level = 0, cls = 0;
    float offlineDays = 0;
};
struct GuildState {
    uint32_t id = 0;                    // PLAYER_GUILDID (0 = none)
    std::string name, motd, info;
    std::vector<std::string> ranks;
    std::vector<GuildMember> members;   // online first, then by name
    bool rosterSeen = false;
    uint32_t rosterMs = 0;
};

// ---------------------------------------------------------------- bank, flights, mail, auctions, trade (UI stage 6)
// Bank slots of the character (bag 255): 39..62 items, 63..68 bank bags (their contents use the slot as bag).
constexpr uint8_t kSlotBankFirst = 39, kSlotBankEnd = 63, kSlotBankBagFirst = 63, kSlotBankBagEnd = 69;
struct BankWindow { bool open = false; uint64_t npc = 0; };
struct TaxiWindow {
    bool open = false;
    uint64_t npc = 0;
    uint32_t current = 0;               // the node we are at
    uint32_t mask[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // known nodes (bit = node id)
    bool known(uint32_t node) const { return node < 256 && (mask[node / 32] >> (node % 32)) & 1; }
};
// SMSG_MAIL_LIST_RESULT (vMaNGOS WorldSession::HandleGetMailList, 1.12: one item per letter).
struct MailEntry {
    uint32_t id = 0;
    uint8_t type = 0;                   // 0 player, 2 auction house, 3 creature, 4 game object
    uint64_t sender = 0;                // type 0
    uint32_t senderEntry = 0;           // other types: creature / object entry or auction house id
    std::string subject;
    uint32_t textId = 0, stationery = 0;
    uint32_t item = 0, itemCount = 0, money = 0, cod = 0, flags = 0;
    float expireDays = 0;
    bool read() const { return (flags & 1) != 0; }
};
struct MailWindow {
    bool open = false;
    uint64_t box = 0;                   // the mailbox object
    bool listed = false;
    std::vector<MailEntry> mails;
};
// SMSG_AUCTION_LIST_RESULT / OWNER_LIST_RESULT entries (AuctionEntry::BuildAuctionInfo).
struct AuctionItem {
    uint32_t id = 0, item = 0, enchant = 0, randomProp = 0, suffix = 0, count = 1, charges = 0;
    uint64_t owner = 0, bidder = 0;
    uint32_t startBid = 0, minOutbid = 0, buyout = 0, timeLeftMs = 0, bid = 0;
    uint32_t nextBid() const { return bid ? bid + minOutbid : startBid; }
};
struct AuctionWindow {
    bool open = false;
    uint64_t npc = 0;
    uint32_t house = 0;
    std::vector<AuctionItem> results, owned;
    uint32_t total = 0, ownedTotal = 0, page = 0;
    std::string query;
    bool waiting = false;               // a list request is out
    uint32_t lastAction = 0, lastError = 0xFFFFFFFFu;   // SMSG_AUCTION_COMMAND_RESULT
};
// Trade with a player (SMSG_TRADE_STATUS / _EXTENDED, vMaNGOS TradeHandler).
struct TradeSlotView { uint32_t entry = 0, display = 0, count = 0; };
struct TradeOffer { uint32_t gold = 0; TradeSlotView slots[7]; };
struct TradeState {
    enum class Phase : uint8_t { None, Asked, Proposed, Open };   // Asked: they want to trade with us
    Phase phase = Phase::None;
    uint64_t with = 0;
    TradeOffer mine, theirs;
    bool weAccepted = false, theyAccepted = false;
    InvItem myItems[7];                 // what we put into each slot (guid 0 = empty)
    bool open() const { return phase == Phase::Open; }
};

// ---------------------------------------------------------------- death (stage 5.10)
enum class Life : uint8_t { Alive, Dead, Ghost };
struct CorpseInfo {
    bool known = false;
    uint32_t map = 0;
    float x = 0, y = 0, z = 0;
    uint64_t guid = 0;                  // the corpse object when it is in range
};
struct ResurrectOffer { bool active = false; uint64_t from = 0; std::string name; bool sickness = false; };

constexpr uint32_t kSpellAttack = 6603;           // "Attack": toggles auto-attack, never sent as a cast
constexpr uint32_t kGlobalCooldownMs = 1500;

// Client texts of SpellCastResult / InventoryResult codes (result_text.cpp, generated from vMaNGOS).
std::string castResultText(uint32_t code);
std::string inventoryResultText(uint32_t code);
std::string moneyText(uint32_t copper);      // "1g 2s 3c"

class GameSession {
public:
    // Hooks WorldClient::onPacket. Position/orientation from SMSG_LOGIN_VERIFY_WORLD.
    void attach(WorldClient& wc, uint32_t nowMs);
    void detach();
    bool attached() const { return wc_ != nullptr; }
    // Sends queued movement packets and object queries.
    void update(uint32_t nowMs);

    // The server moved us (teleport ACK or new world): take the new position once.
    bool takeTeleport(float pos[3], float* orient);
    // SMSG_TRANSFER_PENDING: the server is about to move us to another map (portal, dungeon door, ship to another
    // continent). True once per notice; the new position follows with SMSG_NEW_WORLD (takeTeleport).
    bool takeTransferPending(uint32_t* map);
    uint32_t mapId() const { return map_; }
    uint64_t selfGuid() const { return self_; }
    std::string selfName;               // from the character list (NPC texts say $N)
    // Our own player object: by GUID once attached, else the object the server flagged as SELF (replays).
    const WorldObject* selfObject() const;

    ObjectTable objects;
    Mover mover;

    struct Stats {
        uint32_t teleports = 0, newWorlds = 0, speedChanges = 0, roots = 0, queries = 0, packetsOut = 0, flagAcks = 0;
    } stats;

    // Packet tap (both directions) for recording sessions: dir 0 = server->client, 1 = client->server.
    std::function<void(uint32_t ms, int dir, uint16_t opcode, const Bytes& data)> tap;

    // Feed a server packet directly (replay / tests); attach() routes live packets here.
    bool handle(uint16_t opcode, const Bytes& data, uint32_t nowMs);

    // ---- chat (stage 5.6)
    std::deque<ChatLine> chat;          // newest last, at most kChatKeep
    uint32_t chatCount = 0;             // lines ever added (UI: "new lines since")
    static constexpr size_t kChatKeep = 64;
    // Sends CMSG_MESSAGECHAT in the player's racial language. type Whisper needs 'to', Channel the channel name.
    void sendChat(uint8_t type, const std::string& text, const std::string& to, uint32_t nowMs);
    // A typed line: "/s text", "/y", "/p", "/g", "/w Name text", "/e text", "/1 text" (channel 1 = first joined),
    // anything else = say. Returns false (and adds a local line) if it cannot be sent.
    bool sendTyped(const std::string& line, uint32_t nowMs);
    std::string lineText(const ChatLine& l) const;   // "[Name] says: text" etc.
    void addLocal(const std::string& text, uint32_t nowMs);

    // ---- target and melee (stage 5.7)
    uint64_t target() const { return target_; }
    void setTarget(uint64_t guid, uint32_t nowMs);     // CMSG_SET_SELECTION
    // Next unit to target from (x, y, z) looking along yaw, within 'range': creatures without NPC flags first, then
    // NPCs, players, the dead; ahead before behind, then by angle and distance; after 'current' when cycling.
    uint64_t pickTarget(const float pos[3], float yaw, float range, uint64_t current, uint32_t nowMs) const;
    // Soft targets for the controller scheme (stage 5.11, like WoW: Forever): without factions yet a living
    // creature without NPC flags counts as an enemy. softEnemy: the best one inside the cone 'halfAngle' either side
    // of yaw (mostly ahead, then near). softInteract: what the interact button would use right now - a lootable
    // corpse (5 yd) or an NPC with functions (5.5 yd; a ghost only sees spirit healers); verb "Loot" / "Talk".
    bool enemyLike(const WorldObject& o) const;
    uint64_t softEnemy(const float pos[3], float yaw, float range, float halfAngle, uint32_t nowMs) const;
    uint64_t softInteract(const float pos[3], float yaw, uint32_t nowMs, const char** verb = nullptr) const;
    bool inCombat() const;                             // UNIT_FLAG_IN_COMBAT on us, or auto-attack on
    uint64_t targetOf(uint64_t guid) const;            // UNIT_TARGET of a unit (assist)
    void startAttack(uint32_t nowMs);                  // CMSG_ATTACKSWING at the target
    void stopAttack(uint32_t nowMs);                   // CMSG_ATTACKSTOP
    bool attackWanted() const { return attackWanted_; }   // we asked to auto-attack
    bool attacking() const { return attackOn_; }          // the server confirmed (SMSG_ATTACKSTART)
    uint16_t lastAttackError() const { return attackError_; }   // SMSG_ATTACKSWING_* opcode, 0 = none
    uint32_t lastAttackErrorMs() const { return attackErrorMs_; }
    // The server said we are not facing the target: the yaw to turn to (once per complaint).
    bool takeFacingRequest(float* yaw);
    std::deque<MeleeHit> hits;          // newest last, at most 32
    uint32_t xpGained = 0, kills = 0;

    // ---- loot (stage 5.7, window stage 5.10). With autoLoot everything is taken and the window released
    // at once (the stage-5.7 behaviour, used by w3dlogin --fight); otherwise lootWindow stays open until
    // takeLoot / takeAllLoot / closeLoot.
    void loot(uint64_t corpse, uint32_t nowMs);        // CMSG_LOOT
    uint64_t lootOpen() const { return lootGuid_; }
    bool autoLoot = false;
    LootWindow lootWindow;
    void takeLoot(size_t index, uint32_t nowMs);       // index into lootWindow.items (CMSG_AUTOSTORE_LOOT_ITEM)
    void takeLootMoney(uint32_t nowMs);                // CMSG_LOOT_MONEY
    void takeAllLoot(uint32_t nowMs);                  // money + every item, then release
    void closeLoot(uint32_t nowMs);                    // CMSG_LOOT_RELEASE
    uint32_t lootedCopper = 0, lootedItems = 0;
    std::vector<LootSlot> lastLoot;

    // ---- items (stage 5.10)
    // Static data of an item entry; nullptr until the answer arrives (the query is sent on the next update()).
    const ItemInfo* itemInfo(uint32_t entry);
    const ItemInfo* itemInfoIfKnown(uint32_t entry) const;
    std::string itemName(uint32_t entry);              // "Worn Shortsword", or "Item 25" while unknown
    // Everything the character carries: equipped (bag 255, slots 0..18), bags (19..22), backpack (23..38) and the
    // contents of each bag, in that order. From PLAYER_FIELD_INV_SLOT_HEAD and the item/container objects.
    std::vector<InvItem> inventory() const;
    uint32_t money() const;                            // PLAYER_FIELD_COINAGE (copper)
    uint32_t freeBagSlots() const;
    void useItem(const InvItem& it, uint32_t nowMs);   // CMSG_USE_ITEM on self / the target
    void openItem(const InvItem& it, uint32_t nowMs);  // CMSG_OPEN_ITEM (clams, lockboxes, satchels: a loot window)
    void equipItem(const InvItem& it, uint32_t nowMs); // CMSG_AUTOEQUIP_ITEM
    void unequipItem(const InvItem& it, uint32_t nowMs);   // CMSG_AUTOSTORE_BAG_ITEM into the bags
    void destroyItem(const InvItem& it, uint32_t nowMs);   // CMSG_DESTROYITEM (whole stack)
    uint32_t itemQueries = 0, itemAnswers = 0;
    uint32_t lastInventoryError = 0;                   // InventoryResult of the last SMSG_INVENTORY_CHANGE_FAILURE

    // ---- interaction (stage 5.10): what the A key does with the target. Loots a lootable corpse, talks to an
    // NPC (gossip; the spirit healer when we are a ghost), or reclaims our corpse when it is near.
    // Returns a short reason when nothing could be done ("Too far away" ...), empty on success.
    std::string interact(uint64_t guid, const float pos[3], uint32_t nowMs);
    // What the interact button does with an object, for the prompt ("Loot", "Talk", "Quest", "Buy", "Train", "Open",
    // "Use"), or nullptr when nothing (stage 5.18: game objects - quest chests and quest givers, chests, goobers).
    const char* useVerb(const WorldObject& o) const;
    void useGameObject(uint64_t guid, uint32_t nowMs);   // CMSG_GAMEOBJ_USE
    // How a game object is used (chests, herbs and ore are opened by casting the OPEN_LOCK spell that matches their
    // lock, locks.txt): the prompt verb, the spell (0 = CMSG_GAMEOBJ_USE) and, when we cannot, why not.
    struct ObjectAction { const char* verb = nullptr; uint32_t spell = 0; std::string why; };
    ObjectAction objectAction(const WorldObject& o) const;
    LockTable locks;                                     // locks.txt
    EmoteTable emotes;                                   // emotes.txt (Emotes.dbc: emote -> animation)
    // Emotes and sitting (the animation stage): /dance /wave ... and /sit /stand /sleep
    static uint32_t textEmoteId(const std::string& cmd);
    void textEmote(uint32_t textEmoteId, uint32_t nowMs);
    void setStandState(uint8_t state, uint32_t nowMs);
    // Sound events for the client (audio.cpp names: "coin", "questaccept", "questcomplete", "whisper", "invite",
    // "mail"); the touch-screen UI drains them every frame.
    std::vector<const char*> sounds;
    void sound(const char* e) { if (sounds.size() < 16) sounds.push_back(e); }
    // Sounds in the world (proto3ds/source/sound_world.cpp turns them into positioned sounds): melee swings,
    // casts starting / going off (with up to 4 hit targets) / failing.
    struct WorldSound {
        enum Kind : uint8_t { Melee, SpellStart, SpellGo, SpellFail };
        Kind kind = Melee;
        uint64_t src = 0, dst = 0;
        uint32_t spell = 0, hitInfo = 0, victimState = 0, castMs = 0;
        uint64_t targets[4] = {0, 0, 0, 0};
        uint8_t nTargets = 0;
    };
    std::vector<WorldSound> worldSounds;
    void worldSound(const WorldSound& w) { if (worldSounds.size() < 48) worldSounds.push_back(w); }

    // ---- gossip (stage 5.10)
    GossipMenu gossip;
    void talkTo(uint64_t npc, uint32_t nowMs);         // CMSG_GOSSIP_HELLO
    void selectGossip(size_t optionIndex, uint32_t nowMs);   // CMSG_GOSSIP_SELECT_OPTION
    void selectGossipQuest(size_t questIndex, uint32_t nowMs);   // a quest of the menu (stage 5.14)
    void closeGossip() { gossip = GossipMenu(); }      // 1.12 has no close packet: the window is client-side
    uint32_t gossipMenus = 0;

    // ---- death and resurrection (stage 5.10)
    Life life() const;                                  // from our health and PLAYER_FLAGS_GHOST
    void releaseSpirit(uint32_t nowMs);                // CMSG_REPOP_REQUEST
    CorpseInfo corpse;                                  // where our body lies (MSG_CORPSE_QUERY / the corpse object)
    void queryCorpse(uint32_t nowMs);                  // MSG_CORPSE_QUERY
    uint32_t reclaimDelayLeftMs(uint32_t nowMs) const { return nowMs < reclaimAt_ ? reclaimAt_ - nowMs : 0; }
    float corpseDistance(const float pos[3]) const;    // yards (2D), -1 when unknown or on another map
    bool canReclaim(const float pos[3], uint32_t nowMs) const;   // ghost, within 39 yd, delay over
    void reclaimCorpse(uint32_t nowMs);                // CMSG_RECLAIM_CORPSE
    ResurrectOffer resurrectOffer;                     // SMSG_RESURRECT_REQUEST (a player's spell)
    void answerResurrect(bool accept, uint32_t nowMs); // CMSG_RESURRECT_RESPONSE
    uint64_t spiritHealerConfirm = 0;                  // SMSG_SPIRIT_HEALER_CONFIRM: the healer waiting for a yes
    void acceptSpiritHealer(uint32_t nowMs);           // CMSG_SPIRIT_HEALER_ACTIVATE
    uint32_t deaths = 0, resurrections = 0;

    // ---- spells and the action bar (stage 5.8)
    SpellNames spellNames;
    std::vector<uint32_t> spells;                     // spell book, server order (passives included)
    std::array<ActionButton, 120> actionBar{};        // SMSG_ACTION_BUTTONS (bar 1 = slots 0..11)
    bool actionBarSeen = false;
    CastState cast;                                   // our own cast in progress
    CastState castOf(uint64_t guid, uint32_t nowMs) const;   // anyone's cast with a cast time (UI stage 2: the target's bar)
    uint32_t spellCount = 0;                          // casts that went off (SPELL_GO of ours)
    uint32_t lastCastFail = 0xFFFFFFFFu;              // SpellCastResult of the last SMSG_CAST_FAILED / SPELL_FAILURE
    uint32_t lastCastFailMs = 0;
    bool knowsSpell(uint32_t id) const;
    // Spells to show on the touch bar: the server's bar slots that hold a spell, in slot order (duplicates
    // dropped); without any server bar, the spells of the book that have a real name.
    std::vector<uint32_t> barSpells() const;
    uint32_t cooldownLeftMs(uint32_t spell, uint32_t nowMs) const;   // own cooldown (spell, 0 = ready)
    uint32_t gcdLeftMs(uint32_t nowMs) const { return nowMs < gcdEnd_ ? gcdEnd_ - nowMs : 0; }
    // Casts at the current target (or self when there is none). "Attack" toggles auto-attack instead.
    // Returns false and adds a local line when it cannot be sent (unknown spell, cooldown, busy casting).
    bool castSpell(uint32_t spell, uint32_t nowMs);
    void cancelCast(uint32_t nowMs);                  // CMSG_CANCEL_CAST
    // Own power for the bar: type from UNIT_BYTES_0 byte 3 (0 mana, 1 rage, 2 focus, 3 energy); rage is x10 on the wire.
    struct Power { uint8_t type; uint32_t cur, max; };
    Power selfPower() const;
    // ---- auras (stage 5.16)
    std::vector<AuraView> auras(const WorldObject& o, uint32_t nowMs) const;   // buffs first, then debuffs
    bool cancelAura(uint32_t spell, uint32_t nowMs);   // CMSG_CANCEL_AURA (our own buffs that can be cancelled)
    uint64_t targetOfTarget() const { return targetOf(target_); }

    // ---- vendors (stage 5.13)
    VendorWindow vendor;
    void openVendor(uint64_t npc, uint32_t nowMs);     // CMSG_LIST_INVENTORY
    // Buys 'count' times the vendor's bundle (buyCount items each). False (and a line why) when it is known
    // to fail: not enough money, sold out.
    bool buyItem(size_t index, uint32_t count, uint32_t nowMs);   // CMSG_BUY_ITEM
    bool sellItem(const InvItem& it, uint32_t nowMs);            // CMSG_SELL_ITEM (the whole stack)
    std::vector<BuybackItem> buybackItems() const;
    bool buybackItem(const BuybackItem& b, uint32_t nowMs);      // CMSG_BUYBACK_ITEM
    void repairAll(uint32_t nowMs);                               // CMSG_REPAIR_ITEM with item 0
    void closeVendor() { vendor = VendorWindow(); }
    uint32_t sellValue(const InvItem& it) const;       // what the vendor pays (0 = cannot be sold / unknown)
    uint32_t bought = 0, sold = 0, buybacks = 0, repairs = 0;
    uint8_t lastBuyError = 0xFF, lastSellError = 0xFF;
    // trainers (stage 5.13b)
    TrainerWindow trainer;
    void openTrainer(uint64_t npc, uint32_t nowMs);    // CMSG_TRAINER_LIST
    bool trainSpell(size_t index, uint32_t nowMs);     // CMSG_TRAINER_BUY_SPELL (false + a line when it cannot be learned)
    void closeTrainer() { trainer = TrainerWindow(); }
    uint32_t trained = 0;
    // innkeepers: "Make this inn your home" -> SMSG_BINDER_CONFIRM -> CMSG_BINDER_ACTIVATE
    uint64_t binderConfirm = 0;
    void acceptBinder(uint32_t nowMs);

    // ---- quests (stage 5.14)
    QuestDialog questDialog;
    uint32_t questGiverStatus(uint64_t guid) const;    // QuestMark (0 = none / not asked yet)
    void questHello(uint64_t npc, uint32_t nowMs);      // CMSG_QUESTGIVER_HELLO (quest givers without gossip)
    // A quest from a list (gossip or quest giver): active ones (in the log) ask for completion, others for details.
    void chooseQuest(uint64_t npc, const GossipQuest& q, uint32_t nowMs);
    void acceptQuest(uint32_t nowMs);                   // CMSG_QUESTGIVER_ACCEPT_QUEST (Details)
    void continueQuest(uint32_t nowMs);                 // CMSG_QUESTGIVER_REQUEST_REWARD (RequestItems)
    void completeQuest(size_t choice, uint32_t nowMs);  // CMSG_QUESTGIVER_CHOOSE_REWARD (OfferReward)
    void closeQuestDialog(uint32_t nowMs);              // CMSG_QUESTGIVER_CANCEL
    std::vector<QuestLogEntry> questLog() const;
    const QuestInfo* questInfo(uint32_t id);            // nullptr until SMSG_QUEST_QUERY_RESPONSE (asked on update())
    const QuestInfo* questInfoIfKnown(uint32_t id) const;
    std::string questTitle(uint32_t id);                // "A Threat Within" or "Quest 783"
    // One line per objective: "Kobold Vermin slain: 3/10", "Linen Cloth: 2/6"; "(complete)" when done.
    std::vector<std::string> questProgress(const QuestLogEntry& e);
    std::vector<std::string> questProgressKnown(const QuestLogEntry& e) const;   // without asking (the HUD tracker)
    std::string questTitleKnown(uint32_t id) const;
    void abandonQuest(uint8_t slot, uint32_t nowMs);    // CMSG_QUESTLOG_REMOVE_QUEST
    uint32_t questsAccepted = 0, questsCompleted = 0, questQueries = 0, questUpdates = 0;
    uint32_t lastQuestInvalid = 0xFFFFFFFFu;

    // ---- party (stage 5.17)
    Party party;
    std::string groupInviteFrom;                       // SMSG_GROUP_INVITE: who invites us (empty = no invitation)
    void inviteToGroup(const std::string& name, uint32_t nowMs);   // CMSG_GROUP_INVITE
    void answerGroupInvite(bool accept, uint32_t nowMs);           // CMSG_GROUP_ACCEPT / _DECLINE
    void leaveGroup(uint32_t nowMs);                   // CMSG_GROUP_DISBAND
    void uninvite(const std::string& name, uint32_t nowMs);        // CMSG_GROUP_UNINVITE
    void promoteLeader(uint64_t guid, uint32_t nowMs); // CMSG_GROUP_SET_LEADER
    bool isLeader() const { return party.inGroup && party.leader == self_; }
    bool inParty(uint64_t guid) const;
    // A member's health etc.: from its object when it is in range, else from the party stats.
    PartyMember memberView(const PartyMember& m) const;
    uint32_t partyLists = 0, partyStats = 0;

    // ---- factions and reputation (stage 5.12)
    FactionTable factions;                            // factions.txt (empty: guesses - NPCs with functions friendly)
    ReputationState rep;                              // our reputation list from the server
    Rank reactionTo(const WorldObject& o) const;      // how we see the unit (WorldObject::GetReactionTo, us -> it)
    Rank reactionOf(const WorldObject& o) const;      // how it sees us (GetFactionReactionTo, it -> us)
    Rank shownReaction(const WorldObject& o) const;   // name colour: the worse of the two
    bool canAttack(const WorldObject& o) const;       // IsValidAttackTarget for us (players: PvP flag needed)
    Rank standing(const FactionInfo& f, int32_t* value = nullptr) const;   // our rank (and standing) with a faction
    bool atWarWith(const FactionInfo& f) const;
    struct RepLine { uint32_t faction = 0; std::string name; Rank rank = Rank::Neutral; int32_t value = 0; bool atWar = false; };
    std::vector<RepLine> reputations() const;         // the visible part of the reputation list, by name
    uint32_t repChanges = 0;

    // ---- friends, ignore, guild (UI stage 6)
    std::vector<FriendEntry> friends;
    std::vector<uint64_t> ignored;
    bool friendsSeen = false;
    void requestFriendList(uint32_t nowMs);              // CMSG_FRIEND_LIST
    void addFriend(const std::string& name, uint32_t nowMs);
    void removeFriend(uint64_t guid, uint32_t nowMs);
    void addIgnore(const std::string& name, uint32_t nowMs);
    void removeIgnore(uint64_t guid, uint32_t nowMs);
    GuildState guild;
    uint32_t guildId() const;                            // PLAYER_GUILDID
    void requestGuildRoster(uint32_t nowMs);             // CMSG_GUILD_ROSTER (+ CMSG_GUILD_QUERY for the name)
    void guildInvite(const std::string& name, uint32_t nowMs);
    std::string guildInviteFrom, guildInviteName;        // SMSG_GUILD_INVITE (empty = none)
    void answerGuildInvite(bool accept, uint32_t nowMs); // CMSG_GUILD_ACCEPT / _DECLINE
    void leaveGuild(uint32_t nowMs);                     // CMSG_GUILD_LEAVE
    // Player name from the cache, asking the server when unknown (friends, mail senders, guild invites).
    std::string playerName(uint64_t guid, uint32_t nowMs);

    // ---- bank (UI stage 6)
    BankWindow bank;
    void openBank(uint64_t npc, uint32_t nowMs);         // CMSG_BANKER_ACTIVATE -> SMSG_SHOW_BANK
    std::vector<InvItem> bankItems() const;              // bank slots and the contents of bank bags
    int bankBagSlots() const;                            // bank bag slots bought (PLAYER_BYTES_2 byte 2)
    void depositItem(const InvItem& it, uint32_t nowMs); // CMSG_AUTOBANK_ITEM: from the bags into the bank
    void withdrawItem(const InvItem& it, uint32_t nowMs);   // CMSG_AUTOSTORE_BANK_ITEM: from the bank into the bags
    void buyBankSlot(uint32_t nowMs);                    // CMSG_BUY_BANK_SLOT
    void closeBank() { bank = BankWindow(); }
    // ---- flight masters (UI stage 6)
    TaxiWindow taxi;
    TaxiTable taxiTable;                                 // taxi.txt (names, positions, paths)
    void openTaxi(uint64_t npc, uint32_t nowMs);         // CMSG_TAXIQUERYAVAILABLENODES -> SMSG_SHOWTAXINODES
    // Flies to 'node': a direct path with CMSG_ACTIVATETAXI, longer routes with CMSG_ACTIVATETAXIEXPRESS.
    // False (and a line) when no route over known nodes / not enough money.
    bool flyTo(uint32_t node, uint32_t nowMs);
    uint32_t taxiCost(uint32_t node) const;              // route cost from the current node (0 = unknown)
    void closeTaxi() { taxi = TaxiWindow(); }
    uint32_t flights = 0;
    // ---- talents (UI stage 4)
    TalentTable talents;                                 // talents.txt
    uint32_t talentPoints() const;                       // PLAYER_CHARACTER_POINTS1
    int talentRank(const Talent& t) const;               // ranks learned (from the spell book)
    // CMSG_LEARN_TALENT for the next rank; false + a line when it cannot be learned (points, tier, prerequisite).
    bool learnTalent(uint32_t talentId, uint32_t nowMs);
    uint32_t classId() const;                            // UNIT_BYTES_0 byte 1
    // ---- mail (UI stage 6)
    MailWindow mail;
    void openMailbox(uint64_t box, uint32_t nowMs);      // CMSG_GET_MAIL_LIST
    const std::string* mailText(uint32_t textId, uint32_t mailId, uint32_t nowMs);   // CMSG_ITEM_TEXT_QUERY (cached)
    void takeMailMoney(uint32_t mailId, uint32_t nowMs);
    void takeMailItem(uint32_t mailId, uint32_t nowMs);
    void deleteMail(uint32_t mailId, uint32_t nowMs);
    void markMailRead(uint32_t mailId, uint32_t nowMs);
    // CMSG_SEND_MAIL (postage 30 copper); itemGuid 0 = none. False + a line when it cannot be sent.
    bool sendMail(const std::string& to, const std::string& subject, const std::string& body, uint64_t itemGuid,
                  uint32_t money, uint32_t nowMs);
    void closeMail() { mail = MailWindow(); }
    bool newMail = false;                                // SMSG_RECEIVED_MAIL since the last list
    uint32_t mailsSent = 0;
    // ---- auction house (UI stage 6)
    AuctionWindow auction;
    void openAuction(uint64_t npc, uint32_t nowMs);      // MSG_AUCTION_HELLO
    void searchAuctions(const std::string& name, uint32_t page, uint32_t nowMs);   // CMSG_AUCTION_LIST_ITEMS (50 a page)
    void listOwnAuctions(uint32_t nowMs);                // CMSG_AUCTION_LIST_OWNER_ITEMS
    bool bidAuction(const AuctionItem& a, uint32_t price, uint32_t nowMs);   // CMSG_AUCTION_PLACE_BID (buyout = price)
    bool sellAuction(const InvItem& it, uint32_t bid, uint32_t buyout, uint32_t minutes, uint32_t nowMs);
    void cancelAuction(uint32_t auctionId, uint32_t nowMs);   // CMSG_AUCTION_REMOVE_ITEM
    void closeAuction() { auction = AuctionWindow(); }
    // ---- trade (UI stage 6)
    TradeState trade;
    void proposeTrade(uint64_t player, uint32_t nowMs);  // CMSG_INITIATE_TRADE
    void answerTrade(bool accept, uint32_t nowMs);       // CMSG_BEGIN_TRADE / CMSG_CANCEL_TRADE
    bool offerTradeItem(int tradeSlot, const InvItem& it, uint32_t nowMs);   // CMSG_SET_TRADE_ITEM
    void clearTradeItem(int tradeSlot, uint32_t nowMs);  // CMSG_CLEAR_TRADE_ITEM
    void offerTradeGold(uint32_t copper, uint32_t nowMs);   // CMSG_SET_TRADE_GOLD
    void acceptTrade(uint32_t nowMs);                    // CMSG_ACCEPT_TRADE
    void cancelTrade(uint32_t nowMs);                    // CMSG_CANCEL_TRADE
    uint32_t tradesDone = 0;

private:
    bool handleFriends(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    bool handleServices(uint16_t opcode, const Bytes& data, uint32_t nowMs);   // bank, taxi
    bool handleMail(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    bool handleAuction(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    bool handleTrade(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    std::unordered_map<uint32_t, std::string> mailTexts_;
    std::unordered_set<uint32_t> mailTextAsked_;
    void updateServices(uint32_t nowMs);                 // windows of NPCs / objects we walked away from close
    bool handleFactions(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    bool handleVendor(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    bool handleGroup(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    bool handleQuests(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    void updateQuests(uint32_t nowMs);                // status queries, quest queries, closing windows far away
    std::unordered_map<uint64_t, uint32_t> questStatus_;
    std::unordered_map<uint64_t, uint32_t> statusAskedMs_;
    std::unordered_map<uint32_t, QuestInfo> quests_;
    std::unordered_map<uint32_t, std::string> questTitles_;   // from the quest givers' windows
    std::unordered_set<uint32_t> questAsked_;
    std::unordered_set<uint32_t> questWanted_, questLogIds_;
    bool questLogSeen_ = false;
    uint32_t questLogHash_ = 0, questLevel_ = 0, statusRefreshMs_ = 0;
    // SMSG_QUESTUPDATE_ADD_ITEM comes before the item itself: the "x/y" line waits until the bags show it
    struct PendingItemLine { uint32_t item = 0, ms = 0, before = 0, need = 0; };
    std::vector<PendingItemLine> pendingItemLines_;
    void updateVendor(uint32_t nowMs);
    struct PendingSell { uint64_t guid = 0; uint32_t entry = 0, count = 0, value = 0, ms = 0; };
    std::vector<PendingSell> pendingSells_;
    struct PendingRepair { uint32_t moneyBefore = 0, ms = 0; } pendingRepair_;
    bool bindPending_ = false;
    bool handleSpells(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    bool handleItems(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    bool handleLife(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    void updateLife(uint32_t nowMs);                   // life-state transitions (died, released, alive again)
    void sendItemQueries(uint32_t nowMs);
    void ackFlagChange(uint16_t ackOp, uint32_t counter, uint32_t flag, bool on, uint32_t nowMs);
    std::unordered_map<uint32_t, ItemInfo> items_;
    std::unordered_set<uint32_t> itemAsked_, itemWanted_;
    uint32_t reclaimAt_ = 0, corpseAskedMs_ = 0;
    Life lastLife_ = Life::Alive;
    bool lifeSeen_ = false;
    uint32_t invScanMs_ = 0;
    std::unordered_map<uint32_t, uint32_t> cooldownEnd_;   // spell -> local time
    std::unordered_map<uint64_t, CastState> unitCasts_;    // other units' casts (SMSG_SPELL_START with a cast time)
    struct AuraTime { uint32_t spell = 0, endMs = 0; };
    std::array<AuraTime, kAuraSlots> auraTime_{};          // our auras: SMSG_UPDATE_AURA_DURATION
    uint32_t gcdEnd_ = 0;
    void send(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    void sendActiveMover(uint32_t nowMs);   // CMSG_SET_ACTIVE_MOVER (self)
    void ackForced(uint16_t ackOp, uint64_t guid, uint32_t counter, uint32_t nowMs, const float* speed);
    void sendQueries(uint32_t nowMs);
    bool handleSocial(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    void addLine(ChatLine l);
    uint32_t chatLanguage() const;

    uint64_t target_ = 0;
    bool attackWanted_ = false, attackOn_ = false, faceRequest_ = false;
    uint16_t attackError_ = 0;
    uint32_t attackErrorMs_ = 0;
    uint64_t lootGuid_ = 0;
    std::vector<std::string> channels_;   // joined channels (SMSG_CHANNEL_NOTIFY "you joined")

    WorldClient* wc_ = nullptr;
    uint64_t self_ = 0;
    uint32_t map_ = 0;
    uint32_t now_ = 0;
    bool teleported_ = false;
    bool transferPending_ = false;
    uint32_t transferMap_ = 0;
    std::unordered_set<uint64_t> askedNames_;
    std::unordered_set<uint32_t> askedCreatures_, askedGos_;
};

// Packet log: "W3DL" then records {u32 ms, u8 dir, u16 opcode, u32 len, data}.
class PacketLog {
public:
    bool open(const char* path);
    void write(uint32_t ms, int dir, uint16_t op, const Bytes& data);
    void close();
    ~PacketLog() { close(); }
private:
    FILE* f_ = nullptr;
};
struct LoggedPacket {
    uint32_t ms;
    int dir;
    uint16_t opcode;
    Bytes data;
};
bool readPacketLog(const char* path, std::vector<LoggedPacket>& out);

}  // namespace w3n
