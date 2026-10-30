// game_ui.hpp — the touch screen in the game (UI stages 1-6) and the game side of the controller (stage 5.11).
//
// The touch screen is drawn by the GPU from a HudList (320x240, ui_render): a header (window / tab title, pin, money),
// the content area and a bar of eight tabs at the bottom, 40 px each: Combat (party, target of target, auras,
// cooldowns, nearby units), Map (zone map with us, units, quest givers, the tracked quest), Bags (item grid / paper
// doll), Spells (spell book / talents), Quests (log, tracked quest, reputation), Chat (channel tabs, keyboard, quick
// phrases), Social (group, guild, friends, ignore), Settings (controls, graphics, interface). Windows of NPCs and
// objects open over the content by themselves and close when done: loot, gossip, quest giver, vendor, trainer,
// innkeeper, bank, flight master, mailbox, auctioneer, trade; plus death, invitations, confirmations and tooltips.
//
// Touch: a tap acts on release; a long press (0.5 s) shows the tooltip of an item / spell / aura; dragging a spell or
// a usable item shows the controller bars over the content to drop it on a slot. Combat switches to the Combat tab
// and back to the Map tab afterwards unless a tab is pinned (the pin in the header); SELECT + D-pad left/right turns
// the tabs. Dangerous actions (destroy, selling rare items, abandoning a quest, deleting mail) ask first.
//
// Pad actions come from pad.hpp (scheme Forever or Classic); this class decides what they mean in the game:
// soft targeting (a spell or attack without a living target takes the enemy in front), interact (the target, else
// the nearest lootable corpse / NPC in front), assist, last hostile target, aiming with the C-Stick while L/R is held.
#pragma once
#include <3ds.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "action_layout.hpp"
#include "game.hpp"
#include "hud.hpp"
#include "pad.hpp"

class GameUi {
public:
    struct Input {
        u32 down;                     // raw keys (only the touch bit is read here)
        bool touched;                 // the finger came down this frame (touchX/Y = where)
        int touchX, touchY;           // pixels on the bottom screen
        const float* pos;             // character feet position
        float yaw;                    // character facing
        uint32_t nowMs;
        const char* selfName;
        const PadOut* pad = nullptr;  // this frame's controller intents (nullptr: none)
        PadConfig* cfg = nullptr;     // settings panel edits it
        ActionLayout* bars = nullptr; // the hotbars (bound / used here)
        bool touchHeld = false;       // the finger is on the screen (touchX/Y = where it is now)
        bool touchUp = false;         // the finger left the screen this frame (touchX/Y unused)
        uint32_t mapId = 0;           // the map we are on (Map tab)
        // UI stage 10, ground spells: the C-Stick (dead zone applied, -1..1), the camera's yaw (the stick moves the ring
        // the way the camera looks) and the height of the ground / floor under a point (nullptr: keep the height)
        float stickX = 0, stickY = 0, camYaw = 0;
        bool (*groundAt)(float x, float y, float zRef, float* z) = nullptr;
    };
    struct Output {
        bool faceYaw = false;         // turn the character (and the camera) to 'yaw'
        float yaw = 0;
        bool blockMove = false;       // dead (not yet released): the character cannot move
        bool blockJump = false;       // the jump button did something else (closed a window) or we are dead
        bool keyboardShown = false;   // the system keyboard ran (the frame took long)
        bool quit = false, freeCamera = false, showDebug = false;
        bool characters = false;      // START menu "Characters": log out to the character list
        bool configChanged = false, layoutChanged = false;
        bool graphicsChanged = false; // draw distance / crowd / doodads changed in Settings
        bool soundChanged = false;    // a volume changed in Settings
        std::vector<const char*> sounds;   // events to play this frame (audio.cpp names)
        // UI stage 10: a ground spell is being placed - the ring (where, how big) and the C-Stick is ours, not the camera's
        bool placing = false;
        float placePos[3] = {0, 0, 0};
        float placeRadius = 0;
        std::string placeName;
    };

    // Call every frame while online.
    Output update(w3n::GameSession& g, const Input& in, bool walking);
    // Rebuilds the touch screen's draw list when something changed (or every 250 ms); true when it did (draw it).
    bool draw(w3n::GameSession& g, const Input& in, float fps, uint32_t latencyMs);
    const HudList& screen() const { return list_; }
    bool gamePage() const { return gamePage_; }
    void reset();

    enum class Tab : uint8_t { Chat, Bags, Gear, Spells, Quests, Combat, Map, Social, Settings };
    enum class Panel : uint8_t {
        Tabs, Death, Loot, Gossip, Menu, Context, Settings, Vendor, Quest, Trainer, Binder, ItemInfo, Buffs, Invite,
        SpellInfo, Confirm, Bank, Taxi, Mail, Auction, Trade, Keyboard,
        Inspect,   // UI stage 9: another player's gear and honour
        Pet,       // UI stage 13: the pet's bar, mood, auto-cast, rename, feeding
        Stable,    // UI stage 13: the stable master (hunters)
    };
    Panel panel(const w3n::GameSession& g) const;   // what the centre area shows now
    Tab tab() const { return tab_; }
    // UI stage 14: the Equipment page is shown (no window over it): the top screen shows the character window instead of the HUD
    bool charSheetOpen(const w3n::GameSession& g) const { return tab_ == Tab::Bags && subBags_ == 1 && panel(g) == Panel::Tabs; }
    bool pinned() const { return pinned_; }
    uint64_t aimed() const { return aim_; }          // unit under the C-Stick ring (L/R held), 0 = none
    int shownSet() const { return shownSet_; }       // hotbar set on the bars
    bool binding() const { return bindArmed_; }
    bool dragging() const { return drag_.active; }

    enum class Act : uint8_t {
        Tab, Release, Reclaim, ResAccept, ResDecline, HealerYes, HealerNo, LootItem, LootMoney, LootAll, LootClose,
        GossipOption, GossipClose, SelectItem, Use, Equip, Unequip, Destroy, BindItem, PagePrev, PageNext,
        BarCell, BindSpell, ClearSlot, MenuItem, ContextItem, Setting, ClosePanel,
        // stages 5.12-5.14
        GossipQuest, VendorMode, VendorPick, VendorBuy, VendorSell, VendorBuyback, VendorRepair, VendorClose,
        QuestPick, QuestAccept, QuestContinue, QuestComplete, QuestReward, QuestClose, QuestScroll,
        LogSelect, LogBack, LogAbandon, LogReputation,
        TrainerPick, TrainerTrain, TrainerClose, TrainerFilter, BinderYes, BinderNo,
        ItemInfo, InfoBack, BuffPick, BuffCancel, InviteYes, InviteNo, OpenItem,
        // UI stages 1-6
        Pin, SubView, TargetUnit, CancelCast, ConfirmYes, ConfirmNo, TrackQuest, SpellPick,
        MapMode, MapZoom, ChatChannel, ChatWrite, ChatScroll, ChatPhrases, PhrasePick,
        TalentTree, TalentPick, TalentLearn,
        SkillPick, SkillHeader, SkillAbandon,   // 6.19: the skills page
        SocialPick, FriendAdd, FriendRemove, IgnoreAdd, IgnoreRemove, Whisper, GroupInvite, GroupLeave, GroupKick,
        GroupPromote, GuildRefresh, GuildAccept, GuildDecline,
        BankPick, BankDeposit, BankWithdraw, BankBuySlot, BankClose, BankMode,
        TaxiPick, TaxiFly, TaxiClose,
        MailPick, MailTakeMoney, MailTakeItem, MailDelete, MailCompose, MailSend, MailClose, MailBack, MailField,
        AuctionMode, AuctionSearch, AuctionPick, AuctionBuyout, AuctionBid, AuctionSell, AuctionSellPick,
        AuctionPrice, AuctionClose, AuctionCancel,
        TradeOffer, TradePick, TradeGold, TradeAccept, TradeCancel, TradeAnswer, TradeClear,
        KeyboardKey,
        ChatDest, ChannelSay, ChannelList, ChannelLeave, ChannelJoin,   // stage 5.46: chat destinations and channels
        EmotePick,   // the animation stage: text emotes, sit / stand (arg 1000)
        InspectSlot, // UI stage 9: an item of the inspected player (arg = entry): its tooltip
        // UI stage 13: pets
        PetOpen,     // the pet window (Combat strip, menu)
        PetCommand,  // arg = PetCmdId: attack / follow / stay / passive / defensive / aggressive
        PetCast,     // arg = spell of the pet's book
        PetAuto,     // arg = spell: auto-cast on / off
        PetBind,     // arg = PetCmdId (< 100) or 100000 + spell: arm the hotbar binding
        PetRename, PetDismiss, PetFeed, PetFeedPick,
        StablePick, StableStable, StableUnstable, StableBuy, StableClose,
    };
    // A tap region of the last draw (pixels). Long press shows info (kind 1 item entry / inventory index, 2 spell,
    // 3 aura spell); drag carries a spell (dragKind 1) or an item entry (2) to a hotbar slot.
    struct Button {
        int x, y, w, h;
        Act act;
        int arg;
        uint8_t infoKind = 0, dragKind = 0;   // info: 1 item entry, 2 spell, 3 aura, 4 a carried item (guid)
        uint32_t infoId = 0, dragId = 0;
        uint64_t guid = 0;
        int cx() const { return x + w / 2; }
        int cy() const { return y + h / 2; }
    };
    const std::vector<Button>& buttons() const { return buttons_; }   // tap regions of the last draw (PC test)
    // Runs a hotbar slot (pad addressing): spell with soft targeting, item from the bags, fixed set-0 action.
    void useSlot(w3n::GameSession& g, const Input& in, int slot, Output& out);
    bool placing() const { return place_.on; }
    enum : int { kKbBack = 1001, kKbShift = 1002, kKbSymbols = 1003, kKbOk = 1004, kKbCancel = 1005 };   // Act::KeyboardKey args
    using TextDone = std::function<void(w3n::GameSession&, uint32_t, const std::string&)>;
    // Opens the on-screen keyboard (stage 5.46, game_ui_keyboard.cpp): 'done' runs on OK, Cancel / B drop the text.
    void askText(const Input& in, const std::string& hint, const std::string& initial, bool numbers, TextDone done, size_t maxLen = 255);
    bool keyboardOpen() const { return kb_.on; }      // the on-screen keyboard (stage 5.46) is up: the character stands still
    std::string keyboardText() const { return kb_.text; }

private:
    // ---- drawing helpers (game_ui_widgets.cpp)
    enum class Style : uint8_t { Normal, Primary, Danger, Toggle, ToggleOn, Disabled, Flat };
    void btn(int x, int y, int w, int h, const std::string& label, Act act, int arg, Style st = Style::Normal,
             ui::Font f = ui::Font::Normal);
    Button& region(int x, int y, int w, int h, Act act, int arg);
    void window(int x, int y, int w, int h, bool dialog = false);
    void iconBox(int x, int y, int size, const ui::Image& icon, const std::string& name, uint32_t border, bool dim = false);
    int money(int xRight, int y, uint32_t copper, ui::Font f = ui::Font::Small);   // coins; returns the width
    void header(w3n::GameSession& g, const std::string& title);
    void tabBar();
    void pager(int y, int page, int pages);       // "< 1/3 >" at the bottom right of the content
    void hintStrip(const Input& in);
    void crossbarPicker(w3n::GameSession& g, const Input& in);   // the drop / bind targets over the content
    ui::Image spellIcon(uint32_t spell) const;
    ui::Image itemIcon(w3n::GameSession& g, uint32_t entry);
    std::string spellLabel(const w3n::GameSession& g, uint32_t spell) const;
    ui::Font listFont() const;

    // ---- actions (game_ui.cpp)
    void doAction(w3n::GameSession& g, const Input& in, Act act, int arg, Output& out);
    Output updateInner(w3n::GameSession& g, const Input& in, bool walking);
    void soundEvents(w3n::GameSession& g, const Input& in, Output& out);   // what this frame sounds like
    bool clicked_ = false;            // a button acted this frame
    int phrasePage_ = 0;              // the chat popup: 0 phrases, 1 emotes
    uint8_t soundTab_ = 0xFF, soundPanel_ = 0xFF;
    uint64_t soundTarget_ = 0;
    uint32_t soundLevel_ = 0;
    void pressSpell(w3n::GameSession& g, uint32_t spell, const Input& in, Output& out);
    // UI stage 10: ground spells and items - a ring on the ground moved with the C-Stick; the same button again (or the
    // interact button) casts at it, cancel drops it. With the "At target" setting a target in range is used at once.
    struct Place {
        bool on = false;
        uint32_t spell = 0, item = 0;  // the spell, and the item entry when it is an item's (0: a spell of ours)
        float pos[3] = {0, 0, 0};
        float range = 30, radius = 5;
        uint32_t lastMs = 0;
    } place_;
    void beginPlace(w3n::GameSession& g, const Input& in, uint32_t spell, uint32_t item, Output& out);
    void updatePlace(w3n::GameSession& g, const Input& in, Output& out);
    void confirmPlace(w3n::GameSession& g, const Input& in, Output& out);
    void interactAction(w3n::GameSession& g, const Input& in, Output& out);
    void cancelAction(w3n::GameSession& g, const Input& in);
    void openChat(w3n::GameSession& g, const Input& in, Output& out, const std::string& prefix = "");
    // The on-screen English keyboard (stage 5.46, game_ui_keyboard.cpp): replaces the system applet, which blocked the
    // game for seconds. askText() opens it; 'done' runs on OK (Cancel / B drop the text). Key codes (Act::KeyboardKey
    // arg): 32..126 a character (lower case; Shift / Caps make it upper), kKbBack .. kKbCancel the function keys.
    void drawKeyboard(w3n::GameSession& g, const Input& in);
    void keyboardKey(w3n::GameSession& g, const Input& in, int code);
    void closeKeyboard();
    struct Kb {
        bool on = false, numbers = false, shift = false, caps = false, symbols = false;
        std::string text, hint;
        size_t maxLen = 255;
        TextDone done;
    } kb_;
    Panel kbReturn_ = Panel::Tabs;
    // chat destination of the Write button: "s" say, "y" yell, "p" party, "r" raid, "g" guild, "w" whisper, "c1".."c9" channel
    std::string chatDest_ = "s";
    std::vector<std::string> chatDests(const w3n::GameSession& g) const;
    std::string chatDestLabel(const w3n::GameSession& g, const std::string& d) const;
    std::string chatDestPrefix(const w3n::GameSession& g, const std::string& d) const;
    std::string whisperTarget(const w3n::GameSession& g) const;
    void touch(w3n::GameSession& g, const Input& in, Output& out);
    void setTab(Tab t);
    void autoTabs(w3n::GameSession& g, const Input& in);
    void confirm(const std::string& question, Act act, int arg, uint64_t key = 0);
    uint64_t cycleUnits(const w3n::GameSession& g, const Input& in, bool enemies) const;
    uint64_t aimUnit(const w3n::GameSession& g, const Input& in, float yaw, bool enemies) const;
    void faceTarget(const w3n::GameSession& g, const Input& in, Output& out) const;
    std::vector<w3n::InvItem> listed(const w3n::GameSession& g) const;   // items of the current tab
    std::vector<uint32_t> bookSpells(const w3n::GameSession& g) const;    // the Spells tab
    std::vector<w3n::InvItem> sellable(const w3n::GameSession& g) const;   // bags, not equipped, for the vendor
    void questPrimary(w3n::GameSession& g, const Input& in);              // Accept / Continue / Complete
    std::vector<int> trainerRows(const w3n::GameSession& g) const;   // indices into trainer.spells shown now
    void hint(const std::string& s, uint32_t nowMs) { hint_ = s; hintMs_ = nowMs; dirty_ = true; }
    void dropOnSlot(const Input& in, int slot, Output& out);
    void showInfo(w3n::GameSession& g, const Button& b);
    const Button* hit(int x, int y) const;
    std::vector<Tab> tabOrder() const;

    // ---- tabs and windows (game_ui_tabs.cpp, game_ui_windows.cpp)
    void drawPanel(w3n::GameSession& g, const Input& in);
    void drawCombat(w3n::GameSession& g, const Input& in);
    void drawMap(w3n::GameSession& g, const Input& in);
    void drawChat(w3n::GameSession& g, const Input& in);
    void drawItems(w3n::GameSession& g, const Input& in);
    void drawPaperDoll(w3n::GameSession& g, const Input& in);
    void drawSpells(w3n::GameSession& g, const Input& in);
    void drawTalents(w3n::GameSession& g, const Input& in);
    void drawSkills(w3n::GameSession& g, const Input& in);   // 6.19
    void spellTabs(int active);                               // the Book / Talents / Skills buttons
    void drawQuestLog(w3n::GameSession& g, const Input& in);
    void drawSocial(w3n::GameSession& g, const Input& in);
    void drawSettings(const Input& in);
    void drawDeath(w3n::GameSession& g, const Input& in);
    void drawLoot(w3n::GameSession& g);
    void drawGossip(w3n::GameSession& g);
    void drawVendor(w3n::GameSession& g, const Input& in);
    void drawTrainer(w3n::GameSession& g);
    void drawItemInfo(w3n::GameSession& g);
    void drawSpellInfo(w3n::GameSession& g, const Input& in);
    void drawBuffs(w3n::GameSession& g, const Input& in);
    void drawQuest(w3n::GameSession& g);
    void drawMenu(w3n::GameSession& g);
    void drawContext(w3n::GameSession& g);
    void drawInspect(w3n::GameSession& g);
    void drawConfirm();
    void drawBank(w3n::GameSession& g, const Input& in);
    void drawTaxi(w3n::GameSession& g, const Input& in);
    void drawMail(w3n::GameSession& g, const Input& in);
    void drawAuction(w3n::GameSession& g, const Input& in);
    void drawTrade(w3n::GameSession& g, const Input& in);
    void drawBars(w3n::GameSession& g, const Input& in);
    void drawPet(w3n::GameSession& g, const Input& in);          // UI stage 13
    void drawStable(w3n::GameSession& g, const Input& in);
    int petStrip(w3n::GameSession& g, const Input& in);          // the pet's buttons at the bottom of the Combat tab: its height (0 = no pet)
    ui::Image actionIcon(w3n::GameSession& g, const ActionSlot& a);
    std::string actionName(w3n::GameSession& g, const ActionSlot& a) const;
    std::string panelTitle(w3n::GameSession& g, Panel p) const;

    HudList list_;                    // the touch screen
    bool bigText_ = false;            // PadConfig::bigText (lists and chat in the normal font)
    const ActionLayout* barsView_ = nullptr;   // the hotbars for drawing (set by update / draw)
    std::vector<uint64_t> rows_;      // guid per nearby row (TargetUnit arg)
    std::vector<Button> buttons_;     // tap regions of the last draw
    bool gamePage_ = true, dirty_ = true;
    uint32_t lastChat_ = 0, lastDrawMs_ = 0;
    Tab tab_ = Tab::Map;
    bool pinned_ = false;
    bool wasInCombat_ = false;
    Panel open_ = Panel::Tabs;        // Menu / Context / Settings / tooltips when one is open
    int selected_ = 0, page_ = 0;     // bag/gear/spell list: selected index (absolute) and page
    bool selTapped_ = false;          // the selection came from a tap (a second tap shows the tooltip)
    bool bindArmed_ = false;          // the next hotbar press binds bindWhat_ (Empty = clear the slot)
    ActionSlot bindWhat_;
    int shownSet_ = 0, classicSet_ = 0;
    uint64_t aim_ = 0, lastHostile_ = 0;
    // vendor (stage 5.13): mode 0 buy, 1 sell, 2 buyback; the selected row (absolute) and page
    int vendorMode_ = 0, vendorSel_ = -1, vendorPage_ = 0;
    uint64_t vendorNpc_ = 0;
    int trainerSel_ = -1;              // index into trainer.spells
    uint32_t infoEntry_ = 0;           // the item shown by Panel::ItemInfo (stage 5.15 tooltip)
    uint32_t infoSpell_ = 0;           // the spell / aura shown by Panel::SpellInfo
    Panel infoReturn_ = Panel::Tabs;   // where a tooltip goes back to
    uint32_t buffSel_ = 0;             // Panel::Buffs: the selected aura's spell
    w3n::InvItem infoItem_;            // its instance in the bags (guid 0 = a vendor's / reward item)
    bool trainerAll_ = false;          // also show what we already know
    // quest windows (stage 5.14): text scroll, chosen reward (-1 none), which window they belong to
    int questScroll_ = 0, rewardPick_ = -1;
    uint32_t questKey_ = 0;
    // Quests tab: 0 log, 1 one quest, 2 reputation
    int logView_ = 0, logSel_ = 0;
    uint32_t tracked_ = 0;             // the tracked quest (Map tab, top HUD option)
    std::string hint_;                // the last "why not" (too far, no target ...), 4 s
    uint32_t hintMs_ = 0;
    // sub views: Bags 0 bags / 1 paper doll; Spells 0 book / 1 talents; Social 0 group 1 guild 2 friends 3 ignore;
    // Settings 0 controls / 1 graphics + interface; Chat channel filter; Map 0 zone / 1 continent, zoom 1/2/4
    int subBags_ = 0, subSpells_ = 0, subSocial_ = 0, subSettings_ = 0;
    int chatChannel_ = 0, chatScroll_ = 0;
    bool phrases_ = false;
    int mapMode_ = 0, mapZoom_ = 1;
    int talentTree_ = 0, talentSel_ = -1;
    uint32_t skillSel_ = 0;          // 6.19: the selected skill line; skillFolded_: bit per category (5..11) folded away
    uint32_t skillFolded_ = 0;
    int socialSel_ = -1;
    // confirmation dialog
    std::string confirmText_;
    Act confirmAct_ = Act::ClosePanel;
    int confirmArg_ = 0;
    Panel confirmReturn_ = Panel::Tabs;
    bool confirmed_ = false;           // doAction runs the confirmed action (no second question)
    // touch: the button under the finger, long press, drag
    struct Press {
        bool active = false, moved = false, longFired = false, onButton = false;
        int x0 = 0, y0 = 0, x = 0, y = 0;
        uint32_t startMs = 0;
        Button button{};
    } press_;
    struct Drag {
        bool active = false;
        uint8_t kind = 0;              // 1 spell, 2 item
        uint32_t id = 0;
        int x = 0, y = 0, hoverSlot = -1;
    } drag_;
    std::vector<Button> pickerCells_;   // the crossbar picker's cells (arg = slot)
    // windows of stage 6
    int bankSel_ = -1, bankMode_ = 0;
    int taxiSel_ = -1;
    int mailSel_ = -1, mailView_ = 0;  // 0 list, 1 one letter, 2 compose
    std::string mailTo_, mailSubject_, mailBody_;
    uint32_t mailMoney_ = 0;
    int mailItem_ = -1;                // index into sellable() to attach
    int auctionMode_ = 0, auctionSel_ = -1, auctionSellItem_ = -1;
    std::string auctionQuery_;
    uint32_t auctionBid_ = 0, auctionBuyout_ = 0;
    int auctionHours_ = 1;             // 0 2h, 1 8h, 2 24h
    int tradeSel_ = -1;
    int petSel_ = -1;                  // UI stage 13: the selected entry in the pet window (PetCmdId, 100000 + spell)
    int feedSel_ = -1;                 // the food picked in the feed list (index into petFoodCandidates())
    bool petFeedList_ = false;         // the pet window shows the food list
    int stableSel_ = -1;               // the stabled pet picked (slot)
    uint64_t windowNpc_ = 0;           // the NPC / object of the open stage-6 window (reset when it changes)
    uint64_t inspect_ = 0;             // UI stage 9: the player Panel::Inspect shows
    static constexpr int kTop = 20, kTabY = 196, kTabH = 44, kContentH = kTabY - kTop;
};
