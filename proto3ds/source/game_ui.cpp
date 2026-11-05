// game_ui.cpp — the game logic behind the touch screen and the controller: windows by priority, touch (tap on
// release, long press, drag), automatic tabs, confirmations, and every action a button or pad intent stands for.
// Drawing: game_ui_widgets.cpp (frame, header, tab bar, widgets), game_ui_tabs.cpp, game_ui_windows.cpp.
#include "game_ui.hpp"
#include "ui_render.hpp"

// 6.46: "/gputest" typed in the chat (main.cpp asks with gameUiTakeGpuTest)
static bool g_gpuTestAsk = false;
bool gameUiTakeGpuTest() { const bool r = g_gpuTestAsk; g_gpuTestAsk = false; return r; }

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
    logView_ = 0; logSel_ = 0; watch_.clear(); watchLoaded_ = false; watchReadyMs_ = 0; logIds_.clear();
    subBags_ = subSpells_ = subSocial_ = subSettings_ = 0;
    chatChannel_ = chatScroll_ = 0;
    phrases_ = false;
    mapMode_ = 0; mapZoom_ = 1; mapPanned_ = false; mapPickMap_ = mapPickArea_ = 0; mapUnits_ = false; mapQuests_ = true;
    talentTree_ = 0; talentSel_ = -1; socialSel_ = -1;
    press_ = Press();
    drag_ = Drag();
    bankSel_ = -1; bankMode_ = 0; taxiSel_ = -1; mailSel_ = -1; mailView_ = 0;
    mailTo_.clear(); mailSubject_.clear(); mailBody_.clear(); mailMoney_ = 0; mailItem_ = -1;
    auctionMode_ = 0; auctionSel_ = -1; auctionSellItem_ = -1; auctionQuery_.clear(); auctionBid_ = auctionBuyout_ = 0;
    tradeSel_ = -1;
    windowNpc_ = 0;
    kb_ = Kb();
    kbReturn_ = Panel::Tabs;
    chatDest_ = "s";
}

GameUi::Panel GameUi::panel(const w3n::GameSession& g) const {
    // 6.82: the death page is the Combat tab's content; the other tabs work as always (it used to cover the whole screen)
    if (tab_ == Tab::Combat && (g.life() != w3n::Life::Alive || g.resurrectOffer.active || g.spiritHealerConfirm)) return Panel::Death;
    if (kb_.on) return Panel::Keyboard;
    if (open_ == Panel::Confirm) return Panel::Confirm;
    if (!g.groupInviteFrom.empty() || !g.guildInviteFrom.empty() || g.duel.challenged || g.duel.asked) return Panel::Invite;
    if (!g.lootRolls.empty()) return Panel::Roll;   // 6.88: a group loot roll waits for our choice (touch only: a key could roll by mistake)
    if (g.trade.phase != w3n::TradeState::Phase::None) return Panel::Trade;
    if (g.lootWindow.open) return Panel::Loot;
    if (open_ == Panel::ItemInfo || open_ == Panel::SpellInfo) return open_;   // a tooltip over any window
    if (g.questDialog.open()) return Panel::Quest;
    if (g.vendor.open) return Panel::Vendor;
    if (g.trainer.open) return Panel::Trainer;
    if (g.binderConfirm) return Panel::Binder;
    if (g.bank.open) return Panel::Bank;
    if (g.taxi.open) return Panel::Taxi;
    if (g.stable.open) return Panel::Stable;
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
        bindArmed_ = false;   // 6.57g: a "Bind" begun in the bags must not stay over the next tab (its item's name on the crafting window)
        drag_ = Drag();
    }
    if (t == Tab::Gear) { tab_ = Tab::Bags; subBags_ = 1; }
    else {
        if (t == Tab::Bags && tab_ != Tab::Bags) subBags_ = 0;
        tab_ = t;
    }
    if (open_ == Panel::Menu || open_ == Panel::Context || open_ == Panel::Buffs || open_ == Panel::Pet) open_ = Panel::Tabs;
    dirty_ = true;
}

void GameUi::deathTab(w3n::GameSession& g, const Input& in) {
    (void)in;
    const uint8_t key = (uint8_t)((g.life() == w3n::Life::Dead ? 1 : 0) | (g.resurrectOffer.active ? 2 : 0) | (g.spiritHealerConfirm ? 4 : 0));
    const uint8_t fresh = (uint8_t)(key & ~deathKey_);
    deathKey_ = key;
    if (fresh && !pinned_ && tab_ != Tab::Combat) setTab(Tab::Combat);   // a death, an offer, a question: where they are answered
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
        if (gear != eq) continue;
        if (!gear && bagFilter_ >= 0) {   // 6.95: one bag only
            const int home = it.bag == w3n::kBagSelf ? 0 : it.bag - (w3n::kSlotBagFirst - 1);
            if (home != bagFilter_) continue;
        }
        out.push_back(it);
    }
    return out;
}

// "All", "Pack", or the kind of the bag in that slot: Bag, Quiver, Ammo, Soul, Leather, Herb, Enchant, Engineer
std::string GameUi::bagLabel(const w3n::GameSession& g, int f) const {
    if (f < 0) return tr("All bags", "Все сумки");
    if (f == 0) return tr("Backpack", "Рюкзак");
    for (const w3n::InvItem& it : g.inventory()) {
        if (it.bag != w3n::kBagSelf || it.slot != w3n::kSlotBagFirst + f - 1) continue;
        const w3n::ItemInfo* info = g.itemInfoIfKnown(it.entry);
        const uint32_t sub = info && info->cls == 1 ? info->subclass : 0;
        switch (sub) {
            case 1: return tr("Quiver", "Колчан");
            case 2: return tr("Ammo pouch", "Подсумок");
            case 3: return tr("Soul bag", "Сумка душ");
            case 4: return tr("Leatherwork", "Кожевник.");
            case 6: return tr("Herb bag", "Травы");
            case 7: return tr("Enchanting", "Зачарован.");
            case 8: return tr("Engineering", "Инженер.");
            case 9: return tr("Keyring", "Ключи");
            default: return std::string(tr("Bag", "Сумка")) + " " + std::to_string(f);
        }
    }
    return std::string(tr("Bag", "Сумка")) + " " + std::to_string(f);
}

void GameUi::cycleBagFilter(const w3n::GameSession& g) {
    for (int n = 0; n < 6; ++n) {
        bagFilter_ = bagFilter_ >= 4 ? -1 : bagFilter_ + 1;
        if (bagFilter_ <= 0) break;
        bool have = false;
        for (const w3n::InvItem& it : g.inventory()) have = have || (it.bag == w3n::kBagSelf && it.slot == w3n::kSlotBagFirst + bagFilter_ - 1);
        if (have) break;
    }
    selected_ = page_ = 0;
}

std::vector<w3n::InvItem> GameUi::sellable(const w3n::GameSession& g) const {
    std::vector<w3n::InvItem> out;
    for (const w3n::InvItem& it : g.inventory())
        if (!(it.bag == w3n::kBagSelf && it.slot < w3n::kSlotBackpackFirst)) out.push_back(it);   // bags' contents only
    return out;
}

std::vector<uint32_t> GameUi::bookSpells(const w3n::GameSession& g) const {
    std::vector<uint32_t> out{w3n::kSpellAttack};
    // 6.95: spells without an icon are internal (triggers, effects, ranks the game never shows): hidden, except those drawn
    // with the weapon in hand; only judged when the pack has the icons at all
    const bool judge = ui::assets().iconCount() > 0;
    for (uint32_t s : g.spells)
        if (g.spellNames.known(s) && s != w3n::kSpellAttack && !g.spellNames.hidden(s) && (!judge || ui::assets().spellIcon(s).ok() || weaponSpell(g, s, nullptr)))
            out.push_back(s);
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
    if (g.life() != w3n::Life::Alive) { refuse(g, "You are dead", in.nowMs); return; }
    if (g.spellNames.ground(spell)) { beginPlace(g, in, spell, 0, out); return; }   // UI stage 10
    const w3n::WorldObject* t = g.objects.find(g.target());
    bool toggleOff = spell == w3n::kSpellAttack && g.attackWanted();
    // soft target: without a living target, the enemy in front (melee range for Attack, 30 yd for spells)
    if (!toggleOff && !g.spellNames.selfCast(spell) && (!t || t->dead() || t->self)) {
        uint64_t s = g.softEnemy(in.pos, in.yaw, spell == w3n::kSpellAttack ? 8.0f : 30.0f, 1.05f, in.nowMs);
        if (s) { g.setTarget(s, in.nowMs); t = g.objects.find(s); }
    }
    if (!toggleOff && !g.spellNames.selfCast(spell) && t && t->hasPos && !t->dead() && !t->self) faceTarget(g, in, out);
    g.castSpell(spell, in.nowMs);
}

// ------------------------------------------------------------------ ground spells (UI stage 10)
// Like the console versions of WoW (and ConsolePort's reticle targeting): the spell does not go off at once - a ring
// shows where it will land, the C-Stick moves it (the way the camera looks), the same button again casts. The ring
// starts on the target when it is in range, else in front of us; it never leaves the spell's range.
void GameUi::beginPlace(w3n::GameSession& g, const Input& in, uint32_t spell, uint32_t item, Output& out) {
    const w3n::SpellNames::Ground* gr = g.spellNames.ground(spell);
    if (place_.on && place_.spell == spell && place_.item == item) { confirmPlace(g, in, out); return; }   // pressed again
    place_ = Place();
    place_.spell = spell;
    place_.item = item;
    place_.range = gr ? std::max(1.0f, gr->range) : 30.0f;
    place_.radius = gr ? std::max(1.0f, gr->radius) : 5.0f;
    place_.lastMs = in.nowMs;
    bool onTarget = false;
    if (const w3n::WorldObject* t = g.objects.find(g.target()); t && t->hasPos && !t->dead() && !t->self) {
        float p[3];
        t->positionAt(in.nowMs, p);
        if (dist3(p, in.pos) <= place_.range) { memcpy(place_.pos, p, sizeof p); onTarget = true; }
    }
    if (onTarget && in.cfg && in.cfg->groundCast == 1) { place_.on = true; confirmPlace(g, in, out); return; }   // "At target"
    if (!onTarget) {   // in front of us: as far as the camera looks, up to 10 yd (and the range)
        const float d = std::min(10.0f, place_.range * 0.8f);
        place_.pos[0] = in.pos[0] + cosf(in.camYaw) * d;
        place_.pos[1] = in.pos[1] + sinf(in.camYaw) * d;
        place_.pos[2] = in.pos[2];
        float z;
        if (in.groundAt && in.groundAt(place_.pos[0], place_.pos[1], in.pos[2] + 6.0f, &z)) place_.pos[2] = z;
    }
    place_.on = true;
    dirty_ = true;
}

void GameUi::updatePlace(w3n::GameSession& g, const Input& in, Output& out) {
    if (!place_.on) return;
    if (g.life() != w3n::Life::Alive) { place_.on = false; return; }
    const float dt = std::min(0.1f, (in.nowMs - place_.lastMs) / 1000.0f);
    place_.lastMs = in.nowMs;
    // the C-Stick moves the ring: up = away along the camera, right = to the camera's right; faster when far
    const float speed = 14.0f + 0.5f * place_.range;
    const float fx = cosf(in.camYaw), fy = sinf(in.camYaw);
    place_.pos[0] += (fx * in.stickY + fy * in.stickX) * speed * dt;
    place_.pos[1] += (fy * in.stickY - fx * in.stickX) * speed * dt;
    // within range of us (also when we walk away from it)
    const float dx = place_.pos[0] - in.pos[0], dy = place_.pos[1] - in.pos[1], d = sqrtf(dx * dx + dy * dy);
    const float maxD = std::max(0.5f, place_.range - 0.5f);
    if (d > maxD) { place_.pos[0] = in.pos[0] + dx / d * maxD; place_.pos[1] = in.pos[1] + dy / d * maxD; }
    float z;
    if (in.groundAt && in.groundAt(place_.pos[0], place_.pos[1], std::max(place_.pos[2], in.pos[2]) + 6.0f, &z)) place_.pos[2] = z;
    out.placing = true;
    memcpy(out.placePos, place_.pos, sizeof place_.pos);
    out.placeRadius = place_.radius;
    out.placeName = place_.item ? g.itemName(place_.item) : g.spellNames.name(place_.spell);
}

void GameUi::confirmPlace(w3n::GameSession& g, const Input& in, Output& out) {
    if (!place_.on) return;
    place_.on = false;
    dirty_ = true;
    const float dx = place_.pos[0] - in.pos[0], dy = place_.pos[1] - in.pos[1];
    if (dx * dx + dy * dy > (place_.range + 1.0f) * (place_.range + 1.0f)) { g.error("Out of range", w3n::Vocal::OutOfRange, in.nowMs); return; }
    if (dx * dx + dy * dy > 1.0f) { out.faceYaw = true; out.yaw = atan2f(dy, dx); }   // face where it goes
    if (place_.item) {
        w3n::InvItem it;
        if (barsView_ && barsView_->findItem(g, place_.item, &it)) g.useItemAt(it, place_.pos, in.nowMs);
        else refuse(g, "None left: " + g.itemName(place_.item), in.nowMs);
    } else {
        g.castSpellAt(place_.spell, place_.pos, in.nowMs);
    }
}

// An item that works on a target (quest items): turn to the selected unit first, as for a spell. Without a target it is
// sent as before (the server answers with its own error); never blocked here: a self spell we do not recognise must work.
void GameUi::useItemFaced(w3n::GameSession& g, const w3n::InvItem& it, const Input& in, Output& out) {
    if (g.itemTargetSpell(it.entry)) {
        const w3n::WorldObject* t = g.objects.find(g.target());
        if (t && !t->self && t->hasPos && (t->type == w3n::ObjType::Unit || t->type == w3n::ObjType::Player)) faceTarget(g, in, out);
    }
    g.useItem(it, in.nowMs);
}

void GameUi::useSlot(w3n::GameSession& g, const Input& in, int slot, Output& out) {
    if (!in.bars || slot < 0 || slot >= kPadSlots) return;
    if (slot >= 4 && slot < 8) {   // set 0 face buttons: the fixed actions
        if (bindArmed_) { refuse(g, tr("That button is fixed: pick another", "Эта кнопка занята: выберите другую"), in.nowMs); return; }
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
        if (!in.bars->findItem(g, a.id, &it)) refuse(g, "None left: " + g.itemName(a.id), in.nowMs);
        else if (uint32_t gs = g.itemGroundSpell(a.id)) beginPlace(g, in, gs, a.id, out);   // bombs, dummies: on the ground
        else useItemFaced(g, it, in, out);
    } else if (a.type == ActionSlot::PetCmd) {
        doAction(g, in, Act::PetCommand, (int)a.id, out);
    } else if (a.type == ActionSlot::PetSpell) {
        doAction(g, in, Act::PetCast, (int)a.id, out);
    } else {
        hint(tr("Empty slot: drag a spell here (Spells tab)", "Пустая ячейка: перетащите сюда заклинание (Книга)"), in.nowMs);
    }
    dirty_ = true;
}

void GameUi::dropOnSlot(const Input& in, int slot, Output& out) {
    if (!in.bars || !padSlotBindable(slot) || !drag_.kind) return;
    ActionSlot a{drag_.kind >= 1 && drag_.kind <= 4 ? drag_.kind : (uint8_t)ActionSlot::Item, drag_.id};   // 1 spell, 2 item, 3 pet command, 4 pet spell
    in.bars->bind(slot, a);
    out.layoutChanged = true;
    std::string where = std::string(padSetName(slot / 8)) + (slot / 8 ? "+" : "") + padButtonGlyph(slot % 8);
    hint("Bound to " + where, in.nowMs);
}

void GameUi::interactAction(w3n::GameSession& g, const Input& in, Output& out) {
    const Panel p = panel(g);
    const bool forever = in.cfg && in.cfg->scheme == PadScheme::Forever;
    if (place_.on) { confirmPlace(g, in, out); return; }   // UI stage 10: cast the ground spell at the ring
    if (p == Panel::Loot) { g.takeAllLoot(in.nowMs); return; }
    if (p == Panel::Quest) { questPrimary(g, in); return; }
    if (p == Panel::Invite) {
        if (g.duel.challenged) g.answerDuel(true, in.nowMs);
        else if (g.duel.asked) return;   // (our own challenge: only Cancel)
        else if (!g.groupInviteFrom.empty()) g.answerGroupInvite(true, in.nowMs);
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
    if (!why.empty()) refuse(g, why, in.nowMs);
    else faceTarget(g, in, out);
}

void GameUi::cancelAction(w3n::GameSession& g, const Input& in) {
    if (drag_.active) { drag_ = Drag(); dirty_ = true; return; }
    if (bindArmed_) { bindArmed_ = false; hint("Binding cancelled", in.nowMs); return; }
    if (place_.on) { place_.on = false; dirty_ = true; return; }   // UI stage 10: the ground spell is not cast
    if (card_) { card_ = false; dirty_ = true; return; }   // 6.91: the item's mini window
    const Panel p = panel(g);
    if (p == Panel::Confirm) { open_ = confirmReturn_; return; }
    if (p == Panel::Menu || p == Panel::Context || p == Panel::ItemInfo || p == Panel::SpellInfo || p == Panel::Buffs ||
        p == Panel::Inspect) {
        open_ = Panel::Tabs;
        return;
    }
    if (p == Panel::Pet) { if (petFeedList_) petFeedList_ = false; else open_ = Panel::Tabs; return; }
    if (p == Panel::Stable) { g.closeStable(); return; }
    if (phrases_) { phrases_ = false; return; }
    if (p == Panel::Loot) { g.closeLoot(in.nowMs); return; }
    if (p == Panel::Quest) { g.closeQuestDialog(in.nowMs); return; }
    if (p == Panel::Invite) {
        if (g.duel.challenged || g.duel.asked) g.answerDuel(false, in.nowMs);
        else if (!g.groupInviteFrom.empty()) g.answerGroupInvite(false, in.nowMs);
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

void GameUi::openChat(w3n::GameSession& g, const Input& in, Output& out, const std::string& prefix, const std::string& link) {
    (void)out;
    // the destination of the Write button decides the prefix (an explicit prefix wins); a typed "/command" is never prefixed
    const std::string pre = prefix.empty() ? chatDestPrefix(g, chatDest_) : prefix;
    const std::string dest = prefix.empty() ? chatDestLabel(g, chatDest_) : std::string();
    g.mover.stopAll(in.nowMs);
    // 6.94: an item link from the item's card starts the message (the keyboard shows its [Name] in the hint; what is typed follows it);
    // with a link the text is never taken for a command
    std::string hint = dest.empty() ? std::string(tr("Type a message or /command", "Сообщение или /команда")) : dest + ":";
    if (!link.empty()) {
        const std::vector<w3n::ItemLink> ls = w3n::itemLinks(link);
        hint = (dest.empty() ? std::string(tr("Message", "Сообщение")) + ":" : hint) + " " + (ls.empty() ? std::string() : ls[0].shown);
    }
    askText(in, hint, "", false,
            [pre, link](w3n::GameSession& gg, uint32_t now, const std::string& text) {
                if (text.empty() && link.empty()) return;
                if (link.empty() && text == "/gputest") { g_gpuTestAsk = true; return; }   // 6.46: the GPU test (main.cpp), never sent
                if (!link.empty()) gg.sendTyped(pre + link + (text.empty() ? std::string() : " " + text), now);
                else gg.sendTyped(text[0] == '/' ? text : pre + text, now);
            });
}

const GameUi::Button* GameUi::hit(int x, int y) const {
    // the last drawn wins (overlays are drawn after what they cover)
    for (size_t i = buttons_.size(); i-- > 0;) {
        const Button& b = buttons_[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return &b;
    }
    return nullptr;
}

// 6.91: the item's mini window - over whatever is open (a quest, the loot, a roll, the chat ...), closed by a tap or B
void GameUi::openCard(uint32_t entry, const w3n::InvItem* inst) {
    if (!entry) return;
    infoEntry_ = entry;
    infoItem_ = inst ? *inst : w3n::InvItem();
    card_ = true;
    dirty_ = true;
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
        if (infoEntry_) { infoReturn_ = Panel::Tabs; card_ = true; }   // 6.91: the mini window over the open one
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
        const Button* b = hit(in.touchX, in.touchY);
        // 6.91: the item's mini window goes away with any tap; one outside it also reaches what it touched
        if (card_ && (!b || (b->act != Act::CardClose && b->act != Act::CardLink))) card_ = false;
        if (b) { press_.button = *b; press_.onButton = true; }
        press_.inScroll = qsOn_ && in.touchX >= qsX_ && in.touchX < qsX_ + qsW_ && in.touchY >= qsY_ && in.touchY < qsY_ + qsH_;   // 6.94
        qsAcc_ = 0;
        dirty_ = true;
    } else if (press_.active && in.touchHeld && !in.touchUp) {
        const int prevX = press_.x, prevY = press_.y;
        const bool wasMoved = press_.moved;
        press_.x = in.touchX;
        press_.y = in.touchY;
        if (abs(press_.x - press_.x0) + abs(press_.y - press_.y0) > 8) press_.moved = true;
        if (press_.onButton && press_.button.act == Act::MapTap && press_.moved && mapSpan_ < 0.999f) {   // 6.69, 6.74: the stylus drags the map
            // 6.74: the picture follows the stylus one to one. The step is added to the centre kept here (before, it was taken from the
            // last drawn picture, and the window is redrawn only when marked - so most of the movement was lost), the first step
            // counts from where the stylus went down, and every step asks for a redraw.
            const int fx = wasMoved ? prevX : press_.x0, fy = wasMoved ? prevY : press_.y0;
            const float cu = mapPanned_ ? mapCu_ : mapU0_ + mapSpan_ / 2, cv = mapPanned_ ? mapCv_ : mapV0_ + mapSpan_ / 2;
            mapCu_ = std::clamp(cu - (float)(press_.x - fx) / (float)mapRectW_ * mapSpan_, mapSpan_ / 2, 1 - mapSpan_ / 2);
            mapCv_ = std::clamp(cv - (float)(press_.y - fy) / (float)mapRectH_ * mapSpan_, mapSpan_ / 2, 1 - mapSpan_ / 2);
            mapPanned_ = true;
            dirty_ = true;
        }
        if (press_.inScroll && press_.moved && !drag_.active) {   // 6.94: the quest text follows the finger (a line per 13 px)
            const int fy = (wasMoved || press_.scrolling) ? prevY : press_.y0;
            qsAcc_ -= press_.y - fy;
            press_.scrolling = true;
            const int step = 13;
            while (qsAcc_ >= step) { ++questScroll_; qsAcc_ -= step; dirty_ = true; }
            while (qsAcc_ <= -step) { questScroll_ = std::max(0, questScroll_ - 1); qsAcc_ += step; dirty_ = true; }
        }
        const Button& b = press_.button;
        if (press_.onButton && press_.moved && b.dragKind && !drag_.active && !press_.longFired && !press_.scrolling) {
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
        } else if (press_.onButton && !press_.longFired && !press_.scrolling) {   // (6.94: a scroll of the text is not a tap)
            const Button b = press_.button;
            const int slop = 6;
            if (b.act == Act::MapTap && press_.moved) {}   // 6.69: a drag of the map is not a tap
            else if (press_.x >= b.x - slop && press_.x < b.x + b.w + slop && press_.y >= b.y - slop && press_.y < b.y + b.h + slop)
                doAction(g, in, b.act, b.arg, out);
        }
        press_ = Press();
        dirty_ = true;
    }
}

GameUi::Output GameUi::update(w3n::GameSession& g, const Input& in, bool walking) {
    clicked_ = false;
    syncWatch(g, in);
    Output out = updateInner(g, in, walking);
    updatePlace(g, in, out);   // UI stage 10: the ground spell's ring follows the C-Stick
    soundEvents(g, in, out);
    return out;
}

// 6.63: the quest watch: loaded once from the card, kept to the quests in the log (new ones followed while there is room, as the
// original does on accepting), saved when it changes.
static const char* kWatchFile = "sdmc:/3ds/w3dproto/quests_followed.txt";
void GameUi::syncWatch(w3n::GameSession& g, const Input& in) {
    if (!watchLoaded_) {
        watchLoaded_ = true;
        if (FILE* f = fopen(kWatchFile, "rb")) {
            char buf[256];
            const size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[n] = 0;
            watch_.parse(buf);
        }
        watchReadyMs_ = in.nowMs ? in.nowMs : 1;
    }
    std::vector<w3n::QuestLogEntry> log = g.questLog();
    logIds_.clear();
    for (const w3n::QuestLogEntry& e : log) logIds_.push_back(e.id);
    // the log has arrived when it has a quest, or the world has been ours for a while (a really empty log)
    const bool ready = !logIds_.empty() || (watchReadyMs_ && in.nowMs - watchReadyMs_ > 20000);
    if (watch_.sync(logIds_, ready)) dirty_ = true;
    if (watch_.changedSinceSave()) {
        uiWriteFileAsync(kWatchFile, watch_.serialize());
        watch_.saved();
    }
}

// 6.69: a tap on the map: on the world picture the left half is Kalimdor, the right Eastern Kingdoms; on a continent the zone whose
// rectangle holds the tapped point (the smallest, as the zone map of our own position is chosen) opens.
ui::MapView GameUi::zoneUnder(const Input& in, uint32_t mapId, float wx, float wy, uint32_t* shape) const {
    if (shape) *shape = 0;
    if (in.zoneAtPoint) {
        const uint32_t z = in.zoneAtPoint(mapId, wx, wy);
        if (z) {
            ui::MapView v = ui::assets().zoneByArea(z);
            if (v.ok() && v.m->mapId == mapId) {
                if (shape) *shape = z;
                return v;
            }
        }
    }
    return ui::assets().zoneAt(mapId, wx, wy);
}

void GameUi::mapTap(const Input& in) {
    const float u = mapU0_ + (float)(press_.x - mapRectX_) / (float)mapRectW_ * mapSpan_;
    const float v = mapV0_ + (float)(press_.y - mapRectY_) / (float)mapRectH_ * mapSpan_;
    if (!mapShown_ || mapShown_->mapId == 0xFFFFFFFFu) {   // the world (or its stand-in)
        mapPickMap_ = u < 0.5f ? 1 : 0;
        mapPickArea_ = 0;
        mapMode_ = 3;
        mapPanned_ = false;
        return;
    }
    if (mapShown_->areaId) return;   // a zone: nothing below it
    const float wy = mapShown_->left - u * (mapShown_->left - mapShown_->right);
    const float wx = mapShown_->top - v * (mapShown_->top - mapShown_->bottom);
    ui::MapView z = zoneUnder(in, mapShown_->mapId, wx, wy, nullptr);   // 7.04: by the zone's shape
    if (!z.ok() || !z.m->areaId) return;
    mapPickMap_ = z.m->mapId;
    mapPickArea_ = z.m->areaId;
    mapMode_ = 3;
    mapPanned_ = false;
}

const questdb::Quest* GameUi::questAreas(uint32_t id) {
    for (const QuestAreas& c : qareas_) if (c.id == id) return c.ok ? &c.q : nullptr;
    if (qareas_.size() >= 16) qareas_.erase(qareas_.begin());
    QuestAreas c;
    c.id = id;
    c.ok = qdb_.opened() && qdb_.find(id, &c.q);
    qareas_.push_back(std::move(c));
    return qareas_.back().ok ? &qareas_.back().q : nullptr;
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
    craftTick(g, in);   // 6.38: Create All
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
    deathTab(g, in);
    autoTabs(g, in);
    if (kb_.on) {   // the on-screen keyboard: the character stands still, B closes it
        out.blockMove = out.blockJump = true;
        g.mover.stopAll(in.nowMs);
        if (in.pad && in.pad->cancel) closeKeyboard();
        return out;
    }
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
        else refuse(g, "No recent enemy", in.nowMs);
    }
    if (p->assist) {
        uint64_t tt = g.targetOf(g.target());
        if (tt) g.setTarget(tt, in.nowMs);
        else refuse(g, "Your target has no target", in.nowMs);
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
            if (!d.choices.empty() && rewardPick_ < 0) refuse(g, "Choose a reward first (tap it)", in.nowMs);
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
            bindArmed_ = false;
            if (tab_ == Tab::Bags && arg == 10) cycleBagFilter(g);   // 6.95: the bag chip
            else if (tab_ == Tab::Bags) { subBags_ = arg; selected_ = page_ = 0; }
            else if (tab_ == Tab::Spells) { subSpells_ = arg; page_ = 0; talentSel_ = -1; skillSel_ = 0; craftPick_ = false; }
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
            if (!why.empty()) refuse(g, why, now);
            break;
        }
        case Act::ResAccept: g.answerResurrect(true, now); break;
        case Act::ResDecline: g.answerResurrect(false, now); break;
        case Act::HealerTalk: {   // 6.82
            std::string why = g.talkToHealer(in.pos, now);
            if (!why.empty()) refuse(g, why, now);
            break;
        }
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
        case Act::VendorSellJunk: {   // 6.95: all grey items of the bags
            std::vector<w3n::InvItem> items = sellable(g);
            std::vector<w3n::InvItem> junk;
            uint32_t total = 0;
            for (const w3n::InvItem& it : items) {
                const w3n::ItemInfo* ji = g.itemInfoIfKnown(it.entry);
                if (ji && !ji->missing && ji->quality == 0 && g.sellValue(it)) { junk.push_back(it); total += g.sellValue(it); }
            }
            if (junk.empty()) break;
            if (!confirmed_ && junk.size() > 1) {
                char b[96];
                snprintf(b, sizeof b, "%s %u (%u%s %u%s %u%s)?", tr("Sell grey items:", "Продать хлам:"), (unsigned)junk.size(),
                         (unsigned)(total / 10000), "g", (unsigned)(total / 100 % 100), "s", (unsigned)(total % 100), "c");
                confirm(b, Act::VendorSellJunk, 0);
                break;
            }
            for (const w3n::InvItem& it : junk) g.sellItem(it, now);
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
        case Act::RollVote: g.rollLoot(0, (uint8_t)arg, now); break;   // 6.88
        case Act::InviteYes:
            if (g.duel.challenged) g.answerDuel(true, now);
            else if (!g.groupInviteFrom.empty()) g.answerGroupInvite(true, now);
            else g.answerGuildInvite(true, now);
            break;
        case Act::InviteNo:
            if (g.duel.challenged || g.duel.asked) g.answerDuel(false, now);
            else if (!g.groupInviteFrom.empty()) g.answerGroupInvite(false, now);
            else g.answerGuildInvite(false, now);
            break;
        case Act::InspectSlot: {
            Button b{};
            b.infoKind = 1;
            b.infoId = (uint32_t)arg;
            showInfo(g, b);
            break;
        }
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
            } else if (arg == 999 && press_.button.infoId) {   // a quest's fixed reward / wanted item: its line
                infoEntry_ = press_.button.infoId;
            }
            infoReturn_ = Panel::Tabs;
            if (infoEntry_) card_ = true;   // 6.91: the mini window, not a page of its own
            break;
        }
        case Act::ItemCard: openCard((uint32_t)arg); break;   // 6.91
        case Act::CardClose: card_ = false; break;
        case Act::CardLink: {   // 6.94: "|cffRRGGBB|Hitem:ENTRY:0:0:0|h[Name]|h|r" (the 1.12 form) at the start of a chat message
            const uint32_t entry = infoEntry_;
            card_ = false;
            if (!entry) break;
            const w3n::ItemInfo* info = g.itemInfo(entry);
            static const char* kQ[] = {"9d9d9d", "ffffff", "1eff00", "0070dd", "a335ee", "ff8000", "e5cc80"};
            const unsigned q = info && info->quality < 7 ? (unsigned)info->quality : 1u;
            const std::string name = g.itemName(entry);
            const std::string link = std::string("|cff") + kQ[q] + "|Hitem:" + std::to_string(entry) + ":0:0:0|h[" + name + "]|h|r";
            openChat(g, in, out, "", link);
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
            if (!g.questDialog.choices.empty() && rewardPick_ < 0) refuse(g, "Choose a reward first.", now);
            else g.completeQuest((size_t)std::max(0, rewardPick_), now);
            break;
        case Act::QuestReward:   // 6.91: choosing a reward shows it too
            rewardPick_ = arg;
            if (arg >= 0 && arg < (int)g.questDialog.choices.size()) openCard(g.questDialog.choices[arg].item);
            break;
        case Act::QuestClose: g.closeQuestDialog(now); break;
        case Act::QuestScroll: questScroll_ = std::max(0, questScroll_ + arg); break;
        case Act::LogSelect: logSel_ = arg; logView_ = 1; questScroll_ = 0; break;
        case Act::SkillPick: skillSel_ = skillSel_ == (uint32_t)arg ? 0 : (uint32_t)arg; break;
        case Act::SkillHeader: skillFolded_ ^= 1u << arg; page_ = 0; break;
        case Act::SkillAbandon: {   // (the server lets only the professions go: SKILL_FLAG_UNLEARNABLE, anything else is reported as a cheat)
            const w3n::SkillLine* sl = skillSel_ ? g.skills.find(skillSel_) : nullptr;
            if (!sl || sl->category != 11) break;
            if (!confirmed_) {
                confirm(std::string(tr("Unlearn ", "Забыть навык «")) + sl->name + tr("? Its progress is lost.", "»? Прогресс будет потерян."),
                        Act::SkillAbandon, 0);
                break;
            }
            g.unlearnSkill(skillSel_, now);
            skillSel_ = 0;
            break;
        }
        case Act::LogBack: logView_ = 0; page_ = 0; break;
        case Act::LogReputation: logView_ = 2; page_ = 0; break;
        case Act::TrackQuest: {
            std::vector<w3n::QuestLogEntry> log = g.questLog();
            if (logSel_ >= 0 && logSel_ < (int)log.size()) {
                if (!watch_.toggle(log[logSel_].id)) hint(tr("Only 5 quests can be followed.", "Следить можно за 5 заданиями."), now);
            }
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
            watch_.remove(e.id);
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
            if (g.spellNames.passive((uint32_t)arg)) {   // 6.95: passives are never bound
                hint(tr("Passive: cannot be put on a bar", "Пассивное: нельзя поставить на панель"), now);
                break;
            }
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
            if (arg == 13 && !confirmed_) { confirm(tr("Log out to the character list?", "Выйти к списку персонажей?"), Act::MenuItem, 13); break; }
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
                case 13: out.characters = true; break;
                case 10: setTab(Tab::Map); break;
                case 11: setTab(Tab::Social); break;
                case 12: open_ = Panel::Pet; petFeedList_ = false; petSel_ = -1; break;
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
                        const std::string nm = t->name;
                        askText(in, std::string(tr("Whisper to ", "Шёпот: ")) + nm, "", false, [nm](w3n::GameSession& gg, uint32_t n, const std::string& text) {
                            if (!text.empty()) { gg.lastWhisperTo = nm; gg.sendChat(w3n::Chat::Whisper, text, nm, n); }
                        });
                    }
                    break;
                }
                case 4: g.setTarget(0, now); break;
                case 5: case 6: case 7: {   // stage 5.17: party
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    std::string nm = t ? (t->name.empty() ? g.objects.nameOf(t->guid) : t->name) : std::string();
                    if (!t || t->type != w3n::ObjType::Player || nm.empty()) { refuse(g, "Target a player first", now); break; }
                    if (arg == 5) g.inviteToGroup(nm, now);
                    else if (arg == 6) g.uninvite(nm, now);
                    else g.promoteLeader(t->guid, now);
                    break;
                }
                case 8: g.leaveGroup(now); break;
                case 9: {   // UI stage 6: trade, add friend, guild invite
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    if (t && t->type == w3n::ObjType::Player && !t->self) g.proposeTrade(t->guid, now);
                    else refuse(g, "Target a player first", now);
                    break;
                }
                case 10: {
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    std::string nm = t ? (t->name.empty() ? g.objects.nameOf(t->guid) : t->name) : std::string();
                    if (t && t->type == w3n::ObjType::Player && !nm.empty()) g.addFriend(nm, now);
                    else refuse(g, "Target a player first", now);
                    break;
                }
                case 11: {
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    std::string nm = t ? (t->name.empty() ? g.objects.nameOf(t->guid) : t->name) : std::string();
                    if (t && t->type == w3n::ObjType::Player && !nm.empty()) g.guildInvite(nm, now);
                    else refuse(g, "Target a player first", now);
                    break;
                }
                case 12:   // UI stage 9: inspect, follow, duel, ignore, stop following
                    if (g.inspect(g.target(), in.pos, now)) { inspect_ = g.target(); open_ = Panel::Inspect; }
                    break;
                case 13: g.startFollow(g.target(), in.pos, now); break;
                case 14: g.challengeDuel(now); break;
                case 15: {
                    const w3n::WorldObject* t = g.objects.find(g.target());
                    std::string nm = t ? (t->name.empty() ? g.objects.nameOf(t->guid) : t->name) : std::string();
                    if (t && t->type == w3n::ObjType::Player && !nm.empty()) { g.addIgnore(nm, now); g.setTarget(0, now); }
                    else refuse(g, "Target a player first", now);
                    break;
                }
                case 16: g.stopFollow(now); break;
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
                case 190: in.cfg->groundCast = 0; break;
                case 191: in.cfg->groundCast = 1; break;
                case 180: in.cfg->showStats = true; break;
                case 181: in.cfg->showStats = false; break;
                case 200: in.cfg->perfLog = true; break;
                case 201: in.cfg->perfLog = false; break;
                case 202: in.cfg->perfLog = !in.cfg->perfLog; break;
                case 203: in.cfg->gpuKick = !in.cfg->gpuKick; break;   // stage 6.3
                case 204: in.cfg->pace = (in.cfg->pace + 1) % 3; break;   // 6.71: the frame pacer: off -> 30 -> auto (was VSync, stage 6.4)
                case 205: in.cfg->fadeIn = !in.cfg->fadeIn; break;     // stage 6.11 (the renderer reads it every frame)
                case 70: case 71: case 72: case 73: in.cfg->drawDist = arg - 70; out.graphicsChanged = true; break;
                case 75: in.cfg->autoDetail = !in.cfg->autoDetail; break;   // 6.72 (main.cpp reads it every frame)
                case 210: case 211: case 212: in.cfg->horizon = arg - 210; out.graphicsChanged = true; break;   // 6.34b
                case 80: case 81: case 82: case 83: in.cfg->crowd = arg - 80; out.graphicsChanged = true; break;
                case 95: case 96: case 97: in.cfg->topMode = arg - 95; out.graphicsChanged = true; break;
                case 90: in.cfg->doodads = !in.cfg->doodads; out.graphicsChanged = true; break;
                case 170: case 171: case 172: in.cfg->effects = arg - 170; out.graphicsChanged = true; break;
                case 100: case 101: in.cfg->lang = arg - 100; ui::g_lang = in.cfg->lang; break;
                case 110: in.cfg->bigText = !in.cfg->bigText; break;
                case 120: in.cfg->dimHud = !in.cfg->dimHud; break;
                case 121: in.cfg->chatBubbles = !in.cfg->chatBubbles; break;
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
            } else if (act == Act::Use) useItemFaced(g, it, in, out);
            else if (act == Act::Equip) g.equipItem(it, now);
            else if (act == Act::Unequip) g.unequipItem(it, now);
            else if (!confirmed_) confirm(std::string(tr("Destroy ", "Уничтожить ")) + g.itemName(it.entry) +
                                          (it.count > 1 ? " x" + std::to_string(it.count) : std::string()) + "?", Act::Destroy, 0);
            else g.destroyItem(it, now);
            break;
        }
        // ---- chat (UI stage 3)
        case Act::ChatChannel: {
            chatChannel_ = arg;
            chatScroll_ = 0;
            // the tab picks the matching destination when there is one (the Write button then goes there)
            const std::vector<std::string> ds = chatDests(g);
            auto has = [&](const std::string& d) { return std::find(ds.begin(), ds.end(), d) != ds.end(); };
            if (arg == 2) { if (has("p")) chatDest_ = g.party.raid && has("r") ? "r" : "p"; }
            else if (arg == 3) { if (has("g")) chatDest_ = "g"; }
            else if (arg == 4) { if (has("w")) chatDest_ = "w"; }
            else if (arg == 5) { for (const std::string& d : ds) if (d[0] == 'c') { chatDest_ = d; break; } }
            break;
        }
        case Act::ChatScroll: chatScroll_ = std::max(0, chatScroll_ + arg); break;
        case Act::ChatWrite: openChat(g, in, out); break;
        case Act::ChatDest: {   // the Write button's destination: the next one
            const std::vector<std::string> ds = chatDests(g);
            size_t k = 0;
            for (size_t i = 0; i < ds.size(); ++i) if (ds[i] == chatDest_) k = i + 1;
            chatDest_ = ds[k % ds.size()];
            break;
        }
        case Act::ChannelSay: {   // arg = slot: write into that channel
            const std::string d = "c" + std::to_string(arg);
            chatDest_ = d;
            phrases_ = false;
            openChat(g, in, out);
            break;
        }
        case Act::ChannelList: {
            const auto& ch = g.channelSlots();
            if (arg >= 1 && (size_t)arg <= ch.size() && !ch[arg - 1].empty()) g.listChannel(ch[arg - 1], now);
            phrases_ = false;
            chatChannel_ = 5;
            break;
        }
        case Act::ChannelLeave: {
            const auto& ch = g.channelSlots();
            if (arg >= 1 && (size_t)arg <= ch.size() && !ch[arg - 1].empty()) {
                g.leaveChannel(ch[arg - 1], now);
                if (chatDest_ == "c" + std::to_string(arg)) chatDest_ = "s";
            }
            break;
        }
        case Act::ChannelJoin:
            askText(in, tr("Channel name (then password after a space)", "Канал (через пробел пароль)"), "", false,
                    [](w3n::GameSession& gg, uint32_t n, const std::string& t) {
                        if (!t.empty()) gg.sendTyped("/join " + t, n);
                    }, 40);
            break;
        case Act::KeyboardKey: keyboardKey(g, in, arg); break;
        case Act::ChatPhrases: phrases_ = !phrases_; break;
        case Act::EmotePick:   // 2000 / 2001 the popup's pages, 999 stand up, 1000 sit, 1003 lie down, else a text emote
            if (arg >= 2000) { phrasePage_ = arg - 2000; break; }
            if (arg >= 999) g.setStandState(arg == 999 ? 0 : arg == 1003 ? 3 : 1, now);
            else g.textEmote((uint32_t)arg, now);
            phrases_ = false;
            break;
        case Act::PhrasePick: {
            // the phrase goes where the Write button goes (a channel or a whisper too)
            g.sendTyped(chatDestPrefix(g, chatDest_) + gameUiPhrase(arg), now);
            phrases_ = false;
            break;
        }
        // ---- map (UI stage 3)
        case Act::MapMode:
            mapPanned_ = false;
            if (arg == 1 && mapMode_ == 3) {   // "Continent" from a picked zone: its continent
                mapPickArea_ = 0;
                if (mapPickMap_ == in.mapId) mapMode_ = 1;
            } else {
                mapMode_ = arg;
            }
            break;
        case Act::MapTap: mapTap(in); break;
        case Act::MapZoom: mapZoom_ = std::clamp(arg, 1, 4); if (mapZoom_ == 1) mapPanned_ = false; break;
        case Act::MapUnits: mapUnits_ = arg != 0; break;
        case Act::MapQuests: mapQuests_ = arg != 0; break;
        // ---- talents (UI stage 4)
        case Act::TalentTree: talentTree_ = arg; talentSel_ = -1; break;
        case Act::TalentPick: talentSel_ = arg; break;
        case Act::TalentLearn: if (talentSel_ > 0) g.learnTalent((uint32_t)talentSel_, now); break;
        // ---- social (UI stage 6)
        case Act::SocialPick: socialSel_ = arg; break;
        case Act::FriendAdd: case Act::IgnoreAdd: case Act::GroupInvite: {
            const char* h = act == Act::FriendAdd ? tr("Friend's name", "Имя друга") : act == Act::IgnoreAdd ? tr("Name to ignore", "Кого игнорировать")
                                                                                     : tr("Invite whom?", "Кого пригласить?");
            const Act a0 = act;
            const bool guild = subSocial_ == 1;
            askText(in, h, "", false, [a0, guild](w3n::GameSession& gg, uint32_t n, const std::string& name) {
                if (name.empty()) return;
                if (a0 == Act::FriendAdd) gg.addFriend(name, n);
                else if (a0 == Act::IgnoreAdd) gg.addIgnore(name, n);
                else if (guild) gg.guildInvite(name, n);
                else gg.inviteToGroup(name, n);
            }, 12);
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
            askText(in, std::string(tr("Whisper to ", "Шёпот: ")) + who, "", false, [who](w3n::GameSession& gg, uint32_t n, const std::string& text) {
                if (!text.empty()) { gg.lastWhisperTo = who; gg.sendChat(w3n::Chat::Whisper, text, who, n); }
            });
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
        // ---- pets (UI stage 13)
        case Act::PetOpen: open_ = Panel::Pet; petFeedList_ = false; page_ = 0; break;
        case Act::PetCommand: {
            petSel_ = arg;
            if (!g.pet.active) { refuse(g, tr("You have no pet", "У вас нет питомца"), now); break; }
            switch (arg) {
                case kPetCmdAttack: g.petCommand(w3n::kPetAttack, now); break;
                case kPetCmdFollow: g.petCommand(w3n::kPetFollow, now); break;
                case kPetCmdStay: g.petCommand(w3n::kPetStay, now); break;
                case kPetCmdPassive: g.petReaction(w3n::kPetPassive, now); break;
                case kPetCmdDefensive: g.petReaction(w3n::kPetDefensive, now); break;
                case kPetCmdAggressive: g.petReaction(w3n::kPetAggressive, now); break;
            }
            break;
        }
        case Act::PetCast:
            petSel_ = 100000 + arg;
            if (!g.pet.active) refuse(g, tr("You have no pet", "У вас нет питомца"), now);
            else g.petCastSpell((uint32_t)arg, now);
            break;
        case Act::PetAuto: g.petSetAutocast((uint32_t)arg, !g.petAutocast((uint32_t)arg), now); break;
        case Act::PetBind:
            if (arg >= 100000) bindWhat_ = {ActionSlot::PetSpell, (uint32_t)(arg - 100000)};
            else if (arg >= 1 && arg <= 6) bindWhat_ = {ActionSlot::PetCmd, (uint32_t)arg};
            else break;
            bindArmed_ = true;
            hint("Press a button (or tap a bar cell) for " + actionName(g, bindWhat_), now);
            break;
        case Act::PetRename: {
            askText(in, tr("Pet's name (2-12 letters)", "Имя питомца (2-12 букв)"), g.petName(), false,
                    [](w3n::GameSession& gg, uint32_t n, const std::string& name) { if (!name.empty()) gg.petRename(name, n); }, 12);
            break;
        }
        case Act::PetDismiss:
            if (!g.pet.active) break;
            if (!confirmed_) {
                confirm(g.petCanAbandon() ? tr("Abandon the pet FOR GOOD? It cannot be called back.", "Бросить питомца НАВСЕГДА? Его не вернуть.")
                                          : tr("Dismiss the pet?", "Отпустить питомца?"),
                        Act::PetDismiss, 0);
                break;
            }
            g.petDismiss(now);
            open_ = Panel::Tabs;
            break;
        case Act::PetSendAway:   // 7.07: a hunter's pet sent away with Dismiss Pet (Call Pet brings it back) - not abandoned
            if (g.pet.active && g.petDismissHunter(now)) open_ = Panel::Tabs;
            break;
        case Act::PetFeed: petFeedList_ = arg != 0; page_ = 0; break;
        case Act::PetFeedPick: {
            std::vector<w3n::InvItem> food = g.petFoodCandidates();
            if (arg >= 0 && arg < (int)food.size() && g.feedPet(food[arg], now)) petFeedList_ = false;
            break;
        }
        case Act::StablePick: stableSel_ = arg; break;
        case Act::CraftSkill: craftSkill_ = (uint32_t)arg; craftSel_ = 0; craftPick_ = false; craftLeft_ = 0; page_ = 0; break;
        case Act::CraftPick:
            if (arg == 0) { craftPick_ = false; page_ = 0; }
            else { craftSel_ = (uint32_t)arg; craftLeft_ = 0; }
            break;
        case Act::CraftMake: {
            const w3n::Recipe* r = g.recipes.find(craftSel_);
            if (!r) break;
            if (r->kind == 1) { craftPick_ = true; page_ = 0; break; }
            craftLeft_ = 0;
            for (const w3n::Recipe::Part& p : r->reagents)
                if (g.itemCount(p.entry) < p.count) { refuse(g, tr("You do not have the reagents", "Не хватает реагентов"), now); return; }
            craftCast(g, in, r->spell, 0);   // (a refusal prints its own line)
            break;
        }
        case Act::CraftAll: {
            const w3n::Recipe* r = g.recipes.find(craftSel_);
            if (!r || r->kind != 0) break;
            uint32_t most = 9999;
            for (const w3n::Recipe::Part& p : r->reagents) most = std::min<uint32_t>(most, g.itemCount(p.entry) / p.count);
            if (r->reagents.empty()) most = 1;
            if (most == 0) break;
            if (craftCast(g, in, r->spell, 0)) craftLeft_ = (int)most - 1;
            break;
        }
        case Act::CraftStop: craftLeft_ = 0; g.cancelCast(now); break;
        case Act::CraftHaveMats: craftHaveMats_ = !craftHaveMats_; page_ = 0; break;   // 6.90
        case Act::CraftItem: {
            const std::vector<w3n::InvItem> items = enchantTargets(g, g.recipes.find(craftSel_));   // the picker's own list
            if (arg >= 0 && arg < (int)items.size()) {
                craftPick_ = false;
                craftCast(g, in, craftSel_, items[arg].guid);
            }
            break;
        }
        case Act::StableStable: g.stableCurrentPet(now); stableSel_ = -1; break;
        case Act::StableUnstable:
            for (const w3n::StabledPet& p : g.stable.pets)
                if ((int)p.slot == stableSel_) { g.unstablePet(p, now); stableSel_ = -1; break; }
            break;
        case Act::StableBuy: g.buyStableSlot(now); break;
        case Act::StableClose: g.closeStable(); break;
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
            if (f) {
                askText(in, arg == 0 ? tr("To", "Кому") : arg == 1 ? tr("Subject", "Тема") : tr("Text", "Текст"), *f, false,
                        [f](w3n::GameSession&, uint32_t, const std::string& t) { *f = t; }, arg == 2 ? 500 : arg == 1 ? 64 : 12);
                break;
            }
            if (arg == 3) {   // money in copper via the numpad
                askText(in, tr("Copper (100 = 1s)", "Медь (100 = 1с)"), mailMoney_ ? std::to_string(mailMoney_) : "", true,
                        [this](w3n::GameSession&, uint32_t, const std::string& v) { mailMoney_ = (uint32_t)strtoul(v.c_str(), nullptr, 10); });
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
            if (arg == 0) {
                askText(in, tr("Item name (empty = all)", "Название (пусто = все)"), auctionQuery_, false,
                        [this](w3n::GameSession& gg, uint32_t n, const std::string& q) {
                            auctionQuery_ = q;
                            gg.searchAuctions(q, 0, n);
                            auctionSel_ = -1;
                        }, 40);
                break;
            }
            g.searchAuctions(auctionQuery_, (uint32_t)(arg - 1), now);
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
            uint32_t* v = arg == 0 ? &auctionBid_ : &auctionBuyout_;
            askText(in, arg == 0 ? tr("Starting bid, copper", "Начальная ставка, медь") : tr("Buyout, copper (0 = none)", "Выкуп, медь (0 = нет)"),
                    *v ? std::to_string(*v) : "", true,
                    [v](w3n::GameSession&, uint32_t, const std::string& s) { *v = (uint32_t)strtoul(s.c_str(), nullptr, 10); });
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
            if (tradeSel_ < 0 || tradeSel_ >= (int)items.size()) { refuse(g, tr("Tap one of your items first", "Сначала выберите свой предмет"), now); break; }
            int slot = -1;
            for (int i = 0; i < 6 && slot < 0; ++i)
                if (!g.trade.myItems[i].guid) slot = i;
            if (slot < 0) { refuse(g, tr("All six slots are used", "Все шесть ячеек заняты"), now); break; }
            g.offerTradeItem(slot, items[tradeSel_], now);
            tradeSel_ = -1;
            break;
        }
        case Act::TradeClear: g.clearTradeItem(arg, now); break;
        case Act::TradeGold: {
            askText(in, tr("Copper to give", "Сколько меди отдать"), g.trade.mine.gold ? std::to_string(g.trade.mine.gold) : "", true,
                    [](w3n::GameSession& gg, uint32_t n, const std::string& v) { gg.offerTradeGold((uint32_t)strtoul(v.c_str(), nullptr, 10), n); });
            break;
        }
        case Act::TradeAccept: g.acceptTrade(now); break;
        case Act::TradeCancel: g.cancelTrade(now); break;
        default: break;
    }
}
