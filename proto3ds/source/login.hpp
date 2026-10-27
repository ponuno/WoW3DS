// login.hpp — stage 4: log into a vanilla server from the 3DS.
//
// Bottom-screen text UI: server/account entry (software keyboard), realm and
// character selection, then the world session keeps running (ping) while the
// renderer shows the character's position.
#pragma once
#include <3ds.h>

#include <string>

#include "client.hpp"
#include "game.hpp"

class LoginFlow {
public:
    enum class Phase { Menu, Authenticating, RealmSelect, WorldConnecting, CharSelect, EnteringWorld, InWorld,
                       Offline, Error };

    void init();                       // loads sdmc:/3ds/w3dproto/login.cfg
    // Feed this frame's newly pressed keys. Returns true while the login UI owns
    // the bottom screen (the HUD is hidden).
    bool update(u32 down);
    void shutdown();                   // sends logout, closes sockets

    Phase phase() const { return phase_; }
    bool justEnteredWorld() { bool v = entered_; entered_ = false; return v; }
    // Server position == render position (see WoWee core/coordinates.hpp).
    const float* worldPosition() const { return world_.position(); }
    float orientation() const { return world_.orientation(); }
    uint32_t mapId() const { return world_.mapId(); }
    const w3n::Timings& authTimings() const { return auth_.timings(); }
    const w3n::Timings& worldTimings() const { return world_.timings(); }
    uint32_t latencyMs() const { return world_.lastLatencyMs(); }
    uint64_t bytesReceived() const { return world_.bytesReceived(); }
    size_t opcodesSeen() const { return world_.opcodeCounts().size(); }
    const std::string& characterName() const { return charName_; }
    bool online() const { return phase_ == Phase::InWorld; }
    // Stage 5: in-world session (objects, own movement). Valid while online().
    w3n::GameSession& game() { return game_; }
    uint32_t sessionMs() const { return (uint32_t)(w3n::nowMs() - sessionT0_); }
    bool logging() const { return logOn_; }

private:
    void draw();
    void saveConfig();
    bool editText(const char* hint, std::string& value, bool password);
    void setError(const std::string& e);

    Phase phase_ = Phase::Menu;
    std::string server_ = "127.0.0.1:3724", account_, error_, charName_;
    int protocol_ = 3;
    bool logOn_ = false;           // login.cfg "log=1": record packets to session.w3dl
    uint64_t sessionT0_ = 0;
    w3n::GameSession game_;
    w3n::PacketLog log_;
    int cursor_ = 0;
    bool dirty_ = true, entered_ = false;
    w3n::AuthClient auth_;
    w3n::WorldClient world_;
};
