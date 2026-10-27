// Mailboxes (UI stage 6). Formats: vMaNGOS Handlers/MailHandler.cpp (SMSG_MAIL_LIST_RESULT of 1.12: one item per
// letter, the mail template id after the expiry), Server/Packets/Mail.cpp (CMSG_SEND_MAIL and the mailbox actions,
// SMSG_SEND_MAIL_RESULT), Server/Packets/Misc.cpp (CMSG_ITEM_TEXT_QUERY: text id, mail id, 0).
#include <algorithm>

#include "game.hpp"
#include "wire.hpp"

namespace w3n {

namespace {
enum : uint16_t {
    CMSG_SEND_MAIL = 0x238, SMSG_SEND_MAIL_RESULT = 0x239, CMSG_GET_MAIL_LIST = 0x23A, SMSG_MAIL_LIST_RESULT = 0x23B,
    CMSG_ITEM_TEXT_QUERY = 0x243, SMSG_ITEM_TEXT_QUERY_RESPONSE = 0x244, CMSG_MAIL_TAKE_MONEY = 0x245,
    CMSG_MAIL_TAKE_ITEM = 0x246, CMSG_MAIL_MARK_AS_READ = 0x247, CMSG_MAIL_DELETE = 0x249, SMSG_RECEIVED_MAIL = 0x285,
};
constexpr uint32_t kPostage = 30;   // copper per letter

const char* mailError(uint32_t e) {
    switch (e) {
        case 1: return "You cannot carry that";
        case 2: return "You cannot send mail to yourself";
        case 3: return "You don't have enough money";
        case 4: return "Recipient not found";
        case 5: return "You can't send mail to the other faction";
        case 15: return "The recipient's mailbox is full";
        default: return "The mail could not be sent";
    }
}
}  // namespace

void GameSession::openMailbox(uint64_t box, uint32_t nowMs) {
    mail.open = true;
    mail.box = box;
    Writer w;
    w.u64(box);
    send(CMSG_GET_MAIL_LIST, w.b, nowMs);
    newMail = false;
}

const std::string* GameSession::mailText(uint32_t textId, uint32_t mailId, uint32_t nowMs) {
    if (!textId) return nullptr;
    auto it = mailTexts_.find(textId);
    if (it != mailTexts_.end()) return &it->second;
    if (mailTextAsked_.insert(textId).second) {
        Writer w;
        w.u32(textId);
        w.u32(mailId);
        w.u32(0);
        send(CMSG_ITEM_TEXT_QUERY, w.b, nowMs);
    }
    return nullptr;
}

void GameSession::takeMailMoney(uint32_t mailId, uint32_t nowMs) {
    Writer w;
    w.u64(mail.box);
    w.u32(mailId);
    send(CMSG_MAIL_TAKE_MONEY, w.b, nowMs);
}

void GameSession::takeMailItem(uint32_t mailId, uint32_t nowMs) {
    if (!freeBagSlots()) { addLocal("Your bags are full.", nowMs); return; }
    Writer w;
    w.u64(mail.box);
    w.u32(mailId);
    send(CMSG_MAIL_TAKE_ITEM, w.b, nowMs);
}

void GameSession::deleteMail(uint32_t mailId, uint32_t nowMs) {
    Writer w;
    w.u64(mail.box);
    w.u32(mailId);
    send(CMSG_MAIL_DELETE, w.b, nowMs);
}

void GameSession::markMailRead(uint32_t mailId, uint32_t nowMs) {
    for (MailEntry& m : mail.mails)
        if (m.id == mailId) {
            if (m.read()) return;
            m.flags |= 1;
        }
    Writer w;
    w.u64(mail.box);
    w.u32(mailId);
    send(CMSG_MAIL_MARK_AS_READ, w.b, nowMs);
}

bool GameSession::sendMail(const std::string& to, const std::string& subject, const std::string& body, uint64_t itemGuid,
                           uint32_t money_, uint32_t nowMs) {
    if (!mail.open) return false;
    if (to.empty()) { addLocal("Whom to send it to?", nowMs); return false; }
    if (subject.empty() && !itemGuid && !money_) { addLocal("A letter needs a subject.", nowMs); return false; }
    if ((uint64_t)money_ + kPostage > money()) { addLocal("You don't have enough money (postage is 30c).", nowMs); return false; }
    Writer w;
    w.u64(mail.box);
    w.cstr(to);
    w.cstr(subject.empty() ? std::string("(no subject)") : subject);
    w.cstr(body);
    w.u32(0);          // stationery
    w.u32(0);          // package
    w.u64(itemGuid);
    w.u32(money_);
    w.u32(0);          // cash on delivery
    w.u64(0);          // 1.10+: two constant zeros
    w.u8(0);
    send(CMSG_SEND_MAIL, w.b, nowMs);
    return true;
}

bool GameSession::handleMail(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_MAIL_LIST_RESULT: {
            uint8_t n = r.u8();
            std::vector<MailEntry> list;
            for (uint8_t i = 0; i < n && !r.bad; ++i) {
                MailEntry m;
                m.id = r.u32();
                m.type = r.u8();
                if (m.type == 0) m.sender = r.u64();
                else if (m.type != 5) m.senderEntry = r.u32();
                m.subject = r.cstr();
                m.textId = r.u32();
                r.u32();   // package
                m.stationery = r.u32();
                m.item = r.u32();
                r.u32(); r.u32(); r.u32();   // enchant, random property, suffix factor
                m.itemCount = r.u8();
                r.u32(); r.u32(); r.u32();   // charges, max durability, durability
                m.money = r.u32();
                m.cod = r.u32();
                m.flags = r.u32();
                m.expireDays = r.f32();
                r.u32();   // mail template
                if (!r.bad) list.push_back(m);
            }
            mail.mails = list;
            mail.listed = true;
            for (const MailEntry& m : mail.mails) {
                if (m.sender) playerName(m.sender, nowMs);
                if (m.item) itemInfo(m.item);
            }
            return true;
        }
        case SMSG_ITEM_TEXT_QUERY_RESPONSE: {
            uint32_t id = r.u32();
            std::string text = r.cstr();
            if (!r.bad || !text.empty()) mailTexts_[id] = text;
            return true;
        }
        case SMSG_SEND_MAIL_RESULT: {
            uint32_t id = r.u32(), action = r.u32(), err = r.u32();
            if (r.bad) return true;
            if (err) addLocal(mailError(err), nowMs);
            else if (action == 0) { ++mailsSent; addLocal("Mail sent.", nowMs); }
            else if (action == 4) mail.mails.erase(std::remove_if(mail.mails.begin(), mail.mails.end(), [&](const MailEntry& m) { return m.id == id; }),
                                                  mail.mails.end());
            if (!err && (action == 1 || action == 2))   // money / item taken: the letter changes
                for (MailEntry& m : mail.mails)
                    if (m.id == id) { if (action == 1) m.money = 0; else { m.item = 0; m.itemCount = 0; } }
            return true;
        }
        case SMSG_RECEIVED_MAIL:
            newMail = true;
            addLocal("You have new mail.", nowMs);
            sound("mail");
            return true;
        default:
            return false;
    }
}

}  // namespace w3n
