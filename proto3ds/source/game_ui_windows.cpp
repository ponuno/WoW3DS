// game_ui_windows.cpp — windows that open by themselves over the tabs (UI stages 5/6): death, loot, gossip, quest
// giver, vendor, trainer, tooltips, confirmations, bank, flight master, mailbox, auction house, trade.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "game_ui.hpp"
#include "text.hpp"
#include "ui_style.hpp"

using namespace ui;

namespace {

// WoW world axes: +x north, +y west.
const char* compass(float dx, float dy) {
    static const char* en[8] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
    static const char* ru[8] = {"север", "северо-восток", "восток", "юго-восток", "юг", "юго-запад", "запад", "северо-запад"};
    float a = atan2f(-dy, dx);
    int k = (int)floorf(a / 0.785398f + 0.5f);
    k = (k % 8 + 8) % 8;
    return g_lang ? ru[k] : en[k];
}

// Quest text: paragraphs kept, $B already turned into line breaks by the session.
std::vector<std::string> paragraphs(const std::string& text, int width, Font f) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string para = text.substr(a, b - a);
        if (para.empty()) {
            if (!out.empty() && !out.back().empty()) out.push_back("");
        } else {
            for (const std::string& l : HudList::wrap(para, width, f)) out.push_back(l);
        }
        a = b + 1;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::string timeLeft(uint32_t ms) {
    uint32_t m = ms / 60000;
    if (m >= 60 * 12) return g_lang ? "очень долго" : "very long";
    if (m >= 120) return g_lang ? "долго" : "long";
    if (m >= 30) return g_lang ? "средне" : "medium";
    return g_lang ? "коротко" : "short";
}

}  // namespace

// ------------------------------------------------------------------ death
void GameUi::drawDeath(w3n::GameSession& g, const Input& in) {
    window(8, kTop + 4, kTouchW - 16, kContentH - 8, true);
    const w3n::Life life = g.life();
    const bool forever = in.cfg && in.cfg->scheme == PadScheme::Forever;
    const char* useKey = forever ? " (Y)" : " (A)";
    char b[96];
    int y = kTop + 14;
    if (life == w3n::Life::Dead) {
        list_.textCentered(kTouchW / 2, y, tr("You have died.", "Вы погибли."), kRed, true, Font::Large);
        if (!g.resurrectOffer.active) {
            btn(70, y + 30, 180, 30, std::string(tr("Release Spirit", "Освободить дух")) + useKey, Act::Release, 0, Style::Primary);
            list_.textCentered(kTouchW / 2, y + 68, tr("You will return as a ghost at the nearest graveyard.",
                                                       "Вы вернётесь призраком на ближайшем кладбище."), kGrey);
        }
    } else if (life == w3n::Life::Ghost) {
        list_.textCentered(kTouchW / 2, y, tr("You are a ghost.", "Вы — призрак."), kCyan, true, Font::Large);
        float d = g.corpseDistance(in.pos);
        if (d >= 0) snprintf(b, sizeof b, tr("Your corpse: %.0f yd %s", "Ваше тело: %.0f м, %s"), d, compass(g.corpse.x - in.pos[0], g.corpse.y - in.pos[1]));
        else snprintf(b, sizeof b, "%s", g.corpse.known ? tr("Your corpse is on another map.", "Тело на другой карте.") : tr("Looking for your corpse...", "Поиск тела..."));
        list_.textCentered(kTouchW / 2, y + 22, b, kText, true, Font::Normal);
        if (g.canReclaim(in.pos, in.nowMs)) {
            btn(80, y + 42, 160, 30, std::string(tr("Resurrect", "Воскреснуть")) + useKey, Act::Reclaim, 0, Style::Primary);
        } else if (d >= 0 && d <= 39.0f) {
            snprintf(b, sizeof b, tr("You can resurrect in %u s", "Воскрешение через %u с"), (unsigned)((g.reclaimDelayLeftMs(in.nowMs) + 999) / 1000));
            list_.textCentered(kTouchW / 2, y + 50, b, kGrey, true, Font::Normal);
        } else {
            list_.textCentered(kTouchW / 2, y + 50, tr("Run back to your body (within 39 yd)", "Бегите к телу (ближе 39 м)"), kGrey);
        }
        list_.textCentered(kTouchW / 2, y + 80, std::string(tr("or talk to a spirit healer", "или поговорите с целителем душ")) + useKey, kGrey);
    }
    if (g.spiritHealerConfirm) {
        list_.textCentered(kTouchW / 2, y + 100, tr("Return to life now? You will suffer resurrection sickness.",
                                                    "Вернуться к жизни? Будет слабость после воскрешения."), kText);
        btn(40, y + 120, 110, 26, tr("Accept", "Принять"), Act::HealerYes, 0, Style::Primary);
        btn(170, y + 120, 110, 26, tr("Decline", "Отказать"), Act::HealerNo, 0, Style::Normal);
    } else if (g.resurrectOffer.active) {
        std::string who = g.resurrectOffer.name.empty() ? std::string(tr("Someone", "Кто-то")) : g.resurrectOffer.name;
        list_.textCentered(kTouchW / 2, y + 100, who + tr(" wants to resurrect you.", " хочет воскресить вас."), kText, true, Font::Normal);
        btn(40, y + 120, 110, 26, tr("Accept", "Принять"), Act::ResAccept, 0, Style::Primary);
        btn(170, y + 120, 110, 26, tr("Decline", "Отказать"), Act::ResDecline, 0, Style::Normal);
    }
}

// ------------------------------------------------------------------ loot
void GameUi::drawLoot(w3n::GameSession& g) {
    window(4, kTop + 2, kTouchW - 8, kContentH - 4, true);
    const w3n::LootWindow& w = g.lootWindow;
    int y = kTop + 8;
    if (w.money) {
        list_.rect(10, y, kTouchW - 20, 24, kRow);
        ui::Image coin = assets().namedIcon("money");
        iconBox(12, y + 1, 21, coin, tr("Money", "Деньги"), kGold);
        money(kTouchW - 16, y + 5, w.money, Font::Normal);
        region(10, y, kTouchW - 20, 24, Act::LootMoney, 0);
        y += 26;
    }
    for (size_t i = 0; i < w.items.size() && y + 24 <= kTabY - 30; ++i) {
        const w3n::LootSlot& s = w.items[i];
        const w3n::ItemInfo* info = g.itemInfo(s.itemId);
        list_.rect(10, y, kTouchW - 20, 24, i % 2 ? kRowAlt : kRow);
        iconBox(12, y + 1, 21, itemIcon(g, s.itemId), g.itemName(s.itemId), qualityColor(info ? info->quality : 1));
        std::string nm = g.itemName(s.itemId) + (s.count > 1 ? " x" + std::to_string(s.count) : std::string());
        list_.textFit(38, y + 4, nm, kTouchW - 60, qualityColor(info ? info->quality : 1), true, Font::Normal);
        Button& b = region(10, y, kTouchW - 20, 24, Act::LootItem, (int)i);
        b.infoKind = 1;
        b.infoId = s.itemId;
        y += 26;
    }
    if (!w.money && w.items.empty()) list_.text(14, y + 4, tr("(empty)", "(пусто)"), kGrey, true, Font::Normal);
    btn(10, kTabY - 28, 140, 24, tr("Take all", "Взять всё"), Act::LootAll, 0, Style::Primary);
    btn(kTouchW - 100, kTabY - 28, 90, 24, tr("Close", "Закрыть"), Act::LootClose, 0, Style::Normal);
}

// ------------------------------------------------------------------ gossip
void GameUi::drawGossip(w3n::GameSession& g) {
    window(4, kTop + 2, kTouchW - 8, kContentH - 4, true);
    const w3n::GossipMenu& m = g.gossip;
    int y = kTop + 8;
    if (!m.text.empty()) {
        std::vector<std::string> lines = paragraphs(m.text, kTouchW - 24, Font::Small);
        for (size_t i = 0; i < lines.size() && i < 4; ++i) { list_.text(12, y, lines[i], kText); y += 12; }
        y += 4;
    }
    for (size_t i = 0; i < m.options.size() && y + 22 <= kTabY - 30; ++i) {
        const w3n::GossipOption& o = m.options[i];
        list_.rect(10, y, kTouchW - 20, 22, kRow);
        ui::Image ic = assets().sprite(("gossip_" + std::to_string(std::min<int>(o.icon, 9))).c_str());
        if (ic.ok()) list_.image(13, y + 3, 16, 16, ic);
        else list_.text(15, y + 5, "◆", kGold);
        list_.textFit(34, y + 3, o.text, kTouchW - 50, kText, true, Font::Normal);
        region(10, y, kTouchW - 20, 22, Act::GossipOption, (int)i);
        y += 24;
    }
    for (size_t i = 0; i < m.quests.size() && y + 22 <= kTabY - 30; ++i) {
        const w3n::GossipQuest& q = m.quests[i];
        list_.rect(10, y, kTouchW - 20, 22, kRow);
        const bool grey = w3n::questMarkGrey(q.icon);
        const char* mark = w3n::questMarkGlyph(q.icon);
        ui::Image ic = assets().sprite(mark[0] == '?' ? "quest_done" : "quest_new");
        if (ic.ok() && !grey) list_.image(13, y + 3, 16, 16, ic);
        else list_.text(16, y + 2, mark[0] ? mark : "!", grey ? kGrey : kGold, true, Font::Normal);
        list_.textFit(34, y + 3, q.title + " [" + std::to_string(q.level) + "]", kTouchW - 50, grey ? kGrey : kYellow, true, Font::Normal);
        region(10, y, kTouchW - 20, 22, Act::GossipQuest, (int)i);
        y += 24;
    }
    btn(kTouchW - 110, kTabY - 28, 100, 24, tr("Goodbye", "До свидания"), Act::GossipClose, 0, Style::Normal, Font::Small);
}

// ------------------------------------------------------------------ vendor
void GameUi::drawVendor(w3n::GameSession& g, const Input& in) {
    (void)in;
    const w3n::VendorWindow& v = g.vendor;
    const char* modes[3] = {tr("Buy", "Купить"), tr("Sell", "Продать"), tr("Buyback", "Выкуп")};
    for (int m = 0; m < 3; ++m) btn(2 + m * 66, kTop + 2, 64, 19, modes[m], Act::VendorMode, m, vendorMode_ == m ? Style::ToggleOn : Style::Toggle, Font::Small);
    if (v.canRepair) btn(202, kTop + 2, 60, 19, tr("Repair", "Ремонт"), Act::VendorRepair, 0, Style::Normal, Font::Small);
    btn(kTouchW - 52, kTop + 2, 50, 19, "✕", Act::VendorClose, 0, Style::Normal, Font::Small);
    const int rows = 5, top = kTop + 24, rowH = 25, by = kTabY - 24;
    auto pageOf = [&](int n) {
        int pages = std::max(1, (n + rows - 1) / rows);
        vendorPage_ = std::min(vendorPage_, pages - 1);
        if (vendorSel_ >= n) vendorSel_ = -1;
        return pages;
    };
    auto rowBox = [&](int r, int idx, uint32_t entry, uint32_t count, uint32_t price, bool dim, const char* extra, bool showPrice) {
        const int y = top + r * rowH;
        const bool sel = idx == vendorSel_;
        list_.rect(2, y, kTouchW - 4, rowH - 2, sel ? kRowSel : r % 2 ? kRowAlt : kRow);
        if (sel) list_.outline(2, y, kTouchW - 4, rowH - 2, kGold);
        const w3n::ItemInfo* info = entry ? g.itemInfo(entry) : nullptr;
        iconBox(4, y + 1, 21, itemIcon(g, entry), g.itemName(entry), qualityColor(info ? info->quality : 1), dim);
        std::string nm = entry ? g.itemName(entry) : std::string("...");
        if (count > 1) nm += " x" + std::to_string(count);
        int pw = showPrice ? money(kTouchW - 6, y + 5, price, Font::Small) : HudList::width("-");
        if (!showPrice) list_.textRight(kTouchW - 6, y + 5, "-", kGrey);
        if (extra && extra[0]) list_.textRight(kTouchW - 10 - pw, y + 5, extra, kGrey);
        list_.textFit(30, y + 4, nm, kTouchW - 60 - pw - (extra ? HudList::width(extra) + 4 : 0), dim ? kGrey : qualityColor(info ? info->quality : 1), false, Font::Normal);
        Button& b = region(2, y, kTouchW - 4, rowH - 2, Act::VendorPick, idx);
        b.infoKind = 1;
        b.infoId = entry;
    };
    if (vendorMode_ == 0) {
        const int count = (int)v.items.size();
        const int pages = pageOf(count);
        if (v.noInventory || !count) list_.text(10, top + 6, tr("This vendor has nothing to sell.", "Торговцу нечего продать."), kGrey, true, Font::Normal);
        for (int r = 0; r < rows; ++r) {
            int idx = vendorPage_ * rows + r;
            if (idx >= count) break;
            const w3n::VendorItem& it = v.items[idx];
            char stock[16] = "";
            if (!it.unlimited()) snprintf(stock, sizeof stock, "(%u)", (unsigned)it.stock);
            bool cant = (!it.unlimited() && !it.stock) || it.price > g.money();
            rowBox(r, idx, it.entry, it.buyCount, it.price, cant, stock, true);
        }
        if (vendorSel_ >= 0) {
            const w3n::VendorItem& it = v.items[vendorSel_];
            btn(2, by, 70, 22, tr("Buy", "Купить"), Act::VendorBuy, 1, it.price <= g.money() ? Style::Primary : Style::Disabled, Font::Small);
            if (it.unlimited() || it.stock >= 5) btn(76, by, 64, 22, tr("Buy 5", "Купить 5"), Act::VendorBuy, 5, Style::Normal, Font::Small);
            btn(144, by, 52, 22, tr("Info", "Инфо"), Act::ItemInfo, 1, Style::Normal, Font::Small);
        } else {
            list_.text(6, by + 6, tr("tap an item", "выберите товар"), kGrey);
        }
        pager(by, vendorPage_, pages);
    } else if (vendorMode_ == 1) {
        std::vector<w3n::InvItem> items = sellable(g);
        const int count = (int)items.size();
        const int pages = pageOf(count);
        if (!count) list_.text(10, top + 6, tr("Your bags are empty.", "Ваши сумки пусты."), kGrey, true, Font::Normal);
        for (int r = 0; r < rows; ++r) {
            int idx = vendorPage_ * rows + r;
            if (idx >= count) break;
            const w3n::InvItem& it = items[idx];
            const w3n::ItemInfo* info = g.itemInfo(it.entry);
            bool unsellable = info && !info->missing && info->sellPrice == 0;
            rowBox(r, idx, it.entry, it.count, g.sellValue(it), false, "", !unsellable);
        }
        if (vendorSel_ >= 0) {
            btn(2, by, 70, 22, tr("Sell", "Продать"), Act::VendorSell, 0, Style::Primary, Font::Small);
            btn(76, by, 52, 22, tr("Info", "Инфо"), Act::ItemInfo, 2, Style::Normal, Font::Small);
        } else {
            list_.text(6, by + 6, tr("tap an item to sell it", "выберите, что продать"), kGrey);
        }
        pager(by, vendorPage_, pages);
    } else {
        std::vector<w3n::BuybackItem> bb = g.buybackItems();
        const int count = (int)bb.size();
        pageOf(count);
        if (!count) list_.text(10, top + 6, tr("Nothing sold here yet.", "Вы ещё ничего не продали."), kGrey, true, Font::Normal);
        for (int r = 0; r < rows && r < count; ++r) rowBox(r, r, bb[r].entry, bb[r].count, bb[r].price, bb[r].price > g.money(), "", true);
        if (vendorSel_ >= 0) btn(2, by, 90, 22, tr("Buy back", "Выкупить"), Act::VendorBuyback, 0, Style::Primary, Font::Small);
        else list_.text(6, by + 6, tr("tap an item to buy it back", "выберите, что выкупить"), kGrey);
    }
}

// ------------------------------------------------------------------ trainer
void GameUi::drawTrainer(w3n::GameSession& g) {
    const w3n::TrainerWindow& t = g.trainer;
    std::vector<int> rows = trainerRows(g);
    btn(2, kTop + 2, 110, 19, trainerAll_ ? tr("Show: all", "Показать: все") : tr("Show: learnable", "Показать: доступные"),
        Act::TrainerFilter, 0, Style::Toggle, Font::Small);
    btn(kTouchW - 52, kTop + 2, 50, 19, "✕", Act::TrainerClose, 0, Style::Normal, Font::Small);
    const int per = 5, top = kTop + 24, rowH = 25, by = kTabY - 24;
    int pages = std::max(1, ((int)rows.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    if (rows.empty()) list_.text(10, top + 6, t.spells.empty() ? tr("I can teach you nothing.", "Мне нечему вас научить.")
                                                               : tr("You know everything I can teach.", "Вы знаете всё, чему я учу."), kGrey, true, Font::Normal);
    for (int r = 0; r < per; ++r) {
        int k = page_ * per + r;
        if (k >= (int)rows.size()) break;
        const w3n::TrainerSpell& sp = t.spells[rows[k]];
        const int y = top + r * rowH;
        const bool sel = rows[k] == trainerSel_;
        list_.rect(2, y, kTouchW - 4, rowH - 2, sel ? kRowSel : r % 2 ? kRowAlt : kRow);
        if (sel) list_.outline(2, y, kTouchW - 4, rowH - 2, kGold);
        uint32_t c = sp.state == 0 ? (sp.cost > g.money() ? kRed : kGreen) : sp.state == 1 ? kRed : kGrey;
        iconBox(4, y + 1, 21, spellIcon(sp.spell), g.spellNames.name(sp.spell), kFrameDim, sp.state != 0);
        int pw = sp.cost ? money(kTouchW - 6, y + 5, sp.cost, Font::Small) : HudList::width(tr("free", "даром"));
        if (!sp.cost) list_.textRight(kTouchW - 6, y + 5, tr("free", "даром"), kGreen);
        std::string lvl = "L" + std::to_string(sp.reqLevel);
        list_.textRight(kTouchW - 12 - pw, y + 5, lvl, sp.state == 1 ? kRed : kGrey);
        list_.textFit(30, y + 4, spellLabel(g, sp.spell), kTouchW - 70 - pw, c, false, Font::Normal);
        Button& b = region(2, y, kTouchW - 4, rowH - 2, Act::TrainerPick, rows[k]);
        b.infoKind = 2;
        b.infoId = sp.spell;
    }
    if (trainerSel_ >= 0 && trainerSel_ < (int)t.spells.size() && t.spells[trainerSel_].state == 0)
        btn(2, by, 100, 22, tr("Train", "Обучиться"), Act::TrainerTrain, 0, Style::Primary, Font::Small);
    else
        list_.text(6, by + 6, trainerSel_ >= 0 ? tr("not available yet", "пока недоступно") : tr("tap a spell", "выберите заклинание"), kGrey);
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ tooltips
void GameUi::drawItemInfo(w3n::GameSession& g) {
    window(4, kTop + 2, kTouchW - 8, kContentH - 4, true);
    const w3n::ItemInfo* info = g.itemInfo(infoEntry_);
    if (!info) {
        list_.text(14, kTop + 14, tr("(asking the server...)", "(запрос к серверу...)"), kGrey, true, Font::Normal);
    } else {
        iconBox(12, kTop + 10, 32, itemIcon(g, infoEntry_), info->name, qualityColor(info->quality));
        std::vector<std::string> lines = w3n::itemTooltip(*info, infoItem_.guid ? &infoItem_ : nullptr, g.spellNames);
        int y = kTop + 10;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& l = lines[i];
            uint32_t c = i == 0 ? qualityColor(info->quality)
                       : l.rfind("Use:", 0) == 0 || l.rfind("Equip:", 0) == 0 || l.rfind("Chance", 0) == 0 ? kGreen
                       : l[0] == '"' ? kYellow : l.rfind("Sell", 0) == 0 || l.rfind("No sell", 0) == 0 ? kGrey
                       : l.rfind("Requires", 0) == 0 && g.selfObject() && info->requiredLevel > g.selfObject()->level() ? kRed : kText;
            Font f = i == 0 ? Font::Normal : Font::Small;
            for (const std::string& w : HudList::wrap(l, kTouchW - 64, f)) {
                if (y + HudList::lineHeight(f) > kTabY - 30) break;
                list_.text(50, y, w, c, true, f);
                y += HudList::lineHeight(f);
            }
        }
    }
    btn(10, kTabY - 28, 90, 24, tr("Back", "Назад"), Act::InfoBack, 0, Style::Normal);
}

void GameUi::drawSpellInfo(w3n::GameSession& g, const Input& in) {
    window(4, kTop + 2, kTouchW - 8, kContentH - 4, true);
    const uint32_t s = infoSpell_;
    iconBox(12, kTop + 10, 32, spellIcon(s), g.spellNames.name(s), kFrame);
    list_.textFit(52, kTop + 10, g.spellNames.name(s), kTouchW - 64, kGold, true, Font::Large);
    std::string rk = g.spellNames.rank(s);
    if (!rk.empty()) list_.text(52, kTop + 30, rk, kGrey, true, Font::Normal);
    int y = kTop + 50;
    uint32_t cd = g.cooldownLeftMs(s, in.nowMs);
    char b[64];
    if (cd) { snprintf(b, sizeof b, tr("Cooldown: %.1f s", "Перезарядка: %.1f с"), cd / 1000.0); list_.text(52, y, b, kYellow); y += 14; }
    // our aura of this spell: time left, cancel
    const w3n::WorldObject* me = g.selfObject();
    bool aura = false, cancelable = false;
    if (me)
        for (const w3n::AuraView& a : g.auras(*me, in.nowMs))
            if (a.spell == s) {
                aura = true;
                cancelable = a.cancelable && !a.debuff;
                if (a.leftMs) { snprintf(b, sizeof b, tr("%u s left", "Осталось %u с"), (unsigned)((a.leftMs + 999) / 1000)); list_.text(52, y, b, kText); y += 14; }
                list_.text(52, y, a.debuff ? tr("Debuff", "Отрицательный эффект") : tr("Buff", "Положительный эффект"), a.debuff ? kRed : kGreen);
                y += 14;
            }
    const bool known = g.knowsSpell(s) || s == w3n::kSpellAttack;
    if (known) list_.text(52, y, tr("In your spell book: drag it to a button or tap Bind.", "В вашей книге: перетащите на кнопку или нажмите «Привязать»."), kGrey);
    const int by = kTabY - 28;
    btn(10, by, 90, 24, tr("Back", "Назад"), Act::InfoBack, 0, Style::Normal);
    if (aura && cancelable) { buffSel_ = s; btn(110, by, 120, 24, tr("Cancel buff", "Снять эффект"), Act::BuffCancel, 0, Style::Primary, Font::Small); }
    else if (known) btn(110, by, 120, 24, tr("Bind", "Привязать"), Act::BindSpell, (int)s, Style::Primary, Font::Small);
}

void GameUi::drawConfirm() {
    window(16, kTop + 26, kTouchW - 32, 120, true);
    int y = kTop + 40;
    for (const std::string& l : HudList::wrap(confirmText_, kTouchW - 60, Font::Normal)) {
        if (y > kTop + 90) break;
        list_.textCentered(kTouchW / 2, y, l, kText, true, Font::Normal);
        y += 16;
    }
    btn(40, kTop + 108, 110, 28, tr("Yes", "Да"), Act::ConfirmYes, 0, Style::Danger);
    btn(170, kTop + 108, 110, 28, tr("No", "Нет"), Act::ConfirmNo, 0, Style::Normal);
}

// ------------------------------------------------------------------ quest giver
void GameUi::drawQuest(w3n::GameSession& g) {
    const w3n::QuestDialog& d = g.questDialog;
    const int by = kTabY - 24;
    window(2, kTop + 2, kTouchW - 4, kContentH - 4, true);
    if (d.kind == w3n::QuestDialog::Kind::List) {
        int y = kTop + 8;
        std::vector<std::string> greet = paragraphs(d.text, kTouchW - 24, Font::Small);
        for (size_t i = 0; i < greet.size() && i < 3; ++i) { list_.text(10, y, greet[i], kText); y += 12; }
        y += 4;
        for (size_t i = 0; i < d.list.size() && y + 22 <= by - 2; ++i) {
            const w3n::GossipQuest& q = d.list[i];
            const bool grey = w3n::questMarkGrey(q.icon);
            list_.rect(8, y, kTouchW - 16, 22, kRow);
            list_.text(13, y + 2, w3n::questMarkGlyph(q.icon)[0] ? w3n::questMarkGlyph(q.icon) : "!", grey ? kGrey : kGold, true, Font::Normal);
            list_.textFit(28, y + 3, q.title + " [" + std::to_string(q.level) + "]", kTouchW - 44, grey ? kGrey : kYellow, true, Font::Normal);
            region(8, y, kTouchW - 16, 22, Act::QuestPick, (int)i);
            y += 24;
        }
        btn(kTouchW - 110, by, 100, 22, tr("Goodbye", "До свидания"), Act::QuestClose, 0, Style::Normal, Font::Small);
        return;
    }
    // the text area: title, text, objectives / wanted items, rewards; scrolling by lines
    struct L { uint32_t color; std::string text; Font f; int act = -1, arg = -1; uint32_t item = 0; };
    std::vector<L> body;
    for (const std::string& l : HudList::wrap(d.title, kTouchW - 24, Font::Normal)) body.push_back({kGold, l, Font::Normal});
    for (const std::string& l : paragraphs(d.text, kTouchW - 24, Font::Small)) body.push_back({kText, l, Font::Small});
    if (d.kind == w3n::QuestDialog::Kind::Details && !d.objectives.empty()) {
        body.push_back({kGold, tr("Quest Objectives", "Цели задания"), Font::Normal});
        for (const std::string& l : paragraphs(d.objectives, kTouchW - 24, Font::Small)) body.push_back({kText, l, Font::Small});
    }
    if (d.kind == w3n::QuestDialog::Kind::RequestItems) {
        if (!d.required.empty() || d.requiredMoney) body.push_back({kGold, tr("Required items", "Нужные предметы"), Font::Normal});
        std::vector<w3n::InvItem> inv = g.inventory();
        for (const w3n::QuestItemLine& it : d.required) {
            uint32_t have = 0;
            for (const w3n::InvItem& x : inv) if (x.entry == it.item) have += x.count;
            L l{have >= it.count ? kGreen : kRed, g.itemName(it.item) + " " + std::to_string(std::min(have, it.count)) + "/" + std::to_string(it.count), Font::Small};
            l.item = it.item;
            body.push_back(l);
        }
        if (d.requiredMoney) body.push_back({kText, std::string(tr("Money: ", "Деньги: ")) + w3n::moneyText(d.requiredMoney), Font::Small});
    }
    if (d.kind != w3n::QuestDialog::Kind::RequestItems && (!d.choices.empty() || !d.rewards.empty() || d.money)) {
        body.push_back({kGold, d.choices.empty() ? tr("You will receive:", "Вы получите:") : tr("Choose one reward:", "Выберите награду:"), Font::Normal});
        for (size_t i = 0; i < d.choices.size(); ++i) {
            const w3n::ItemInfo* info = g.itemInfo(d.choices[i].item);
            const bool pick = d.kind == w3n::QuestDialog::Kind::OfferReward;
            L l{qualityColor(info ? info->quality : 1), g.itemName(d.choices[i].item) + (d.choices[i].count > 1 ? " x" + std::to_string(d.choices[i].count) : std::string()),
                Font::Normal, pick ? (int)Act::QuestReward : -1, pick ? (int)i : -1, d.choices[i].item};
            body.push_back(l);
        }
        if (!d.choices.empty() && !d.rewards.empty()) body.push_back({kGold, tr("You will also receive:", "А также:"), Font::Normal});
        for (const w3n::QuestItemLine& it : d.rewards) {
            const w3n::ItemInfo* info = g.itemInfo(it.item);
            L l{qualityColor(info ? info->quality : 1), g.itemName(it.item) + (it.count > 1 ? " x" + std::to_string(it.count) : std::string()), Font::Normal};
            l.item = it.item;
            body.push_back(l);
        }
        if (d.money) body.push_back({kText, std::string(tr("Money: ", "Деньги: ")) + w3n::moneyText(d.money), Font::Small});
    }
    const int top = kTop + 8, bottom = by - 4;
    auto lineH = [&](const L& l) { return l.item ? 24 : HudList::lineHeight(l.f); };
    int hTotal = 0;
    for (const L& l : body) hTotal += lineH(l);
    int maxScroll = 0, acc = 0;
    while (maxScroll < (int)body.size() && hTotal - acc > bottom - top) acc += lineH(body[maxScroll++]);
    questScroll_ = std::min(questScroll_, maxScroll);
    int y = top;
    for (int i = questScroll_; i < (int)body.size(); ++i) {
        const L& l = body[i];
        int h = lineH(l);
        if (y + h > bottom) break;
        if (l.item) {   // an item line: icon, name; rewards to choose are buttons
            const bool picked = l.act == (int)Act::QuestReward && l.arg == rewardPick_;
            list_.rect(10, y, kTouchW - 20, 22, picked ? kRowSel : kRow);
            if (picked) list_.outline(10, y, kTouchW - 20, 22, kGold);
            iconBox(12, y + 1, 20, itemIcon(g, l.item), l.text, l.color);
            list_.textFit(38, y + (l.f == Font::Small ? 5 : 3), l.text, kTouchW - 56, l.color, true, l.f);
            Button& b = region(10, y, kTouchW - 20, 22, l.act >= 0 ? (Act)l.act : Act::ItemInfo, l.act >= 0 ? l.arg : 999);
            b.infoKind = 1;
            b.infoId = l.item;
        } else {
            list_.text(10, y, l.text, l.color, true, l.f);
        }
        y += h;
    }
    switch (d.kind) {
        case w3n::QuestDialog::Kind::Details: btn(4, by, 96, 22, tr("Accept", "Принять"), Act::QuestAccept, 0, Style::Primary, Font::Small); break;
        case w3n::QuestDialog::Kind::RequestItems:
            if (d.completable) btn(4, by, 96, 22, tr("Continue", "Далее"), Act::QuestContinue, 0, Style::Primary, Font::Small);
            else list_.text(8, by + 6, tr("not done yet", "ещё не выполнено"), kGrey);
            break;
        case w3n::QuestDialog::Kind::OfferReward:
            btn(4, by, 96, 22, tr("Complete", "Завершить"), Act::QuestComplete, 0, d.choices.empty() || rewardPick_ >= 0 ? Style::Primary : Style::Toggle, Font::Small);
            break;
        default: break;
    }
    if (maxScroll) {
        btn(110, by, 46, 22, "▲", Act::QuestScroll, -3, questScroll_ ? Style::Normal : Style::Disabled);
        btn(160, by, 46, 22, "▼", Act::QuestScroll, 3, questScroll_ < maxScroll ? Style::Normal : Style::Disabled);
    }
    btn(kTouchW - 96, by, 92, 22, d.kind == w3n::QuestDialog::Kind::Details ? tr("Decline", "Отказаться") : tr("Close", "Закрыть"),
        Act::QuestClose, 0, Style::Normal, Font::Small);
}

// ------------------------------------------------------------------ bank: the bank's items or the bags', a grid
void GameUi::drawBank(w3n::GameSession& g, const Input& in) {
    (void)in;
    btn(2, kTop + 2, 70, 19, tr("Bank", "Банк"), Act::BankMode, 0, bankMode_ == 0 ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(74, kTop + 2, 70, 19, tr("Bags", "Сумки"), Act::BankMode, 1, bankMode_ == 1 ? Style::ToggleOn : Style::Toggle, Font::Small);
    char b[48];
    snprintf(b, sizeof b, "%s %d/6", tr("bag slots", "ячейки сумок"), g.bankBagSlots());
    list_.text(150, kTop + 6, b, kGrey);
    btn(kTouchW - 52, kTop + 2, 50, 19, "✕", Act::BankClose, 0, Style::Normal, Font::Small);
    std::vector<w3n::InvItem> items = bankMode_ == 0 ? g.bankItems() : sellable(g);
    if (bankMode_ == 0)   // the bank bags themselves are not moved from here
        items.erase(std::remove_if(items.begin(), items.end(), [](const w3n::InvItem& it) {
                        return it.bag == w3n::kBagSelf && it.slot >= w3n::kSlotBankBagFirst; }), items.end());
    const int cols = 7, per = 21, cell = 42;
    const int pages = std::max(1, ((int)items.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    if (bankSel_ >= (int)items.size()) bankSel_ = -1;
    if (items.empty()) list_.text(10, kTop + 30, bankMode_ == 0 ? tr("The bank is empty.", "В банке пусто.") : tr("Your bags are empty.", "Сумки пусты."), kGrey, true, Font::Normal);
    for (int k = 0; k < per; ++k) {
        int idx = page_ * per + k;
        if (idx >= (int)items.size()) break;
        const w3n::InvItem& it = items[idx];
        const int x = 6 + (k % cols) * cell, y = kTop + 24 + (k / cols) * cell;
        const w3n::ItemInfo* info = g.itemInfo(it.entry);
        iconBox(x + 1, y + 1, cell - 4, itemIcon(g, it.entry), g.itemName(it.entry), qualityColor(info ? info->quality : 1));
        if (it.count > 1) list_.textRight(x + cell - 4, y + cell - 14, std::to_string(it.count), kWhite, true);
        if (idx == bankSel_) list_.outline(x - 1, y - 1, cell, cell, kGold);
        Button& bt = region(x - 1, y - 1, cell, cell, Act::BankPick, idx);
        bt.infoKind = 4;
        bt.infoId = it.entry;
        bt.guid = it.guid;
    }
    const int by = kTabY - 24;
    if (bankSel_ >= 0)
        btn(2, by, 100, 22, bankMode_ == 0 ? tr("To the bags", "В сумки") : tr("To the bank", "В банк"),
            bankMode_ == 0 ? Act::BankWithdraw : Act::BankDeposit, 0, Style::Primary, Font::Small);
    if (g.bankBagSlots() < 6) btn(106, by, 104, 22, tr("Buy bag slot", "Купить ячейку"), Act::BankBuySlot, 0, Style::Normal, Font::Small);
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ flight master: the known nodes of this continent
void GameUi::drawTaxi(w3n::GameSession& g, const Input& in) {
    (void)in;
    const w3n::TaxiWindow& t = g.taxi;
    const w3n::TaxiNode* here = g.taxiTable.node(t.current);
    struct Row { uint32_t id; std::string name; uint32_t cost; };
    std::vector<Row> rows;
    for (uint32_t n = 1; n < 256; ++n) {
        if (!t.known(n) || n == t.current) continue;
        const w3n::TaxiNode* tn = g.taxiTable.node(n);
        if (tn && here && tn->map != here->map) continue;
        rows.push_back({n, tn ? tn->name : std::string(tr("Node ", "Точка ")) + std::to_string(n), g.taxiCost(n)});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.name < b.name; });
    list_.textFit(4, kTop + 4, std::string(tr("From: ", "Отсюда: ")) + (here ? here->name : std::to_string(t.current)), kTouchW - 60, kGold, true, Font::Normal);
    btn(kTouchW - 52, kTop + 2, 50, 19, "✕", Act::TaxiClose, 0, Style::Normal, Font::Small);
    const int per = 5, top = kTop + 24, rowH = 25, by = kTabY - 24;
    int pages = std::max(1, ((int)rows.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    if (rows.empty()) list_.text(10, top + 6, tr("You know no other flight path here.", "Других маршрутов вы не знаете."), kGrey, true, Font::Normal);
    for (int r = 0; r < per; ++r) {
        int k = page_ * per + r;
        if (k >= (int)rows.size()) break;
        const Row& row = rows[k];
        const int y = top + r * rowH;
        const bool sel = (int)row.id == taxiSel_;
        list_.rect(2, y, kTouchW - 4, rowH - 2, sel ? kRowSel : r % 2 ? kRowAlt : kRow);
        if (sel) list_.outline(2, y, kTouchW - 4, rowH - 2, kGold);
        int pw = row.cost ? money(kTouchW - 6, y + 5, row.cost, Font::Small) : 0;
        list_.textFit(8, y + 4, row.name, kTouchW - 24 - pw, row.cost > g.money() ? kGrey : kText, false, Font::Normal);
        region(2, y, kTouchW - 4, rowH - 2, Act::TaxiPick, (int)row.id);
    }
    if (taxiSel_ > 0) btn(2, by, 100, 22, tr("Fly", "Лететь"), Act::TaxiFly, 0, Style::Primary, Font::Small);
    else list_.text(6, by + 6, tr("tap a destination", "выберите пункт назначения"), kGrey);
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ mailbox: letters, one letter, a new letter
void GameUi::drawMail(w3n::GameSession& g, const Input& in) {
    const int by = kTabY - 24;
    const std::vector<w3n::MailEntry>& mails = g.mail.mails;
    if (mailSel_ >= (int)mails.size()) { mailSel_ = -1; if (mailView_ == 1) mailView_ = 0; }
    auto sender = [&](const w3n::MailEntry& m) {
        if (m.type == 0) { std::string n = g.playerName(m.sender, in.nowMs); return n.empty() ? std::string("...") : n; }
        if (m.type == 2) return std::string(tr("Auction house", "Аукцион"));
        return std::string(tr("Someone", "Отправитель"));
    };
    if (mailView_ == 2) {   // compose
        window(2, kTop + 2, kTouchW - 4, kContentH - 4, true);
        auto field = [&](int y, const char* label, const std::string& value, int arg) {
            list_.text(10, y + 5, label, kGold);
            btn(80, y, kTouchW - 92, 22, value.empty() ? "..." : value, Act::MailField, arg, Style::Toggle, Font::Small);
        };
        field(kTop + 8, tr("To", "Кому"), mailTo_, 0);
        field(kTop + 34, tr("Subject", "Тема"), mailSubject_, 1);
        field(kTop + 60, tr("Text", "Текст"), mailBody_, 2);
        field(kTop + 86, tr("Money", "Деньги"), mailMoney_ ? w3n::moneyText(mailMoney_) : std::string(), 3);
        std::vector<w3n::InvItem> items = sellable(g);
        std::string att = mailItem_ >= 0 && mailItem_ < (int)items.size() ? g.itemName(items[mailItem_].entry) : std::string(tr("(none: tap)", "(нет: нажмите)"));
        field(kTop + 112, tr("Item", "Предмет"), att, 4);
        btn(4, by, 96, 22, tr("Send", "Отправить"), Act::MailSend, 0, Style::Primary, Font::Small);
        list_.text(108, by + 6, tr("postage 30c", "сбор 30м"), kGrey);
        btn(kTouchW - 80, by, 76, 22, tr("Back", "Назад"), Act::MailBack, 0, Style::Normal, Font::Small);
        return;
    }
    if (mailView_ == 1 && mailSel_ >= 0) {   // one letter
        const w3n::MailEntry& m = mails[mailSel_];
        window(2, kTop + 2, kTouchW - 4, kContentH - 4, true);
        list_.textFit(10, kTop + 8, std::string(tr("From: ", "От: ")) + sender(m), kTouchW - 24, kGold);
        list_.textFit(10, kTop + 20, m.subject, kTouchW - 24, kText, true, Font::Normal);
        int y = kTop + 38;
        const std::string* text = g.mailText(m.textId, m.id, in.nowMs);
        if (m.textId && !text) { list_.text(10, y, tr("(loading the text...)", "(загрузка текста...)"), kGrey); y += 12; }
        if (text)
            for (const std::string& l : paragraphs(*text, kTouchW - 24, Font::Small)) {
                if (y > kTop + 110) break;
                list_.text(10, y, l, kText);
                y += 12;
            }
        int x = 4;
        if (m.money) { btn(x, by, 110, 22, std::string(tr("Take ", "Взять ")) + w3n::moneyText(m.money), Act::MailTakeMoney, 0, Style::Primary, Font::Small); x += 114; }
        if (m.item) {
            const w3n::ItemInfo* info = g.itemInfo(m.item);
            iconBox(10, kTop + 122, 24, itemIcon(g, m.item), g.itemName(m.item), qualityColor(info ? info->quality : 1));
            list_.textFit(40, kTop + 128, g.itemName(m.item) + (m.itemCount > 1 ? " x" + std::to_string(m.itemCount) : std::string()), 200,
                          qualityColor(info ? info->quality : 1));
            btn(x, by, 80, 22, tr("Take item", "Взять"), Act::MailTakeItem, 0, Style::Primary, Font::Small);
            x += 84;
        }
        if (m.cod) list_.text(10, kTop + 112, std::string(tr("Cash on delivery: ", "Наложенный платёж: ")) + w3n::moneyText(m.cod), kRed);
        btn(x, by, 64, 22, tr("Delete", "Удалить"), Act::MailDelete, 0, Style::Danger, Font::Small);
        btn(kTouchW - 70, by, 66, 22, tr("Back", "Назад"), Act::MailBack, 0, Style::Normal, Font::Small);
        return;
    }
    // the letters
    btn(kTouchW - 52, kTop + 2, 50, 19, "✕", Act::MailClose, 0, Style::Normal, Font::Small);
    char b[48];
    snprintf(b, sizeof b, "%s: %d", tr("Letters", "Писем"), (int)mails.size());
    list_.text(6, kTop + 6, g.mail.listed ? b : tr("Checking the mail...", "Проверка почты..."), kGold);
    const int per = 5, top = kTop + 24, rowH = 25;
    int pages = std::max(1, ((int)mails.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    if (g.mail.listed && mails.empty()) list_.text(10, top + 6, tr("No mail.", "Писем нет."), kGrey, true, Font::Normal);
    for (int r = 0; r < per; ++r) {
        int i = page_ * per + r;
        if (i >= (int)mails.size()) break;
        const w3n::MailEntry& m = mails[i];
        const int y = top + r * rowH;
        list_.rect(2, y, kTouchW - 4, rowH - 2, r % 2 ? kRowAlt : kRow);
        if (m.item) iconBox(4, y + 1, 21, itemIcon(g, m.item), g.itemName(m.item), kFrameDim);
        else { list_.rect(4, y + 1, 21, 21, m.read() ? kBgLight : hudRgba(90, 70, 30)); list_.textCentered(14, y + 5, "✉", kText); }
        list_.textFit(30, y + 1, m.subject.empty() ? "..." : m.subject, 190, m.read() ? kText : kGold, false, Font::Normal);
        list_.textFit(30, y + 13, sender(m), 150, kGrey, false);
        if (m.money) money(kTouchW - 6, y + 7, m.money, Font::Small);
        region(2, y, kTouchW - 4, rowH - 2, Act::MailPick, i);
    }
    btn(2, by, 100, 22, tr("New letter", "Написать"), Act::MailCompose, 0, Style::Primary, Font::Small);
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ auction house: browse / own auctions / sell
void GameUi::drawAuction(w3n::GameSession& g, const Input& in) {
    (void)in;
    const char* modes[3] = {tr("Browse", "Поиск"), tr("My auctions", "Мои лоты"), tr("Sell", "Продать")};
    for (int m = 0; m < 3; ++m) btn(2 + m * 80, kTop + 2, 78, 19, modes[m], Act::AuctionMode, m, auctionMode_ == m ? Style::ToggleOn : Style::Toggle, Font::Small);
    btn(kTouchW - 52, kTop + 2, 50, 19, "✕", Act::AuctionClose, 0, Style::Normal, Font::Small);
    const int per = 5, top = kTop + 46, rowH = 25, by = kTabY - 24;
    const w3n::AuctionWindow& a = g.auction;
    auto row = [&](int r, const w3n::AuctionItem& it, bool sel, Act act, int arg) {
        const int y = top + r * rowH;
        const w3n::ItemInfo* info = g.itemInfo(it.item);
        list_.rect(2, y, kTouchW - 4, rowH - 2, sel ? kRowSel : r % 2 ? kRowAlt : kRow);
        if (sel) list_.outline(2, y, kTouchW - 4, rowH - 2, kGold);
        iconBox(4, y + 1, 21, itemIcon(g, it.item), g.itemName(it.item), qualityColor(info ? info->quality : 1));
        if (it.count > 1) list_.textRight(26, y + 11, std::to_string(it.count), kWhite, true);
        list_.textFit(30, y + 1, g.itemName(it.item), 150, qualityColor(info ? info->quality : 1), false);
        list_.text(30, y + 12, timeLeft(it.timeLeftMs), kGrey, false);
        money(kTouchW - 6, y + 1, it.bid ? it.bid : it.startBid, Font::Small);
        if (it.buyout) money(kTouchW - 6, y + 12, it.buyout, Font::Small);
        Button& b = region(2, y, kTouchW - 4, rowH - 2, act, arg);
        b.infoKind = 1;
        b.infoId = it.item;
    };
    if (auctionMode_ == 0) {
        btn(2, kTop + 24, 200, 19, auctionQuery_.empty() ? tr("Search: everything (tap)", "Поиск: всё (нажмите)") : auctionQuery_, Act::AuctionSearch, 0, Style::Toggle, Font::Small);
        btn(206, kTop + 24, 64, 19, tr("Refresh", "Обновить"), Act::AuctionSearch, (int)a.page + 1, Style::Normal, Font::Small);
        if (a.waiting) list_.text(10, top + 6, tr("Searching...", "Поиск..."), kGrey, true, Font::Normal);
        else if (a.results.empty()) list_.text(10, top + 6, tr("Nothing found.", "Ничего не найдено."), kGrey, true, Font::Normal);
        int shown = std::min<int>((int)a.results.size(), per);
        const int sub = page_ % 10;   // 5 rows of the 50 the server sent
        for (int r = 0; r < shown; ++r) {
            int i = sub * per + r;
            if (i >= (int)a.results.size()) break;
            row(r, a.results[i], i == auctionSel_, Act::AuctionPick, i);
        }
        if (auctionSel_ >= 0 && auctionSel_ < (int)a.results.size()) {
            const w3n::AuctionItem& it = a.results[auctionSel_];
            btn(2, by, 92, 22, std::string(tr("Bid ", "Ставка ")) + w3n::moneyText(it.nextBid()), Act::AuctionBid, 0, Style::Normal, Font::Small);
            if (it.buyout) btn(98, by, 100, 22, std::string(tr("Buy ", "Выкуп ")) + w3n::moneyText(it.buyout), Act::AuctionBuyout, 0, Style::Primary, Font::Small);
        }
        int pages = std::max(1, ((int)a.results.size() + per - 1) / per);
        page_ = std::min(page_, pages - 1);
        pager(by, page_, pages);
        if (a.total > a.results.size()) {
            char b[48];
            snprintf(b, sizeof b, "%u %s", (unsigned)a.total, tr("in all", "всего"));
            list_.textRight(kTouchW - 6, kTop + 28, b, kGrey);
        }
        return;
    }
    if (auctionMode_ == 1) {
        if (a.owned.empty()) list_.text(10, top - 16, tr("You have no auctions here.", "У вас нет лотов."), kGrey, true, Font::Normal);
        int pages = std::max(1, ((int)a.owned.size() + per - 1) / per);
        page_ = std::min(page_, pages - 1);
        for (int r = 0; r < per; ++r) {
            int i = page_ * per + r;
            if (i >= (int)a.owned.size()) break;
            row(r, a.owned[i], i == auctionSel_, Act::AuctionPick, i);
        }
        if (auctionSel_ >= 0 && auctionSel_ < (int)a.owned.size() && !a.owned[auctionSel_].bid)
            btn(2, by, 110, 22, tr("Cancel auction", "Снять лот"), Act::AuctionCancel, auctionSel_, Style::Danger, Font::Small);
        pager(by, page_, pages);
        return;
    }
    // sell: pick an item from the bags, set prices and time
    std::vector<w3n::InvItem> items = sellable(g);
    const int cols = 7, cell = 36, per2 = 14;
    int pages = std::max(1, ((int)items.size() + per2 - 1) / per2);
    page_ = std::min(page_, pages - 1);
    for (int k = 0; k < per2; ++k) {
        int idx = page_ * per2 + k;
        if (idx >= (int)items.size()) break;
        const w3n::InvItem& it = items[idx];
        const int x = 6 + (k % cols) * (cell + 4), y = kTop + 26 + (k / cols) * (cell + 4);
        const w3n::ItemInfo* info = g.itemInfo(it.entry);
        iconBox(x, y, cell, itemIcon(g, it.entry), g.itemName(it.entry), qualityColor(info ? info->quality : 1));
        if (it.count > 1) list_.textRight(x + cell - 1, y + cell - 11, std::to_string(it.count), kWhite, true);
        if (idx == auctionSellItem_) list_.outline(x - 2, y - 2, cell + 4, cell + 4, kGold);
        Button& b = region(x - 1, y - 1, cell + 2, cell + 2, Act::AuctionSellPick, idx);
        b.infoKind = 4;
        b.infoId = it.entry;
        b.guid = it.guid;
    }
    const int py = kTop + 110;
    if (auctionSellItem_ >= 0 && auctionSellItem_ < (int)items.size()) {
        list_.textFit(6, py, g.itemName(items[auctionSellItem_].entry), 300, kGold);
        static const char* kHours[3] = {"2h", "8h", "24h"};
        btn(6, py + 14, 98, 20, std::string(tr("Bid ", "Ставка ")) + (auctionBid_ ? w3n::moneyText(auctionBid_) : "?"), Act::AuctionPrice, 0, Style::Toggle, Font::Small);
        btn(108, py + 14, 98, 20, std::string(tr("Buyout ", "Выкуп ")) + (auctionBuyout_ ? w3n::moneyText(auctionBuyout_) : "-"), Act::AuctionPrice, 1, Style::Toggle, Font::Small);
        btn(210, py + 14, 50, 20, kHours[auctionHours_], Act::AuctionPrice, 2, Style::Toggle, Font::Small);
        btn(2, by, 96, 22, tr("Create", "Выставить"), Act::AuctionSell, 0, auctionBid_ ? Style::Primary : Style::Disabled, Font::Small);
    } else {
        list_.text(6, py, tr("Tap an item to sell.", "Выберите предмет для продажи."), kGrey);
    }
    pager(by, page_, pages);
}

// ------------------------------------------------------------------ trade with a player
void GameUi::drawTrade(w3n::GameSession& g, const Input& in) {
    const w3n::TradeState& t = g.trade;
    std::string who = g.playerName(t.with, in.nowMs);
    if (who.empty()) who = tr("someone", "игрок");
    if (t.phase == w3n::TradeState::Phase::Asked) {
        window(20, kTop + 30, kTouchW - 40, 110, true);
        list_.textCentered(kTouchW / 2, kTop + 48, who, kGold, true, Font::Large);
        list_.textCentered(kTouchW / 2, kTop + 72, tr("wants to trade with you.", "хочет обменяться с вами."), kText, true, Font::Normal);
        btn(36, kTop + 100, 116, 28, tr("Accept", "Принять"), Act::TradeAnswer, 1, Style::Primary);
        btn(168, kTop + 100, 116, 28, tr("Decline", "Отказать"), Act::TradeAnswer, 0, Style::Normal);
        return;
    }
    if (t.phase == w3n::TradeState::Phase::Proposed) {
        window(20, kTop + 30, kTouchW - 40, 100, true);
        list_.textCentered(kTouchW / 2, kTop + 50, std::string(tr("Waiting for ", "Ждём ответа: ")) + who, kText, true, Font::Normal);
        btn(100, kTop + 90, 120, 28, tr("Cancel", "Отмена"), Act::TradeCancel, 0, Style::Normal);
        return;
    }
    // two offers side by side: 6 slots + gold each
    auto offer = [&](int x0, const char* title, const w3n::TradeOffer& o, bool mine, bool accepted) {
        window(x0, kTop + 2, 154, 92);
        list_.textFit(x0 + 5, kTop + 5, title, 100, accepted ? kGreen : kGold);
        if (accepted) list_.textRight(x0 + 150, kTop + 5, "✓", kGreen, true, Font::Normal);
        for (int s = 0; s < 6; ++s) {
            const int x = x0 + 5 + (s % 3) * 50, y = kTop + 18 + (s / 3) * 34;
            const w3n::TradeSlotView& v = o.slots[s];
            if (v.entry) {
                const w3n::ItemInfo* info = g.itemInfo(v.entry);
                iconBox(x, y, 30, itemIcon(g, v.entry), g.itemName(v.entry), qualityColor(info ? info->quality : 1));
                if (v.count > 1) list_.textRight(x + 30, y + 19, std::to_string(v.count), kWhite, true);
                Button& b = region(x - 1, y - 1, 32, 32, mine ? Act::TradeClear : Act::ItemInfo, mine ? s : 999);
                b.infoKind = 1;
                b.infoId = v.entry;
            } else {
                list_.rect(x, y, 30, 30, kRowAlt);
                list_.outline(x - 1, y - 1, 32, 32, kFrameDim);
            }
        }
        money(x0 + 150, kTop + 80, o.gold, Font::Small);
    };
    offer(2, tr("You", "Вы"), t.mine, true, t.weAccepted);
    offer(164, who.c_str(), t.theirs, false, t.theyAccepted);
    // our bags to pick from
    std::vector<w3n::InvItem> items = sellable(g);
    const int cols = 8, cell = 30, per = 8;
    int pages = std::max(1, ((int)items.size() + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    for (int k = 0; k < per; ++k) {
        int idx = page_ * per + k;
        if (idx >= (int)items.size()) break;
        const w3n::InvItem& it = items[idx];
        bool offered = false;
        for (const w3n::InvItem& o : t.myItems) offered = offered || (o.guid && o.guid == it.guid);
        const int x = 6 + (k % cols) * (cell + 8), y = kTop + 100;
        const w3n::ItemInfo* info = g.itemInfo(it.entry);
        iconBox(x, y, cell, itemIcon(g, it.entry), g.itemName(it.entry), qualityColor(info ? info->quality : 1), offered);
        if (idx == tradeSel_) list_.outline(x - 2, y - 2, cell + 4, cell + 4, kGold);
        Button& b = region(x - 1, y - 1, cell + 2, cell + 2, Act::TradePick, idx);
        b.infoKind = 4;
        b.infoId = it.entry;
        b.guid = it.guid;
    }
    if (pages > 1) {
        btn(kTouchW - 30, kTop + 100, 26, 14, "▲", Act::PagePrev, 0, page_ > 0 ? Style::Normal : Style::Disabled, Font::Small);
        btn(kTouchW - 30, kTop + 116, 26, 14, "▼", Act::PageNext, 0, page_ + 1 < pages ? Style::Normal : Style::Disabled, Font::Small);
    }
    const int by = kTabY - 24;
    btn(2, by, 74, 22, tr("Offer", "Предложить"), Act::TradeOffer, 0, tradeSel_ >= 0 ? Style::Normal : Style::Disabled, Font::Small);
    btn(80, by, 64, 22, tr("Gold", "Деньги"), Act::TradeGold, 0, Style::Normal, Font::Small);
    btn(148, by, 84, 22, t.weAccepted ? tr("Accepted", "Принято") : tr("Accept", "Принять"), Act::TradeAccept, 0,
        t.weAccepted ? Style::ToggleOn : Style::Primary, Font::Small);
    btn(236, by, 80, 22, tr("Cancel", "Отмена"), Act::TradeCancel, 0, Style::Danger, Font::Small);
}
