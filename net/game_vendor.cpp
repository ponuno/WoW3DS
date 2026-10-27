// game_vendor.cpp — vendors of GameSession (stage 5.13): the item list, buying, selling, buyback and repairs.
//
// Layouts from vMaNGOS (1.12.1):
//   CMSG_LIST_INVENTORY u64 vendor -> SMSG_LIST_INVENTORY: u64 vendor, u8 count, count x {u32 slot (visible rows, from 1),
//     u32 item, u32 display, u32 stock (0xFFFFFFFF unlimited), u32 price, u32 max durability, u32 buy count};
//     count 0 is followed by a u8 error (0 = "vendor has no inventory") (WorldSession::SendListInventory).
//   CMSG_BUY_ITEM: u64 vendor, u32 item, u8 count (bundles of buy count), u8 0 (Player::BuyItemFromVendor);
//     SMSG_BUY_ITEM: u64 vendor, u32 slot, u32 new stock, u32 bought; SMSG_BUY_FAILED: u64 vendor, u32 item, u8 BuyResult.
//   CMSG_SELL_ITEM: u64 vendor, u64 item, u8 count (0 = the whole stack); success has no answer (the item goes to
//     a buyback slot and the money field changes), SMSG_SELL_ITEM: u64 vendor, u64 item, u8 SellResult on failure.
//   CMSG_BUYBACK_ITEM: u64 vendor, u32 slot (69 .. 80 = BUYBACK_SLOT_START + index);
//   CMSG_REPAIR_ITEM: u64 npc, u64 item (0 = everything) (WorldSession::HandleRepairItemOpcode).
// Buyback slots: PLAYER_FIELD_VENDORBUYBACK_SLOT_1 (0x270, 12 GUIDs), PLAYER_FIELD_BUYBACK_PRICE_1 (0x4CA, 12 prices).
// Trainers (WorldSession::SendTrainerList / SendTrainerSpellHelper / HandleTrainerBuySpellOpcode):
//   CMSG_TRAINER_LIST u64 -> SMSG_TRAINER_LIST: u64, u32 type, u32 n, n x {u32 spell, u8 state, u32 cost,
//     u32 prof dialog, u32 prof button, u8 level, u32 skill, u32 skill value, u32 required spell, u32 2nd, u32 0}, greeting;
//   CMSG_TRAINER_BUY_SPELL u64, u32 spell -> SMSG_TRAINER_BUY_SUCCEEDED u64, u32 | _FAILED u64, u32, u32 reason.
// Innkeepers: SMSG_BINDER_CONFIRM u64 -> CMSG_BINDER_ACTIVATE u64 (WorldSession::HandleBinderActivateOpcode).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_LIST_INVENTORY = 0x19E, SMSG_LIST_INVENTORY = 0x19F, CMSG_SELL_ITEM = 0x1A0, SMSG_SELL_ITEM = 0x1A1,
    CMSG_BUY_ITEM = 0x1A2, SMSG_BUY_ITEM = 0x1A4, SMSG_BUY_FAILED = 0x1A5, CMSG_BUYBACK_ITEM = 0x290,
    CMSG_REPAIR_ITEM = 0x2A8, CMSG_TRAINER_LIST = 0x1B0, SMSG_TRAINER_LIST = 0x1B1, CMSG_TRAINER_BUY_SPELL = 0x1B2,
    SMSG_TRAINER_BUY_SUCCEEDED = 0x1B3, SMSG_TRAINER_BUY_FAILED = 0x1B4, CMSG_BINDER_ACTIVATE = 0x1B5,
    SMSG_PLAYERBINDERROR = 0x1B6, SMSG_BINDER_CONFIRM = 0x2EB, SMSG_BINDPOINTUPDATE = 0x155,
};
constexpr uint16_t kBuybackGuid1 = 0x270, kBuybackPrice1 = 0x4CA;
constexpr uint32_t kBuybackSlotStart = 69, kBuybackSlots = 12;
constexpr uint32_t kNpcRepair = 0x4000;

const char* buyErrorText(uint8_t e) {
    switch (e) {
        case 0: return "The vendor does not have that item.";
        case 1: case 7: return "That item is sold out.";
        case 2: return "You don't have enough money.";
        case 4: return "The merchant doesn't like you.";
        case 5: return "You are too far away.";
        case 8: return "You can't carry any more of those items.";
        case 11: return "Your PvP rank is too low.";
        case 12: return "Your reputation is not high enough.";
        default: return "You cannot buy that.";
    }
}

const char* sellErrorText(uint8_t e) {
    switch (e) {
        case 1: return "The merchant can't find that item.";
        case 2: return "The merchant doesn't want that item.";
        case 3: return "The merchant doesn't like you.";
        case 4: return "You don't own that item.";
        case 6: return "You can only do that with empty bags.";
        default: return "You cannot sell that.";
    }
}
}  // namespace

void GameSession::openVendor(uint64_t npc, uint32_t nowMs) {
    if (!npc) return;
    Writer w;
    w.u64(npc);
    send(CMSG_LIST_INVENTORY, w.b, nowMs);
}

bool GameSession::buyItem(size_t index, uint32_t count, uint32_t nowMs) {
    if (!vendor.open || index >= vendor.items.size() || count == 0) return false;
    const VendorItem& v = vendor.items[index];
    if (count > 255) count = 255;
    if (!v.unlimited() && v.stock == 0) { addLocal("That item is sold out.", nowMs); return false; }
    if ((uint64_t)v.price * count > money()) { addLocal("You don't have enough money.", nowMs); return false; }
    Writer w;
    w.u64(vendor.npc);
    w.u32(v.entry);
    w.u8((uint8_t)count);
    w.u8(0);
    send(CMSG_BUY_ITEM, w.b, nowMs);
    return true;
}

uint32_t GameSession::sellValue(const InvItem& it) const {
    const ItemInfo* info = itemInfoIfKnown(it.entry);
    if (!info || info->missing) return 0;
    return info->sellPrice * std::max<uint32_t>(1, it.count);
}

bool GameSession::sellItem(const InvItem& it, uint32_t nowMs) {
    if (!vendor.open || !it.guid) return false;
    if (it.equipped()) { addLocal("Take it off first.", nowMs); return false; }
    const ItemInfo* info = itemInfoIfKnown(it.entry);
    if (info && !info->missing && info->sellPrice == 0) { addLocal("The merchant doesn't want that item.", nowMs); return false; }
    Writer w;
    w.u64(vendor.npc);
    w.u64(it.guid);
    w.u8(0);   // the whole stack
    send(CMSG_SELL_ITEM, w.b, nowMs);
    if (pendingSells_.size() >= 8) pendingSells_.erase(pendingSells_.begin());
    pendingSells_.push_back({it.guid, it.entry, it.count, sellValue(it), nowMs ? nowMs : 1});
    return true;
}

std::vector<BuybackItem> GameSession::buybackItems() const {
    std::vector<BuybackItem> out;
    const WorldObject* me = selfObject();
    if (!me) return out;
    for (uint32_t i = 0; i < kBuybackSlots; ++i) {
        uint64_t g = (uint64_t)me->field(kBuybackGuid1 + 2 * i) | ((uint64_t)me->field(kBuybackGuid1 + 2 * i + 1) << 32);
        if (!g) continue;
        BuybackItem b;
        b.slot = kBuybackSlotStart + i;
        b.guid = g;
        b.price = me->field(kBuybackPrice1 + i);
        if (const WorldObject* o = objects.find(g)) {
            b.entry = o->entry();
            b.count = std::max<uint32_t>(1, o->field(UF::ITEM_FIELD_STACK_COUNT));
        }
        out.push_back(b);
    }
    return out;
}

bool GameSession::buybackItem(const BuybackItem& b, uint32_t nowMs) {
    if (!vendor.open || b.slot < kBuybackSlotStart || b.slot >= kBuybackSlotStart + kBuybackSlots) return false;
    if (b.price > money()) { addLocal("You don't have enough money.", nowMs); return false; }
    Writer w;
    w.u64(vendor.npc);
    w.u32(b.slot);
    send(CMSG_BUYBACK_ITEM, w.b, nowMs);
    ++buybacks;
    return true;
}

void GameSession::repairAll(uint32_t nowMs) {
    if (!vendor.open || !vendor.canRepair) { addLocal("This merchant cannot repair.", nowMs); return; }
    Writer w;
    w.u64(vendor.npc);
    w.u64(0);
    send(CMSG_REPAIR_ITEM, w.b, nowMs);
    ++repairs;
    pendingRepair_ = {money(), nowMs ? nowMs : 1};
}

void GameSession::openTrainer(uint64_t npc, uint32_t nowMs) {
    if (!npc) return;
    Writer w;
    w.u64(npc);
    send(CMSG_TRAINER_LIST, w.b, nowMs);
}

bool GameSession::trainSpell(size_t index, uint32_t nowMs) {
    if (!trainer.open || index >= trainer.spells.size()) return false;
    const TrainerSpell& t = trainer.spells[index];
    if (t.state == 2) { addLocal("You already know that.", nowMs); return false; }
    if (t.state == 1) {
        const WorldObject* me = selfObject();
        if (me && t.reqLevel > me->level()) addLocal("You must be level " + std::to_string(t.reqLevel) + " to learn that.", nowMs);
        else addLocal("You can't learn that yet.", nowMs);
        return false;
    }
    if (t.cost > money()) { addLocal("You don't have enough money.", nowMs); return false; }
    Writer w;
    w.u64(trainer.npc);
    w.u32(t.spell);
    send(CMSG_TRAINER_BUY_SPELL, w.b, nowMs);
    return true;
}

void GameSession::acceptBinder(uint32_t nowMs) {
    if (!binderConfirm) return;
    Writer w;
    w.u64(binderConfirm);
    send(CMSG_BINDER_ACTIVATE, w.b, nowMs);
    binderConfirm = 0;
    bindPending_ = true;
}

// Called from update(): sales and repairs have no success answer, the inventory and the money show them.
void GameSession::updateVendor(uint32_t nowMs) {
    if (!pendingSells_.empty()) {
        std::vector<InvItem> inv = inventory();
        for (size_t i = 0; i < pendingSells_.size();) {
            const PendingSell& p = pendingSells_[i];
            bool still = false;
            for (const InvItem& it : inv) still = still || it.guid == p.guid;
            if (!still) {
                std::string what = itemName(p.entry);
                if (p.count > 1) what += " x" + std::to_string(p.count);
                addLocal("Sold " + what + (p.value ? " for " + moneyText(p.value) : std::string()) + ".", nowMs);
                ++sold;
            } else if (nowMs - p.ms <= 5000) {
                ++i;
                continue;
            }
            pendingSells_.erase(pendingSells_.begin() + i);
        }
    }
    if (pendingRepair_.ms) {
        uint32_t m = money();
        if (m != pendingRepair_.moneyBefore) {
            if (m < pendingRepair_.moneyBefore) addLocal("Repaired for " + moneyText(pendingRepair_.moneyBefore - m) + ".", nowMs);
            pendingRepair_ = PendingRepair();
        } else if (nowMs - pendingRepair_.ms > 3000) {
            addLocal("Nothing to repair.", nowMs);
            pendingRepair_ = PendingRepair();
        }
    }
}

bool GameSession::handleVendor(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_LIST_INVENTORY: {
            VendorWindow v;
            v.npc = r.u64();
            uint8_t n = r.u8();
            if (r.bad) return true;
            if (n == 0) {
                v.noInventory = true;
            } else {
                if ((size_t)n * 28 > r.left()) return true;
                for (uint8_t i = 0; i < n; ++i) {
                    VendorItem it;
                    it.slot = r.u32(); it.entry = r.u32(); it.displayId = r.u32(); it.stock = r.u32();
                    it.price = r.u32(); it.maxDurability = r.u32(); it.buyCount = std::max<uint32_t>(1, r.u32());
                    v.items.push_back(it);
                    itemInfo(it.entry);   // names for the window
                }
                if (r.bad) return true;
            }
            const WorldObject* npc = objects.find(v.npc);
            v.canRepair = npc && (npc->field(UF::UNIT_NPC_FLAGS) & kNpcRepair);
            v.open = true;
            vendor = std::move(v);
            gossip = GossipMenu();   // the vendor window replaces the gossip menu
            questDialog = QuestDialog();
            trainer = TrainerWindow();
            if (vendor.noInventory) addLocal("This vendor has nothing to sell.", nowMs);
            return true;
        }
        case SMSG_TRAINER_LIST: {
            TrainerWindow t;
            t.npc = r.u64();
            t.type = r.u32();
            uint32_t n = r.u32();
            if (r.bad || n > 512 || (size_t)n * 38 > r.left()) return true;
            for (uint32_t i = 0; i < n; ++i) {
                TrainerSpell s;
                s.spell = r.u32();
                s.state = r.u8();
                s.cost = r.u32();
                r.u32(); r.u32();   // profession confirmation dialog / button
                s.reqLevel = r.u8();
                s.reqSkill = r.u32();
                s.reqSkillValue = r.u32();
                s.reqSpell = r.u32();
                s.reqSpell2 = r.u32();
                r.u32();
                t.spells.push_back(s);
            }
            t.greeting = r.cstr();
            if (r.bad) return true;
            t.open = true;
            trainer = std::move(t);
            gossip = GossipMenu();
            questDialog = QuestDialog();
            vendor = VendorWindow();
            return true;
        }
        case SMSG_TRAINER_BUY_SUCCEEDED: {
            uint64_t g = r.u64();
            uint32_t spell = r.u32();
            if (r.bad) return true;
            ++trained;   // SMSG_LEARNED_SPELL of the taught spell says "You have learned ..."
            if (trainer.open && trainer.npc == g) {
                for (TrainerSpell& t : trainer.spells)
                    if (t.spell == spell) t.state = 2;
                openTrainer(g, nowMs);   // the next ranks may be learnable now: ask for the list again
            }
            return true;
        }
        case SMSG_TRAINER_BUY_FAILED: {
            r.u64();
            r.u32();
            uint32_t why = r.u32();
            if (r.bad) return true;
            addLocal(why == 1 ? "You don't have enough money." : why == 2 ? "Your skill is not high enough." : "You can't learn that.", nowMs);
            return true;
        }
        case SMSG_BINDER_CONFIRM: {
            uint64_t g = r.u64();
            if (r.bad) return true;
            binderConfirm = g;
            gossip = GossipMenu();   // the question replaces any NPC window
            vendor = VendorWindow();
            trainer = TrainerWindow();
            questDialog = QuestDialog();
            return true;
        }
        case SMSG_PLAYERBINDERROR:
            addLocal("You can't make this place your home.", nowMs);
            return true;
        case SMSG_BINDPOINTUPDATE: {
            float x = r.f32(), y = r.f32(), z = r.f32();
            r.u32();
            r.u32();   // area
            if (!r.bad && bindPending_) addLocal("This inn is now your home.", nowMs);
            bindPending_ = false;   // the one at login is not news
            (void)x; (void)y; (void)z;
            return true;
        }
        case SMSG_BUY_ITEM: {
            uint64_t g = r.u64();
            uint32_t slot = r.u32(), stock = r.u32(), count = r.u32();
            if (r.bad) return true;
            if (vendor.open && vendor.npc == g)
                for (VendorItem& it : vendor.items)
                    if (it.slot == slot) {
                        it.stock = stock;
                        std::string what = itemName(it.entry);
                        if (count > 1) what += " x" + std::to_string(count);
                        uint32_t bundles = it.buyCount ? std::max<uint32_t>(1, count / it.buyCount) : 1;
                        addLocal("Bought " + what + " for " + moneyText(it.price * bundles) + ".", nowMs);
                    }
            ++bought;
            return true;
        }
        case SMSG_BUY_FAILED: {
            r.u64();
            uint32_t entry = r.u32();
            uint8_t e = r.u8();
            if (r.bad) return true;
            lastBuyError = e;
            (void)entry;
            addLocal(buyErrorText(e), nowMs);
            return true;
        }
        case SMSG_SELL_ITEM: {
            r.u64();
            uint64_t item = r.u64();
            uint8_t e = r.u8();
            if (r.bad) return true;
            lastSellError = e;
            pendingSells_.erase(std::remove_if(pendingSells_.begin(), pendingSells_.end(),
                                               [&](const PendingSell& p) { return p.guid == item; }), pendingSells_.end());
            addLocal(sellErrorText(e), nowMs);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
