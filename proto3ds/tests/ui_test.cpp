// PC check of the bottom-screen game UI (stages 5.10/5.11): replays recorded sessions of net/tests/run_stage5.sh
// (--life: items, loot, gossip, deaths; --fight: spells and a living wolf) into a GameSession, stops at each
// situation, draws the console page into a text file (ui_render.py turns it into a PNG) and drives it with taps
// and controller intents: every action must send the packet it stands for.
//   ui_test life.w3dl fight.w3dl outdir [trade.w3dl factions.txt]   (stages 5.12-5.14: quests, vendor, reactions)
#include <fcntl.h>
#include <math.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "game_ui.hpp"
#include "ui_raster.hpp"

using namespace w3n;

// every screenshot also goes onto a contact sheet (ui_sheet.png, 4 per row)
static std::vector<std::pair<std::string, Canvas>> g_shots;

struct Sent { uint16_t op; Bytes data; };

struct Rig {
    GameSession s;
    GameUi ui;
    PadConfig cfg;
    ActionLayout bars;
    std::vector<Sent> sent;
    GameUi::Input in{};
    float pos[3] = {0, 0, 0};
    uint32_t last = 0;
    std::string dir;

    bool replay(const std::vector<LoggedPacket>& pk, const std::function<bool(GameSession&)>& ready) {
        s.selfName = "Testone";
        s.tap = [&](uint32_t, int d, uint16_t op, const Bytes& b) { if (d == 1) sent.push_back({op, b}); };
        bool got = false;
        for (const LoggedPacket& p : pk) {
            last = p.ms;
            if (p.dir == 1) {   // our own movement: where we are
                if (MoveOp::isRelayedMove(p.opcode) || p.opcode == MoveOp::Heartbeat) {
                    Reader r(p.data);
                    MovementInfo m;
                    if (readMovementInfo(r, m)) s.mover.reset(m.x, m.y, m.z, m.o, p.ms);
                }
                continue;
            }
            if (p.opcode == 0x236 && p.data.size() >= 20) {
                Reader r(p.data);
                r.u32();
                float x = r.f32(), y = r.f32(), z = r.f32(), o = r.f32();
                s.mover.reset(x, y, z, o, p.ms);
            }
            s.handle(p.opcode, p.data, p.ms);
            s.objects.takeCreated();
            s.update(p.ms);
            if (ready(s)) { got = true; break; }
        }
        const MovementInfo& m = s.mover.info();
        pos[0] = m.x; pos[1] = m.y; pos[2] = m.z;
        in = GameUi::Input{0, false, 0, 0, pos, m.o, last + 100, "Testone", nullptr, &cfg, &bars};
        in.mapId = s.mapId();
        bars.autofill(s);
        sent.clear();
        return got;
    }
    void scratch() {   // a draw that refreshes the tap regions
        in.nowMs += 300;   // draw() redraws at most every 250 ms unless something changed
        ui.draw(s, in, 30.0f, 85);
    }
    void draw(const std::string& name) {   // the touch screen of one draw into dir/ui_<name>.png
        scratch();
        Canvas c(kTouchW, kTouchH);
        c.draw(ui.screen());
        c.png(dir + "/ui_" + name + ".png");
        g_shots.push_back({name, c});
    }
    std::vector<std::string> heard;   // every sound event the UI asked for
    bool sawCharacters = false;       // an update asked for the character list (START menu, 6.25)
    GameUi::Output upd(const GameUi::Input& t) {
        GameUi::Output o = ui.update(s, t, true);
        sawCharacters |= o.characters;
        for (const char* e : o.sounds) heard.push_back(e);
        return o;
    }
    bool heardIt(const char* e) const { for (const std::string& h : heard) if (h == e) return true; return false; }
    // A finger on the button's centre: down, (held), up - the UI acts on release.
    bool tap(GameUi::Act act, int arg = -1, uint32_t holdMs = 0) {
        scratch();
        for (const GameUi::Button& b : ui.buttons()) {
            if (b.act != act || (arg >= 0 && b.arg != arg)) continue;
            GameUi::Input t = in;
            t.down = KEY_TOUCH;
            t.touched = true;
            t.touchHeld = true;
            t.touchX = b.cx();
            t.touchY = b.cy();
            upd(t);
            if (holdMs) {
                t.touched = false;
                t.down = 0;
                t.nowMs += holdMs;
                upd(t);
            }
            t.touched = false;
            t.touchHeld = false;
            t.touchUp = true;
            t.down = 0;
            t.nowMs += holdMs + 30;
            upd(t);
            in.nowMs = t.nowMs + 300;
            return true;
        }
        return false;
    }
    // The on-screen keyboard (stage 5.46) must be up: types the text key by key (Shift for capitals, ?123 for symbols
    // outside the letter layer) and presses OK ('ok' false: Cancel).
    bool type(const std::string& text, bool ok = true) {
        if (!ui.keyboardOpen()) return false;
        static const std::string kSym = "!@#$%^&*()-_=+[]{}:;~|\\\"<>`";
        for (char c : text) {
            const bool upper = c >= 'A' && c <= 'Z';
            const bool sym = kSym.find(c) != std::string::npos;
            if (upper && !tap(GameUi::Act::KeyboardKey, GameUi::kKbShift)) return false;
            if (sym && !tap(GameUi::Act::KeyboardKey, GameUi::kKbSymbols)) return false;
            if (!tap(GameUi::Act::KeyboardKey, upper ? c + 32 : (unsigned char)c)) return false;
            if (sym && !tap(GameUi::Act::KeyboardKey, GameUi::kKbSymbols)) return false;
        }
        return tap(GameUi::Act::KeyboardKey, ok ? GameUi::kKbOk : GameUi::kKbCancel);
    }
    // Drag from a button to a point (the hotbar picker's cell of 'slot').
    bool drag(GameUi::Act act, int arg, int slot) {
        scratch();
        const GameUi::Button* from = nullptr;
        for (const GameUi::Button& b : ui.buttons())
            if (b.act == act && b.arg == arg) from = &b;
        if (!from) return false;
        GameUi::Input t = in;
        t.down = KEY_TOUCH;
        t.touched = true;
        t.touchHeld = true;
        t.touchX = from->cx();
        t.touchY = from->cy();
        upd(t);
        t.touched = false;
        t.down = 0;
        t.touchX += 20;   // moving: the drag starts, the picker appears
        t.nowMs += 50;
        upd(t);
        in.nowMs = t.nowMs;
        scratch();
        const GameUi::Button* cell = nullptr;
        for (const GameUi::Button& b : ui.buttons())
            if (b.act == GameUi::Act::BarCell && b.arg == slot) cell = &b;
        if (!cell) return false;
        t.touchX = cell->cx();
        t.touchY = cell->cy();
        t.nowMs = in.nowMs + 50;
        upd(t);
        in.nowMs = t.nowMs;
        draw("drag");
        t.touchHeld = false;
        t.touchUp = true;
        t.nowMs = in.nowMs + 30;
        upd(t);
        in.nowMs = t.nowMs + 300;
        return true;
    }
    GameUi::Output pad(const PadOut& p) {
        GameUi::Input t = in;
        t.pad = &p;
        GameUi::Output o = upd(t);
        in.nowMs += 300;
        return o;
    }
    bool sentOp(uint16_t op) const {
        for (const Sent& x : sent) if (x.op == op) return true;
        return false;
    }
    std::string ops() const {
        std::string o;
        char b[8];
        for (const Sent& x : sent) { snprintf(b, sizeof b, " 0x%03X", x.op); o += b; }
        return o;
    }
};

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    if (const char* pack = getenv("W3D_UI_PACK")) {   // the screenshots with a converted interface pack
        char err[96];
        if (!ui::assets().open(pack, err, sizeof err)) fprintf(stderr, "ui pack: %s\n", err);
    }
    std::vector<LoggedPacket> life, fight;
    if (!readPacketLog(argv[1], life)) fprintf(stderr, "life log truncated\n");
    if (!readPacketLog(argv[2], fight)) fprintf(stderr, "fight log truncated\n");
    std::string dir = argv[3];
    int fails = 0, scenes = 0;
    auto result = [&](const char* name, bool ok, const std::string& detail) {
        ++scenes;
        if (!ok) ++fails;
        printf("%s %-14s %s\n", ok ? "ok  " : "FAIL", name, detail.c_str());
    };
    auto bagsReady = [](GameSession& s) { return s.inventory().size() >= 7 && s.itemInfoIfKnown(117) && s.itemInfoIfKnown(6948); };

    // ---- stage 5.10 windows (taps)
    struct TapScene {
        const char* name;
        std::function<bool(GameSession&)> ready;
        int tab;                       // -1: stay on Chat
        uint32_t select;               // item entry to select first (0 = none)
        GameUi::Act tap;
        int arg, taps;
        uint16_t expect;
    };
    std::vector<TapScene> taps = {
        {"bags", bagsReady, (int)GameUi::Tab::Bags, 117, GameUi::Act::Use, -1, 1, 0x0AB},
        {"bags_destroy", bagsReady, (int)GameUi::Tab::Bags, 6948, GameUi::Act::Destroy, -1, 2, 0x111},
        {"gear", bagsReady, (int)GameUi::Tab::Gear, 0, GameUi::Act::Unequip, -1, 1, 0x10B},
        {"loot", [](GameSession& s) { return s.lootWindow.open && s.itemInfoIfKnown(750); }, -1, 0, GameUi::Act::LootItem, 1, 1, 0x108},
        {"loot_all", [](GameSession& s) { return s.lootWindow.open && s.itemInfoIfKnown(750); }, -1, 0, GameUi::Act::LootAll, -1, 1, 0x15F},
        {"gossip", [](GameSession& s) { return s.gossip.open && !s.gossip.text.empty(); }, -1, 0, GameUi::Act::GossipOption, 1, 1, 0x17C},
        {"dead", [](GameSession& s) { return s.life() == Life::Dead && !s.resurrectOffer.active; }, -1, 0, GameUi::Act::Release, -1, 1, 0x15A},
        {"ghost", [](GameSession& s) { return s.life() == Life::Ghost && s.corpse.known && s.stats.teleports > 0; }, -1, 0,
         GameUi::Act::Tab, -1, 0, 0},
        {"healer", [](GameSession& s) { return s.spiritHealerConfirm != 0; }, -1, 0, GameUi::Act::HealerYes, -1, 1, 0x21C},
        {"resurrect", [](GameSession& s) { return s.resurrectOffer.active; }, -1, 0, GameUi::Act::ResAccept, -1, 1, 0x15C},
    };
    for (const TapScene& c : taps) {
        Rig r;
        r.dir = dir;
        if (!r.replay(life, c.ready)) { result(c.name, false, "never reached"); continue; }
        if (c.tab == (int)GameUi::Tab::Gear) {   // UI stage 3: equipment is a view of the Bags tab
            if (!r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Bags) || !r.tap(GameUi::Act::SubView, 1)) { result(c.name, false, "no gear view"); continue; }
        } else if (c.tab >= 0 && !r.tap(GameUi::Act::Tab, c.tab)) { result(c.name, false, "no tab button"); continue; }
        if (c.select) {
            std::vector<InvItem> inv = r.s.inventory();
            int idx = 0, k = 0;
            for (const InvItem& it : inv) {
                if (it.bag == kBagSelf && it.slot < kSlotBackpackFirst) continue;
                if (it.entry == c.select) idx = k;
                ++k;
            }
            r.tap(GameUi::Act::SelectItem, idx);
        }
        r.draw(c.name);
        r.sent.clear();
        bool ok = true;
        for (int t = 0; t < c.taps && ok; ++t) {
            size_t before = r.sent.size();
            // UI stage 3: Destroy asks first (a confirmation window), Yes destroys
            GameUi::Act a = c.taps == 2 && t == 1 ? GameUi::Act::ConfirmYes : c.tap;
            if (!r.tap(a, t == 1 ? -1 : c.arg)) { ok = false; break; }
            if (c.taps == 2 && t == 0) {
                if (r.sent.size() != before || r.ui.panel(r.s) != GameUi::Panel::Confirm) ok = false;
                r.draw(std::string(c.name) + "_armed");
            }
        }
        if (c.expect) ok = ok && r.sentOp(c.expect);
        if (ok && c.expect == 0x17C) ok = r.sent.back().data.size() == 12 && r.sent.back().data[8] == 1;
        if (ok && c.expect == 0x15C) ok = r.sent.back().data.size() == 9 && r.sent.back().data[8] == 1;
        if (ok && c.expect == 0x0AB) ok = r.sent.back().data[0] == 255 && r.sent.back().data[1] == 23;
        result(c.name, ok, "tap ->" + r.ops());
    }

    {   // 6.25: START menu "Characters": a button, a question, then the request for the character list (and Quit stays last)
        Rig r;
        r.dir = dir;
        bool got = r.replay(life, bagsReady);
        PadOut m;
        m.menu = true;
        r.pad(m);
        r.scratch();
        int chars = -1, quit = -1, k = 0;
        for (const GameUi::Button& b : r.ui.buttons()) {
            if (b.act != GameUi::Act::MenuItem) continue;
            if (b.arg == 13) chars = k;
            if (b.arg == 9) quit = k;
            ++k;
        }
        r.draw("menu_characters");
        r.tap(GameUi::Act::MenuItem, 13);
        const bool asked = r.ui.panel(r.s) == GameUi::Panel::Confirm && !r.sawCharacters;
        r.draw("menu_characters_confirm");
        r.tap(GameUi::Act::ConfirmYes);
        result("menu_characters", got && chars >= 0 && quit > chars && asked && r.sawCharacters,
               "button " + std::to_string(chars) + " quit " + std::to_string(quit) + (asked ? " asked" : " NOT asked"));
    }

    // ---- stage 5.11: controller intents
    {   // Forever Y at the dead wolf (no target): soft interact selects it and loots
        Rig r;
        r.dir = dir;
        bool got = r.replay(life, bagsReady);
        PadOut p;
        p.interact = true;
        r.pad(p);
        result("soft_loot", got && r.sentOp(0x13D) && r.sentOp(0x15D), "Y without target ->" + r.ops());
    }
    {   // the hotbars: autofilled from the server bar, drawn, a D-pad press casts with soft targeting at the wolf
        Rig r;
        r.dir = dir;
        bool got = r.replay(fight, [](GameSession& s) {
            for (auto& [g, o] : s.objects.all()) if (o.entry() == 69 && !o.dead() && o.hasPos) return !s.spells.empty() && s.actionBarSeen;
            return false;
        });
        // stand next to the wolf, facing it, without a target
        const WorldObject* wolf = nullptr;
        for (auto& [g, o] : r.s.objects.all()) if (o.entry() == 69) wolf = &o;
        float wp[3] = {0, 0, 0};
        if (wolf) wolf->positionAt(r.in.nowMs, wp);
        r.pos[0] = wp[0] - 3.0f; r.pos[1] = wp[1]; r.pos[2] = wp[2];
        r.in.yaw = 0;
        r.draw("bars");
        // slot 1 (D-pad right) = the second spell of the server bar = Heroic Strike (78)
        bool barOk = r.bars.at(0).id == kSpellAttack && r.bars.at(1).id == 78;
        PadOut p;
        p.slot = 1;
        GameUi::Output o = r.pad(p);
        bool castOk = r.sentOp(0x13D) && r.sentOp(0x12E) && r.s.target() == (wolf ? wolf->guid : 1) && o.faceYaw;
        result("soft_cast", got && barOk && castOk, "D-right without target ->" + r.ops());
        // bind: Spells tab, tap Battle Stance, press ZR+X (slot 20) -> bound and saved
        r.sent.clear();
        r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Spells);
        r.draw("spells");
        bool tapped = r.tap(GameUi::Act::BindSpell, 2457);
        PadOut b;
        b.slot = 20;
        GameUi::Output bo = r.pad(b);
        bool bound = tapped && r.bars.at(20).type == ActionSlot::Spell && r.bars.at(20).id == 2457 && bo.layoutChanged && r.sent.empty();
        r.bars.save((dir + "/bars_test.txt").c_str());
        ActionLayout back;
        back.load((dir + "/bars_test.txt").c_str());
        bound = bound && back.at(20).id == 2457 && back.at(1).id == 78;
        result("bind", bound, "Spells > Battle Stance, ZR+X");
        // ZR held: the touch bar shows set 2 with Battle Stance on X
        PadOut z;
        z.set = 2;
        r.pad(z);
        r.draw("bars_zr");
        result("bars_zr", r.ui.shownSet() == 2, "set shown " + std::to_string(r.ui.shownSet()));
        // enemy mode: aim at the wolf with the C-Stick, release R -> target
        r.s.setTarget(0, r.in.nowMs);
        r.sent.clear();
        PadOut aim;
        aim.mode = PadOut::Enemy;
        aim.aiming = true;
        aim.aimYaw = 0;   // straight at the wolf
        r.pad(aim);
        uint64_t aimed = r.ui.aimed();
        PadOut rel;
        rel.released = PadOut::Enemy;
        r.pad(rel);
        result("aim", wolf && aimed == wolf->guid && r.s.target() == wolf->guid && r.sentOp(0x13D), "R + C-Stick, release");
        // cancel (A), like Esc: first the cast in progress (Heroic Strike never got its SPELL_GO here), then the target
        PadOut c;
        c.cancel = true;
        r.sent.clear();
        r.pad(c);
        bool first = r.sentOp(0x12F) && r.s.target() != 0;
        r.pad(c);
        result("cancel", first && r.s.target() == 0, "A: cancel cast, then clear target ->" + r.ops());
        // menu, settings (switch to Classic), context
        PadOut m;
        m.menu = true;
        r.pad(m);
        r.draw("menu");
        bool menuOk = r.ui.panel(r.s) == GameUi::Panel::Menu;
        r.tap(GameUi::Act::MenuItem, 6);   // stage 5.14: Quests is item 4; 5.16: Buffs 5, Settings 6 (UI stage 3: the tab)
        r.draw("settings");
        bool setOk = r.ui.tab() == GameUi::Tab::Settings && r.ui.panel(r.s) == GameUi::Panel::Tabs && r.tap(GameUi::Act::Setting, 1) &&
                     r.cfg.scheme == PadScheme::Classic;
        r.cfg.scheme = PadScheme::Forever;
        // Graphics/sound page: the music volume + and - (pad.txt "music")
        r.tap(GameUi::Act::SubView, 1);
        const int mv = r.cfg.musicVol;
        r.tap(GameUi::Act::Setting, 141);
        r.draw("settings_sound");
        setOk = setOk && r.cfg.musicVol == mv + 1;
        r.tap(GameUi::Act::Setting, 140);
        setOk = setOk && r.cfg.musicVol == mv;
        // Interface page (5.35e): FPS / ping on and off (the button must be on the page: tap finds it in the draw list)
        r.tap(GameUi::Act::SubView, 2);
        setOk = setOk && r.tap(GameUi::Act::Setting, 180) && r.cfg.showStats;
        r.draw("settings_iface");
        setOk = setOk && r.tap(GameUi::Act::Setting, 181) && !r.cfg.showStats;
        r.tap(GameUi::Act::ClosePanel);
        r.s.setTarget(wolf ? wolf->guid : 0, r.in.nowMs);
        PadOut ctx;
        ctx.context = true;
        r.pad(ctx);
        r.draw("context");
        bool ctxOk = r.ui.panel(r.s) == GameUi::Panel::Context;
        result("menus", got && menuOk && setOk && ctxOk, "START menu, settings, X context");
    }
    // ---- stages 5.12-5.14: factions, quests, vendor (the --trade session; needs its factions.txt)
    if (argc >= 6) {
        std::vector<LoggedPacket> trade;
        if (!readPacketLog(argv[4], trade)) fprintf(stderr, "trade log truncated\n");
        FactionTable ft;
        bool ftOk = ft.load(argv[5]);
        auto rig = [&](Rig& r, const std::function<bool(GameSession&)>& ready) {
            r.dir = dir;
            r.s.factions = ft;
            return r.replay(trade, ready);
        };
        auto named = [](GameSession& s, uint32_t entry) { return s.itemInfoIfKnown(entry) != nullptr; };
        {   // nearby list and target line coloured by reaction
            Rig r;
            bool got = rig(r, [](GameSession& s) {
                int n = 0;
                for (auto& [g, o] : s.objects.all()) n += o.isUnitLike() && !o.name.empty();
                return s.rep.initialized && n >= 12;
            });
            for (auto& [g, o] : r.s.objects.all()) if (o.name == "Defias Bandit") r.s.setTarget(g, r.in.nowMs);
            r.draw("reactions");
            bool ok = got && ftOk;
            for (auto& [g, o] : r.s.objects.all()) {
                if (o.name == "Defias Bandit") ok = ok && r.s.canAttack(o) && r.s.shownReaction(o) == Rank::Hated;
                if (o.name == "Booty Bay Bruiser") ok = ok && !r.s.canAttack(o) && r.s.shownReaction(o) == Rank::Neutral;
            }
            result("reactions", ok, "factions.txt loaded " + std::to_string(ft.templateCount()) + " templates");
        }
        {   // gossip with a quest -> tap it -> CMSG_QUESTGIVER_QUERY_QUEST
            Rig r;
            bool got = rig(r, [](GameSession& s) { return s.gossip.open && !s.gossip.quests.empty() && s.questLog().empty(); });
            r.draw("gossip_quest");
            r.tap(GameUi::Act::GossipQuest, 0);
            result("gossip_quest", got && r.sentOp(0x186) && r.sent.back().data.size() == 12, "tap ->" + r.ops());
        }
        {   // quest details -> Accept (and the Decline path with A)
            Rig r;
            bool got = rig(r, [&](GameSession& s) { return s.questDialog.kind == QuestDialog::Kind::Details && s.questDialog.quest == 7 &&
                                                           named(s, 5405) && named(s, 1394); });
            r.draw("quest_details");
            bool panelOk = r.ui.panel(r.s) == GameUi::Panel::Quest;
            r.tap(GameUi::Act::QuestScroll, 3);
            r.draw("quest_details_scrolled");
            r.tap(GameUi::Act::QuestAccept);
            result("quest_details", got && panelOk && r.sentOp(0x189) && !r.s.questDialog.open(), "Accept ->" + r.ops());
            Rig d;
            rig(d, [&](GameSession& s) { return s.questDialog.kind == QuestDialog::Kind::Details && s.questDialog.quest == 7; });
            PadOut c;
            c.cancel = true;
            d.pad(c);
            result("quest_decline", d.sentOp(0x190) && !d.s.questDialog.open(), "A ->" + d.ops());
        }
        {   // items wanted -> Continue; the reward: Complete refused until a choice is tapped
            Rig r;
            bool got = rig(r, [](GameSession& s) { return s.questDialog.kind == QuestDialog::Kind::RequestItems; });
            r.draw("quest_items");
            r.tap(GameUi::Act::QuestContinue);
            bool cont = r.sentOp(0x18C);
            Rig o;
            bool got2 = rig(o, [&](GameSession& s) { return s.questDialog.kind == QuestDialog::Kind::OfferReward && named(s, 1394); });
            o.draw("quest_reward");
            o.tap(GameUi::Act::QuestComplete);
            bool refused = !o.sentOp(0x18E);
            o.tap(GameUi::Act::QuestReward, 1);
            o.draw("quest_reward_picked");
            o.tap(GameUi::Act::QuestComplete);
            bool chose = o.sentOp(0x18E) && o.sent.back().data.size() == 16 && o.sent.back().data[12] == 1;
            result("quest_reward", got && cont && got2 && refused && chose, "Continue, Complete (refused), pick 2nd, Complete ->" + o.ops());
        }
        {   // a quest giver's list (McBride) -> tap the second quest
            Rig r;
            bool got = rig(r, [](GameSession& s) { return s.questDialog.kind == QuestDialog::Kind::List; });
            r.draw("quest_list");
            r.tap(GameUi::Act::QuestPick, 1);
            bool ok = r.sentOp(0x186) && r.sent.back().data.size() == 12 && r.sent.back().data[8] == (783 & 0xFF);
            result("quest_list", got && ok, "tap 2nd ->" + r.ops());
        }
        {   // the quest log: list, one quest with its progress, Abandon twice -> CMSG_QUESTLOG_REMOVE_QUEST
            Rig r;
            bool got = rig(r, [](GameSession& s) {
                auto log = s.questLog();
                return !log.empty() && log[0].complete && s.questInfoIfKnown(log[0].id) && s.itemInfoIfKnown(750);
            });
            r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Quests);
            r.draw("quest_log");
            r.tap(GameUi::Act::LogSelect, 0);
            r.draw("quest_log_detail");
            r.tap(GameUi::Act::LogAbandon);
            bool once = !r.sentOp(0x194) && r.ui.panel(r.s) == GameUi::Panel::Confirm;
            r.draw("quest_log_abandon");
            r.tap(GameUi::Act::ConfirmYes);
            result("quest_log", got && once && r.sentOp(0x194) && r.sent.back().data.size() == 1 && r.sent.back().data[0] == 0,
                   "Abandon x2 ->" + r.ops());
        }
        {   // reputation page
            Rig r;
            bool got = rig(r, [](GameSession& s) { return s.repChanges >= 2; });
            r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Quests);
            r.tap(GameUi::Act::LogReputation);
            r.draw("reputation");
            result("reputation", got && r.s.reputations().size() == 3, std::to_string(r.s.reputations().size()) + " factions");
        }
        {   // vendor: buy (pick + Buy), sell (mode, pick linen, Sell), buyback, repair
            Rig r;
            bool got = rig(r, [&](GameSession& s) { return s.vendor.open && named(s, 159) && named(s, 2947) && named(s, 999); });
            r.draw("vendor");
            r.tap(GameUi::Act::VendorPick, 0);
            r.draw("vendor_pick");
            r.tap(GameUi::Act::VendorBuy, 1);
            bool buy = r.sentOp(0x1A2) && r.sent.back().data.size() == 14;
            r.sent.clear();
            r.tap(GameUi::Act::VendorPick, 2);   // 100g: too dear
            r.tap(GameUi::Act::VendorBuy, 1);
            bool dear = !r.sentOp(0x1A2);
            r.tap(GameUi::Act::VendorMode, 1);
            int linen = -1, k = 0;
            for (const InvItem& it : r.s.inventory()) {
                if (it.bag == kBagSelf && it.slot < kSlotBackpackFirst) continue;
                if (it.entry == 2589) linen = k;
                ++k;
            }
            r.tap(GameUi::Act::VendorPick, linen);
            r.draw("vendor_sell");
            r.tap(GameUi::Act::VendorSell);
            bool sell = r.sentOp(0x1A0) && r.sent.back().data.size() == 17;
            r.tap(GameUi::Act::VendorRepair);
            bool repair = r.sentOp(0x2A8);
            result("vendor", got && buy && dear && sell && repair && linen >= 0, "buy, too dear, sell, repair ->" + r.ops());
            Rig b;
            bool got2 = rig(b, [](GameSession& s) { return s.vendor.open && !s.buybackItems().empty(); });
            b.tap(GameUi::Act::VendorMode, 2);
            b.tap(GameUi::Act::VendorPick, 0);
            b.draw("vendor_buyback");
            b.tap(GameUi::Act::VendorBuyback);
            result("vendor_buyback", got2 && b.sentOp(0x290), "Buyback ->" + b.ops());
        }
    }
    if (argc >= 6) {   // stage 5.13b: the trainer (Train refused for a red spell, sent for a green one) and the innkeeper
        std::vector<LoggedPacket> trade;
        readPacketLog(argv[4], trade);
        FactionTable ft;
        ft.load(argv[5]);
        Rig r;
        r.dir = dir;
        r.s.factions = ft;
        r.s.spellNames.add(7380, "Heroic Strike", "Rank 2");
        r.s.spellNames.add(3127, "Parry", "");
        bool got = r.replay(trade, [](GameSession& s) { return s.trainer.open && s.trainer.spells.size() == 3 && s.trainer.spells[0].state == 0; });
        r.draw("trainer");
        r.tap(GameUi::Act::TrainerPick, 1);   // Parry: level 6
        r.tap(GameUi::Act::TrainerTrain);
        bool refused = !r.sentOp(0x1B2);
        r.tap(GameUi::Act::TrainerPick, 0);
        r.draw("trainer_pick");
        r.tap(GameUi::Act::TrainerTrain);
        bool sent = r.sentOp(0x1B2) && r.sent.back().data.size() == 12;
        r.tap(GameUi::Act::TrainerFilter);
        r.draw("trainer_all");
        result("trainer", got && refused && sent && r.ui.panel(r.s) == GameUi::Panel::Trainer, "red refused, green ->" + r.ops());
        Rig b;
        b.dir = dir;
        bool got2 = b.replay(trade, [](GameSession& s) { return s.binderConfirm != 0; });
        b.draw("binder");
        b.tap(GameUi::Act::BinderYes);
        result("innkeeper", got2 && b.sentOp(0x1B5) && b.s.binderConfirm == 0, "Yes ->" + b.ops());
        // stage 5.15: item tooltips - a second tap on a bag item, Info on a vendor item, A goes back
        Rig t;
        t.dir = dir;
        bool got3 = t.replay(trade, [](GameSession& s) {
            return s.vendor.open && s.itemInfoIfKnown(999) && s.itemInfoIfKnown(25) && s.itemInfoIfKnown(2589);
        });
        t.tap(GameUi::Act::VendorPick, 2);
        t.tap(GameUi::Act::ItemInfo, 1);
        t.draw("item_vendor");
        bool vi = t.ui.panel(t.s) == GameUi::Panel::ItemInfo;
        PadOut c;
        c.cancel = true;
        t.pad(c);
        bool back = t.ui.panel(t.s) == GameUi::Panel::Vendor;
        t.s.closeVendor();
        t.tap(GameUi::Act::Tab, (int)GameUi::Tab::Bags);
        t.tap(GameUi::Act::SubView, 1);
        t.tap(GameUi::Act::SelectItem, 0);
        t.tap(GameUi::Act::SelectItem, 0);
        t.draw("item_gear");
        bool gi = t.ui.panel(t.s) == GameUi::Panel::ItemInfo;
        result("item_info", got3 && vi && back && gi && t.sent.empty(), "vendor Info, A back, gear tap twice");
        // stage 5.16: START > Buffs, tap Well Fed, Cancel buff -> CMSG_CANCEL_AURA; the debuff cannot be cancelled
        Rig a;
        a.dir = dir;
        a.s.spellNames.add(1127, "Well Fed", "");
        a.s.spellNames.add(6788, "Weakened Soul", "");
        bool got4 = a.replay(trade, [](GameSession& s) {
            const WorldObject* me = s.selfObject();
            return me && s.auras(*me, 1).size() == 2 && s.rep.initialized;
        });
        PadOut m;
        m.menu = true;
        a.pad(m);
        a.tap(GameUi::Act::MenuItem, 5);
        a.draw("buffs");
        bool panelOk = a.ui.panel(a.s) == GameUi::Panel::Buffs;
        a.tap(GameUi::Act::BuffPick, 6788);
        a.tap(GameUi::Act::BuffCancel);
        bool auraRefused = !a.sentOp(0x136);
        a.tap(GameUi::Act::BuffPick, 1127);
        a.tap(GameUi::Act::BuffCancel);
        // stage 5.18: Y next to the quest chest uses it (CMSG_GAMEOBJ_USE) without selecting it
        Rig o;
        o.dir = dir;
        o.s.factions = ft;
        bool got5 = o.replay(trade, [](GameSession& s) {
            for (auto& [g, ob] : s.objects.all()) if (ob.type == ObjType::GameObject && ob.name == "Milly's Harvest") return true;
            return false;
        });
        const WorldObject* chest = nullptr;
        for (auto& [g, ob] : o.s.objects.all()) if (ob.type == ObjType::GameObject && ob.name == "Milly's Harvest") chest = &ob;
        if (chest) { o.pos[0] = chest->mv.x - 1.0f; o.pos[1] = chest->mv.y + 1.5f; o.pos[2] = chest->mv.z; }
        o.in.yaw = chest ? atan2f(chest->mv.y - o.pos[1], chest->mv.x - o.pos[0]) : 0;
        PadOut y;
        y.interact = true;
        o.pad(y);
        result("object_use", got5 && chest && o.sentOp(0x0B1) && !o.sentOp(0x13D), "Y at the chest ->" + o.ops());
        // herbs / ore / locked chests: the OPEN_LOCK spell of the lock (locks.txt) cast at the object
        if (chest) {
            ObjectTable::GoInfo gi;
            gi.type = 3;
            gi.data[0] = 99;
            o.s.objects.setGoInfo(chest->entry(), gi);
            o.s.locks.addLock(99, {{2, 2, 50}});   // LOCK_KEY_SKILL, LockType 2 = herbalism, skill 50
            o.s.locks.addSpell(2366, 2);           // Herb Gathering
            o.s.locks.addSpell(2575, 3);           // Mining
            o.sent.clear();
            o.pad(y);
            const bool refused = !o.sentOp(0x0B1) && !o.sentOp(0x12E);
            const char* verb = o.s.useVerb(*chest);
            o.s.spells.push_back(2366);
            o.pad(y);
            bool castOk = false;
            for (const Sent& x : o.sent)
                if (x.op == 0x12E && x.data.size() >= 7) {
                    uint32_t sp = x.data[0] | (x.data[1] << 8) | (x.data[2] << 16) | ((uint32_t)x.data[3] << 24);
                    castOk = sp == 2366 && x.data[4] == 0x00 && x.data[5] == 0x08;
                }
            result("object_lock", refused && castOk && verb && !strcmp(verb, "Herb") && !o.sentOp(0x0B1),
                   std::string("no skill: refused, Herbalism -> CAST 2366 at the GO ->") + o.ops());
        }
        result("buffs", got4 && panelOk && auraRefused && a.sentOp(0x136) && a.sent.back().data.size() == 4, "debuff refused, buff ->" + a.ops());
    }
    if (argc >= 7) {   // stage 5.17: the party session - the invitation panel, the context menu of a party member
        std::vector<LoggedPacket> party;
        readPacketLog(argv[6], party);
        Rig r;
        r.dir = dir;
        bool got = r.replay(party, [](GameSession& s) { return !s.groupInviteFrom.empty(); });
        r.draw("invite");
        bool panelOk = r.ui.panel(r.s) == GameUi::Panel::Invite;
        r.tap(GameUi::Act::InviteYes);
        result("invite", got && panelOk && r.sentOp(0x072) && r.s.groupInviteFrom.empty(), "Accept ->" + r.ops());
        {   // sounds: the invitation (session event), the window closing / a click, tabs with their own sound
            const bool inv = r.heardIt("invite");
            r.heard.clear();
            r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Bags);
            const bool bags = r.heardIt("bagsopen");
            r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Social);
            const bool tab = r.heardIt("tab");
            r.heard.clear();
            r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Social);   // the same tab again: a plain click
            const bool click = r.heardIt("click") && !r.heardIt("tab");
            std::string all;
            for (const std::string& h : r.heard) all += " " + h;
            result("sounds", inv && bags && tab && click, "invite, bagsopen, tab, click:" + all);
        }
        {   // the animation stage: emotes and sitting from the chat tab's phrases
            r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Chat);
            r.tap(GameUi::Act::ChatPhrases);
            r.tap(GameUi::Act::EmotePick, 2001);
            r.draw("emotes");
            r.sent.clear();
            const bool danced = r.tap(GameUi::Act::EmotePick, 34);
            bool dance = false, sit = false;
            for (const auto& x : r.sent) if (x.op == 0x104 && x.data.size() >= 16 && x.data[0] == 34) dance = true;
            r.tap(GameUi::Act::ChatPhrases);
            r.tap(GameUi::Act::EmotePick, 2001);
            r.tap(GameUi::Act::EmotePick, 1000);
            for (const auto& x : r.sent) if (x.op == 0x101 && x.data.size() == 1 && x.data[0] == 1) sit = true;
            result("emotes", danced && dance && sit, "Dance -> 0x104, Sit -> 0x101:" + r.ops());
        }
        {   // stage 5.46: the on-screen keyboard, chat destinations, channels, commands
            r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Chat);
            r.sent.clear();
            const bool opened = r.tap(GameUi::Act::ChatWrite) && r.ui.keyboardOpen() && r.ui.panel(r.s) == GameUi::Panel::Keyboard;
            r.draw("keyboard");
            GameUi::Input moving = r.in;
            moving.pad = nullptr;
            const bool typed = r.type("Hello, Azeroth!");
            bool say = false;
            for (const auto& x : r.sent)
                if (x.op == 0x095 && x.data.size() > 9 && x.data[0] == Chat::Say && std::string((const char*)&x.data[8]) == "Hello, Azeroth!") say = true;
            result("kbd_say", opened && typed && say && !r.ui.keyboardOpen(), "Write -> typed text -> CMSG_MESSAGECHAT say:" + r.ops());
            r.sent.clear();
            r.tap(GameUi::Act::ChatWrite);
            const bool cmd = r.type("/roll 20");
            result("kbd_command", cmd && r.sentOp(0x1FB) && !r.sentOp(0x095), "/roll 20 typed with the digits and the slash -> MSG_RANDOM_ROLL:" + r.ops());
            r.sent.clear();
            r.tap(GameUi::Act::ChatWrite);
            r.type("never sent", false);
            result("kbd_cancel", r.sent.empty() && !r.ui.keyboardOpen() && r.ui.panel(r.s) == GameUi::Panel::Tabs, "Cancel sends nothing");
            {   // backspace, shift (one capital) and caps (a double tap), the symbols layer
                std::string got;
                r.ui.askText(r.in, "Test", "ab", false, [&](GameSession&, uint32_t, const std::string& t) { got = t; });
                r.tap(GameUi::Act::KeyboardKey, GameUi::kKbBack);
                r.tap(GameUi::Act::KeyboardKey, GameUi::kKbShift);
                r.tap(GameUi::Act::KeyboardKey, 'x');   // X, then Shift is spent
                r.tap(GameUi::Act::KeyboardKey, 'y');
                r.tap(GameUi::Act::KeyboardKey, GameUi::kKbShift);
                r.tap(GameUi::Act::KeyboardKey, GameUi::kKbShift);   // Caps
                r.tap(GameUi::Act::KeyboardKey, 'q');
                r.tap(GameUi::Act::KeyboardKey, 'w');
                r.tap(GameUi::Act::KeyboardKey, GameUi::kKbShift);   // Caps off
                r.tap(GameUi::Act::KeyboardKey, 'e');
                r.tap(GameUi::Act::KeyboardKey, GameUi::kKbSymbols);
                r.draw("keyboard_symbols");
                r.tap(GameUi::Act::KeyboardKey, '@');
                r.tap(GameUi::Act::KeyboardKey, GameUi::kKbOk);
                result("kbd_keys", got == "aXyQWe@", "backspace, shift, caps, symbols: '" + got + "'");
            }
            {   // the numpad (money): digits only, nine at most
                std::string got = "unset";
                r.ui.askText(r.in, "Copper", "15", true, [&](GameSession&, uint32_t, const std::string& t) { got = t; });
                r.draw("keypad");
                const bool noLetter = !r.tap(GameUi::Act::KeyboardKey, 'a');
                r.type("0123456789");
                result("kbd_numpad", noLetter && got == "150123456", "numpad: no letters, 9 digits max: '" + got + "'");
            }
            {   // movement is blocked while the keyboard is up
                r.ui.askText(r.in, "x", "", false, [](GameSession&, uint32_t, const std::string&) {});
                GameUi::Output o = r.upd(r.in);
                const bool block = o.blockMove && o.blockJump;
                r.tap(GameUi::Act::KeyboardKey, GameUi::kKbCancel);
                GameUi::Output o2 = r.upd(r.in);
                result("kbd_block", block && !o2.blockMove, "the character stands still while typing");
            }
            // channels: join (server side), write, who, leave, join another
            {
                Writer w;
                w.u8(0x02); w.cstr("General - Elwynn Forest"); w.u32(0x18); w.u32(0);
                r.s.handle(0x099, w.b, r.in.nowMs);
                Writer w2;
                w2.u8(0x02); w2.cstr("Trade - Stormwind City"); w2.u32(0x18); w2.u32(0);
                r.s.handle(0x099, w2.b, r.in.nowMs);
            }
            r.tap(GameUi::Act::ChatPhrases);
            r.tap(GameUi::Act::EmotePick, 2002);
            r.draw("channels");
            r.sent.clear();
            const bool sayOpen = r.tap(GameUi::Act::ChannelSay, 2) && r.ui.keyboardOpen();
            r.type("WTS sword");
            bool chanMsg = false;
            for (const auto& x : r.sent)
                if (x.op == 0x095 && x.data.size() > 12 && x.data[0] == Chat::Channel) {
                    const char* to = (const char*)&x.data[8];
                    chanMsg = std::string(to) == "Trade - Stormwind City" && std::string(to + strlen(to) + 1) == "WTS sword";
                }
            result("chan_say", sayOpen && chanMsg, "Channels page: Write on channel 2 -> chat type 0x0E to its name:" + r.ops());
            r.sent.clear();
            r.tap(GameUi::Act::ChatPhrases);
            r.tap(GameUi::Act::EmotePick, 2002);
            r.tap(GameUi::Act::ChannelList, 1);
            const bool listed = r.sentOp(0x09A);
            r.sent.clear();
            r.tap(GameUi::Act::ChatPhrases);
            r.tap(GameUi::Act::EmotePick, 2002);
            r.tap(GameUi::Act::ChannelLeave, 1);
            const bool left = r.sentOp(0x098);
            r.sent.clear();
            const bool jopen = r.tap(GameUi::Act::ChannelJoin) && r.ui.keyboardOpen();
            r.type("MyChannel pw");
            bool joined = false;
            for (const auto& x : r.sent)
                if (x.op == 0x097) joined = std::string((const char*)x.data.data()) == "MyChannel" && std::string((const char*)x.data.data() + 10) == "pw";
            result("chan_manage", listed && left && jopen && joined, "Who -> CHANNEL_LIST, Leave -> LEAVE_CHANNEL, Join... -> JOIN_CHANNEL name + password:" + r.ops());
            // the destination button cycles; the label shows it
            r.tap(GameUi::Act::Tab, (int)GameUi::Tab::Chat);
            r.draw("chat_dest");
            r.s.chat.clear();
            result("chat_dest", r.tap(GameUi::Act::ChatDest) && r.tap(GameUi::Act::ChatDest) && r.tap(GameUi::Act::ChatDest), "the destination button cycles");
            // the system messages arrive in the right tabs
            r.tap(GameUi::Act::ChatChannel, 6);
            Writer sm;
            sm.u32(3); sm.cstr("Maintenance tonight");
            r.s.handle(0x291, sm.b, r.in.nowMs);
            r.draw("chat_system");
            result("chat_tabs", r.s.chat.size() == 1 && r.s.chat.back().type == Chat::System, "SERVER_MESSAGE reaches the chat as a system line");
        }
        Rig c;
        c.dir = dir;
        bool got2 = c.replay(party, [](GameSession& s) { return s.party.inGroup && s.isLeader() && s.party.members.size() == 2; });
        uint64_t far = 0;
        for (const PartyMember& m : c.s.party.members) if (m.name == "Otherguy") far = m.guid;
        PadOut pick;   // L + D-pad up: the first party member
        pick.mode = PadOut::Friend;
        pick.partyPick = 0;
        c.pad(pick);
        bool picked = c.s.target() == c.s.party.members[0].guid;
        c.s.setTarget(far, c.in.nowMs);
        PadOut ctx;
        ctx.context = true;
        c.pad(ctx);
        c.draw("party_context");
        c.sent.clear();
        bool uninv = c.tap(GameUi::Act::ContextItem, 6);
        result("party_context", got2 && picked && uninv && c.sentOp(0x075),
               "L+D-up, X, Uninvite ->" + c.ops() + " (" + std::to_string(got2) + std::to_string(picked) + std::to_string(uninv) + ")");
        {   // UI stage 9: another player - inspect, duel (challenge, their challenge, countdown, result), follow
            uint64_t near = 0;
            float best = 1e9f;
            for (const auto& [g, o] : c.s.objects.all())
                if (o.type == ObjType::Player && !o.self && o.hasPos) {
                    float p[3];
                    o.positionAt(c.in.nowMs, p);
                    const float d = (p[0] - c.pos[0]) * (p[0] - c.pos[0]) + (p[1] - c.pos[1]) * (p[1] - c.pos[1]);
                    if (d < best) { best = d; near = g; }
                }
            if (near && best > 20.0f * 20.0f) {   // bring them close (the inspect / follow range is 28 yd)
                WorldObject* o = c.s.objects.find(near);
                o->mv.x = c.pos[0] + 4; o->mv.y = c.pos[1]; o->mv.z = c.pos[2]; o->pathActive = false; o->posMs = c.in.nowMs;
                o->mv.flags = 0;
            }
            c.s.setTarget(near, c.in.nowMs);
            PadOut ctx2;
            ctx2.context = true;
            c.pad(ctx2);
            c.draw("player_menu");
            c.sent.clear();
            const bool insp = c.tap(GameUi::Act::ContextItem, 12);
            const bool inspPanel = c.ui.panel(c.s) == GameUi::Panel::Inspect;
            {   // the honour answer
                Writer w;
                w.u64(near); w.u8(9); w.u32(3);
                for (int k = 0; k < 6; ++k) w.u16(0);
                w.u32(250); w.u32(1); w.u32(0); w.u32(900); w.u32(0); w.u32(12); w.u8(30);
                c.s.handle(0x2D6, w.b, c.in.nowMs);
            }
            c.draw("inspect");
            result("inspect", near && insp && inspPanel && c.sentOp(0x114) && c.sentOp(0x2D6) && c.s.honor.known && c.s.honor.lifetimeHK == 250,
                   "Inspect -> panel, 0x114 + 0x2D6, honour:" + c.ops());
            c.tap(GameUi::Act::ClosePanel);
            c.pad(ctx2);
            c.sent.clear();
            const bool duelTap = c.tap(GameUi::Act::ContextItem, 14);
            bool spell = false;
            for (const auto& x : c.sent) if (x.op == 0x12E && x.data.size() >= 4 && x.data[0] == (7266 & 0xFF) && x.data[1] == (7266 >> 8)) spell = true;
            // the server's answer to our challenge, then cancelled; then they challenge us and we accept
            auto req = [&](uint64_t from) { Writer w; w.u64(0xF110000000000123ull); w.u64(from); c.s.handle(0x167, w.b, c.in.nowMs); };
            req(c.s.selfGuid() ? c.s.selfGuid() : c.s.objects.self());
            const bool waiting = c.ui.panel(c.s) == GameUi::Panel::Invite && c.s.duel.asked;
            c.draw("duel_wait");
            c.tap(GameUi::Act::InviteNo);
            const bool cancelled = c.sentOp(0x16D);
            { Writer w; w.u8(0); c.s.handle(0x16A, w.b, c.in.nowMs); }
            req(near);
            c.draw("duel_request");
            c.sent.clear();
            c.tap(GameUi::Act::InviteYes);
            const bool accepted = c.sentOp(0x16C) && c.s.duel.fighting;
            { Writer w; w.u32(3000); c.s.handle(0x2B7, w.b, c.in.nowMs); }
            c.s.update(c.in.nowMs + 3500);
            const WorldObject* opp = c.s.objects.find(near);
            const bool attackable = opp && c.s.canAttack(*opp);
            { Writer w; w.u8(0); w.cstr("Testone"); w.cstr("Otherguy"); c.s.handle(0x16B, w.b, c.in.nowMs); }
            { Writer w; w.u8(1); c.s.handle(0x16A, w.b, c.in.nowMs); }
            const bool over = !c.s.duel.active() && c.s.duel.result == "Testone has defeated Otherguy in a duel" && !c.s.canAttack(*opp);
            result("duel", duelTap && spell && waiting && cancelled && accepted && attackable && over,
                   "Duel -> CAST 7266; wait / Cancel -> 0x16D; Accept -> 0x16C; countdown; result (" + std::to_string(spell) +
                   std::to_string(waiting) + std::to_string(cancelled) + std::to_string(accepted) + std::to_string(attackable) +
                   std::to_string(over) + ")");
            // follow: the menu starts it, the session steers until 2 yd, our own move stops it
            c.pad(ctx2);
            const bool fol = c.tap(GameUi::Act::ContextItem, 13) && c.s.following == near;
            float yaw = 0;
            const bool run = c.s.followStep(c.pos, &yaw, c.in.nowMs);
            c.s.stopFollow(c.in.nowMs);
            result("follow", fol && run && c.s.following == 0, "Follow -> runs towards them, stops on request");
        }
    }
    {   // UI stage 10: a ground spell (Blizzard) - the ring, moved by the C-Stick, cast with the same button; cancel; "At target"
        Rig q;
        q.dir = dir;
        q.replay(fight, [](GameSession&) { return false; });
        q.s.spellNames.addGround(10, 30.0f, 8.0f);
        q.s.spells.push_back(10);
        q.bars.bind(8, ActionSlot{ActionSlot::Spell, 10});   // ZL + D-pad up
        q.in.camYaw = 0;
        q.in.groundAt = [](float, float, float, float* z) { *z = 5.0f; return true; };
        q.s.setTarget(0, q.in.nowMs);
        GameUi::Output o;
        q.ui.useSlot(q.s, q.in, 8, o);
        GameUi::Output u = q.ui.update(q.s, q.in, true);
        const bool started = q.ui.placing() && u.placing && fabsf(u.placePos[0] - (q.pos[0] + 10.0f)) < 0.1f && u.placePos[2] == 5.0f;
        // a second of the C-Stick to the right: the ring goes to the camera's right (-y when looking along +x), within range
        GameUi::Input mv = q.in;
        mv.stickX = 1.0f;
        for (int k = 0; k < 20; ++k) { mv.nowMs += 100; u = q.ui.update(q.s, mv, true); }
        const float dx = u.placePos[0] - q.pos[0], dy = u.placePos[1] - q.pos[1];
        const bool moved = dy < -5.0f && sqrtf(dx * dx + dy * dy) <= 30.0f;
        q.in.nowMs = mv.nowMs;
        q.draw("ground_aim");
        q.sent.clear();
        q.ui.useSlot(q.s, q.in, 8, o);   // the same button again: cast at the ring
        bool cast = false;
        for (const auto& x : q.sent)
            if (x.op == 0x12E && x.data.size() == 18 && x.data[0] == 10 && x.data[4] == 0x40) {
                float px, pz;
                memcpy(&px, &x.data[6], 4);
                memcpy(&pz, &x.data[14], 4);
                cast = fabsf(px - u.placePos[0]) < 0.01f && pz == 5.0f;
            }
        const bool done = !q.ui.placing();
        // cancel: the ring goes, nothing is cast
        q.ui.useSlot(q.s, q.in, 8, o);
        q.sent.clear();
        PadOut cancel;
        cancel.cancel = true;
        q.pad(cancel);
        const bool cancelled = !q.ui.placing() && !q.sentOp(0x12E);
        // "At target": a unit in range takes the spell at once
        uint64_t near = 0;
        for (const auto& [g, ob] : q.s.objects.all()) {
            if (ob.self || !ob.isUnitLike() || ob.dead() || !ob.hasPos) continue;
            float p[3];
            ob.positionAt(q.in.nowMs, p);
            if ((p[0] - q.pos[0]) * (p[0] - q.pos[0]) + (p[1] - q.pos[1]) * (p[1] - q.pos[1]) < 25 * 25) { near = g; break; }
        }
        q.cfg.groundCast = 1;
        q.s.setTarget(near, q.in.nowMs);
        q.s.cast = CastState();
        q.in.nowMs += 2000;
        q.sent.clear();
        q.ui.useSlot(q.s, q.in, 8, o);
        const bool quick = near && !q.ui.placing() && q.sentOp(0x12E);
        result("ground_spell", started && moved && cast && done && cancelled && quick,
               "ring 10 yd ahead, C-Stick moves it, again -> CAST 10 at it (0x40), cancel, At target (" + std::to_string(started) +
               std::to_string(moved) + std::to_string(cast) + std::to_string(done) + std::to_string(cancelled) + std::to_string(quick) + ")");
    }
    {   // ---- UI stage 13: pets - the Combat strip, the pet window, binding to the crossbar, the stable master
        Rig q;
        q.dir = dir;
        q.replay(fight, [](GameSession&) { return false; });
        const uint64_t me = q.s.objects.self(), pg = 0xF130000000000555ull;
        auto fieldsW = [](Writer& w, std::initializer_list<std::pair<int, uint32_t>> f) {
            uint32_t mask[8] = {};
            int top = 0;
            for (auto& p : f) { mask[p.first / 32] |= 1u << (p.first % 32); top = std::max(top, p.first / 32 + 1); }
            w.u8((uint8_t)top);
            for (int i = 0; i < top; ++i) w.u32(mask[i]);
            for (int b = 0; b < top; ++b)
                for (int bit = 0; bit < 32; ++bit)
                    for (auto& p : f) if (p.first == b * 32 + bit) w.u32(p.second);
        };
        {   // the pet: a unit next to us, named by the name query, and our SUMMON field pointing at it
            Writer w;
            w.u32(1); w.u8(0); w.u8(3); w.packedGuid(pg); w.u8(3);
            w.u8(0x60);
            w.u32(0); w.u32(0); w.f32(q.pos[0] + 2); w.f32(q.pos[1]); w.f32(q.pos[2]); w.f32(0); w.u32(0);
            for (int k = 0; k < 6; ++k) w.f32(7);
            fieldsW(w, {{UF::UNIT_HEALTH, 300}, {UF::UNIT_MAXHEALTH, 400}, {UF::UNIT_LEVEL, 14}, {UF::UNIT_FIELD_PETNUMBER, 3},
                        {UF::UNIT_POWER1 + 2, 90}, {UF::UNIT_MAXPOWER1 + 2, 100}, {UF::UNIT_POWER1 + 4, 800000}, {UF::UNIT_MAXPOWER1 + 4, 1050000},
                        {UF::UNIT_BYTES_0, 2u << 24}, {UF::UNIT_BYTES_1, 5u << 8}, {UF::UNIT_FLAGS, 0x10}});
            q.s.handle(0x0A9, w.b, q.in.nowMs);
            Writer v;
            v.u32(1); v.u8(0); v.u8(0); v.packedGuid(me);
            fieldsW(v, {{UF::UNIT_FIELD_SUMMON, (uint32_t)pg}, {UF::UNIT_FIELD_SUMMON + 1, (uint32_t)(pg >> 32)}});
            q.s.handle(0x0A9, v.b, q.in.nowMs);
            Writer nq; nq.u32(3); nq.cstr("Fang"); nq.u32(1);
            q.s.handle(0x053, nq.b, q.in.nowMs);
            Writer ps;
            ps.u64(pg); ps.u32(0); ps.u8(1); ps.u8(1); ps.u8(0); ps.u8(0);
            const uint32_t bar[10] = {2 | 7u << 24, 1 | 7u << 24, 0 | 7u << 24, 16827 | 0xC1u << 24, 17253 | 0x81u << 24, 0, 0,
                                      2 | 6u << 24, 1 | 6u << 24, 0 | 6u << 24};
            for (uint32_t b : bar) ps.u32(b);
            ps.u8(3); ps.u32(16827 | 0xC1u << 24); ps.u32(17253 | 0x81u << 24); ps.u32(24604 | 0x01u << 24);
            ps.u16(0);
            q.s.handle(0x179, ps.b, q.in.nowMs);
        }
        q.s.spellNames.add(16827, "Claw", "Rank 1");
        q.s.spellNames.add(17253, "Bite", "Rank 1");
        q.s.spellNames.add(24604, "Furious Howl", "");
        q.in.nowMs += 300;
        q.sent.clear();
        const bool strip = q.tap(GameUi::Act::Tab, (int)GameUi::Tab::Combat);
        q.draw("pet_strip");
        bool have = false;
        for (const GameUi::Button& b : q.ui.buttons()) have = have || b.act == GameUi::Act::PetOpen;
        // attack needs a target: the wolf of the fight log
        uint64_t foe = 0;
        for (const auto& [g, ob] : q.s.objects.all())
            if (g != me && g != pg && ob.isUnitLike() && !ob.dead()) { foe = g; break; }
        q.s.setTarget(foe, q.in.nowMs);
        q.sent.clear();
        const bool atk = foe && q.tap(GameUi::Act::PetCommand, kPetCmdAttack);
        bool atkOk = false;
        for (const Sent& x : q.sent)
            if (x.op == 0x175 && x.data.size() == 20) { Reader r(x.data); atkOk = r.u64() == pg && r.u32() == (2u | 7u << 24) && r.u64() == foe; }
        q.sent.clear();
        const bool stance = q.tap(GameUi::Act::PetCommand, kPetCmdAggressive);
        bool stanceOk = false;
        for (const Sent& x : q.sent)
            if (x.op == 0x175) { Reader r(x.data); r.u64(); stanceOk = r.u32() == (2u | 6u << 24); }
        q.sent.clear();
        const bool cast = q.tap(GameUi::Act::PetCast, 17253);
        bool castOk = false;
        for (const Sent& x : q.sent)
            if (x.op == 0x175) { Reader r(x.data); r.u64(); castOk = r.u32() == (17253u | 0x81u << 24); }
        result("pet_strip", strip && have && atk && atkOk && stance && stanceOk && cast && castOk,
               "Combat strip: attack with a target -> PET_ACTION 0x07000002 at it, aggressive 0x06000002, Bite 0x81 (" + std::to_string(atkOk) +
                   std::to_string(stanceOk) + std::to_string(castOk) + ")");

        // the window
        q.tap(GameUi::Act::PetOpen);
        q.draw("pet_window");
        const bool open = q.ui.panel(q.s) == GameUi::Panel::Pet;
        q.sent.clear();
        const bool autoTap = q.tap(GameUi::Act::PetAuto, 17253);
        bool autoOk = false;
        for (const Sent& x : q.sent) autoOk = autoOk || (x.op == 0x2F3 && x.data.size() == 13 && x.data[12] == 1);
        // bind: tap Bind after selecting the attack order, then press a bar button
        q.tap(GameUi::Act::PetCommand, kPetCmdStay);
        q.sent.clear();
        const bool bindTap = q.tap(GameUi::Act::PetBind, kPetCmdStay);
        const bool armed = q.ui.binding();
        GameUi::Output bo;
        q.ui.useSlot(q.s, q.in, 9, bo);
        const bool bound = q.bars.at(9).type == ActionSlot::PetCmd && q.bars.at(9).id == kPetCmdStay;
        q.sent.clear();
        q.ui.useSlot(q.s, q.in, 9, bo);
        bool slotOk = false;
        for (const Sent& x : q.sent) if (x.op == 0x175) { Reader r(x.data); r.u64(); slotOk = r.u32() == (0u | 7u << 24); }
        // drag a pet spell from the window onto slot 10
        q.tap(GameUi::Act::PetOpen);
        const bool dragged = q.drag(GameUi::Act::PetCast, 17253, 10);
        const bool dragBound = q.bars.at(10).type == ActionSlot::PetSpell && q.bars.at(10).id == 17253;
        q.sent.clear();
        q.ui.useSlot(q.s, q.in, 10, bo);
        bool dragCast = false;
        for (const Sent& x : q.sent) if (x.op == 0x175) { Reader r(x.data); r.u64(); dragCast = (r.u32() & 0xFFFFFF) == 17253; }
        // the bar draws the pet's order lit and the spell with its icon
        // rename: the flag 0x10 is set on the pet
        q.tap(GameUi::Act::PetOpen);
        q.sent.clear();
        const bool ren = q.tap(GameUi::Act::PetRename) && q.type("Rex");
        bool renOk = false;
        for (const Sent& x : q.sent) if (x.op == 0x177) { Reader r(x.data); r.u64(); renOk = r.cstr() == "Rex"; }
        // feed: no known food -> an empty list that closes with Back
        q.tap(GameUi::Act::PetFeed, 1);
        q.draw("pet_feed");
        q.tap(GameUi::Act::PetFeed, 0);
        // dismiss asks first
        q.sent.clear();
        q.tap(GameUi::Act::PetDismiss);
        const bool asked = q.ui.panel(q.s) == GameUi::Panel::Confirm && !q.sentOp(0x176);
        q.draw("pet_confirm");
        q.tap(GameUi::Act::ConfirmYes);
        const bool dismissed = q.sentOp(0x176);
        result("pet_window", open && autoTap && autoOk && bindTap && armed && bound && slotOk && dragged && dragBound && dragCast && ren && renOk &&
                                 asked && dismissed,
               "window, Auto -> 0x2F3, Bind + button, drag a spell to a button and run both, rename 0x177, Abandon asks then 0x176 (" +
                   std::to_string(open) + std::to_string(autoOk) + std::to_string(bound) + std::to_string(slotOk) + std::to_string(dragBound) +
                   std::to_string(dragCast) + std::to_string(renOk) + std::to_string(asked) + std::to_string(dismissed) + ")");

        // the stable master
        {
            Writer w;
            w.u64(0x1111); w.u8(2); w.u8(1);
            w.u32(3); w.u32(1234); w.u32(14); w.cstr("Fang"); w.u32(4); w.u8(1);
            w.u32(8); w.u32(1235); w.u32(9); w.cstr("Sly"); w.u32(2); w.u8(2);
            q.s.handle(0x26F, w.b, q.in.nowMs);
        }
        q.in.nowMs += 300;
        q.draw("pet_stable");
        const bool stOpen = q.ui.panel(q.s) == GameUi::Panel::Stable;
        q.sent.clear();
        q.tap(GameUi::Act::StableStable);
        const bool stOk = q.sentOp(0x270);
        q.tap(GameUi::Act::StablePick, 2);
        q.draw("pet_stable_pick");
        q.sent.clear();
        q.tap(GameUi::Act::StableUnstable);
        bool swapOk = false;
        for (const Sent& x : q.sent) if (x.op == 0x275) { Reader r(x.data); r.u64(); swapOk = r.u32() == 8; }
        q.sent.clear();
        q.tap(GameUi::Act::StableBuy);
        const bool buyOk = q.sentOp(0x272);
        q.tap(GameUi::Act::StableClose);
        const bool closed = q.ui.panel(q.s) != GameUi::Panel::Stable;
        result("pet_stable", stOpen && stOk && swapOk && buyOk && closed, "stable list, stable it 0x270, swap 0x275, buy slot 0x272, close (" +
               std::to_string(stOpen) + std::to_string(stOk) + std::to_string(swapOk) + std::to_string(buyOk) + std::to_string(closed) + ")");
    }
    {   // 6.19: the skills page — rows from the player fields, folding, selection, Unlearn only for professions
        Rig q;
        q.dir = dir;
        q.replay(fight, [](GameSession&) { return false; });
        q.in.nowMs += 300;
        w3n::WorldObject* me = const_cast<w3n::WorldObject*>(q.s.selfObject());
        bool ok = me != nullptr;
        if (ok) {
            const char* lines[] = {"S\t186\t11\tMining\tMining lets you extract ore from deposits and smelt it into bars. A long long description that has to wrap over several lines of the small font.",
                                   "S\t164\t11\tBlacksmithing\tForge weapons.", "S\t185\t9\tCooking\tCook food.", "S\t43\t6\tSwords\tWeapon skill.",
                                   "S\t293\t8\tPlate Mail\tArmor.", "S\t98\t10\tLanguage: Common\tCommon.", "S\t26\t7\tArms\tArms."};
            FILE* f = fopen("/tmp/w3d_ui/skills.txt", "w");
            for (const char* l : lines) fprintf(f, "%s\n", l);
            fclose(f);
            q.s.skills.load("/tmp/w3d_ui/skills.txt");
            me->fields.resize(std::max<size_t>(me->fields.size(), w3n::UF::PLAYER_SKILL_INFO_1_1 + 128 * 3));
            const uint32_t sk[][4] = {{186, 133, 150, 5}, {164, 300, 300, 0}, {185, 1, 75, 0}, {43, 90, 300, 0}, {293, 1, 1, 0}, {98, 300, 300, 0}, {26, 90, 300, 0}};
            for (int i = 0; i < 7; ++i) {
                const int b = w3n::UF::PLAYER_SKILL_INFO_1_1 + i * 3;
                me->fields[b] = sk[i][0] | (1u << 16);
                me->fields[b + 1] = sk[i][1] | (sk[i][2] << 16);
                me->fields[b + 2] = sk[i][3] << 16;
            }
        }
        q.tap(GameUi::Act::Tab, (int)GameUi::Tab::Spells);
        const bool onPage = q.tap(GameUi::Act::SubView, 2);
        q.draw("skills");
        q.tap(GameUi::Act::SkillPick, 186);
        q.draw("skills_pick");
        q.sent.clear();
        q.tap(GameUi::Act::SkillAbandon);
        q.draw("skills_confirm");
        q.tap(GameUi::Act::ConfirmYes);
        bool sentUnlearn = false;
        for (const Sent& x : q.sent) if (x.op == 0x202) { Reader r(x.data); sentUnlearn = r.u32() == 186; }
        q.tap(GameUi::Act::SkillPick, 43);   // a weapon skill: Unlearn must not exist
        q.sent.clear();
        q.tap(GameUi::Act::SkillAbandon);
        q.tap(GameUi::Act::ConfirmYes);
        const bool weaponSafe = !q.sentOp(0x202);
        q.tap(GameUi::Act::SkillHeader, 6);
        q.draw("skills_folded");
        result("skills", ok && onPage && sentUnlearn && weaponSafe,
               "page opens, Unlearn asks then 0x202 (186) for a profession, never for a weapon skill (" + std::to_string(ok) + std::to_string(onPage) +
                   std::to_string(sentUnlearn) + std::to_string(weaponSafe) + ")");
    }
    {   // 6.20: spell descriptions (spelldesc.bin) in the spell window and in the Use: lines of an item
        Rig q;
        q.dir = dir;
        q.replay(fight, [](GameSession&) { return false; });
        q.in.nowMs += 300;
        const char* t1 = "A strike that increases melee damage by 11.";
        const char* t2 = "Restores 80 health over 12 sec.";
        std::string blob("SDSC", 4);
        auto p32 = [&](uint32_t v) { blob.append((const char*)&v, 4); };
        p32(1); p32(2);
        uint32_t off = 12 + 2 * 12;
        p32(78); p32(off); p32((uint32_t)strlen(t1)); off += (uint32_t)strlen(t1);
        p32(2457); p32(off); p32((uint32_t)strlen(t2));
        blob += t1; blob += t2;
        FILE* f = fopen("/tmp/w3d_ui/spelldesc.bin", "wb");
        fwrite(blob.data(), 1, blob.size(), f);
        fclose(f);
        const bool loaded = q.s.spellDesc.load("/tmp/w3d_ui/spelldesc.bin");
        const bool texts = q.s.spellDesc.text(78) == t1 && q.s.spellDesc.text(2457) == t2 && q.s.spellDesc.text(5).empty();
        q.tap(GameUi::Act::Tab, (int)GameUi::Tab::Spells);
        q.tap(GameUi::Act::BindSpell, 2457, 700);
        q.draw("spell_info_desc");
        const bool opened = q.ui.panel(q.s) == GameUi::Panel::SpellInfo;
        w3n::ItemInfo it;
        it.entry = 5; it.name = "Test Potion"; it.spellId[0] = 2457; it.spellTrigger[0] = 0; it.sellPrice = 5;
        const auto lines = w3n::itemTooltip(it, nullptr, q.s.spellNames, &q.s.spellDesc);
        bool use = false;
        for (const std::string& l : lines) use = use || l == std::string("Use: ") + t2;
        const auto plain = w3n::itemTooltip(it, nullptr, q.s.spellNames);
        bool fallback = false;
        for (const std::string& l : plain) fallback = fallback || l.rfind("Use: ", 0) == 0 && l.find("Restores") == std::string::npos;
        result("spell_desc", loaded && texts && opened && use && fallback,
               "spelldesc.bin read on demand, spell window, Use: line with the description (name without the file) (" + std::to_string(loaded) + std::to_string(texts) +
                   std::to_string(opened) + std::to_string(use) + std::to_string(fallback) + ")");
    }
    {   // contact sheet of every screenshot
        const int cols = 4, w = kTouchW + 8, h = kTouchH + 22;
        int rows = ((int)g_shots.size() + cols - 1) / cols;
        Canvas sheet(cols * w, std::max(1, rows) * h, 0xFF101010u);
        for (size_t i = 0; i < g_shots.size(); ++i) {
            int x = (int)(i % cols) * w + 4, y = (int)(i / cols) * h + 18;
            sheet.blit(g_shots[i].second, x, y);
            HudList l;
            l.clear();
            l.text(x, y - 15, g_shots[i].first, 0xFF00D1FFu);
            sheet.draw(l);
        }
        sheet.png(dir + "/ui_sheet.png");
    }
    printf("UI scenes: %d, failed %d\n", scenes, fails);
    return fails ? 1 : 0;
}
