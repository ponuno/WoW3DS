// game_items.cpp — items of GameSession (stage 5.10): item queries, the character's inventory from its
// update fields, the loot window, use / equip / unequip / destroy, item push results and inventory errors.
//
// Layouts from vMaNGOS (1.12.1): WorldSession::HandleItemQuerySingleOpcode (static item data, 10 stats,
// 5 damages, 5 spells), WorldPackets::Item (AutoEquipItem, AutoStoreBagItem, DestroyItem,
// InventoryChangeFailure, ItemPushResult), WorldPackets::Spell::UseItem, WorldPackets::Loot (LootResponse,
// LootRemoved, LootMoneyNotify). Field indices from UpdateFields_1_12_1.h (see objects.hpp).
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "game.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_ITEM_QUERY_SINGLE = 0x056, SMSG_ITEM_QUERY_SINGLE_RESPONSE = 0x058, CMSG_USE_ITEM = 0x0AB, CMSG_OPEN_ITEM = 0x0AC,
    CMSG_AUTOSTORE_LOOT_ITEM = 0x108, CMSG_AUTOEQUIP_ITEM = 0x10A, CMSG_AUTOSTORE_BAG_ITEM = 0x10B,
    CMSG_DESTROYITEM = 0x111, SMSG_INVENTORY_CHANGE_FAILURE = 0x112,
    CMSG_LOOT = 0x15D, CMSG_LOOT_MONEY = 0x15E, CMSG_LOOT_RELEASE = 0x15F, SMSG_LOOT_RESPONSE = 0x160,
    SMSG_LOOT_RELEASE_RESPONSE = 0x161, SMSG_LOOT_REMOVED = 0x162, SMSG_LOOT_MONEY_NOTIFY = 0x163,
    SMSG_LOOT_CLEAR_MONEY = 0x165, SMSG_ITEM_PUSH_RESULT = 0x166,
};
constexpr uint32_t kQueryBatch = 8;   // item queries per update(): the answers are ~250 bytes each

const char* lootErrorText(uint8_t e) {
    switch (e) {
        case 0: return "You don't have permission to loot that corpse.";
        case 4: return "You are too far away to loot that corpse.";
        case 5: return "You must be facing the corpse to loot it.";
        case 6: return "Someone is already looting that corpse.";
        case 8: return "You need to be standing up to loot something!";
        case 9: return "You can't loot anything while stunned!";
        case 15: return "Your target has already had its pockets picked.";
        case 16: return "You can't do that while shapeshifted.";
        default: return "You cannot loot that.";
    }
}
}  // namespace

std::string moneyText(uint32_t copper) {
    char b[48];
    const unsigned c = (unsigned)copper;   // uint32_t is unsigned long on devkitARM
    if (c >= 10000) snprintf(b, sizeof b, "%ug %us %uc", c / 10000, c / 100 % 100, c % 100);
    else if (c >= 100) snprintf(b, sizeof b, "%us %uc", c / 100, c % 100);
    else snprintf(b, sizeof b, "%uc", c);
    return b;
}

const char* itemQualityName(uint32_t q) {
    static const char* n[] = {"Poor", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Artifact"};
    return q < 7 ? n[q] : "?";
}

const char* inventoryTypeName(uint32_t t) {
    static const char* n[] = {"", "Head", "Neck", "Shoulder", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrist",
                              "Hands", "Finger", "Trinket", "One-Hand", "Off Hand", "Ranged", "Back", "Two-Hand",
                              "Bag", "Tabard", "Chest", "Main Hand", "Off Hand", "Held In Off-hand", "Ammo",
                              "Thrown", "Ranged", "Quiver", "Relic"};
    return t < sizeof n / sizeof *n ? n[t] : "?";
}

const char* equipSlotName(uint8_t s) {
    static const char* n[] = {"Head", "Neck", "Shoulder", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrist", "Hands",
                              "Finger", "Finger", "Trinket", "Trinket", "Back", "Main Hand", "Off Hand", "Ranged",
                              "Tabard"};
    return s < 19 ? n[s] : s < kSlotBackpackFirst ? "Bag" : "Backpack";
}

const char* itemSubclassName(uint32_t cls, uint32_t sub) {
    if (cls == 2) {   // weapons
        static const char* n[] = {"Axe", "Axe", "Bow", "Gun", "Mace", "Mace", "Polearm", "Sword", "Sword", "", "Staff", "",
                                  "", "Fist Weapon", "", "Dagger", "Thrown", "Spear", "Crossbow", "Wand", "Fishing Pole"};
        return sub < sizeof n / sizeof *n ? n[sub] : "";
    }
    if (cls == 4) {   // armor
        static const char* n[] = {"", "Cloth", "Leather", "Mail", "Plate", "Buckler", "Shield", "Libram", "Idol", "Totem"};
        return sub < sizeof n / sizeof *n ? n[sub] : "";
    }
    return "";
}

std::vector<std::string> itemTooltip(const ItemInfo& i, const InvItem* it, const SpellNames& spells) {
    std::vector<std::string> out;
    char b[96];
    out.push_back(i.name.empty() ? "Item " + std::to_string(i.entry) : i.name);
    if (i.missing) { out.push_back("(the server does not know this item)"); return out; }
    static const char* bind[] = {"", "Binds when picked up", "Binds when equipped", "Binds when used", "Quest Item"};
    if (i.bonding >= 1 && i.bonding <= 4) out.push_back(bind[i.bonding]);
    if (i.maxCount == 1) out.push_back("Unique");
    if (i.startQuest) out.push_back("This Item Begins a Quest");
    if (i.openable()) out.push_back(i.lockId ? "Locked" : "Right Click to Open");
    std::string slot = i.inventoryType ? inventoryTypeName(i.inventoryType) : "";
    std::string type = itemSubclassName(i.cls, i.subclass);
    if (i.containerSlots && i.inventoryType == 18) { slot = std::to_string(i.containerSlots) + " Slot Bag"; type.clear(); }
    if (!slot.empty() || !type.empty()) out.push_back(slot + (slot.empty() || type.empty() ? "" : "  ") + type);
    if (i.dmgMax > 0) {
        snprintf(b, sizeof b, "%d - %d Damage  Speed %.2f", (int)i.dmgMin, (int)i.dmgMax, i.delayMs / 1000.0);
        out.push_back(b);
        if (i.delayMs) {
            snprintf(b, sizeof b, "(%.1f damage per second)", (i.dmgMin + i.dmgMax) / 2.0 / (i.delayMs / 1000.0));
            out.push_back(b);
        }
    }
    if (i.armor) out.push_back(std::to_string(i.armor) + " Armor");
    static const char* stat[] = {"Mana", "Health", "", "Agility", "Strength", "Intellect", "Spirit", "Stamina"};
    for (int k = 0; k < 10; ++k) {
        if (!i.statValue[k] || i.statType[k] < 0 || i.statType[k] > 7 || !stat[i.statType[k]][0]) continue;
        snprintf(b, sizeof b, "%+d %s", (int)i.statValue[k], stat[i.statType[k]]);
        out.push_back(b);
    }
    static const char* res[] = {"Holy", "Fire", "Nature", "Frost", "Shadow", "Arcane"};
    for (int k = 0; k < 6; ++k)
        if (i.resist[k]) {
            snprintf(b, sizeof b, "%+d %s Resistance", (int)i.resist[k], res[k]);
            out.push_back(b);
        }
    if (i.maxDurability) {
        unsigned cur = it && it->maxDurability ? (unsigned)it->durability : (unsigned)i.maxDurability;
        snprintf(b, sizeof b, "Durability %u / %u", cur, (unsigned)i.maxDurability);
        out.push_back(b);
    }
    if (i.requiredLevel > 1) out.push_back("Requires Level " + std::to_string(i.requiredLevel));
    for (int k = 0; k < 5; ++k) {
        if (!i.spellId[k]) continue;
        const char* how = i.spellTrigger[k] == 0 ? "Use: " : i.spellTrigger[k] == 1 ? "Equip: " : i.spellTrigger[k] == 2 ? "Chance on hit: " : "";
        if (!how[0]) continue;
        out.push_back(how + spells.name(i.spellId[k]));
    }
    if (!i.description.empty()) out.push_back("\"" + i.description + "\"");
    if (i.sellPrice) out.push_back("Sell price: " + moneyText(i.sellPrice * (it && it->count > 1 ? it->count : 1)));
    else out.push_back("No sell price");
    return out;
}

bool ItemInfo::usable() const {
    if (startQuest) return true;           // "This item begins a quest": using it shows the quest (stage 5.15)
    for (int i = 0; i < 5; ++i)
        if (spellId[i] && spellTrigger[i] == 0) return true;   // ITEM_SPELLTRIGGER_ON_USE
    return false;
}

// ------------------------------------------------------------------ item cache
const ItemInfo* GameSession::itemInfoIfKnown(uint32_t entry) const {
    auto it = items_.find(entry);
    return it == items_.end() ? nullptr : &it->second;
}

const ItemInfo* GameSession::itemInfo(uint32_t entry) {
    if (!entry) return nullptr;
    if (const ItemInfo* i = itemInfoIfKnown(entry)) return i;
    if (!itemAsked_.count(entry)) itemWanted_.insert(entry);
    return nullptr;
}

std::string GameSession::itemName(uint32_t entry) {
    const ItemInfo* i = itemInfo(entry);
    if (i && !i->name.empty()) return i->name;
    return "Item " + std::to_string(entry);
}

void GameSession::sendItemQueries(uint32_t nowMs) {
    // Items that appeared in the object table (our bags) are asked for too, so the bag page has names at once.
    uint32_t sent = 0;
    for (auto it = itemWanted_.begin(); it != itemWanted_.end() && sent < kQueryBatch;) {
        uint32_t e = *it;
        it = itemWanted_.erase(it);
        if (items_.count(e) || !itemAsked_.insert(e).second) continue;
        Writer w;
        w.u32(e);
        w.u64(0);
        send(CMSG_ITEM_QUERY_SINGLE, w.b, nowMs);
        ++itemQueries;
        ++sent;
    }
}

// ------------------------------------------------------------------ inventory
uint32_t GameSession::money() const {
    const WorldObject* me = selfObject();
    return me ? me->field(UF::PLAYER_FIELD_COINAGE) : 0;
}

std::vector<InvItem> GameSession::inventory() const {
    std::vector<InvItem> out;
    const WorldObject* me = selfObject();
    if (!me) return out;
    auto guidAt = [](const WorldObject& o, uint32_t field) {
        return (uint64_t)o.field((uint16_t)field) | ((uint64_t)o.field((uint16_t)(field + 1)) << 32);
    };
    auto add = [&](uint8_t bag, uint8_t slot, uint64_t g) {
        if (!g) return;
        InvItem it;
        it.bag = bag;
        it.slot = slot;
        it.guid = g;
        if (const WorldObject* o = objects.find(g)) {
            it.entry = o->entry();
            it.count = o->field(UF::ITEM_FIELD_STACK_COUNT);
            if (!it.count) it.count = 1;
            it.durability = o->field(UF::ITEM_FIELD_DURABILITY);
            it.maxDurability = o->field(UF::ITEM_FIELD_MAXDURABILITY);
        }
        out.push_back(it);
    };
    for (uint8_t s = 0; s < kSlotBackpackEnd; ++s) add(kBagSelf, s, guidAt(*me, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * s));
    for (uint8_t b = kSlotBagFirst; b < kSlotBackpackFirst; ++b) {
        const WorldObject* bag = objects.find(guidAt(*me, UF::PLAYER_FIELD_INV_SLOT_HEAD + 2u * b));
        if (!bag || bag->type != ObjType::Container) continue;
        uint32_t n = std::min<uint32_t>(bag->field(UF::CONTAINER_FIELD_NUM_SLOTS), 36);
        for (uint32_t i = 0; i < n; ++i) add(b, (uint8_t)i, guidAt(*bag, UF::CONTAINER_FIELD_SLOT_1 + 2u * i));
    }
    return out;
}

uint32_t GameSession::freeBagSlots() const {
    const WorldObject* me = selfObject();
    if (!me) return 0;
    uint32_t used = 0, total = kSlotBackpackEnd - kSlotBackpackFirst;
    for (const InvItem& it : inventory()) {
        if (it.bag == kBagSelf && it.slot >= kSlotBackpackFirst) ++used;
        else if (it.bag != kBagSelf) ++used;
        else if (it.isBagSlot())
            if (const WorldObject* b = objects.find(it.guid)) total += std::min<uint32_t>(b->field(UF::CONTAINER_FIELD_NUM_SLOTS), 36);
    }
    return total > used ? total - used : 0;
}

void GameSession::useItem(const InvItem& it, uint32_t nowMs) {
    Writer w;
    w.u8(it.bag);
    w.u8(it.slot);
    w.u8(0);      // spell slot of the item (its first on-use spell)
    w.u16(0);     // targets: none = self (food, drink, potions, hearthstone)
    send(CMSG_USE_ITEM, w.b, nowMs);
}

void GameSession::openItem(const InvItem& it, uint32_t nowMs) {   // u8 bag, u8 slot (WorldPackets::Spell::OpenItem)
    Writer w;
    w.u8(it.bag);
    w.u8(it.slot);
    send(CMSG_OPEN_ITEM, w.b, nowMs);
}

void GameSession::equipItem(const InvItem& it, uint32_t nowMs) {
    Writer w;
    w.u8(it.bag);
    w.u8(it.slot);
    send(CMSG_AUTOEQUIP_ITEM, w.b, nowMs);
}

void GameSession::unequipItem(const InvItem& it, uint32_t nowMs) {
    if (freeBagSlots() == 0) { addLocal(inventoryResultText(50 /* INVENTORY_FULL */), nowMs); return; }
    Writer w;
    w.u8(it.bag);
    w.u8(it.slot);
    w.u8(kBagSelf);   // destination: any free slot of the backpack or the bags
    send(CMSG_AUTOSTORE_BAG_ITEM, w.b, nowMs);
}

void GameSession::destroyItem(const InvItem& it, uint32_t nowMs) {
    Writer w;
    w.u8(it.bag);
    w.u8(it.slot);
    w.u8((uint8_t)std::min<uint32_t>(it.count, 255));
    w.u8(0); w.u8(0); w.u8(0);   // unused
    send(CMSG_DESTROYITEM, w.b, nowMs);
}

// ------------------------------------------------------------------ loot window
void GameSession::loot(uint64_t corpse, uint32_t nowMs) {
    if (!corpse) return;
    Writer w;
    w.u64(corpse);
    send(CMSG_LOOT, w.b, nowMs);
}

void GameSession::takeLootMoney(uint32_t nowMs) {
    if (!lootWindow.open || !lootWindow.money) return;
    send(CMSG_LOOT_MONEY, {}, nowMs);
}

void GameSession::takeLoot(size_t index, uint32_t nowMs) {
    if (!lootWindow.open || index >= lootWindow.items.size()) return;
    Writer w;
    w.u8(lootWindow.items[index].slot);
    send(CMSG_AUTOSTORE_LOOT_ITEM, w.b, nowMs);
}

void GameSession::closeLoot(uint32_t nowMs) {
    if (!lootGuid_) { lootWindow = LootWindow(); return; }
    Writer w;
    w.u64(lootGuid_);
    send(CMSG_LOOT_RELEASE, w.b, nowMs);
    lootWindow.open = false;   // the release answer clears lootGuid_
}

void GameSession::takeAllLoot(uint32_t nowMs) {
    if (!lootWindow.open) return;
    takeLootMoney(nowMs);
    for (size_t i = 0; i < lootWindow.items.size(); ++i) takeLoot(i, nowMs);
    closeLoot(nowMs);
}

// ------------------------------------------------------------------ server packets
bool GameSession::handleItems(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_ITEM_QUERY_SINGLE_RESPONSE: {
            uint32_t entry = r.u32();
            if (r.bad) return true;
            ++itemAnswers;
            if (entry & 0x80000000u) {   // unknown to the server
                ItemInfo& i = items_[entry & 0x7FFFFFFFu];
                i.entry = entry & 0x7FFFFFFFu;
                i.missing = true;
                return true;
            }
            ItemInfo i;
            i.entry = entry;
            i.cls = r.u32();
            i.subclass = r.u32();
            i.name = r.cstr();
            r.cstr(); r.cstr(); r.cstr();   // names 2..4: always empty
            i.displayId = r.u32();
            i.quality = r.u32();
            i.flags = r.u32();
            i.buyPrice = r.u32();
            i.sellPrice = r.u32();
            i.inventoryType = r.u32();
            r.u32(); r.u32();               // allowable class / race masks
            i.itemLevel = r.u32();
            i.requiredLevel = r.u32();
            i.requiredSkill = r.u32();
            i.requiredSkillRank = r.u32();
            for (int k = 0; k < 5; ++k) r.u32();   // spell, honor rank, city rank, rep faction, rep rank
            i.maxCount = r.u32();
            i.maxStack = r.u32();
            i.containerSlots = r.u32();
            if (r.bad) return true;         // the name and the basics are what the UI needs: keep going only if complete
            for (int k = 0; k < 10; ++k) { i.statType[k] = (int32_t)r.u32(); i.statValue[k] = (int32_t)r.u32(); }
            for (int k = 0; k < 5; ++k) {
                float mn = r.f32(), mx = r.f32();
                r.u32();                    // damage school
                if (k == 0) { i.dmgMin = mn; i.dmgMax = mx; }
            }
            i.armor = r.u32();
            for (int k = 0; k < 6; ++k) i.resist[k] = (int32_t)r.u32();
            i.delayMs = r.u32();
            r.u32();                        // ammo type
            r.f32();                        // ranged range modifier
            for (int k = 0; k < 5; ++k) {
                i.spellId[k] = r.u32();
                i.spellTrigger[k] = r.u32();
                r.u32(); r.u32(); r.u32(); r.u32();   // charges, cooldown, category, category cooldown
            }
            i.bonding = r.u32();
            i.description = r.cstr();
            r.u32(); r.u32(); r.u32();      // page text, language, page material
            i.startQuest = r.u32();
            i.lockId = r.u32();
            for (int k = 0; k < 5; ++k) r.u32();    // material, sheath, random property, block, item set
            // (stage 5.15 fix: 10 fields, not 11 - the max durability used to be read from the Area field)
            i.maxDurability = r.u32();
            if (r.bad) {   // shorter than vMaNGOS writes it: keep the head, drop what came after the stats
                ItemInfo head;
                head.entry = i.entry; head.cls = i.cls; head.subclass = i.subclass; head.name = i.name;
                head.displayId = i.displayId; head.quality = i.quality; head.flags = i.flags; head.buyPrice = i.buyPrice;
                head.sellPrice = i.sellPrice; head.inventoryType = i.inventoryType; head.itemLevel = i.itemLevel;
                head.requiredLevel = i.requiredLevel; head.maxStack = i.maxStack; head.containerSlots = i.containerSlots;
                i = head;
            }
            if (i.maxStack == 0) i.maxStack = 1;
            items_[entry] = std::move(i);
            return true;
        }
        case SMSG_INVENTORY_CHANGE_FAILURE: {
            uint8_t res = r.u8();
            if (r.bad || res == 0) return true;
            lastInventoryError = res;
            std::string t = inventoryResultText(res);
            if (res == 1) {   // CANT_EQUIP_LEVEL_I: u32 required level first
                uint32_t lvl = r.u32();
                if (!r.bad) t = "You must reach level " + std::to_string(lvl) + " to use that item.";
            }
            addLocal(t, nowMs);
            return true;
        }
        case SMSG_ITEM_PUSH_RESULT: {
            uint64_t who = r.u64();
            uint32_t received = r.u32(), created = r.u32();
            r.u32();   // show in chat
            r.u8();    // bag
            r.u32();   // slot (0xFFFFFFFF: added to a stack)
            uint32_t entry = r.u32();
            r.u32(); r.u32();   // suffix factor, random property
            uint32_t count = r.u32();
            if (r.bad || who != self_) return true;
            std::string nm = itemName(entry);
            std::string t = std::string(created ? "You create: " : received ? "You receive item: " : "You receive loot: ") +
                            "[" + nm + "]" + (count > 1 ? "x" + std::to_string(count) : std::string()) + ".";
            ChatLine l;
            l.ms = nowMs;
            l.type = Chat::Loot;
            l.text = t;
            l.channel = std::to_string(entry);   // the entry, so the name can be filled in once the query answers
            addLine(std::move(l));
            return true;
        }
        case SMSG_LOOT_RESPONSE: {
            uint64_t g = r.u64();
            uint8_t lootType = r.u8();
            if (r.bad) return true;
            if (lootType == 0) {
                uint8_t err = r.u8();
                addLocal(lootErrorText(r.bad ? 255 : err), nowMs);
                return true;
            }
            uint32_t gold = r.u32();
            uint8_t n = r.u8();
            lastLoot.clear();
            for (uint8_t i = 0; i < n && !r.bad; ++i) {
                LootSlot s;
                s.slot = r.u8();
                s.itemId = r.u32();
                s.count = r.u32();
                r.u32();   // display id
                r.u32();   // unknown
                r.u32();   // random property
                uint8_t slotType = r.u8();
                if (!r.bad && slotType == 0) lastLoot.push_back(s);   // 0 = allowed to loot
            }
            if (r.bad) return true;
            lootGuid_ = g;
            lootWindow.open = true;
            lootWindow.guid = g;
            lootWindow.money = gold;
            lootWindow.items = lastLoot;
            for (const LootSlot& s : lastLoot) itemInfo(s.itemId);   // names for the window
            if (autoLoot || (!gold && lastLoot.empty())) {   // the window shows it otherwise
                std::string t = "Loot:";
                if (gold) t += " " + moneyText(gold);
                for (const LootSlot& s : lastLoot) t += " item " + std::to_string(s.itemId) + " x" + std::to_string(s.count);
                if (!gold && lastLoot.empty()) t += " nothing";
                addLocal(t, nowMs);
            }
            if (autoLoot || (!gold && lastLoot.empty())) takeAllLoot(nowMs);
            return true;
        }
        case SMSG_LOOT_REMOVED: {
            uint8_t slot = r.u8();
            if (r.bad) return true;
            ++lootedItems;
            for (size_t i = 0; i < lootWindow.items.size(); ++i)
                if (lootWindow.items[i].slot == slot) { lootWindow.items.erase(lootWindow.items.begin() + (long)i); break; }
            return true;
        }
        case SMSG_LOOT_MONEY_NOTIFY: {   // our share of the money when in a group
            uint32_t c = r.u32();
            if (!r.bad) addLocal("Your share of the loot is " + moneyText(c) + ".", nowMs);
            return true;
        }
        case SMSG_LOOT_CLEAR_MONEY:
            if (lootWindow.money) {
                lootedCopper += lootWindow.money;
                addLocal("You loot " + moneyText(lootWindow.money) + ".", nowMs);
                sound("coin");
            }
            lootWindow.money = 0;
            return true;
        case SMSG_LOOT_RELEASE_RESPONSE:
            lootGuid_ = 0;
            lootWindow = LootWindow();
            return true;
        default:
            return false;
    }
}

}  // namespace w3n
