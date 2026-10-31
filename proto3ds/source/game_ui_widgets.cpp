// game_ui_widgets.cpp — the touch screen's frame (header, tab bar, overlays) and the widgets every tab and window
// is built from: buttons, windows (9-slice from the client's dialog / tooltip borders, flat in the stub skin), icon
// boxes (the client's icons, initials in the stub skin), money with coins, pagers, the hotbar picker.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "game_ui.hpp"
#include "text.hpp"
#include "ui_style.hpp"

using namespace ui;

namespace {

std::string initials(const std::string& name) {
    std::string out;
    size_t i = 0;
    while (i < name.size() && out.size() < 4) {
        while (i < name.size() && (name[i] == ' ' || name[i] == '(' || name[i] == '-' || name[i] == '\'')) ++i;
        if (i >= name.size()) break;
        size_t j = i;
        uint32_t cp = ui::nextCodepoint(name, j);   // the first letter (UTF-8)
        std::string word;
        size_t k = i;
        while (k < name.size() && name[k] != ' ') ++k;
        word = name.substr(i, k - i);
        if (word != "of" && word != "the" && word != "and" && cp != '0') out.append(name, i, j - i);
        i = k;
        if (HudList::width(out, Font::Small) > 18) break;
    }
    return out.empty() ? "?" : out;
}

uint32_t nameTint(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return hudRgba(40 + (int)(h & 63), 34 + (int)((h >> 8) & 47), 28 + (int)((h >> 16) & 63));
}

}  // namespace

ui::Font GameUi::listFont() const { return bigText_ ? Font::Normal : Font::Small; }

ui::Image GameUi::spellIcon(uint32_t spell) const {
    ui::Image im = assets().spellIcon(spell);
    if (!im.ok() && spell == w3n::kSpellAttack) im = assets().namedIcon("attack");
    return im;
}

ui::Image GameUi::itemIcon(w3n::GameSession& g, uint32_t entry) {
    const w3n::ItemInfo* info = entry ? g.itemInfo(entry) : nullptr;
    return info ? assets().itemIcon(info->displayId) : ui::Image();
}

std::string GameUi::spellLabel(const w3n::GameSession& g, uint32_t spell) const {
    std::string n = g.spellNames.name(spell);
    std::string r = g.spellNames.rank(spell);
    if (r.rfind("Rank ", 0) == 0) n += " " + r.substr(5);
    return n;
}

GameUi::Button& GameUi::region(int x, int y, int w, int h, Act act, int arg) {
    Button b{x, y, w, h, act, arg};
    buttons_.push_back(b);
    return buttons_.back();
}

void GameUi::window(int x, int y, int w, int h, bool dialog) {
    list_.rect(x, y, w, h, kPanel);
    ui::Image fr = assets().sprite(dialog ? "dialog" : "panel");
    if (fr.ok()) {
        list_.nine(x, y, w, h, fr, 0xFFFFFFFFu, false);
    } else {
        list_.outline(x, y, w, h, dialog ? kFrame : kFrameDim);
        if (dialog) list_.outline(x + 2, y + 2, w - 4, h - 4, hudAlpha(kFrameDim, 160));
    }
}

void GameUi::btn(int x, int y, int w, int h, const std::string& label, Act act, int arg, Style st, Font f) {
    const bool pressed = press_.active && press_.onButton && press_.button.act == act && press_.button.arg == arg &&
                         press_.button.x == x && press_.button.y == y;
    uint32_t text = kText;
    ui::Image skin;
    switch (st) {
        case Style::Primary: case Style::Danger:
            skin = assets().sprite(pressed ? "button_down" : "button");
            if (skin.ok()) list_.nine(x, y, w, h, skin);
            else {
                list_.rect(x, y, w, h, pressed ? hudRgba(80, 10, 10) : st == Style::Danger ? hudRgba(110, 18, 14) : hudRgba(120, 22, 16));
                list_.outline(x, y, w, h, kFrame);
                list_.rect(x + 1, y + 1, w - 2, 1, hudAlpha(kWhite, 50));
            }
            text = st == Style::Danger ? hudRgba(255, 200, 190) : kGold;
            break;
        case Style::Disabled:
            skin = assets().sprite("button_off");
            if (skin.ok()) list_.nine(x, y, w, h, skin);
            else { list_.rect(x, y, w, h, kRowAlt); list_.outline(x, y, w, h, kFrameDim); }
            text = kGrey;
            break;
        case Style::Toggle:
            list_.rect(x, y, w, h, pressed ? kRowSel : kRow);
            list_.outline(x, y, w, h, kFrameDim);
            text = kGrey;
            break;
        case Style::ToggleOn:
            list_.rect(x, y, w, h, kRowSel);
            list_.outline(x, y, w, h, kGold);
            text = kGold;
            break;
        case Style::Flat:
            if (pressed) list_.rect(x, y, w, h, hudAlpha(kRowSel, 160));
            break;
        default:
            list_.rect(x, y, w, h, pressed ? kRowSel : kBgLight);
            list_.outline(x, y, w, h, kFrameDim);
            list_.rect(x + 1, y + 1, w - 2, 1, hudAlpha(kWhite, 25));
            break;
    }
    if (!label.empty()) {
        std::string t = HudList::fit(label, w - 6, f);
        list_.textCentered(x + w / 2, y + (h - HudList::lineHeight(f)) / 2 + (pressed ? 1 : 0), t, text, true, f);
    }
    if (st != Style::Disabled) region(x, y, w, h, act, arg);
}

void GameUi::iconBox(int x, int y, int size, const ui::Image& icon, const std::string& name, uint32_t border, bool dim) {
    if (icon.ok()) {
        list_.image(x, y, size, size, icon);
    } else {
        list_.rect(x, y, size, size, nameTint(name));
        Font f = size >= 30 ? Font::Normal : Font::Small;
        std::string t = initials(name);
        while (t.size() > 1 && HudList::width(t, f) > size - 2) {
            size_t k = t.size() - 1;
            while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) --k;   // drop the last UTF-8 character
            t.resize(k);
        }
        list_.textCentered(x + size / 2, y + (size - HudList::lineHeight(f)) / 2, t, kText, true, f);
    }
    if (dim) list_.rect(x, y, size, size, hudRgba(0, 0, 0, 150));
    list_.outline(x - 1, y - 1, size + 2, size + 2, border);
}

int GameUi::money(int xRight, int y, uint32_t copper, Font f) {
    const uint32_t gold = copper / 10000, silver = (copper / 100) % 100, cop = copper % 100;
    ui::Image coins[3] = {assets().sprite("coin_gold"), assets().sprite("coin_silver"), assets().sprite("coin_copper")};
    const uint32_t colors[3] = {hudRgba(255, 215, 40), hudRgba(210, 210, 220), hudRgba(220, 140, 70)};
    const char* letters[3] = {tr("g", "з"), tr("s", "с"), tr("c", "м")};
    uint32_t parts[3] = {gold, silver, cop};
    int x = xRight;
    const int lh = HudList::lineHeight(f), cs = std::min(lh - 2, 11);
    for (int k = 2; k >= 0; --k) {
        if (!parts[k] && !(k == 2 && !gold && !silver)) continue;
        if (coins[k].ok()) {
            x -= cs;
            list_.image(x, y + (lh - cs) / 2, cs, cs, coins[k]);
            x -= 1;
            std::string n = std::to_string(parts[k]);
            x -= HudList::width(n, f);
            list_.text(x, y, n, kText, true, f);
        } else {
            std::string n = std::to_string(parts[k]) + letters[k];
            x -= HudList::width(n, f);
            list_.text(x, y, n, colors[k], true, f);
        }
        x -= 4;
    }
    return xRight - x;
}

void GameUi::pager(int y, int page, int pages) {
    if (pages <= 1) return;
    char b[32];
    snprintf(b, sizeof b, "%d/%d", page + 1, pages);
    btn(kTouchW - 92, y, 28, 22, "◀", Act::PagePrev, 0, page > 0 ? Style::Normal : Style::Disabled);
    list_.textCentered(kTouchW - 47, y + 5, b, kGrey);
    btn(kTouchW - 32, y, 28, 22, "▶", Act::PageNext, 0, page + 1 < pages ? Style::Normal : Style::Disabled);
}

void GameUi::header(w3n::GameSession& g, const std::string& title) {
    list_.rect(0, 0, kTouchW, kTop, kBgLight);
    list_.rect(0, kTop - 1, kTouchW, 1, kFrameDim);
    // pin (right corner): no automatic tab switching while on
    list_.rect(kTouchW - 22, 2, 20, kTop - 5, pinned_ ? kRowSel : kRow);
    list_.outline(kTouchW - 22, 2, 20, kTop - 5, pinned_ ? kGold : kFrameDim);
    list_.textCentered(kTouchW - 12, 3, "◆", pinned_ ? kGold : kGrey);
    region(kTouchW - 26, 0, 26, kTop, Act::Pin, 0);
    int mw = money(kTouchW - 28, 4, g.money(), Font::Small);
    const bool hinting = !hint_.empty() && lastDrawMs_ - hintMs_ < 4000;
    std::string t = hinting ? trText(hint_) : title;
    Font f = hinting ? Font::Small : Font::Normal;
    list_.textFit(6, hinting ? 5 : 2, t, kTouchW - 40 - mw, hinting ? kCyan : kGold, true, f);
}

std::vector<GameUi::Tab> tabOrderStatic() {
    return {GameUi::Tab::Combat, GameUi::Tab::Map, GameUi::Tab::Bags, GameUi::Tab::Spells, GameUi::Tab::Quests,
            GameUi::Tab::Chat, GameUi::Tab::Social, GameUi::Tab::Settings};
}

void GameUi::tabBar() {
    static const char* kIcon[8] = {"tab_combat", "tab_map", "tab_bags", "tab_spells", "tab_quests", "tab_chat", "tab_social", "tab_settings"};
    const char* labels[8] = {tr("Fight", "Бой"), tr("Map", "Карта"), tr("Bags", "Сумки"), tr("Spells", "Книга"),
                             tr("Quests", "Квесты"), tr("Chat", "Чат"), tr("Social", "Люди"), tr("Setup", "Опции")};
    std::vector<Tab> order = tabOrderStatic();
    const Tab cur = tab_ == Tab::Gear ? Tab::Bags : tab_;
    list_.rect(0, kTabY, kTouchW, kTabH, kBg);
    for (int i = 0; i < 8; ++i) {
        const int x = i * 40, y = kTabY;
        const bool on = order[i] == cur;
        list_.rect(x + 1, y + 2, 38, kTabH - 3, on ? kRowSel : kPanel);
        list_.rect(x + 1, y, 38, on ? 2 : 1, on ? kGold : kFrameDim);
        ui::Image icon = assets().namedIcon(kIcon[i]);
        if (icon.ok()) {
            list_.image(x + 6, y + 4, 28, 28, icon, on ? 0xFFFFFFFFu : hudRgba(170, 170, 170));
            list_.outline(x + 5, y + 3, 30, 30, on ? kGold : kFrameDim);
        } else {   // stub skin: the first letter, large
            list_.rect(x + 6, y + 4, 28, 28, on ? hudRgba(70, 52, 20) : kBgLight);
            list_.outline(x + 5, y + 3, 30, 30, on ? kGold : kFrameDim);
            size_t j = 0;
            std::string l(labels[i]);
            ui::nextCodepoint(l, j);
            list_.textCentered(x + 20, y + 9, l.substr(0, j), on ? kGold : kGrey, true, Font::Large);
        }
        list_.textCentered(x + 20, y + 33, labels[i], on ? kGold : kGrey, true, Font::Small);
        region(x, y, 40, kTabH, Act::Tab, (int)order[i]);
    }
}

// What a hotbar entry looks like (UI stage 13: the pet's commands and spells too).
ui::Image GameUi::actionIcon(w3n::GameSession& g, const ActionSlot& a) {
    switch (a.type) {
        case ActionSlot::Spell: case ActionSlot::PetSpell: return spellIcon(a.id);
        case ActionSlot::Item: return itemIcon(g, a.id);
        case ActionSlot::PetCmd: return assets().namedIcon(petCmdIcon(a.id));
        default: return ui::Image();
    }
}

std::string GameUi::actionName(w3n::GameSession& g, const ActionSlot& a) const {
    switch (a.type) {
        case ActionSlot::Spell: case ActionSlot::PetSpell: return g.spellNames.name(a.id);
        case ActionSlot::Item: return g.itemName(a.id);
        case ActionSlot::PetCmd: return petCmdName(a.id);
        default: return "";
    }
}

// The hotbars of the current page over the content: 4 sets (rows) x 8 buttons; tap a cell (bind armed) or drop on it.
void GameUi::crossbarPicker(w3n::GameSession& g, const Input& in) {
    (void)in;
    const ActionLayout* bars = barsView_;
    window(2, kTop + 1, kTouchW - 4, kContentH - 2, true);
    std::string what;
    if (drag_.active) what = actionName(g, ActionSlot{drag_.kind >= 1 && drag_.kind <= 4 ? drag_.kind : (uint8_t)ActionSlot::Item, drag_.id});
    else if (bindWhat_.type == ActionSlot::Spell) what = spellLabel(g, bindWhat_.id);
    else if (bindWhat_.type != ActionSlot::Empty) what = actionName(g, bindWhat_);
    std::string title = bindWhat_.type == ActionSlot::Empty && !drag_.active ? tr("Clear which button?", "Какую кнопку очистить?")
                                                                              : std::string(tr("Bind: ", "Привязать: ")) + what;
    list_.textFit(10, kTop + 6, title, 220, kGold, true, Font::Normal);
    if (!drag_.active) btn(kTouchW - 74, kTop + 4, 66, 20, tr("Cancel", "Отмена"), Act::ClosePanel, 0, Style::Normal, Font::Small);
    static const char* kSet[4] = {"", "ZL", "ZR", "ZL+R"};
    static const char* kGlyph[8] = {"↑", "→", "↓", "←", "X", "A", "B", "Y"};
    const int cell = 32, gap = 3, x0 = 40, y0 = kTop + 30;
    for (int set = 0; set < 4; ++set) {
        const int y = y0 + set * (cell + gap);
        list_.textRight(x0 - 4, y + 10, set ? kSet[set] : "—", set == shownSet_ ? kGold : kGrey);
        for (int b = 0; b < 8; ++b) {
            const int slot = set * 8 + b, x = x0 + b * (cell + gap);
            if (b == 4) list_.rect(x - 2, y, 1, cell, kFrameDim);   // D-pad | face buttons
            if (!padSlotBindable(slot)) {
                list_.rect(x, y, cell, cell, kRowAlt);
                list_.textCentered(x + cell / 2, y + 2, kGlyph[b], kGrey);
                list_.textCentered(x + cell / 2, y + 16, HudList::fit(padFixedName(b), cell - 2), hudRgba(110, 106, 98));
                continue;
            }
            const ActionSlot a = bars ? bars->at(slot) : ActionSlot();
            const bool hover = drag_.active && drag_.hoverSlot == slot;
            if (a.type != ActionSlot::Empty) iconBox(x, y, cell, actionIcon(g, a), actionName(g, a), hover ? kGold : kFrameDim, true);
            else { list_.rect(x, y, cell, cell, kRow); list_.outline(x - 1, y - 1, cell + 2, cell + 2, hover ? kGold : kFrameDim); }
            if (hover) list_.outline(x - 2, y - 2, cell + 4, cell + 4, kGold);
            list_.text(x + 2, y + cell - 12, kGlyph[b], kGold);
            Button& r = region(x, y, cell, cell, Act::BarCell, slot);
            pickerCells_.push_back(r);
        }
    }
    list_.textCentered(kTouchW / 2, y0 + 4 * (cell + gap) + 1,
                       drag_.active ? tr("Drop it on a button", "Отпустите на кнопке") : tr("Tap a cell or press the button", "Нажмите ячейку или кнопку"),
                       kGrey);
}

std::string GameUi::panelTitle(w3n::GameSession& g, Panel p) const {
    auto npcName = [&](uint64_t guid, const char* def) {
        const w3n::WorldObject* o = g.objects.find(guid);
        return o && !o->name.empty() ? o->name : std::string(def);
    };
    switch (p) {
        case Panel::Death: return g.life() == w3n::Life::Ghost ? tr("Ghost", "Призрак") : g.life() == w3n::Life::Dead ? tr("Dead", "Смерть") : tr("Resurrection", "Воскрешение");
        case Panel::Loot: return std::string(tr("Loot: ", "Добыча: ")) + npcName(g.lootWindow.guid, tr("corpse", "труп"));
        case Panel::Gossip: return npcName(g.gossip.npc, "NPC");
        case Panel::Vendor: return npcName(g.vendor.npc, tr("Vendor", "Торговец"));
        case Panel::Quest: return npcName(g.questDialog.npc, tr("Quest", "Задание"));
        case Panel::Trainer: return npcName(g.trainer.npc, tr("Trainer", "Учитель"));
        case Panel::Binder: return npcName(g.binderConfirm, tr("Innkeeper", "Хозяин таверны"));
        case Panel::Bank: return tr("Bank", "Банк");
        case Panel::Taxi: return tr("Flight master", "Распорядитель полётов");
        case Panel::Mail: return tr("Mailbox", "Почтовый ящик");
        case Panel::Auction: return tr("Auction house", "Аукцион");
        case Panel::Trade: return tr("Trade", "Обмен");
        case Panel::ItemInfo: return tr("Item", "Предмет");
        case Panel::SpellInfo: return tr("Spell", "Заклинание");
        case Panel::Buffs: return tr("Buffs and debuffs", "Эффекты");
        case Panel::Invite: return g.duel.challenged || g.duel.asked ? tr("Duel", "Дуэль") : !g.groupInviteFrom.empty() ? tr("Group invitation", "Приглашение в группу") : tr("Guild invitation", "Приглашение в гильдию");
        case Panel::Menu: return tr("Menu", "Меню");
        case Panel::Confirm: return tr("Are you sure?", "Вы уверены?");
        case Panel::Context: return npcName(g.target(), tr("Target", "Цель"));
        case Panel::Inspect: return npcName(inspect_, tr("Inspect", "Осмотр"));
        case Panel::Keyboard: return kb_.hint;
        case Panel::Pet: return g.petName().empty() ? tr("Pet", "Питомец") : g.petName();
        case Panel::Stable: return npcName(g.stable.npc, tr("Stable master", "Хозяин стойл"));
        default: break;
    }
    switch (tab_) {
        case Tab::Combat: return tr("Combat", "Бой");
        case Tab::Map: return tr("Map", "Карта");
        case Tab::Bags: case Tab::Gear: return subBags_ ? tr("Equipment", "Экипировка") : tr("Bags", "Сумки");
        case Tab::Spells: return subSpells_ == 2 ? tr("Skills", "Навыки") : subSpells_ ? tr("Talents", "Таланты") : tr("Spellbook", "Книга заклинаний");
        case Tab::Quests: return tr("Quest log", "Журнал заданий");
        case Tab::Chat: return tr("Chat", "Чат");
        case Tab::Social: return tr("Social", "Общение");
        case Tab::Settings: return tr("Settings", "Настройки");
    }
    return "";
}

void GameUi::drawPanel(w3n::GameSession& g, const Input& in) {
    const Panel p = panel(g);
    switch (p) {
        case Panel::Death: drawDeath(g, in); break;
        case Panel::Loot: drawLoot(g); break;
        case Panel::Gossip: drawGossip(g); break;
        case Panel::Vendor: drawVendor(g, in); break;
        case Panel::Quest: drawQuest(g); break;
        case Panel::Trainer: drawTrainer(g); break;
        case Panel::ItemInfo: drawItemInfo(g); break;
        case Panel::SpellInfo: drawSpellInfo(g, in); break;
        case Panel::Buffs: drawBuffs(g, in); break;
        case Panel::Confirm: drawConfirm(); break;
        case Panel::Bank: drawBank(g, in); break;
        case Panel::Taxi: drawTaxi(g, in); break;
        case Panel::Mail: drawMail(g, in); break;
        case Panel::Auction: drawAuction(g, in); break;
        case Panel::Trade: drawTrade(g, in); break;
        case Panel::Invite: {
            window(20, kTop + 30, kTouchW - 40, 110, true);
            const bool fv0 = in.cfg && in.cfg->scheme == PadScheme::Forever;
            if (g.duel.challenged || g.duel.asked) {   // UI stage 9: a duel - theirs (Accept / Decline) or ours (Cancel)
                const std::string who = g.duel.opponentName.empty() ? std::string("?") : g.duel.opponentName;
                list_.textCentered(kTouchW / 2, kTop + 48, who, kGold, true, Font::Large);
                if (g.duel.challenged) {
                    list_.textCentered(kTouchW / 2, kTop + 72, tr("challenges you to a duel.", "вызывает вас на дуэль."), kText, true, Font::Normal);
                    btn(36, kTop + 100, 116, 28, std::string(tr("Accept", "Принять")) + (fv0 ? " (Y)" : " (A)"), Act::InviteYes, 0, Style::Primary);
                    btn(168, kTop + 100, 116, 28, std::string(tr("Decline", "Отказать")) + (fv0 ? " (A)" : " (B)"), Act::InviteNo, 0, Style::Normal);
                } else {
                    list_.textCentered(kTouchW / 2, kTop + 72, tr("Duel requested. Waiting...", "Вызов на дуэль отправлен. Ждём..."), kText, true, Font::Normal);
                    btn(102, kTop + 100, 116, 28, std::string(tr("Cancel", "Отменить")) + (fv0 ? " (A)" : " (B)"), Act::InviteNo, 0, Style::Normal);
                }
                break;
            }
            const bool group = !g.groupInviteFrom.empty();
            std::string who = group ? g.groupInviteFrom : g.guildInviteFrom;
            list_.textCentered(kTouchW / 2, kTop + 48, who, kGold, true, Font::Large);
            list_.textCentered(kTouchW / 2, kTop + 72,
                               group ? tr("invites you to a group.", "приглашает вас в группу.")
                                     : std::string(tr("invites you to the guild ", "приглашает вас в гильдию ")) + g.guildInviteName,
                               kText, true, Font::Normal);
            const bool fv = in.cfg && in.cfg->scheme == PadScheme::Forever;
            btn(36, kTop + 100, 116, 28, std::string(tr("Accept", "Принять")) + (fv ? " (Y)" : " (A)"), Act::InviteYes, 0, Style::Primary);
            btn(168, kTop + 100, 116, 28, std::string(tr("Decline", "Отказать")) + (fv ? " (A)" : " (B)"), Act::InviteNo, 0, Style::Normal);
            break;
        }
        case Panel::Binder:
            window(20, kTop + 30, kTouchW - 40, 110, true);
            list_.textCentered(kTouchW / 2, kTop + 50, tr("Do you want to make this inn", "Сделать эту таверну"), kText, true, Font::Normal);
            list_.textCentered(kTouchW / 2, kTop + 68, tr("your new home?", "вашим домом?"), kText, true, Font::Normal);
            btn(36, kTop + 100, 116, 28, tr("Yes", "Да"), Act::BinderYes, 0, Style::Primary);
            btn(168, kTop + 100, 116, 28, tr("No", "Нет"), Act::BinderNo, 0, Style::Normal);
            break;
        case Panel::Menu: drawMenu(g); break;
        case Panel::Context: drawContext(g); break;
        case Panel::Inspect: drawInspect(g); break;
        case Panel::Keyboard: drawKeyboard(g, in); break;
        case Panel::Pet: drawPet(g, in); break;
        case Panel::Stable: drawStable(g, in); break;
        default:
            switch (tab_) {
                case Tab::Combat: drawCombat(g, in); break;
                case Tab::Map: drawMap(g, in); break;
                case Tab::Chat: drawChat(g, in); break;
                case Tab::Bags: case Tab::Gear: if (subBags_) drawPaperDoll(g, in); else drawItems(g, in); break;
                case Tab::Spells: if (subSpells_ == 3) drawCraft(g, in); else if (subSpells_ == 2) drawSkills(g, in); else if (subSpells_) drawTalents(g, in); else drawSpells(g, in); break;
                case Tab::Quests: drawQuestLog(g, in); break;
                case Tab::Social: drawSocial(g, in); break;
                case Tab::Settings: drawSettings(in); break;
            }
            break;
    }
}

bool GameUi::draw(w3n::GameSession& g, const Input& in, float fps, uint32_t latencyMs) {
    (void)fps;
    (void)latencyMs;
    if (!gamePage_) return false;
    if (!dirty_ && g.chatCount == lastChat_ && in.nowMs - lastDrawMs_ < 250) return false;
    dirty_ = false;
    lastChat_ = g.chatCount;
    lastDrawMs_ = in.nowMs;
    if (in.cfg) { ui::g_lang = in.cfg->lang; bigText_ = in.cfg->bigText; }
    barsView_ = in.bars;
    list_.clear();
    buttons_.clear();
    pickerCells_.clear();
    list_.rect(0, 0, kTouchW, kTouchH, kBg);
    const Panel p = panel(g);
    header(g, panelTitle(g, p));
    list_.clip(0, kTop, kTouchW, kContentH);
    drawPanel(g, in);
    list_.noClip();
    tabBar();
    if (bindArmed_ || drag_.active) {
        list_.clip(0, kTop, kTouchW, kContentH);
        crossbarPicker(g, in);
        list_.noClip();
    }
    if (drag_.active) {   // the icon under the finger
        const ActionSlot da{drag_.kind >= 1 && drag_.kind <= 4 ? drag_.kind : (uint8_t)ActionSlot::Item, drag_.id};
        ui::Image im = actionIcon(g, da);
        std::string nm = actionName(g, da);
        list_.fade = 210;
        iconBox(drag_.x - 16, drag_.y - 34, 32, im, nm, kGold);
        list_.fade = 255;
    } else if (press_.active && press_.onButton && press_.button.act != Act::Tab) {
        const Button& b = press_.button;   // a thin highlight on what the finger is on
        list_.outline(b.x - 1, b.y - 1, b.w + 2, b.h + 2, hudAlpha(kGold, 200));
    }
    return true;
}
