// Trade with another player (UI stage 6). Formats: vMaNGOS Server/Packets/Trade.cpp (client packets, TradeStatus),
// Handlers/TradeHandler.cpp (SendUpdateTrade: SMSG_TRADE_STATUS_EXTENDED, 7 slots of 15 u32 / guid fields each;
// the flow: INITIATE -> the other side gets BEGIN_TRADE -> its CMSG_BEGIN_TRADE -> OPEN_WINDOW to both).
#include <algorithm>

#include "game.hpp"
#include "wire.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_INITIATE_TRADE = 0x116, CMSG_BEGIN_TRADE = 0x117, CMSG_BUSY_TRADE = 0x118, CMSG_ACCEPT_TRADE = 0x11A,
    CMSG_UNACCEPT_TRADE = 0x11B, CMSG_CANCEL_TRADE = 0x11C, CMSG_SET_TRADE_ITEM = 0x11D, CMSG_CLEAR_TRADE_ITEM = 0x11E,
    CMSG_SET_TRADE_GOLD = 0x11F, SMSG_TRADE_STATUS = 0x120, SMSG_TRADE_STATUS_EXTENDED = 0x121,
};
enum : uint32_t {
    kBusy = 0, kBegin = 1, kOpen = 2, kCanceled = 3, kAccept = 4, kBusy2 = 5, kNoTarget = 6, kBack = 7, kComplete = 8,
    kRejected = 9, kTooFar = 10, kWrongFaction = 11, kClose = 12, kUnknown13 = 13, kIgnoreYou = 14,
};
const char* tradeError(uint32_t s) {
    switch (s) {
        case kBusy: case kBusy2: return "That player is busy";
        case kNoTarget: return "Trade target not found";
        case kRejected: return "Trade declined";
        case kTooFar: return "Trade target is too far away";
        case kWrongFaction: return "You can't trade with the other faction";
        case kIgnoreYou: return "That player is ignoring you";
        case 15: return "You can't trade while stunned";
        case 16: return "That player is stunned";
        case 17: return "You can't trade while dead";
        case 18: return "That player is dead";
        case 19: return "You are logging out";
        case 20: return "That player is logging out";
        default: return "Trade failed";
    }
}
}  // namespace

void GameSession::proposeTrade(uint64_t player, uint32_t nowMs) {
    if (!player || player == self_) return;
    if (trade.phase != TradeState::Phase::None) { addLocal("You are already trading.", nowMs); return; }
    Writer w;
    w.u64(player);
    send(CMSG_INITIATE_TRADE, w.b, nowMs);
    trade = TradeState();
    trade.phase = TradeState::Phase::Proposed;
    trade.with = player;
}

void GameSession::answerTrade(bool accept, uint32_t nowMs) {
    if (trade.phase != TradeState::Phase::Asked) return;
    send(accept ? CMSG_BEGIN_TRADE : CMSG_CANCEL_TRADE, {}, nowMs);
    if (!accept) trade = TradeState();
}

bool GameSession::offerTradeItem(int slot, const InvItem& it, uint32_t nowMs) {
    if (!trade.open() || slot < 0 || slot >= 7 || !it.guid) return false;
    if (it.equipped()) { addLocal("Unequip it first.", nowMs); return false; }
    for (int i = 0; i < 7; ++i)
        if (trade.myItems[i].guid == it.guid && i != slot) { addLocal("That item is already offered.", nowMs); return false; }
    Writer w;
    w.u8((uint8_t)slot);
    w.u8(it.bag);
    w.u8(it.slot);
    send(CMSG_SET_TRADE_ITEM, w.b, nowMs);
    trade.myItems[slot] = it;
    return true;
}

void GameSession::clearTradeItem(int slot, uint32_t nowMs) {
    if (!trade.open() || slot < 0 || slot >= 7) return;
    Writer w;
    w.u8((uint8_t)slot);
    send(CMSG_CLEAR_TRADE_ITEM, w.b, nowMs);
    trade.myItems[slot] = InvItem();
    trade.myItems[slot].guid = 0;
}

void GameSession::offerTradeGold(uint32_t copper, uint32_t nowMs) {
    if (!trade.open()) return;
    copper = std::min(copper, money());
    Writer w;
    w.u32(copper);
    send(CMSG_SET_TRADE_GOLD, w.b, nowMs);
    trade.mine.gold = copper;
}

void GameSession::acceptTrade(uint32_t nowMs) {
    if (!trade.open()) return;
    Writer w;
    w.u32(1);
    send(CMSG_ACCEPT_TRADE, w.b, nowMs);
    trade.weAccepted = true;
}

void GameSession::cancelTrade(uint32_t nowMs) {
    if (trade.phase == TradeState::Phase::None) return;
    send(CMSG_CANCEL_TRADE, {}, nowMs);
    trade = TradeState();
}

bool GameSession::handleTrade(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_TRADE_STATUS: {
            uint32_t s = r.u32();
            if (r.bad) return true;
            switch (s) {
                case kBegin: {
                    uint64_t g = r.u64();
                    trade = TradeState();
                    trade.phase = TradeState::Phase::Asked;
                    trade.with = g;
                    std::string n = playerName(g, nowMs);
                    addLocal((n.empty() ? std::string("Someone") : n) + " wants to trade with you.", nowMs);
                    sound("invite");
                    break;
                }
                case kOpen:
                    if (trade.phase == TradeState::Phase::None) trade.with = target();
                    trade.phase = TradeState::Phase::Open;
                    for (InvItem& it : trade.myItems) it.guid = 0;
                    break;
                case kAccept: trade.theyAccepted = true; break;
                case kBack: trade.theyAccepted = trade.weAccepted = false; break;
                case kComplete:
                    ++tradesDone;
                    addLocal("Trade complete.", nowMs);
                    trade = TradeState();
                    break;
                case kCanceled: case kClose: case kUnknown13:
                    if (trade.phase != TradeState::Phase::None) addLocal("Trade cancelled.", nowMs);
                    trade = TradeState();
                    break;
                default:
                    addLocal(tradeError(s), nowMs);
                    trade = TradeState();
                    break;
            }
            return true;
        }
        case SMSG_TRADE_STATUS_EXTENDED: {
            uint8_t whose = r.u8();
            r.u32();
            r.u32();
            TradeOffer o;
            o.gold = r.u32();
            r.u32();   // spell on the lowest slot
            for (int i = 0; i < 7 && !r.bad; ++i) {
                uint8_t slot = r.u8();
                TradeSlotView v;
                v.entry = r.u32();
                v.display = r.u32();
                v.count = r.u32();
                r.u32();     // wrapped
                r.u64();     // gift creator
                r.u32();     // enchantment
                r.u64();     // creator
                r.skip(6 * 4);   // charges, suffix factor, random property, lock id, max durability, durability
                if (slot < 7) o.slots[slot] = v;
                if (v.entry) itemInfo(v.entry);
            }
            if (r.bad && !o.gold) return true;
            if (whose) trade.theirs = o;
            else trade.mine = o;
            return true;
        }
        default:
            return false;
    }
}

}  // namespace w3n
