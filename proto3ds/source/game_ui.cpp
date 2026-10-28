// game_ui.cpp — the game logic behind the touch screen and the controller: windows by priority, touch (tap on
// release, long press, drag), automatic tabs, confirmations, and every action a button or pad intent stands for.
// Drawing: game_ui_widgets.cpp (frame, header, tab bar, widgets), game_ui_tabs.cpp, game_ui_windows.cpp.
#include "game_ui.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "text.hpp"
#include "ui_style.hpp"

using ui::tr;

namespace {

float dist3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

bool lootableOrNpc(const w3n::WorldObject& o) {
    if (o.type != w3n::ObjType::Unit) return false;
    if (o.dead()) return (o.field(w3n::UF::UNIT_DYNAMIC_FLAGS) & 1) != 0;
    return o.field(w3n::UF::UNIT_NPC_FLAGS) != 0;
}

const char* kPhrasesEn[] = {"Hello!", "Thanks!", "Need help?", "Ready.", "Wait a moment.", "Follow me.", "Good game!", "Bye!"};
const char* kPhrasesRu[] = {"Привет!", "Спасибо!", "Нужна помощь?", "Готов.", "Подожди минутку.", "Иди за мной.", "Хорошая игра!", "Пока!"};

}  // namespace

const char* gameUiPhrase(int i) { return ui::g_lang ? kPhrasesRu[i & 7] : kPhrasesEn[i & 7]; }

void GameUi::reset() {
    rows_.clear();
    buttons_.clear();
    list_.clear();
    lastChat_ = 0;
    dirty_ = true;
    tab_ = Tab::Map;
    pinned_ = false;
    wasInCombat_ = false;
    open_ = Panel::Tabs;
    selected_ = page_ = 0;
    selTapped_ = false;
    bindArmed_ = false;
    aim_ = lastHostile_ = 0;
    vendorMode_ = 0; vendorSel_ = -1; vendorPage_ = 0; vendorNpc_ = 0;
    trainerSel_ = -1;
    questScroll_ = 0; rewardPick_ = -1; questKey_ = 0;
    logView_ = 0; logSel_ = 0; tracked_ = 0;
    subBags_ = subSpells_ = subSocial_ = subSettings_ = 0;
    chatChannel_ = chatScroll_ = 0;
    phrases_ = false;
    mapMode_ = 0; mapZoom_ = 1;
    talentTree_ = 0; talentSel_ = -1; socialSel_ = -1;
    press_ = Press();
    drag_ = Drag();
    bankSel_ = -1; bankMode_ = 0; taxiSel_ = -1; mailSel_ = -1; mailView_ = 0;
    mailTo_.clear(); mailSubject_.clear(); mailBody_.clear(); mailMoney_ = 0; mailItem_ = -1;
    auctionMode_ = 0; auctionSel_ = -1; auctionSellItem_ = -1; auctionQuery_.clear(); auctionBid_ = auctionBuyout_ = 0;
    tradeSel_ = -1;
    windowNpc_ = 0;
}

GameUi::Panel GameUi::panel(const w3n::GameSession& g) const {
    if (g.life() != w3n::Life::Alive || g.resurrectOffer.active || g.spiritHealerConfirm) return Panel::Death;
    if (open_ == Panel::Confirm) return Panel::Confirm;
    if (!g.groupInviteFrom.empty() || !g.guildInviteFrom.empty()) return Panel::Invite;
    if (g.trade.phase != w3n::TradeState::Phase::None) return Panel::Trade;
    if (g.lootWindow.open) return Panel::Loot;
    if (open_ == Panel::ItemInfo || open_ == Panel::SpellInfo) return open_;   // a tooltip over any window
    if (g.questDialog.open()) return Panel::Quest;
    if (g.vendor.open) return Panel::Vendor;
    if (g.trainer.open) return Panel::Trainer;
    if (g.binderConfirm) return Panel::Binder;
    if (g.bank.open) return Panel::Bank;
    if (g.taxi.open) return Panel::Taxi;
    if (g.mail.open) return Panel::Mail;
    if (g.auction.open) return Panel::Auction;
    if (g.gossip.open) return Panel::Gossip;
    return open_ == Panel::Settings ? Panel::Tabs : open_;
}

std::vector<GameUi::Tab> GameUi::tabOrder() const {
    return {Tab::Combat, Tab::Map, Tab::Bags, Tab::Spells, Tab::Quests, Tab::Chat, Tab::Social, Tab::Settings};
}

void GameUi::setTab(Tab t) {
    if (t != tab_) {
        selected_ = page_ = 0;
        selTapped_ = false;
        chatScroll_ = 0;
        phrases_ = false;
        socialSel_ = -1;
    }
    if (t == Tab::Gear) { tab_ = Tab::Bags; subBags_ = 1; }
    else {
        if (t == Tab::Bags && tab_ != Tab::Bags) subBags_ = 0;
        tab_ = t;
    }
    if (open_ == Panel::Menu || open_ == Panel::Context || open_ == Panel::Buffs) open_ = Panel::Tabs;
    dirty_ = true;
}

void GameUi::autoTabs(w3n::GameSession& g, const Input& in) {
    const bool combat = g.inCombat();
    if (combat == wasInCombat_) return;
    wasInCombat_ = combat;
    if (pinned_ || (in.cfg && !in.cfg->autoTabs) || panel(g) != Panel::Tabs) return;
    if (combat && tab_ != Tab::Combat) setTab(Tab::Combat);
    else if (!combat && tab_ == Tab::Combat) setTab(Tab::Map);
}

void GameUi::confirm(const std::string& question, Act act, int arg, uint64_t key) {
    (void)key;
    confirmText_ = question;
    confirmAct_ = act;
    confirmArg_ = arg;
    if (open_ != Panel::Confirm) confirmReturn_ = open_;
    open_ = Panel::Confirm;
    dirty_ = true;
}

std::vector<w3n::InvItem> GameUi::listed(const w3n::GameSession& g) const {
    std::vector<w3n::InvItem> out;
    const bool gear = tab_ == Tab::Gear || (tab_ == Tab::Bags && subBags_ == 1);
    for (const w3n::InvItem& it : g.inventory()) {
        bool eq = it.bag == w3n::kBagSelf && it.slot < w3n::kSlotBackpackFirst;   // equipped + the bags themselves
        if (gear == eq) out.push_back(it);
    }
    return out;
}

std::vector<w3n::InvItem> GameUi::sellable(const w3n::GameSession& g) const {
    std::vector<w3n::InvItem> out;
    for (const w3n::InvItem& it : g.inventory())
        if (!(it.bag == w3n::kBagSelf && it.slot < w3n::kSlotBackpackFirst)) out.push_back(it);   // bags' contents only
    return out;
}

std::vector<uint32_t> GameUi::bookSpells(const w3n::GameSession& g) const {
    std::vector<uint32_t> out{w3n::kSpellAttack};
    for (uint32_t s : g.spells)
        if (g.spellNames.known(s) && s != w3n::kSpellAttack) out.push_back(s);
    std::sort(out.begin() + 1, out.end(), [&](uint32_t a, uint32_t b) {
        std::string na = g.spellNames.name(a), nb = g.spellNames.name(b);
        return na != nb ? na < nb : g.spellNames.rank(a) < g.spellNames.rank(b);
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ------------------------------------------------------------------ targeting helpers
uint64_t GameUi::cycleUnits(const w3n::GameSession& g, const Input& in, bool enemies) const {
    struct C { float key; uint64_t guid; };
    std::vector<C> c;
    for (auto& [guid, o] : g.objects.all()) {
        if (o.self || !o.hasPos || !o.isUnitLike() || o.dead()) continue;
        if (enemies != g.enemyLike(o)) continue;
        float p[3];
        o.positionAt(in.nowMs, p);
        float d = dist3(p, in.pos);
        if (d > 40.0f) continue;
        float a = fabsf(remainderf(atan2f(p[1] - in.pos[1], p[0] - in.pos[0]) - in.yaw, 6.2831853f));
        c.push_back({a * 8.0f + d, guid});
    }
    if (c.empty()) return 0;
    std::sort(c.begin(), c.end(), [](const C& a, const C& b) { return a.key != b.key ? a.key < b.key : a.guid < b.guid; });
    for (size_t i = 0; i < c.size(); ++i)
        if (c[i].guid == g.target()) return c[(i + 1) % c.size()].guid;
    return c[0].guid;
}

uint64_t GameUi::aimUnit(const w3n::GameSession& g, const Input& in, float yaw, bool enemies) const {
    uint64_t best = 0;
    float bestKey = 1e9f;
    for (auto& [guid, o] : g.objects.all()) {
        if (o.self || !o.hasPos || !o.isUnitLike() || o.dead()) continue;
        if (enemies != g.enemyLike(o)) continue;
        float p[3];
        o.positionAt(in.nowMs, p);
        float d = dist3(p, in.pos);
        if (d > 40.0f) continue;
        float a = fabsf(remainderf(atan2f(p[1] - in.pos[1], p[0] - in.pos[0]) - yaw, 6.2831853f));
        if (a > 0.6f) continue;   // ~35 degrees either side of where the stick points
        float key = a * 30.0f + d;
        if (key < bestKey) { bestKey = key; best = guid; }
    }
    return best;
}

void GameUi::faceTarget(const w3n::GameSession& g, const Input& in, Output& out) const {
    const w3n::WorldObject* t = g.objects.find(g.target());
    if (!t || !t->hasPos || t->self) return;
    float p[3];
    t->positionAt(in.nowMs, p);
    out.faceYaw = true;
    out.yaw = atan2f(p[1] - in.pos[1], p[0] - in.pos[0]);
}

// ------------------------------------------------------------------ actions
void GameUi::pressSpell(w3n::GameSession& g, uint32_t spell, const Input& in, Output& out) {
    if (g.life() != w3n::Life::Alive) { hint("You are dead", in.nowMs); return; }
    const w3n::WorldObject* t = g.objects.find(g.target());
    bool toggleOff = spell == w3n::kSpellAttack && g.attackWanted();
    // soft target: without a living target, the enemy in front (melee range for Attack, 30 yd for spells)
    if (!toggleOff && (!t || t->dead() || t->self)) {
        uint64_t s = g.softEnemy(in.pos, in.yaw, spell == w3n::kSpellAttack ? 8.0f : 30.0f, 1.05f, in.nowMs);
        if (s) { g.setTarget(s, in.nowMs); t = g.objects.find(s); }
    }
    if (!toggleOff && t && t->hasPos && !t->dead() && !t->self) faceTarget(g, in, out);
    g.castSpell(spell, in.nowMs);
}

void GameUi::useSlot(w3n::GameSession& g, const Input& in, int slot, Output& out) {
    if (!in.bars || slot < 0 || slot >= kPadSlots) return;
    if (slot >= 4 && slot < 8) {   // set 0 face buttons: the fixed actions
        if (bindArmed_) { hint(tr("That button is fixed: pick another", "Эта кнопка занята: выберите другую"), in.nowMs); return; }
        if (slot == 4) open_ = open_ == Panel::Context ? Panel::Tabs : Panel::Context;
        else if (slot == 5) cancelAction(g, in);
        else if (slot == 7) interactAction(g, in, out);
        dirty_ = true;
        return;
    }
    if (bindArmed_) {
        in.bars->bind(slot, bindWhat_);
        bindArmed_ = false;
        out.layoutChanged = true;
        std::string where = std::string(padSetName(slot / 8)) + (slot / 8 ? "+" : "") + padButtonGlyph(slot % 8);
        hint(bindWhat_.type == ActionSlot::Empty ? "Cleared " + where : "Bound to " + where, in.nowMs);
        return;
    }
    const ActionSlot& a = in.bars->at(slot);
    if (a.type == ActionSlot::Spell) {
        pressSpell(g, a.id, in, out);
    } else if (a.type == ActionSlot::Item) {
        w3n::InvItem it;
        if (in.bars->findItem(g, a.id, &it)) g.useItem(it, in.nowMs);
        else hint("None left: " + g.itemName(a.id), in.nowMs);
    } else {
        hint(tr("Empty slot: drag a spell here (Spells tab)", "Пустая ячейка: перетащите сюда заклинание (Книга)"), in.nowMs);
    }
    dirty_ = true;
}

void GameUi::dropOnSlot(const Input& in, int slot, Output& out) {
    if (!in.bars || !padSlotBindable(slot) || !drag_.kind) return;
    ActionSlot a{drag_.kind == 1 ? ActionSlot::Spell : ActionSlot::Item, drag_.id};
    in.bars->bind(slot, a);
    out.layoutChanged = true;
    std::string where = std::string(padSetName(slot / 8)) + (slot / 8 ? "+" : "") + padButtonGlyph(slot % 8);
    hint("Bound to " + where, in.nowMs);
}

void GameUi::interactAction(w3n::GameSession& g, const Input& in, Output& out) {
    const Panel p = panel(g);
    const bool forever = in.cfg && in.cfg->scheme == PadScheme::Forever;
    if (p == Panel::Loot) { g.takeAllLoot(in.nowMs); return; }
    if (p == Panel::Quest) { questPrimary(g, in); return; }
    if (p == Panel::Invite) {
        if (!g.groupInviteFrom.empty()) g.answerGroupInvite(true, in.nowMs);
        else g.answerGuildInvite(true, in.nowMs);
        return;
    }
    if (p == Panel::Confirm) { doAction(g, in, Act::ConfirmYes, 0, out); return; }
    if (p == Panel::Trade) {
        if (g.trade.phase == w3n::TradeState::Phase::Asked) g.answerTrade(true, in.nowMs);
        else if (g.trade.open()) g.acceptTrade(in.nowMs);
        return;
    }
    if (p == Panel::Vendor) { hint(tr("Tap an item, then Buy / Sell", "Выберите предмет, затем Купить / Продать"), in.nowMs); return; }
    if (p == Panel::Trainer) {
        if (trainerSel_ >= 0) g.trainSpell((size_t)trainerSel_, in.nowMs);
        else hint(tr("Tap a spell, then Train", "Выберите заклинание, затем Обучиться"), in.nowMs);
        return;
    }
    if (p == Panel::Binder) { g.acceptBinder(in.nowMs); return; }
    if (p == Panel::Taxi) { if (taxiSel_ > 0) g.flyTo((uint32_t)taxiSel_, in.nowMs); return; }
    if (g.life() == w3n::Life::Dead && !g.resurrectOffer.active) { g.releaseSpirit(in.nowMs); return; }
    const w3n::WorldObject* t = g.objects.find(g.target());
    // Forever: in combat (or at a living enemy) the interact button starts auto-attack
    if (forever && g.life() == w3n::Life::Alive && !g.attackWanted() &&
        ((t && g.enemyLike(*t)) || (g.inCombat() && (!t || t->dead())))) {
        pressSpell(g, w3n::kSpellAttack, in, out);
        return;
    }
    uint64_t use = g.target();
    bool targetOk = false;
    if (t && t->hasPos && lootableOrNpc(*t)) {
        float q[3];
        t->positionAt(in.nowMs, q);
        targetOk = dist3(q, in.pos) <= 5.5f;
    }
    if (!targetOk && g.life() == w3n::Life::Alive) {   // nothing usable selected: what is in front of us
        uint64_t s = g.softInteract(in.pos, in.yaw, in.nowMs);
        const w3n::WorldObject* so = g.objects.find(s);
        if (s) {
            use = s;
            if (!so || so->type != w3n::ObjType::GameObject) g.setTarget(s, in.nowMs);   // objects are not selected
        }
    } else if (!targetOk && g.life() == w3n::Life::Ghost && !g.canReclaim(in.pos, in.nowMs)) {
        uint64_t s = g.softInteract(in.pos, in.yaw, in.nowMs);   // a spirit healer next to us
        if (s) { use = s; g.setTarget(s, in.nowMs); }
    }
    std::string why = g.interact(use, in.pos, in.nowMs);
    if (!why.empty()) hint(why, in.nowMs);
    else faceTarget(g, in, out);
}

void GameUi::cancelAction(w3n::GameSession& g, const Input& in) {
    if (drag_.active) { drag_ = Drag(); dirty_ = true; return; }
    if (bindArmed_) { bindArmed_ = false; hint("Binding cancelled", in.nowMs); return; }
    const Panel p = panel(g);
    if (p == Panel::Confirm) { open_ = confirmReturn_; return; }
    if (p == Panel::Menu || p == Panel::Context || p == Panel::ItemInfo || p == Panel::SpellInfo || p == Panel::Buffs) {
        open_ = Panel::Tabs;
        return;
    }
    if (phrases_) { phrases_ = false; return; }
    if (p == Panel::Loot) { g.closeLoot(in.nowMs); return; }
    if (p == Panel::Quest) { g.closeQuestDialog(in.nowMs); return; }
    if (p == Panel::Invite) {
        if (!g.groupInviteFrom.empty()) g.answerGroupInvite(false, in.nowMs);
        else g.answerGuildInvite(false, in.nowMs);
        return;
    }
    if (p == Panel::Trade) {
        if (g.trade.phase == w3n::TradeState::Phase::Asked) g.answerTrade(false, in.nowMs);
        else g.cancelTrade(in.nowMs);
        return;
    }
    if (p == Panel::Vendor) { g.closeVendor(); return; }
    if (p == Panel::Trainer) { g.closeTrainer(); return; }
    if (p == Panel::Binder) { g.binderConfirm = 0; return; }
    if (p == Panel::Bank) { g.closeBank(); return; }
    if (p == Panel::Taxi) { g.closeTaxi(); return; }
    if (p == Panel::Mail) { if (mailView_) mailView_ = 0; else g.closeMail(); return; }
    if (p == Panel::Auction) { g.closeAuction(); return; }
    if (p == Panel::Gossip) { g.closeGossip(); return; }
    if (g.cast.active) { g.cancelCast(in.nowMs); return; }
    if (g.attackWanted()) { g.stopAttack(in.nowMs); return; }
    if (g.target()) g.setTarget(0, in.nowMs);
}

bool GameUi::keyboard(w3n::GameSession& g, const Input& in, Output& out, const char* hintText, std::string& text, bool numbers) {
    // The keyboard applet blocks this thread: release the movement keys first.
    g.mover.stopAll(in.nowMs);
    g.update(in.nowMs);
    SwkbdState kb;
    char buf[256] = {0};
    swkbdInit(&kb, numbers ? SWKBD_TYPE_NUMPAD : SWKBD_TYPE_WESTERN, 2, numbers ? 9 : (int)sizeof(buf) - 1);
    swkbdSetHintText(&kb, hintText);
    if (!text.empty()) swkbdSetInitialText(&kb, text.c_str());
    SwkbdButton b = swkbdInputText(&kb, buf, sizeof buf);
    out.keyboardShown = true;
    dirty_ = true;
    if (b != SWKBD_BUTTON_RIGHT) return false;
    text = buf;
    return true;
}

void GameUi::openChat(w3n::GameSession& g, const Input& in, Output& out, const std::string& prefix) {
    std::string text;
    if (!keyboard(g, in, out, "say  /y /p /g /w Name /e /join X /1", text)) return;
    if (text.empty()) return;
    if (!prefix.empty() && text[0] != '/') text = prefix + text;
    g.sendTyped(text, in.nowMs);
}

const GameUi::Button* GameUi::hit(int x, int y) const {
    // the last drawn wins (overlays are drawn after what they cover)
    for (size_t i = buttons_.size(); i-- > 0;) {
        const Button& b = buttons_[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return &b;
    }
    return nullptr;
}

void GameUi::showInfo(w3n::GameSession& g, const Button& b) {
    infoReturn_ = open_;
    if (b.infoKind == 1 || b.infoKind == 4) {
        infoEntry_ = b.infoId;
        infoItem_ = w3n::InvItem();
        if (b.infoKind == 4 && b.guid) {
            for (const w3n::InvItem& it : g.inventory())
                if (it.guid == b.guid) { infoItem_ = it; infoEntry_ = it.entry; }
            for (const w3n::InvItem& it : g.bankItems())
                if (it.guid == b.guid) { infoItem_ = it; infoEntry_ = it.entry; }
        }
        if (infoEntry_) open_ = Panel::ItemInfo;
    } else if (b.infoKind == 2 || b.infoKind == 3) {
        infoSpell_ = b.infoId;
        open_ = Panel::SpellInfo;
    }
    dirty_ = true;
}

void GameUi::touch(w3n::GameSession& g, const Input& in, Output& out) {
    constexpr uint32_t kLongMs = 500;
    if (in.touched) {
        press_ = Press();
        press_.active = true;
        press_.x0 = press_.x = in.touchX;
        press_.y0 = press_.y = in.touchY;
        press_.startMs = in.nowMs;
        if (const Button* b = hit(in.touchX, in.touchY)) { press_.button = *b; press_.onButton = true; }
        dirty_ = true;
    } else if (press_.active && in.touchHeld && !in.touchUp) {
        press_.x = in.touchX;
        press_.y = in.touchY;
        if (abs(press_.x - press_.x0) + abs(press_.y - press_.y0) > 8) press_.moved = true;
        const Button& b = press_.button;
        if (press_.onButton && press_.moved && b.dragKind && !drag_.active && !press_.longFired) {
            drag_ = Drag();
            drag_.active = true;
            drag_.kind = b.dragKind;
            drag_.id = b.dragId;
            dirty_ = true;
        }
        if (drag_.active) {
            drag_.x = press_.x;
            drag_.y = press_.y;
            int hover = -1;
            for (const Button& c : pickerCells_)
                if (press_.x >= c.x && press_.x < c.x + c.w && press_.y >= c.y && press_.y < c.y + c.h) hover = c.arg;
            drag_.hoverSlot = hover;
            dirty_ = true;
        } else if (press_.onButton && !press_.moved && !press_.longFired && b.infoKind && in.nowMs - press_.startMs >= kLongMs) {
            press_.longFired = true;
            showInfo(g, b);
        }
    }
    if (press_.active && in.touchUp) {
        if (drag_.active) {
            if (drag_.hoverSlot >= 0) dropOnSlot(in, drag_.hoverSlot, out);
            drag_ = Drag();
        } else if (press_.onButton && !press_.longFired) {
            const Button b = press_.button;
            const int slop = 6;
            if (press_.x >= b.x - slop && press_.x < b.x + b.w + slop && press_.y >= b.y - slop && press_.y < b.y + b.h + slop)
                doAction(g, in, b.act, b.arg, out);
        }
        press_ = Press();
        dirty_ = true;
    }
}

GameUi::Output GameUi::update(w3n::GameSession& g, const Input& in, bool walking) {
    clicked_ = false;
    Output out = updateInner(g, in, walking);
    soundEvents(g, in, out);
    return out;
}

// Sounds of the frame: the session's events (loot money, quests, whispers, mail, invites), a level gained, a new
// target (hostile / friendly / neutral / lost) and the touch screen itself: a window opening or closing (NPC windows,
// the auction house, the menu), the tab turned (bags, spell book, quest log have their own), else a button's click.
void GameUi::soundEvents(w3n::GameSession& g, const Input& in, Output& out) {
    for (const char* e : g.sounds) out.sounds.push_back(e);
    g.sounds.clear();
    if (const w3n::WorldObject* me = g.objects.find(g.selfGuid())) {
        const uint32_t lv = me->level();
        if (soundLevel_ && lv > soundLevel_) out.sounds.push_back("levelup");
        if (lv) soundLevel_ = lv;
    }
    const uint64_t t = g.target();
    if (t != soundTarget_) {
        const w3n::WorldObject* o = t ? g.objects.find(t) : nullptr;
        if (!t) out.sounds.push_back("targetlost");
        else if (o && !o->self) {
            const int r = (int)g.reactionTo(*o);
            out.sounds.push_back(g.enemyLike(*o) ? "targethostile" : r >= (int)w3n::Rank::Friendly ? "targetfriend" : "targetneutral");
        }
        soundTarget_ = t;
    }
    const uint8_t p = (uint8_t)panel(g), tb = (uint8_t)tab_;
    auto panelSound = [](Panel q, bool open) -> const char* {
        switch (q) {
            case Panel::Vendor: case Panel::Gossip: case Panel::Quest: case Panel::Trainer: case Panel::Binder:
            case Panel::Bank: case Panel::Taxi: case Panel::Mail: case Panel::Trade:
                return open ? "npcopen" : "npcclose";
            case Panel::Auction: return open ? "auctionopen" : "auctionclose";
            case Panel::Menu: case Panel::Settings: return open ? "open" : "close";
            default: return nullptr;
        }
    };
    const char* ui = nullptr;
    if (soundPanel_ != 0xFF && p != soundPanel_) {
        ui = panelSound((Panel)p, true);
        if (!ui) ui = panelSound((Panel)soundPanel_, false);
    }
    if (!ui && soundTab_ != 0xFF && tb != soundTab_ && (clicked_ || (in.pad && in.pad->tabDelta))) {
        switch (tab_) {
            case Tab::Bags: ui = "bagsopen"; break;
            case Tab::Spells: ui = "spellsopen"; break;
            case Tab::Quests: ui = "questsopen"; break;
            default: ui = "tab"; break;
        }
    }
    if (!ui && clicked_) ui = "click";
    if (ui) out.sounds.push_back(ui);
    soundPanel_ = p;
    soundTab_ = tb;
}

GameUi::Output GameUi::updateInner(w3n::GameSession& g, const Input& in, bool walking) {
    Output out;
    const w3n::Life life = g.life();
    out.blockMove = life == w3n::Life::Dead;
    if (in.cfg) { ui::g_lang = in.cfg->lang; bigText_ = in.cfg->bigText; }
    barsView_ = in.bars;
    if (!gamePage_) {   // the debug page (console): the top-right corner comes back
        if (in.touched && in.touchY < 16 && in.touchX >= 260) { gamePage_ = true; dirty_ = true; }
        return out;
    }
    // a new vendor / quest window starts at its top
    if (g.vendor.open && g.vendor.npc != vendorNpc_) { vendorNpc_ = g.vendor.npc; vendorMode_ = 0; vendorSel_ = -1; vendorPage_ = 0; dirty_ = true; }
    if (!g.vendor.open) vendorNpc_ = 0;
    if (!g.trainer.open) trainerSel_ = -1;
    {
        uint32_t key = g.questDialog.open() ? ((uint32_t)g.questDialog.kind << 24) ^ g.questDialog.quest ^ (uint32_t)g.questDialog.npc : 0;
        if (key != questKey_) { questKey_ = key; questScroll_ = 0; rewardPick_ = -1; dirty_ = true; }
    }
    {   // stage-6 windows start fresh for every NPC / object
        uint64_t w = g.bank.open ? g.bank.npc : g.taxi.open ? g.taxi.npc : g.mail.open ? g.mail.box : g.auction.open ? g.auction.npc : 0;
        if (w != windowNpc_) {
            windowNpc_ = w;
            bankSel_ = -1; bankMode_ = 0; taxiSel_ = -1; mailSel_ = -1; mailView_ = 0; auctionMode_ = 0; auctionSel_ = -1;
            auctionSellItem_ = -1; page_ = 0;
            if (g.auction.open) g.listOwnAuctions(in.nowMs);
            dirty_ = true;
        }
    }
    // the last enemy we had selected (Forever: R + B)
    if (const w3n::WorldObject* t = g.objects.find(g.target()))
        if (g.enemyLike(*t)) lastHostile_ = g.target();
    touch(g, in, out);
    autoTabs(g, in);
    float yaw;
    if (g.takeFacingRequest(&yaw)) { out.faceYaw = true; out.yaw = yaw; }
    const PadOut* p = in.pad;
    const bool forever = in.cfg && in.cfg->scheme == PadScheme::Forever;
    if (p) {
        int set = forever ? p->set : classicSet_;
        if (set != shownSet_) { shownSet_ = set; dirty_ = true; }
    }
    if (p && p->menu) { open_ = open_ == Panel::Menu ? Panel::Tabs : Panel::Menu; dirty_ = true; }
    if (p && p->freeCamera) out.freeCamera = true;
    if (p && p->tabDelta) {   // SELECT + D-pad left / right
        std::vector<Tab> order = tabOrder();
        Tab cur = tab_ == Tab::Gear ? Tab::Bags : tab_;
        int k = 0;
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == cur) k = (int)i;
        k = (k + p->tabDelta + (int)order.size()) % (int)order.size();
        setTab(order[k]);
        if (open_ != Panel::Tabs && open_ != Panel::Confirm) open_ = Panel::Tabs;
    }
    if (!walking || !p) return out;
    const Panel pnl = panel(g);
    if (p->jump) {
        // classic: B closes the window; nobody jumps while dead
        if (!forever && pnl == Panel::Loot) { g.closeLoot(in.nowMs); out.blockJump = true; }
        else if (!forever && pnl == Panel::Gossip) { g.closeGossip(); out.blockJump = true; }
        else if (!forever && pnl == Panel::Quest) { g.closeQuestDialog(in.nowMs); out.blockJump = true; }
        else if (!forever && pnl == Panel::Vendor) { g.closeVendor(); out.blockJump = true; }
        else if (!forever && pnl == Panel::Trainer) { g.closeTrainer(); out.blockJump = true; }
        else if (!forever && pnl == Panel::Invite) { g.answerGroupInvite(false, in.nowMs); out.blockJump = true; }
        if (life == w3n::Life::Dead) out.blockJump = true;
        dirty_ = true;
    }
    if (p->cancel) { cancelAction(g, in); dirty_ = true; }
    if (p->interact) { interactAction(g, in, out); dirty_ = true; }
    if (p->context) { open_ = open_ == Panel::Context ? Panel::Tabs : Panel::Context; dirty_ = true; }
    if (p->chat) openChat(g, in, out);
    if (p->nextTarget) {
        uint64_t t = g.pickTarget(in.pos, in.yaw, 50.0f, g.target(), in.nowMs);
        if (t) g.setTarget(t, in.nowMs);
        dirty_ = true;
    }
    if (p->toggleAttack || p->attack) { pressSpell(g, w3n::kSpellAttack, in, out); dirty_ = true; }
    if (p->nextSet) { classicSet_ = (classicSet_ + 1) % kPadSets; shownSet_ = classicSet_; dirty_ = true; }
    if (p->pageDelta && in.bars) {
        in.bars->turnPage(p->pageDelta);
        hint("Hotbar page " + std::to_string(in.bars->page + 1) + "/" + std::to_string(ActionLayout::kPages), in.nowMs);
    }
    // targeting modes (Forever L / R)
    if (p->aiming) {
        uint64_t a = aimUnit(g, in, p->aimYaw, p->mode == PadOut::Enemy);
        if (a != aim_) { aim_ = a; dirty_ = true; }
    } else if (p->mode == PadOut::None && p->released == PadOut::None) {
        aim_ = 0;
    }
    if (p->released != PadOut::None) {
        if (aim_) g.setTarget(aim_, in.nowMs);
        aim_ = 0;
        dirty_ = true;
    }
    if (p->lastHostile) {
        const w3n::WorldObject* o = g.objects.find(lastHostile_);
        if (o && !o->dead()) g.setTarget(lastHostile_, in.nowMs);
        else hint("No recent enemy", in.nowMs);
    }
    if (p->assist) {
        uint64_t tt = g.targetOf(g.target());
        if (tt) g.setTarget(tt, in.nowMs);
        else hint("Your target has no target", in.nowMs);
    }
    if (p->selfTarget) g.setTarget(g.selfGuid(), in.nowMs);
    if (p->partyPick >= 0) {   // stage 5.17: L + D-pad up/right/down/left = party member 1..4 (else the next friend)
        if (p->partyPick < (int)g.party.members.size()) g.setTarget(g.party.members[p->partyPick].guid, in.nowMs);
        else if (!g.party.inGroup) { uint64_t t = cycleUnits(g, in, false); if (t) g.setTarget(t, in.nowMs); }
        dirty_ = true;
    }
    if (p->cycle) {
        uint64_t t = cycleUnits(g, in, p->mode != PadOut::Friend);
        if (t) g.setTarget(t, in.nowMs);
        dirty_ = true;
    }
    if (p->slot >= 0) useSlot(g, in, p->slot, out);
    return out;
}

void GameUi::questPrimary(w3n::GameSession& g, const Input& in) {
    const w3n::QuestDialog& d = g.questDialog;
    switch (d.kind) {
        case w3n::QuestDialog::Kind::Details: g.acceptQuest(in.nowMs); break;
        case w3n::QuestDialog::Kind::RequestItems: g.continueQuest(in.nowMs); break;
        case w3n::QuestDialog::Kind::OfferReward:
            if (!d.choices.empty() && rewardPick_ < 0) hint("Choose a reward first (tap it)", in.nowMs);
            else g.completeQuest((size_t)std::max(0, rewardPick_), in.nowMs);
            break;
        case w3n::QuestDialog::Kind::List:
            if (!d.list.empty()) g.chooseQuest(d.npc, d.list[0], in.nowMs);
            break;
        default: break;
    }
}

std::vector<int> GameUi::trainerRows(const w3n::GameSession& g) const {
    std::vector<int> rows;
    for (size_t i = 0; i < g.trainer.spells.size(); ++i)
        if (trainerAll_ || g.trainer.spells[i].state != 2) rows.push_back((int)i);
    // learnable first, then not yet, then known; by level inside
    std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
        const w3n::TrainerSpell &x = g.trainer.spells[a], &y = g.trainer.spells[b];
        int ka = x.state == 0 ? 0 : x.state == 1 ? 1 : 2, kb = y.state == 0 ? 0 : y.state == 1 ? 1 : 2;
        return ka != kb ? ka < kb : x.reqLevel < y.reqLevel;
    });
    return rows;
}

void GameUi::doAction(w3n::GameSession& g, const Input& in, Act act, int arg, Output& out) {
    clicked_ = true;
    const uint32_t now = in.nowMs;
    dirty_ = true;
    switch (act) {
        case Act::Tab: setTab((Tab)arg); break;
        case Act::Pin: pinned_ = !pinned_; hint(pinned_ ? tr("Tab pinned: no automatic switching", "Вкладка закреплена: без автопереключения")
                                                     : tr("Tabs switch with combat again", "Вкладки снова переключаются по бою"), now); break;
        case Act::SubView:
            if (tab_ == Tab::Bags) { subBags_ = arg; selected_ = page_ = 0; }
            else if (tab_ == Tab::Spells) { subSpells_ = arg; page_ = 0; talentSel_ = -1; }
            else if (tab_ == Tab::Social) {
                subSocial_ = arg; socialSel_ = -1; page_ = 0;
                if (arg == 1) g.requestGuildRoster(now);
                if (arg >= 2 && !g.friendsSeen) g.requestFriendList(now);
            } else if (tab_ == Tab::Settings) subSettings_ = arg;
            else if (tab_ == Tab::Quests) { logView_ = arg; page_ = 0; }
            break;
        case Act::TargetUnit:
            if (arg >= 0 && arg < (int)rows_.size() && rows_[arg]) g.setTarget(rows_[arg], now);
            break;
        case Act::CancelCast: g.cancelCast(now); break;
        case Act::ConfirmYes: {
            open_ = confirmReturn_ == Panel::Confirm ? Panel::Tabs : confirmReturn_;
            confirmed_ = true;
            doAction(g, in, confirmAct_, confirmArg_, out);
            confirmed_ = false;
            break;
        }
        case Act::ConfirmNo: open_ = confirmReturn_ == Panel::Confirm ? Panel::Tabs : confirmReturn_; break;
        case Act::Release: g.releaseSpirit(now); break;
        case Act::Reclaim: {
            std::string why = g.interact(0, in.pos, now);
            if (!why.empty()) hint(why, now);
            break;
        }
        case Act::ResAccept: g.answerResurrect(true, now); break;
        case Act::ResDecline: g.answerResurrect(false, now); break;
        case Act::HealerYes: g.acceptSpiritHealer(now); break;
        case Act::HealerNo: g.spiritHealerConfirm = 0; break;
        case Act::LootItem: g.takeLoot((size_t)arg, now); break;
        case Act::LootMoney: g.takeLootMoney(now); break;
        case Act::LootAll: g.takeAllLoot(now); break;
        case Act::LootClose: g.closeLoot(now); break;
        case Act::GossipOption: g.selectGossip((size_t)arg, now); break;
        case Act::GossipQuest: g.selectGossipQuest((size_t)arg, now); break;
        case Act::GossipClose: g.closeGossip(); break;
        case Act::VendorMode: vendorMode_ = arg; vendorSel_ = -1; vendorPage_ = 0; break;
        case Act::VendorPick:
            if (arg == vendorSel_ && vendorMode_ < 2) doAction(g, in, Act::ItemInfo, vendorMode_ == 0 ? 1 : 2, out);
            vendorSel_ = arg;
            break;
        case Act::VendorBuy:
            if (vendorSel_ >= 0 && vendorSel_ < (int)g.vendor.items.size()) g.buyItem((size_t)vendorSel_, (uint32_t)arg, now);
            break;
        case Act::VendorSell: {
            std::vector<w3n::InvItem> items = sellable(g);
            if (vendorSel_ < 0 || vendorSel_ >= (int)items.size()) break;
            const w3n::ItemInfo* info = g.itemInfoIfKnown(items[vendorSel_].entry);
            if (info && info->quality >= 3 && !confirmed_) {   // rare and better: ask first
                confirm(std::string(tr("Sell ", "Продать ")) + g.itemName(items[vendorSel_].entry) + "?", Act::VendorSell, 0);
                break;
            }
            g.sellItem(items[vendorSel_], now);
            vendorSel_ = -1;
            break;
        }
        case Act::VendorBuyback: {
            std::vector<w3n::BuybackItem> bb = g.buybackItems();
            if (vendorSel_ >= 0 && vendorSel_ < (int)bb.size()) { g.buybackItem(bb[vendorSel_], now); vendorSel_ = -1; }
            break;
        }
        case Act::VendorRepair: g.repairAll(now); break;
        case Act::VendorClose: g.closeVendor(); break;
        case Act::TrainerPick: trainerSel_ = arg; break;
        case Act::TrainerTrain: if (trainerSel_ >= 0) g.trainSpell((size_t)trainerSel_, now); break;
        case Act::TrainerClose: g.closeTrainer(); break;
        case Act::TrainerFilter: trainerAll_ = !trainerAll_; page_ = 0; break;
        case Act::BinderYes: g.acceptBinder(now); break;
        case Act::BinderNo: g.binderConfirm = 0; break;
        case Act::InfoBack: open_ = (infoReturn_ == Panel::ItemInfo || infoReturn_ == Panel::SpellInfo) ? Panel::Tabs : infoReturn_; break;
        case Act::BuffPick:
            buffSel_ = (uint32_t)arg;
            if (panel(g) != Panel::Buffs) { infoReturn_ = open_; infoSpell_ = (uint32_t)arg; open_ = Panel::SpellInfo; }
            break;
        case Act::BuffCancel: if (buffSel_ && g.cancelAura(buffSel_, now)) buffSel_ = 0; break;
        case Act::InviteYes:
            if (!g.groupInviteFrom.empty()) g.answerGroupInvite(true, now);
            else g.answerGuildInvite(true, now);
            break;
        case Act::InviteNo:
            if (!g.groupInviteFrom.empty()) g.answerGroupInvite(false, now);
            else g.answerGuildInvite(false, now);
            break;
        case Act::ItemInfo: {
            infoReturn_ = open_;
            infoEntry_ = 0;
            infoItem_ = w3n::InvItem();
            if (arg == 0) {   // the selected bag / gear item
                std::vector<w3n::InvItem> items = listed(g);
                if (selected_ >= 0 && selected_ < (int)items.size()) { infoItem_ = items[selected_]; infoEntry_ = infoItem_.entry; }
            } else if (arg == 1 && vendorSel_ >= 0 && vendorSel_ < (int)g.vendor.items.size()) {
                infoEntry_ = g.vendor.items[vendorSel_].entry;
            } else if (arg == 2) {
                std::vector<w3n::InvItem> items = sellable(g);
                if (vendorSel_ >= 0 && vendorSel_ < (int)items.size()) { infoItem_ = items[vendorSel_]; infoEntry_ = infoItem_.entry; }
            } else if (arg >= 100 && arg - 100 < (int)g.questDialog.choices.size()) {
                infoEntry_ = g.questDialog.choices[arg - 100].item;
            }
            if (infoEntry_) open_ = Panel::ItemInfo;
            break;
        }
        case Act::SpellPick:
            infoReturn_ = open_;
            infoSpell_ = (uint32_t)arg;
            open_ = Panel::SpellInfo;
            break;
        case Act::QuestPick:
            if (arg >= 0 && arg < (int)g.questDialog.list.size()) g.chooseQuest(g.questDialog.npc, g.questDialog.list[arg], now);
            break;
        case Act::QuestAccept: g.acceptQuest(now); break;
        case Act::QuestContinue: g.continueQuest(now); break;
        case Act::QuestComplete:
            if (!g.questDialog.choices.empty() && rewardPick_ < 0) hint("Choose a reward first.", now);
            else g.completeQuest((size_t)std::max(0, rewardPick_), now);
            break;
        case Act::QuestReward: rewardPick_ = arg; break;
        case Act::QuestClose: g.closeQuestDialog(now); break;
        case Act::QuestScroll: questScroll_ = std::max(0, questScroll_ + arg); break;
        case Act::LogSelect: logSel_ = arg; logView_ = 1; questScroll_ = 0; break;
        case Act::LogBack: logView_ = 0; page_ = 0; break;
        case Act::LogReputation: logView_ = 2; page_ = 0; break;
        case Act::TrackQuest: {
            std::vector<w3n::QuestLogEntry> log = g.questLog();
            if (logSel_ >= 0 && logSel_ < (int)log.size()) tracked_ = tracked_ == log[logSel_].id ? 0 : log[logSel_].id;
            break;
        }
        case Act::LogAbandon: {
            std::vector<w3n::QuestLogEntry> log = g.questLog();
            if (logSel_ < 0 || logSel_ >= (int)log.size()) break;
            const w3n::QuestLogEntry& e = log[logSel_];
            if (!confirmed_) {
                confirm(std::string(tr("Abandon \"", "Отказаться от «")) + g.questTitle(e.id) + tr("\"?", "»?"), Act::LogAbandon, 0);
                break;
            }
            g.abandonQuest(e.slot, now);
            if (tracked_ == e.id) tracked_ = 0;
            logView_ = 0;
            break;
        }
        case Act::SelectItem:   // a second tap on the selected item shows its tooltip (stage 5.15)
            if (arg == selected_ && selTapped_) doAction(g, in, Act::ItemInfo, 0, out);
            selected_ = arg;
            selTapped_ = true;
            break;
        case Act::PagePrev:
            if (panel(g) == Panel::Vendor) vendorPage_ = std::max(0, vendorPage_ - 1);
            else page_ = std::max(0, page_ - 1);
            break;
        case Act::PageNext:
            if (panel(g) == Panel::Vendor) ++vendorPage_;
            else ++page_;
            break;
        case Act::BarCell: useSlot(g, in, arg, out); break;
        case Act::BindSpell:
            bindArmed_ = true;
            bindWhat_ = {ActionSlot::Spell, (uint32_t)arg};
            hint("Press a button (or tap a bar cell) for " + g.spellNames.name((uint32_t)arg), now);
            break;
        case Act::ClearSlot:
            bindArmed_ = true;
            bindWhat_ = ActionSlot();
            hint(tr("Press the button (or tap the cell) to clear", "Нажмите кнопку (или ячейку), чтобы очистить"), now);
            break;
        case Act::MenuItem:
            if (arg == 9 && !confirmed_) { confirm(tr("Quit the game?", "Выйти из игры?"), Act::MenuItem, 9); break; }
            open_ = Panel::Tabs;
            switch (arg) {
                case 0: setTab(Tab::Chat); openChat(g, in, out); break;
                case 1: setTab(Tab::Bags); subBags_ = 0; break;
                case 2: setTab(Tab::Bags); subBags_ = 1; break;
                case 3: setTab(Tab::Spells); break;
                case 4: setTab(Tab::Quests); logView_ = 0; break;
                case 5: open_ = Panel::Buffs; buffSel_ = 0; break;
                case 6: setTab(Tab::Settings); break;
                case 8: gamePage_ = false; out.showDebug = true; break;
                case 9: out.quit = true; break;
                case 10: setTab(Tab::Map); break;
                case 11: setTab(Tab::Social); break;
            }
            selected_ = page_ = 0;
            break;
        case Act::ContextItem:
            open_ = Panel::Tabs;
            switch (arg) {
                case 0: pressSpell(g, w3n::kSpellAttack, in, out); break;
                case 1: interactAction(g, in, out); break;
                case 2: { uint64_t t = g.targetOf(g.target()); if (t) g.setTarget(t, now); break; }
                case 3: {
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    if (t && t->type == w3n::ObjType::Player && !t->name.empty()) {
                        std::string text;
                        if (keyboard(g, in, out, ("Whisper to " + t->name).c_str(), text) && !text.empty())
                            g.sendChat(w3n::Chat::Whisper, text, t->name, now);
                    }
                    break;
                }
                case 4: g.setTarget(0, now); break;
                case 5: case 6: case 7: {   // stage 5.17: party
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    std::string nm = t ? (t->name.empty() ? g.objects.nameOf(t->guid) : t->name) : std::string();
                    if (!t || t->type != w3n::ObjType::Player || nm.empty()) { hint("Target a player first", now); break; }
                    if (arg == 5) g.inviteToGroup(nm, now);
                    else if (arg == 6) g.uninvite(nm, now);
                    else g.promoteLeader(t->guid, now);
                    break;
                }
                case 8: g.leaveGroup(now); break;
                case 9: {   // UI stage 6: trade, add friend, guild invite
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    if (t && t->type == w3n::ObjType::Player && !t->self) g.proposeTrade(t->guid, now);
                    else hint("Target a player first", now);
                    break;
                }
                case 10: {
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    std::string nm = t ? (t->name.empty() ? g.objects.nameOf(t->guid) : t->name) : std::string();
                    if (t && t->type == w3n::ObjType::Player && !nm.empty()) g.addFriend(nm, now);
                    else hint("Target a player first", now);
                    break;
                }
                case 11: {
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    std::string nm = t ? (t->name.empty() ? g.objects.nameOf(t->guid) : t->name) : std::string();
                    if (t && t->type == w3n::ObjType::Player && !nm.empty()) g.guildInvite(nm, now);
                    else hint("Target a player first", now);
                    break;
                }
            }
            break;
        case Act::Setting:
            if (!in.cfg) break;
            switch (arg) {
                case 0: in.cfg->scheme = PadScheme::Forever; break;
                case 1: in.cfg->scheme = PadScheme::Classic; break;
                case 10: case 11: case 12: in.cfg->runThreshold = 0.3f + 0.2f * (arg - 10); break;
                case 20: in.cfg->faceMovement = true; break;
                case 21: in.cfg->faceMovement = false; break;
                case 30: in.cfg->toggleModifiers = false; break;
                case 31: in.cfg->toggleModifiers = true; break;
                case 40: in.cfg->invertY = !in.cfg->invertY; break;
                case 50: in.cfg->camSpeed = std::max(0.6f, in.cfg->camSpeed - 0.3f); break;
                case 51: in.cfg->camSpeed = std::min(4.5f, in.cfg->camSpeed + 0.3f); break;
                case 60: in.cfg->questTracker = true; break;
                case 61: in.cfg->questTracker = false; break;
                case 180: in.cfg->showStats = true; break;
                case 181: in.cfg->showStats = false; break;
                case 70: case 71: case 72: case 73: in.cfg->drawDist = arg - 70; out.graphicsChanged = true; break;
                case 80: case 81: case 82: case 83: in.cfg->crowd = arg - 80; out.graphicsChanged = true; break;
                case 95: case 96: case 97: in.cfg->topMode = arg - 95; out.graphicsChanged = true; break;
                case 90: in.cfg->doodads = !in.cfg->doodads; out.graphicsChanged = true; break;
                case 170: case 171: case 172: in.cfg->effects = arg - 170; out.graphicsChanged = true; break;
                case 100: case 101: in.cfg->lang = arg - 100; ui::g_lang = in.cfg->lang; break;
                case 110: in.cfg->bigText = !in.cfg->bigText; break;
                case 120: in.cfg->dimHud = !in.cfg->dimHud; break;
                case 130: in.cfg->autoTabs = !in.cfg->autoTabs; break;
                case 140: case 141: case 150: case 151: case 160: case 161: {
                    int& v = arg < 150 ? in.cfg->musicVol : arg < 160 ? in.cfg->sfxVol : in.cfg->ambVol;
                    v = std::clamp(v + ((arg & 1) ? 1 : -1), 0, 10);
                    out.soundChanged = true;
                    break;
                }
            }
            out.configChanged = true;
            break;
        case Act::ClosePanel: open_ = Panel::Tabs; bindArmed_ = false; break;
        case Act::OpenItem: {
            std::vector<w3n::InvItem> items = listed(g);
            if (selected_ >= 0 && selected_ < (int)items.size()) g.openItem(items[selected_], now);
            break;
        }
        case Act::BindItem: case Act::Use: case Act::Equip: case Act::Unequip: case Act::Destroy: {
            std::vector<w3n::InvItem> items = listed(g);
            if (selected_ < 0 || selected_ >= (int)items.size()) break;
            const w3n::InvItem& it = items[selected_];
            if (act == Act::BindItem) {
                bindArmed_ = true;
                bindWhat_ = {ActionSlot::Item, it.entry};
                hint("Press a button (or tap a bar cell) for " + g.itemName(it.entry), now);
            } else if (act == Act::Use) g.useItem(it, now);
            else if (act == Act::Equip) g.equipItem(it, now);
            else if (act == Act::Unequip) g.unequipItem(it, now);
            else if (!confirmed_) confirm(std::string(tr("Destroy ", "Уничтожить ")) + g.itemName(it.entry) +
                                          (it.count > 1 ? " x" + std::to_string(it.count) : std::string()) + "?", Act::Destroy, 0);
            else g.destroyItem(it, now);
            break;
        }
        // ---- chat (UI stage 3)
        case Act::ChatChannel: chatChannel_ = arg; chatScroll_ = 0; break;
        case Act::ChatScroll: chatScroll_ = std::max(0, chatScroll_ + arg); break;
        case Act::ChatWrite: {
            static const char* kPrefix[] = {"", "/s ", "/p ", "/g ", "", ""};
            std::string prefix = arg >= 0 && arg < 6 ? kPrefix[arg] : "";
            if (arg == 4) {   // whisper: to the last one who whispered
                for (size_t i = g.chat.size(); i-- > 0;)
                    if (g.chat[i].type == w3n::Chat::Whisper || g.chat[i].type == w3n::Chat::WhisperInform) {
                        std::string who = g.chat[i].name.empty() ? g.chat[i].channel : g.chat[i].name;
                        if (!who.empty()) { prefix = "/w " + who + " "; break; }
                    }
            }
            openChat(g, in, out, prefix);
            break;
        }
        case Act::ChatPhrases: phrases_ = !phrases_; break;
        case Act::EmotePick:   // 2000 / 2001 the popup's pages, 999 stand up, 1000 sit, 1003 lie down, else a text emote
            if (arg >= 2000) { phrasePage_ = arg - 2000; break; }
            if (arg >= 999) g.setStandState(arg == 999 ? 0 : arg == 1003 ? 3 : 1, now);
            else g.textEmote((uint32_t)arg, now);
            phrases_ = false;
            break;
        case Act::PhrasePick: {
            static const uint8_t kType[] = {w3n::Chat::Say, w3n::Chat::Say, w3n::Chat::Party, w3n::Chat::Guild, w3n::Chat::Say, w3n::Chat::Say};
            uint8_t type = chatChannel_ >= 0 && chatChannel_ < 6 ? kType[chatChannel_] : (uint8_t)w3n::Chat::Say;
            if (type == w3n::Chat::Party && !g.party.inGroup) type = w3n::Chat::Say;
            if (type == w3n::Chat::Guild && !g.guildId()) type = w3n::Chat::Say;
            g.sendChat(type, gameUiPhrase(arg), "", now);
            phrases_ = false;
            break;
        }
        // ---- map (UI stage 3)
        case Act::MapMode: mapMode_ = arg; break;
        case Act::MapZoom: mapZoom_ = std::clamp(arg, 1, 4); break;
        // ---- talents (UI stage 4)
        case Act::TalentTree: talentTree_ = arg; talentSel_ = -1; break;
        case Act::TalentPick: talentSel_ = arg; break;
        case Act::TalentLearn: if (talentSel_ > 0) g.learnTalent((uint32_t)talentSel_, now); break;
        // ---- social (UI stage 6)
        case Act::SocialPick: socialSel_ = arg; break;
        case Act::FriendAdd: case Act::IgnoreAdd: case Act::GroupInvite: {
            std::string name;
            const char* h = act == Act::FriendAdd ? tr("Friend's name", "Имя друга") : act == Act::IgnoreAdd ? tr("Name to ignore", "Кого игнорировать")
                                                                                     : tr("Invite whom?", "Кого пригласить?");
            if (!keyboard(g, in, out, h, name) || name.empty()) break;
            if (act == Act::FriendAdd) g.addFriend(name, now);
            else if (act == Act::IgnoreAdd) g.addIgnore(name, now);
            else if (subSocial_ == 1) g.guildInvite(name, now);
            else g.inviteToGroup(name, now);
            break;
        }
        case Act::FriendRemove:
            if (arg >= 0 && arg < (int)g.friends.size()) {
                if (!confirmed_) { confirm(std::string(tr("Remove ", "Удалить ")) + g.playerName(g.friends[arg].guid, now) + tr(" from friends?", " из друзей?"), Act::FriendRemove, arg); break; }
                g.removeFriend(g.friends[arg].guid, now);
                socialSel_ = -1;
            }
            break;
        case Act::IgnoreRemove:
            if (arg >= 0 && arg < (int)g.ignored.size()) { g.removeIgnore(g.ignored[arg], now); socialSel_ = -1; }
            break;
        case Act::Whisper: {
            std::string who;
            if (subSocial_ == 2 && arg >= 0 && arg < (int)g.friends.size()) who = g.playerName(g.friends[arg].guid, now);
            else if (subSocial_ == 1 && arg >= 0 && arg < (int)g.guild.members.size()) who = g.guild.members[arg].name;
            else if (subSocial_ == 0 && arg >= 0 && arg < (int)g.party.members.size()) who = g.party.members[arg].name;
            if (who.empty()) break;
            std::string text;
            if (keyboard(g, in, out, (std::string(tr("Whisper to ", "Шёпот: ")) + who).c_str(), text) && !text.empty())
                g.sendChat(w3n::Chat::Whisper, text, who, now);
            break;
        }
        case Act::GroupLeave:
            if (subSocial_ == 1) {
                if (!confirmed_) { confirm(tr("Leave your guild?", "Покинуть гильдию?"), Act::GroupLeave, 0); break; }
                g.leaveGuild(now);
            } else {
                g.leaveGroup(now);
            }
            break;
        case Act::GroupKick:
            if (arg >= 0 && arg < (int)g.party.members.size()) g.uninvite(g.party.members[arg].name, now);
            break;
        case Act::GroupPromote:
            if (arg >= 0 && arg < (int)g.party.members.size()) g.promoteLeader(g.party.members[arg].guid, now);
            break;
        case Act::GuildRefresh: g.requestGuildRoster(now); break;
        case Act::GuildAccept: g.answerGuildInvite(true, now); break;
        case Act::GuildDecline: g.answerGuildInvite(false, now); break;
        // ---- bank (UI stage 6)
        case Act::BankMode: bankMode_ = arg; bankSel_ = -1; page_ = 0; break;
        case Act::BankPick: bankSel_ = arg; break;
        case Act::BankDeposit: {
            std::vector<w3n::InvItem> items = sellable(g);
            if (bankSel_ >= 0 && bankSel_ < (int)items.size()) { g.depositItem(items[bankSel_], now); bankSel_ = -1; }
            break;
        }
        case Act::BankWithdraw: {
            std::vector<w3n::InvItem> items = g.bankItems();
            if (bankSel_ >= 0 && bankSel_ < (int)items.size()) { g.withdrawItem(items[bankSel_], now); bankSel_ = -1; }
            break;
        }
        case Act::BankBuySlot:
            if (!confirmed_) { confirm(tr("Buy a bank bag slot?", "Купить ячейку для сумки в банке?"), Act::BankBuySlot, 0); break; }
            g.buyBankSlot(now);
            break;
        case Act::BankClose: g.closeBank(); break;
        // ---- flight master (UI stage 6)
        case Act::TaxiPick: taxiSel_ = arg; break;
        case Act::TaxiFly: if (taxiSel_ > 0) g.flyTo((uint32_t)taxiSel_, now); break;
        case Act::TaxiClose: g.closeTaxi(); break;
        // ---- mail (UI stage 6)
        case Act::MailPick: {
            mailSel_ = arg;
            mailView_ = 1;
            questScroll_ = 0;
            if (arg >= 0 && arg < (int)g.mail.mails.size()) g.markMailRead(g.mail.mails[arg].id, now);
            break;
        }
        case Act::MailBack: mailView_ = 0; break;
        case Act::MailTakeMoney: if (mailSel_ >= 0 && mailSel_ < (int)g.mail.mails.size()) g.takeMailMoney(g.mail.mails[mailSel_].id, now); break;
        case Act::MailTakeItem: if (mailSel_ >= 0 && mailSel_ < (int)g.mail.mails.size()) g.takeMailItem(g.mail.mails[mailSel_].id, now); break;
        case Act::MailDelete:
            if (mailSel_ >= 0 && mailSel_ < (int)g.mail.mails.size()) {
                const w3n::MailEntry& m = g.mail.mails[mailSel_];
                if ((m.item || m.money) && !confirmed_) {
                    confirm(tr("Delete the letter with what is attached?", "Удалить письмо вместе с вложением?"), Act::MailDelete, 0);
                    break;
                }
                g.deleteMail(m.id, now);
                mailView_ = 0;
                mailSel_ = -1;
            }
            break;
        case Act::MailCompose: mailView_ = 2; mailItem_ = -1; mailMoney_ = 0; break;
        case Act::MailField: {
            std::string* f = arg == 0 ? &mailTo_ : arg == 1 ? &mailSubject_ : arg == 2 ? &mailBody_ : nullptr;
            if (f) { keyboard(g, in, out, arg == 0 ? tr("To", "Кому") : arg == 1 ? tr("Subject", "Тема") : tr("Text", "Текст"), *f); break; }
            if (arg == 3) {   // money in copper via the numpad
                std::string v = mailMoney_ ? std::to_string(mailMoney_) : "";
                if (keyboard(g, in, out, tr("Copper (100 = 1s)", "Медь (100 = 1с)"), v, true)) mailMoney_ = (uint32_t)strtoul(v.c_str(), nullptr, 10);
            }
            if (arg == 4) { mailItem_ = mailItem_ + 1 >= (int)sellable(g).size() ? -1 : mailItem_ + 1; }   // next item to attach
            break;
        }
        case Act::MailSend: {
            std::vector<w3n::InvItem> items = sellable(g);
            uint64_t item = mailItem_ >= 0 && mailItem_ < (int)items.size() ? items[mailItem_].guid : 0;
            if (g.sendMail(mailTo_, mailSubject_, mailBody_, item, mailMoney_, now)) {
                mailView_ = 0;
                mailSubject_.clear();
                mailBody_.clear();
                mailMoney_ = 0;
                mailItem_ = -1;
            }
            break;
        }
        case Act::MailClose: g.closeMail(); break;
        // ---- auction house (UI stage 6)
        case Act::AuctionMode:
            auctionMode_ = arg;
            auctionSel_ = -1;
            if (arg == 1) g.listOwnAuctions(now);
            break;
        case Act::AuctionSearch: {
            std::string q = auctionQuery_;
            if (arg == 0 && !keyboard(g, in, out, tr("Item name (empty = all)", "Название (пусто = все)"), q)) break;
            auctionQuery_ = q;
            g.searchAuctions(q, arg > 0 ? (uint32_t)(arg - 1) : 0, now);
            auctionSel_ = -1;
            break;
        }
        case Act::AuctionPick: auctionSel_ = arg; break;
        case Act::AuctionBuyout: case Act::AuctionBid: {
            if (auctionSel_ < 0 || auctionSel_ >= (int)g.auction.results.size()) break;
            const w3n::AuctionItem& a = g.auction.results[auctionSel_];
            uint32_t price = act == Act::AuctionBuyout ? a.buyout : a.nextBid();
            if (!price) break;
            if (!confirmed_) {
                char b[160];
                snprintf(b, sizeof b, "%s %s %s?", act == Act::AuctionBuyout ? tr("Buy out", "Выкупить") : tr("Bid", "Ставка"),
                         g.itemName(a.item).c_str(), w3n::moneyText(price).c_str());
                confirm(b, act, arg);
                break;
            }
            g.bidAuction(a, price, now);
            break;
        }
        case Act::AuctionSellPick: auctionSellItem_ = arg; auctionBid_ = auctionBuyout_ = 0; break;
        case Act::AuctionPrice: {
            if (arg == 2) { auctionHours_ = (auctionHours_ + 1) % 3; break; }
            uint32_t& v = arg == 0 ? auctionBid_ : auctionBuyout_;
            std::string s = v ? std::to_string(v) : "";
            if (keyboard(g, in, out, arg == 0 ? tr("Starting bid, copper", "Начальная ставка, медь") : tr("Buyout, copper (0 = none)", "Выкуп, медь (0 = нет)"), s, true))
                v = (uint32_t)strtoul(s.c_str(), nullptr, 10);
            break;
        }
        case Act::AuctionSell: {
            std::vector<w3n::InvItem> items = sellable(g);
            if (auctionSellItem_ < 0 || auctionSellItem_ >= (int)items.size()) break;
            static const uint32_t kMin[3] = {120, 480, 1440};
            if (g.sellAuction(items[auctionSellItem_], auctionBid_, auctionBuyout_, kMin[auctionHours_], now)) {
                auctionSellItem_ = -1;
                g.listOwnAuctions(now);
            }
            break;
        }
        case Act::AuctionCancel:
            if (arg >= 0 && arg < (int)g.auction.owned.size()) {
                if (!confirmed_) { confirm(tr("Cancel this auction? The deposit is lost.", "Снять лот? Залог не вернётся."), Act::AuctionCancel, arg); break; }
                g.cancelAuction(g.auction.owned[arg].id, now);
            }
            break;
        case Act::AuctionClose: g.closeAuction(); break;
        // ---- trade (UI stage 6)
        case Act::TradeAnswer: g.answerTrade(arg != 0, now); break;
        case Act::TradePick: tradeSel_ = arg; break;
        case Act::TradeOffer: {
            std::vector<w3n::InvItem> items = sellable(g);
            if (tradeSel_ < 0 || tradeSel_ >= (int)items.size()) { hint(tr("Tap one of your items first", "Сначала выберите свой предмет"), now); break; }
            int slot = -1;
            for (int i = 0; i < 6 && slot < 0; ++i)
                if (!g.trade.myItems[i].guid) slot = i;
            if (slot < 0) { hint(tr("All six slots are used", "Все шесть ячеек заняты"), now); break; }
            g.offerTradeItem(slot, items[tradeSel_], now);
            tradeSel_ = -1;
            break;
        }
        case Act::TradeClear: g.clearTradeItem(arg, now); break;
        case Act::TradeGold: {
            std::string v = g.trade.mine.gold ? std::to_string(g.trade.mine.gold) : "";
            if (keyboard(g, in, out, tr("Copper to give", "Сколько меди отдать"), v, true)) g.offerTradeGold((uint32_t)strtoul(v.c_str(), nullptr, 10), now);
            break;
        }
        case Act::TradeAccept: g.acceptTrade(now); break;
        case Act::TradeCancel: g.cancelTrade(now); break;
        default: break;
    }
}
