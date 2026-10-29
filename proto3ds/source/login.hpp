// login.hpp — log into a vanilla server from the 3DS (stage 4), through the original client's screens (stage 5.29):
// login, realm list, character list and creation (glue_ui.cpp draws them, this class does what they ask: the network and
// the software keyboard). While the screens are up they own both screens and the buttons; main.cpp draws their 3D scene
// (scene()) and the two draw lists.
#pragma once
#include <3ds.h>

#include <string>

#include "client.hpp"
#include "game.hpp"
#include "glue_data.hpp"
#include "glue_ui.hpp"
#include "w3d_character.h"

class LoginFlow {
public:
    enum class Phase { Menu, Authenticating, RealmSelect, WorldConnecting, CharSelect, CharCreate, EnteringWorld, InWorld,
                       Offline };

    void init();                       // loads sdmc:/3ds/w3dproto/login.cfg and glue.txt
    // This frame's buttons and touch. Returns true while the login screens own the screens and the buttons.
    bool update(u32 down, u32 held, bool touching, int tx, int ty, float dt);
    void shutdown();                   // sends logout, closes sockets
    bool quitRequested() const { return quit_; }

    Phase phase() const { return phase_; }
    bool glueActive() const { return phase_ != Phase::InWorld && phase_ != Phase::Offline; }
    // The screens' draw lists (rebuilt by update) and a counter that changes whenever the touch screen must be redrawn.
    const HudList& glueTop() const { return top_; }
    const HudList& glueBottom() const { return bottom_; }
    uint32_t glueRevision() const { return ui_.revision(); }
    // What the 3D scene behind the top screen shows: a GLUE scene id (W3D_GLUE_MAINMENU or a race) and maybe a character.
    struct Scene {
        uint32_t id = W3D_GLUE_MAINMENU;
        bool character = false;
        W3dLook look{};
        uint32_t equip[W3D_EQ_SLOTS] = {};
        uint8_t equipInv[W3D_EQ_SLOTS] = {};
        float yaw = 0;                 // turned by L / R (added to the scene's facing)
    };
    Scene scene() const;
    void setSceneDrawn(bool drawn) { model_.sceneDrawn = drawn; }

    bool justEnteredWorld() { bool v = entered_; entered_ = false; return v; }
    // Server position == render position (see WoWee core/coordinates.hpp).
    const float* worldPosition() const { return world_.position(); }
    float orientation() const { return world_.orientation(); }
    uint32_t mapId() const { return world_.mapId(); }
    const glue::Texts& glueTexts() const { return texts_; }   // glue.txt: GlueStrings, zone names
    const w3n::Timings& authTimings() const { return auth_.timings(); }
    const w3n::Timings& worldTimings() const { return world_.timings(); }
    uint32_t latencyMs() const { return world_.lastLatencyMs(); }
    const w3n::WorldClient& worldClient() const { return world_; }
    uint64_t bytesReceived() const { return world_.bytesReceived(); }
    size_t opcodesSeen() const { return world_.opcodeCounts().size(); }
    const std::string& characterName() const { return charName_; }
    bool online() const { return phase_ == Phase::InWorld; }
    // Stage 5: in-world session (objects, own movement). Valid while online().
    w3n::GameSession& game() { return game_; }
    uint32_t sessionMs() const { return (uint32_t)(w3n::nowMs() - sessionT0_); }
    bool logging() const { return logOn_; }

private:
    void saveConfig();
    bool editText(const char* hint, std::string& value, bool password, int maxLen = 64);
    void message(const std::string& text);
    void act(GlueAction a);
    void network();
    void fillModel();
    void enterWorld();
    void selectByName(const std::string& name);

    Phase phase_ = Phase::Menu;
    std::string server_ = "127.0.0.1:3724", account_, password_, charName_, lastRealm_, lastChar_;
    int protocol_ = 3;
    bool logOn_ = false;           // login.cfg "log=1": record packets to session.w3dl
    uint64_t sessionT0_ = 0;
    w3n::GameSession game_;
    w3n::PacketLog log_;
    bool entered_ = false, quit_ = false;
    w3n::AuthClient auth_;
    w3n::WorldClient world_;
    // the screens
    GlueUi ui_;
    GlueModel model_;
    glue::Texts texts_;
    HudList top_, bottom_;
    enum class Pending { None, Auth, World };   // a blocking connect runs one frame after its dialog is shown
    Pending pending_ = Pending::None;
    std::string wait_;
    bool waitFixed_ = false;
    uint64_t waitSince_ = 0;
    enum class Busy { None, Create, Delete, List };
    Busy busy_ = Busy::None;
    std::string selectAfterList_;   // the character to choose once the list arrives (a new one)
    bool touchWas_ = false;
    bool lookPending_ = false;      // a new creation: a random look once the race's choices are known
};
