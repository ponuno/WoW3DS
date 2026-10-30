// Auction house (UI stage 6). Formats: vMaNGOS Server/Packets/AuctionHouse.cpp (client packets, HelloResponse,
// CommandResult), AuctionHouse/AuctionHouseMgr.cpp AuctionEntry::BuildAuctionInfo and the list results
// (u32 count, entries, u32 total), Handlers/AuctionHouseHandler.cpp (sell times in minutes: 120, 480, 1440).
#include <algorithm>

#include "game.hpp"
#include "wire.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    MSG_AUCTION_HELLO = 0x255, CMSG_AUCTION_SELL_ITEM = 0x256, CMSG_AUCTION_REMOVE_ITEM = 0x257,
    CMSG_AUCTION_LIST_ITEMS = 0x258, CMSG_AUCTION_LIST_OWNER_ITEMS = 0x259, CMSG_AUCTION_PLACE_BID = 0x25A,
    SMSG_AUCTION_COMMAND_RESULT = 0x25B, SMSG_AUCTION_LIST_RESULT = 0x25C, SMSG_AUCTION_OWNER_LIST_RESULT = 0x25D,
    SMSG_AUCTION_BIDDER_NOTIFICATION = 0x25E, SMSG_AUCTION_OWNER_NOTIFICATION = 0x25F,
    SMSG_AUCTION_REMOVED_NOTIFICATION = 0x28D,
};

bool readAuction(Reader& r, AuctionItem& a) {
    a.id = r.u32();
    a.item = r.u32();
    a.enchant = r.u32();
    a.randomProp = r.u32();
    a.suffix = r.u32();
    a.count = r.u32();
    a.charges = r.u32();
    a.owner = r.u64();
    a.startBid = r.u32();
    a.minOutbid = r.u32();
    a.buyout = r.u32();
    a.timeLeftMs = r.u32();
    a.bidder = r.u64();
    a.bid = r.u32();
    return !r.bad;
}

const char* auctionError(uint32_t e) {
    switch (e) {
        case 1: return "You cannot carry that";
        case 3: return "You don't have enough money";
        case 4: return "Item not found";
        case 5: return "There is already a higher bid";
        case 7: return "Your bid increment is too small";
        case 10: return "You cannot bid on your own auction";
        default: return "Auction house error";
    }
}
}  // namespace

void GameSession::openAuction(uint64_t npc, uint32_t nowMs) {
    Writer w;
    w.u64(npc);
    send(MSG_AUCTION_HELLO, w.b, nowMs);
}

void GameSession::searchAuctions(const std::string& name, uint32_t page, uint32_t nowMs) {
    if (!auction.open) return;
    Writer w;
    w.u64(auction.npc);
    w.u32(page * 50);
    w.cstr(name);
    w.u8(0);               // level min
    w.u8(0);               // level max
    w.u32(0xFFFFFFFFu);    // inventory type: any
    w.u32(0xFFFFFFFFu);    // class
    w.u32(0xFFFFFFFFu);    // subclass
    w.u32(0xFFFFFFFFu);    // quality
    w.u8(0);               // usable only
    send(CMSG_AUCTION_LIST_ITEMS, w.b, nowMs);
    auction.query = name;
    auction.page = page;
    auction.waiting = true;
}

void GameSession::listOwnAuctions(uint32_t nowMs) {
    if (!auction.open) return;
    Writer w;
    w.u64(auction.npc);
    w.u32(0);
    send(CMSG_AUCTION_LIST_OWNER_ITEMS, w.b, nowMs);
}

bool GameSession::bidAuction(const AuctionItem& a, uint32_t price, uint32_t nowMs) {
    if (!auction.open) return false;
    if (a.owner == self_) { fail("You cannot bid on your own auction.", nowMs); return false; }
    if (price > money()) { fail("You don't have enough money.", nowMs); return false; }
    if (price < a.nextBid() && !(a.buyout && price >= a.buyout)) { fail("Your bid is too low.", nowMs); return false; }
    Writer w;
    w.u64(auction.npc);
    w.u32(a.id);
    w.u32(price);
    send(CMSG_AUCTION_PLACE_BID, w.b, nowMs);
    return true;
}

bool GameSession::sellAuction(const InvItem& it, uint32_t bid, uint32_t buyout, uint32_t minutes, uint32_t nowMs) {
    if (!auction.open || !it.guid) return false;
    if (!bid) { fail("Set a starting bid.", nowMs); return false; }
    if (buyout && buyout < bid) { fail("The buyout must not be below the bid.", nowMs); return false; }
    if (minutes != 120 && minutes != 480 && minutes != 1440) minutes = 480;
    Writer w;
    w.u64(auction.npc);
    w.u64(it.guid);
    w.u32(bid);
    w.u32(buyout);
    w.u32(minutes);
    send(CMSG_AUCTION_SELL_ITEM, w.b, nowMs);
    return true;
}

void GameSession::cancelAuction(uint32_t auctionId, uint32_t nowMs) {
    if (!auction.open) return;
    Writer w;
    w.u64(auction.npc);
    w.u32(auctionId);
    send(CMSG_AUCTION_REMOVE_ITEM, w.b, nowMs);
}

bool GameSession::handleAuction(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case MSG_AUCTION_HELLO: {
            uint64_t npc = r.u64();
            uint32_t house = r.u32();
            if (r.bad) return true;
            closeGossip();
            auction = AuctionWindow();
            auction.open = true;
            auction.npc = npc;
            auction.house = house;
            return true;
        }
        case SMSG_AUCTION_LIST_RESULT: case SMSG_AUCTION_OWNER_LIST_RESULT: {
            uint32_t n = r.u32();
            std::vector<AuctionItem> list;
            for (uint32_t i = 0; i < n && i < 50 && !r.bad; ++i) {
                AuctionItem a;
                if (readAuction(r, a)) list.push_back(a);
            }
            uint32_t total = r.u32();
            for (const AuctionItem& a : list) itemInfo(a.item);
            if (op == SMSG_AUCTION_LIST_RESULT) { auction.results = list; auction.total = r.bad ? (uint32_t)list.size() : total; auction.waiting = false; }
            else { auction.owned = list; auction.ownedTotal = r.bad ? (uint32_t)list.size() : total; }
            return true;
        }
        case SMSG_AUCTION_COMMAND_RESULT: {
            uint32_t id = r.u32(), action = r.u32(), err = r.u32();
            if (r.bad) return true;
            auction.lastAction = action;
            auction.lastError = err;
            if (err) { fail(auctionError(err), nowMs); return true; }
            if (action == 0) addLocal("Auction created.", nowMs);
            else if (action == 1) {
                addLocal("Auction cancelled.", nowMs);
                auction.owned.erase(std::remove_if(auction.owned.begin(), auction.owned.end(), [&](const AuctionItem& a) { return a.id == id; }),
                                    auction.owned.end());
            } else if (action == 2) {
                addLocal("Bid accepted.", nowMs);
                for (AuctionItem& a : auction.results)
                    if (a.id == id) { a.bidder = self_; }
            }
            return true;
        }
        case SMSG_AUCTION_BIDDER_NOTIFICATION: {
            r.u32();
            r.u32();
            r.u64();
            uint32_t bid = r.u32();
            r.u32();
            uint32_t item = r.u32();
            if (r.bad) return true;
            addLocal((bid ? "You have been outbid on " : "You won an auction for ") + itemName(item) + ".", nowMs);
            return true;
        }
        case SMSG_AUCTION_OWNER_NOTIFICATION: {
            r.u32();
            uint32_t bid = r.u32();
            r.u32();
            r.u64();
            uint32_t item = r.u32();
            if (r.bad) return true;
            addLocal(std::string(bid ? "Your auction of " : "Your auction of ") + itemName(item) + (bid ? " sold." : " got a bid."), nowMs);
            return true;
        }
        case SMSG_AUCTION_REMOVED_NOTIFICATION:
            return true;
        default:
            return false;
    }
}

}  // namespace w3n
