// glue_ui.hpp — the login, realm, character selection and character creation screens (stage 5.29), after the original
// client's "glue" screens. The top screen is the game's own: the 3D scene (drawn by main.cpp) with the frames, fields, lists
// and buttons over it, worked with the D-pad and buttons; the touch screen mirrors the choices as big touch targets, or
// shows the race and class descriptions while a character is being created. The character model turns with L / R.
//
// Platform-free (draw lists + input -> actions): login.cpp does what the actions ask (network, the software keyboard),
// tests/glue_test.cpp draws every screen on the PC and drives it with simulated buttons and taps.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "client.hpp"
#include "glue_data.hpp"
#include "hud.hpp"

enum class GlueScreen : uint8_t { Login, RealmList, CharSelect, CharCreate };

enum class GlueAction : uint8_t {
    None,
    EditServer, EditAccount, EditPassword, Login, Offline, Quit, ToggleProtocol, ToggleLog,   // login
    CancelWait, DismissMessage,                                                             // dialogs
    RealmAccept, RealmCancel,                                                               // realm list
    EnterWorld, CreateNew, DeleteCharacter, ChangeRealm, Back,                              // character list
    EditName, CreateAccept, CreateBack,                                                     // creation
};

// What the screens show; login.cpp fills it every frame.
struct GlueModel {
    GlueScreen screen = GlueScreen::Login;
    std::string server, account;
    bool havePassword = false;
    int protocol = 3;
    bool packetLog = false;
    std::string wait;              // non-empty: a "Connecting..." dialog over the screen (Cancel unless waitFixed)
    bool waitFixed = false;        // cannot be cancelled (entering the world, creating a character)
    std::string message;           // non-empty: a dialog with Okay
    const std::vector<w3n::Realm>* realms = nullptr;
    std::string realmName;
    const std::vector<w3n::Character>* chars = nullptr;
    const glue::Texts* texts = nullptr;
    glue::Appearance appearance;   // character creation: what the chosen race / sex has
    bool appearanceKnown = false;  // false while its CHRS entry loads (or the pack has none)
    bool sceneDrawn = false;       // a 3D scene is behind the top screen (else a backdrop is painted)
};

struct GlueInput {
    uint32_t down = 0, held = 0;   // KEY_* bits (libctru values)
    bool touchDown = false;        // the stylus touched this frame
    bool touching = false;         // held
    int tx = 0, ty = 0;
    float dt = 0;
};

struct GlueCreate {
    uint8_t race = 1, sex = 0, cls = 1;
    uint8_t skin = 0, face = 0, hairStyle = 0, hairColor = 0, facial = 0;
    std::string name;
};

class GlueUi {
public:
    // Input for this frame (with the widgets of the last build), then the draw lists.
    GlueAction update(const GlueModel& m, const GlueInput& in);
    void buildTop(HudList& l, const GlueModel& m);
    void buildBottom(HudList& l, const GlueModel& m);

    // selections, read by login.cpp
    int realm = 0, character = 0;
    GlueCreate create;
    float modelYaw = 0;            // radians added to the character's facing (L / R)
    // Creation starts from this race / sex with the first class it may be and a random look.
    void startCreate(uint8_t race, uint8_t sex, const GlueModel& m);
    void randomize(const GlueModel& m);
    // Keeps the look inside the choices the race has (after the race, sex, skin or hair style changed).
    void clampLook(const glue::Appearance& a);
    // Bumped whenever the touch screen must be drawn again.
    uint32_t revision() const { return rev_; }
    void touched() { ++rev_; }
    int focusKeyForTest() const { return focus_; }

private:
    struct Widget {
        int x, y, w, h;
        GlueAction act;
        int arg;                   // what the widget stands for (row, race, class ...), see kind
        uint8_t kind;              // WidgetKind
        bool focusable;
    };
    enum WidgetKind : uint8_t { kButton = 0, kField, kRace, kSex, kClass, kLook, kRealmRow, kCharRow, kTab, kScrollUp,
                                kScrollDown };
    void addTop(int x, int y, int w, int h, GlueAction a, int arg, uint8_t kind, bool focusable = true) {
        top_.push_back({x, y, w, h, a, arg, kind, focusable});
    }
    void addBottom(int x, int y, int w, int h, GlueAction a, int arg, uint8_t kind) {
        bottom_.push_back({x, y, w, h, a, arg, kind, false});
    }
    GlueAction activate(const Widget& w, const GlueModel& m);
    void stepLook(int row, int dir, const GlueModel& m);
    void moveFocus(int dx, int dy);
    int focusKey(const Widget& w) const { return (int)w.kind << 16 | (int)w.act << 8 | (w.arg & 0xFF); }
    bool focused(uint8_t kind, GlueAction a, int arg) const { return focus_ == ((int)kind << 16 | (int)a << 8 | (arg & 0xFF)); }
    void defaultFocus(GlueScreen s);
    void selectRace(uint8_t race, const GlueModel& m);
    // drawing
    void backdrop(HudList& l, int w, int h, const GlueModel& m) const;
    void panel(HudList& l, int x, int y, int w, int h, int alpha = 225) const;
    void button(HudList& l, int x, int y, int w, int h, const std::string& text, bool enabled, bool focus,
                const char* hint = nullptr) const;
    void field(HudList& l, int x, int y, int w, int h, const std::string& text, bool focus, bool password,
               const char* placeholder) const;
    void iconButton(HudList& l, int x, int y, int size, const char* iconName, const char* fallback, bool selected,
                    bool focus, bool enabled) const;
    void dialogs(HudList& l, int w, int h, const GlueModel& m, bool topScreen);
    void topLogin(HudList& l, const GlueModel& m);
    void topRealms(HudList& l, const GlueModel& m);
    void topChars(HudList& l, const GlueModel& m);
    void topCreate(HudList& l, const GlueModel& m);
    void bottomLogin(HudList& l, const GlueModel& m);
    void bottomRealms(HudList& l, const GlueModel& m);
    void bottomChars(HudList& l, const GlueModel& m);
    void bottomCreate(HudList& l, const GlueModel& m);

    std::vector<Widget> top_, bottom_;
    GlueScreen screen_ = GlueScreen::Login;
    bool screenSeen_ = false;
    int focus_ = -1;
    bool confirmDelete_ = false;
    int infoTab_ = 0;              // creation, touch screen: 0 race, 1 class
    int infoScroll_ = 0, infoHeight_ = 0;
    int listScroll_ = 0;           // realm / character list on the touch screen
    int dragY_ = -1, dragScroll_ = 0;
    uint32_t rand_ = 0x2545F491u;
    uint32_t rev_ = 1;
    float blink_ = 0;
    std::string lastMessage_, lastWait_;
};
