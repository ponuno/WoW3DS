// Stage 5.29: the glue screens on the PC. Draws login, realm list, character list and creation (both screens) with the
// PC rasteriser and drives them with simulated buttons and taps: every control must lead to its action, dialogs must be
// modal, the look must stay inside what the race has.
//   glue_test <outdir> [ui.w3dp] [glue.txt] [lang]
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "glue_ui.hpp"
#include "ui_raster.hpp"
#include "ui_style.hpp"
#include "w3d_character.h"

static int g_bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_bad; } } while (0)

constexpr uint32_t A = 1, B = 2, SEL = 4, START = 8, RIGHT = 16, LEFT = 32, UP = 64, DOWN = 128, R = 256, L = 512, X = 1024,
                   Y = 2048;

static std::vector<std::pair<std::string, Canvas>> g_shots;

struct Rig {
    GlueUi ui;
    GlueModel m;
    HudList top, bottom;
    std::string dir;
    GlueAction step(uint32_t down = 0, int tx = -1, int ty = -1, uint32_t held = 0, float dt = 1.0f / 30) {
        GlueInput in;
        in.down = down;
        in.held = held | down;
        in.dt = dt;
        if (tx >= 0) { in.touchDown = true; in.touching = true; in.tx = tx; in.ty = ty; }
        GlueAction a = ui.update(m, in);
        ui.buildTop(top, m);
        ui.buildBottom(bottom, m);
        return a;
    }
    void settle() { step(); step(); }   // a new screen takes a frame to build its widgets
    void shot(const std::string& name) {
        Canvas t(kHudW, kHudH), b(kTouchW, kTouchH);
        t.backdrop();
        t.draw(top);
        b.draw(bottom);
        Canvas both(kHudW, kHudH + kTouchH, 0xFF000000u);
        both.blit(t, 0, 0);
        both.blit(b, (kHudW - kTouchW) / 2, kHudH);
        both.png(dir + "/glue_" + name + ".png");
        g_shots.push_back({name, both});
    }
};

// A CHRS blob with a few looks: skins 0..2, faces 0..3 for skin 0 and 0..1 for the others, hair styles 0..4 with colours
// (style 2: colours 0..1 only), facial 0..2.
static std::vector<uint8_t> fakeChrs() {
    std::vector<W3dCharSection> sec;
    for (uint8_t sk = 0; sk < 3; ++sk) sec.push_back({W3D_CSEC_SKIN, 0, sk, 0, {0, 0}});
    for (uint8_t f = 0; f < 4; ++f) sec.push_back({W3D_CSEC_FACE, f, 0, 0, {0, 0}});
    for (uint8_t sk = 1; sk < 3; ++sk)
        for (uint8_t f = 0; f < 2; ++f) sec.push_back({W3D_CSEC_FACE, f, sk, 0, {0, 0}});
    for (uint8_t hs = 0; hs < 5; ++hs)
        for (uint8_t hc = 0; hc < (hs == 2 ? 2 : 6); ++hc) sec.push_back({W3D_CSEC_HAIR, hs, hc, 0, {0, 0}});
    std::vector<W3dCharStyle> st;
    for (uint8_t v = 0; v < 3; ++v) st.push_back({1, v, {0, 0, 0}});
    W3dCharHeader h{};
    h.race = 1;
    h.sectionCount = (uint32_t)sec.size();
    h.styleCount = (uint32_t)st.size();
    h.offSections = sizeof h;
    h.offStyles = (uint32_t)(sizeof h + sec.size() * sizeof(W3dCharSection));
    std::vector<uint8_t> b(h.offStyles + st.size() * sizeof(W3dCharStyle));
    memcpy(b.data(), &h, sizeof h);
    memcpy(b.data() + h.offSections, sec.data(), sec.size() * sizeof(W3dCharSection));
    memcpy(b.data() + h.offStyles, st.data(), st.size() * sizeof(W3dCharStyle));
    return b;
}

int main(int argc, char** argv) {
    Rig r;
    r.dir = argc > 1 ? argv[1] : ".";
    if (argc > 2 && argv[2][0]) {
        char err[160] = "";
        if (!ui::assets().open(argv[2], err, sizeof err)) std::printf("ui pack: %s (stub skin)\n", err);
    }
    glue::Texts texts;
    if (argc > 3 && argv[3][0]) texts.load(argv[3]);
    texts.addZone(12, "Elwynn Forest");
    texts.addZone(141, "Teldrassil");
    ui::g_lang = argc > 4 ? std::atoi(argv[4]) : 0;
    r.m.texts = &texts;

    // ------------------------------------------------------------ login
    r.m.screen = GlueScreen::Login;
    r.m.server = "logon.example.org:3724";
    r.m.account = "TESTACCOUNT";
    r.settle();
    r.shot("login");
    CHECK(r.step(A) == GlueAction::EditAccount);                    // the account field has the focus first
    r.step(UP);
    CHECK(r.step(A) == GlueAction::EditServer);
    r.step(DOWN); r.step(DOWN);
    CHECK(r.step(A) == GlueAction::EditPassword);
    r.m.havePassword = true;
    r.step(DOWN);
    CHECK(r.step(A) == GlueAction::Login);
    r.step(RIGHT);
    CHECK(r.step(A) == GlueAction::Offline || r.step(A) == GlueAction::Quit);
    CHECK(r.step(START) == GlueAction::Quit);
    CHECK(r.step(0, 160, 24 + 2 * 34 + 10) == GlueAction::EditPassword);   // tap the password row
    CHECK(r.step(0, 160, 140) == GlueAction::Login);
    CHECK(r.step(0, 50, 180) == GlueAction::Offline);
    CHECK(r.step(0, 160, 180) == GlueAction::ToggleProtocol);
    // waiting and errors
    r.m.wait = "Connecting...";
    r.step();
    r.shot("login_wait");
    CHECK(r.step(0, 160, 140) != GlueAction::Login);                 // the dialog is modal
    CHECK(r.step(A) == GlueAction::CancelWait);
    r.m.wait.clear();
    r.m.message = "Unable to connect. Please check your Internet connection and try again.";
    r.step();
    r.shot("login_error");
    CHECK(r.step(0, 160, 140) != GlueAction::Login);
    CHECK(r.step(B) == GlueAction::DismissMessage);
    r.m.message.clear();

    // ------------------------------------------------------------ realm list
    std::vector<w3n::Realm> realms(3);
    realms[0].name = "Stonetavern"; realms[0].icon = 1; realms[0].population = 1.6f; realms[0].characters = 2;
    realms[1].name = "Test Realm (RP)"; realms[1].icon = 6; realms[1].population = 0.4f;
    realms[2].name = "Closed Realm"; realms[2].flags = 2;
    r.m.realms = &realms;
    r.m.screen = GlueScreen::RealmList;
    r.settle();
    r.shot("realms");
    r.step(DOWN);
    CHECK(r.ui.realm == 1);
    CHECK(r.step(A) == GlueAction::RealmAccept);
    CHECK(r.step(B) == GlueAction::RealmCancel);
    CHECK(r.step(0, 100, 22 + 2 * 30 + 5) == GlueAction::None && r.ui.realm == 2);   // a tap chooses ...
    CHECK(r.step(0, 100, 22 + 2 * 30 + 5) == GlueAction::RealmAccept);              // ... a second one accepts
    CHECK(r.step(0, 60, 210) == GlueAction::RealmAccept);
    CHECK(r.step(0, 200, 210) == GlueAction::RealmCancel);

    // ------------------------------------------------------------ character list
    std::vector<w3n::Character> chars(2);
    chars[0].name = "Ponuno"; chars[0].race = 1; chars[0].cls = 2; chars[0].level = 12; chars[0].zone = 12;
    chars[1].name = "Thrallson"; chars[1].race = 2; chars[1].cls = 7; chars[1].level = 60; chars[1].zone = 141;
    r.m.chars = &chars;
    r.m.realmName = "Stonetavern";
    r.m.screen = GlueScreen::CharSelect;
    r.settle();
    r.shot("chars");
    r.step(DOWN);
    CHECK(r.ui.character == 1);
    r.step(0, -1, -1, L, 0.5f);
    CHECK(r.ui.modelYaw > 0.5f);                                                     // L turns the model
    CHECK(r.step(A) == GlueAction::EnterWorld);
    CHECK(r.step(X) == GlueAction::CreateNew);
    CHECK(r.step(SEL) == GlueAction::ChangeRealm);
    CHECK(r.step(B) == GlueAction::Back);
    r.step(Y);                                                                       // delete: asks first
    r.shot("chars_delete");
    CHECK(r.step(B) == GlueAction::None);
    r.step(Y);
    CHECK(r.step(A) == GlueAction::DeleteCharacter);
    CHECK(r.step(0, 40, 225) == GlueAction::None);                                   // the touch Delete asks too ...
    CHECK(r.step(0, 160 - 92 + 20, 120 + 10) != GlueAction::EnterWorld);             // (modal)
    r.step(B);
    CHECK(r.step(0, 100, 2 + 5) == GlueAction::None && r.ui.character == 0);         // tap a row
    CHECK(r.step(0, 100, 2 + 5) == GlueAction::EnterWorld);                          // again: enter the world
    CHECK(r.step(0, 230, 195) == GlueAction::CreateNew);
    CHECK(r.step(0, 150, 225) == GlueAction::ChangeRealm);
    std::vector<w3n::Character> none;
    r.m.chars = &none;
    r.step();
    r.shot("chars_empty");
    CHECK(r.step(A) == GlueAction::None);
    r.m.chars = &chars;

    // ------------------------------------------------------------ character creation
    std::vector<uint8_t> chrs = fakeChrs();
    glue::appearanceFrom(chrs.data(), (uint32_t)chrs.size(), 0, 0, r.m.appearance);
    CHECK(r.m.appearance.skins.size() == 3 && r.m.appearance.faces.size() == 4 && r.m.appearance.hairStyles.size() == 5 &&
          r.m.appearance.hairColors.size() == 6 && r.m.appearance.facials.size() == 3);
    glue::Appearance a2;
    glue::appearanceFrom(chrs.data(), (uint32_t)chrs.size(), 1, 2, a2);
    CHECK(a2.faces.size() == 2 && a2.hairColors.size() == 2);                        // faces per skin, colours per style
    r.m.appearanceKnown = true;
    r.m.screen = GlueScreen::CharCreate;
    r.settle();
    r.ui.startCreate(1, 0, r.m);
    r.ui.create.name = "Newhero";
    r.step();
    r.shot("create_race");
    CHECK(r.ui.create.race == 1 && r.ui.create.cls == 1);                            // Human, the first class: Warrior
    // the race grid: Human -> Horde column (Orc), select it
    r.step(RIGHT);
    r.step(A);
    CHECK(r.ui.create.race == 2);
    r.step(DOWN); r.step(DOWN); r.step(A);                                           // Tauren
    CHECK(r.ui.create.race == 6);
    CHECK(glue::allowed(6, r.ui.create.cls));
    r.ui.create.cls = 11;                                                            // a druid
    for (int i = 0; i < 3; ++i) r.step(UP);
    r.step(A);                                                                       // Orc: no druids -> the first Orc class
    CHECK(r.ui.create.race == 2 && r.ui.create.cls == 1);
    // the look rows: focus one with the D-pad and turn it with left / right
    for (int i = 0; i < 12; ++i) r.step(RIGHT);                                     // to the right column
    for (int i = 0; i < 4; ++i) r.step(DOWN);
    const GlueCreate before = r.ui.create;
    r.step(RIGHT);
    const GlueCreate after = r.ui.create;
    const bool changed = before.skin != after.skin || before.face != after.face || before.hairStyle != after.hairStyle ||
                         before.hairColor != after.hairColor || before.facial != after.facial;
    CHECK(changed);
    for (int i = 0; i < 40; ++i) r.step(i & 1 ? RIGHT : LEFT);
    r.ui.clampLook(r.m.appearance);
    CHECK(std::find(r.m.appearance.skins.begin(), r.m.appearance.skins.end(), r.ui.create.skin) != r.m.appearance.skins.end());
    r.step(X);                                                                      // randomize
    CHECK(r.step(Y) == GlueAction::EditName);
    CHECK(r.step(START) == GlueAction::CreateAccept);
    CHECK(r.step(B) == GlueAction::CreateBack);
    r.step(0, -1, -1, R, 0.5f);
    CHECK(r.ui.modelYaw < 0.0f);
    r.step(0, 240, 10);                                                             // the Class tab
    r.shot("create_class");
    r.step(0, 80, 10);
    r.ui.startCreate(8, 1, r.m);
    r.step();
    r.shot("create_troll");
    // drag the description
    GlueInput in;
    in.touchDown = true; in.touching = true; in.tx = 160; in.ty = 200;
    r.ui.update(r.m, in);
    in.touchDown = false; in.ty = 120;
    r.ui.update(r.m, in);
    r.ui.buildBottom(r.bottom, r.m);
    r.m.appearanceKnown = false;
    r.step();
    r.shot("create_loading");
    r.m.appearanceKnown = true;
    r.m.wait = "Creating character...";
    r.m.waitFixed = true;
    r.step();
    r.shot("create_wait");
    CHECK(r.step(A) == GlueAction::None);                                           // cannot be cancelled
    r.m.wait.clear();
    r.m.waitFixed = false;
    r.m.message = "That name is unavailable";
    r.step();
    CHECK(r.step(A) == GlueAction::DismissMessage);
    r.m.message.clear();

    // the start outfits and zones of glue.txt
    uint32_t disp[19];
    uint8_t inv[19];
    texts.addOutfit(1, 1, 0, {{7101, 5}, {7002, 8}, {1234, 18}, {55, 13}});
    CHECK(texts.outfit(1, 1, 0, disp, inv) && disp[4] == 7101 && disp[7] == 7002 && disp[15] == 55);
    CHECK(glue::slotForInventoryType(18) < 0 && glue::slotForInventoryType(20) == 4 && glue::slotForInventoryType(17) == 15);
    // a sheet of every screenshot (2 per row)
    const int cw = kHudW, ch = kHudH + kTouchH;
    Canvas sheet(cw * 2 + 6, (int)((g_shots.size() + 1) / 2) * (ch + 6), 0xFF202020u);
    for (size_t i = 0; i < g_shots.size(); ++i) sheet.blit(g_shots[i].second, (int)(i % 2) * (cw + 6), (int)(i / 2) * (ch + 6));
    sheet.png(r.dir + "/glue_sheet.png");
    std::printf("GLUE SCREENS: %s (%zu screenshots in %s)\n", g_bad ? "FAIL" : "PASS", g_shots.size(), r.dir.c_str());
    return g_bad != 0;
}
