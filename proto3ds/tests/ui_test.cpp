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
    GameUi::Output upd(const GameUi::Input& t) {
        GameUi::Output o = ui.update(s, t, true);
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
