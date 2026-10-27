#include "login.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>

static const char* kCfgPath = "sdmc:/3ds/w3dproto/login.cfg";

void LoginFlow::init() {
    FILE* f = fopen(kCfgPath, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            if (k == "server") server_ = v;
            else if (k == "account") account_ = v;
            else if (k == "protocol") protocol_ = atoi(v.c_str()) == 8 ? 8 : 3;
            else if (k == "log") logOn_ = atoi(v.c_str()) != 0;
        }
        fclose(f);
    }
    phase_ = Phase::Menu;
    dirty_ = true;
}

void LoginFlow::saveConfig() {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/w3dproto", 0777);
    FILE* f = fopen(kCfgPath, "w");
    if (!f) return;
    // The password is never stored.
    fprintf(f, "server=%s\naccount=%s\nprotocol=%d\nlog=%d\n", server_.c_str(), account_.c_str(), protocol_, logOn_ ? 1 : 0);
    fclose(f);
}

bool LoginFlow::editText(const char* hint, std::string& value, bool password) {
    SwkbdState kb;
    char buf[128];
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, sizeof(buf) - 1);
    swkbdSetHintText(&kb, hint);
    if (!password) swkbdSetInitialText(&kb, value.c_str());
    swkbdSetPasswordMode(&kb, password ? SWKBD_PASSWORD_HIDE_DELAY : SWKBD_PASSWORD_NONE);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    SwkbdButton b = swkbdInputText(&kb, buf, sizeof buf);
    dirty_ = true;
    if (b != SWKBD_BUTTON_RIGHT) return false;
    value = buf;
    memset(buf, 0, sizeof buf);
    return true;
}

void LoginFlow::setError(const std::string& e) {
    game_.detach();
    log_.close();
    error_ = e;
    phase_ = Phase::Error;
    dirty_ = true;
}

static void status(const char* msg) {   // shown before a blocking call
    // The bottom console is single-buffered: flushing the cache is enough to
    // show the text before the blocking TCP connect (no swap, citro3d owns that).
    printf("\x1b[2J\x1b[1;1H%s\n", msg);
    gfxFlushBuffers();
    gspWaitForVBlank();
}

bool LoginFlow::update(u32 down) {
    w3n::ClientVersion ver;
    ver.authProtocol = (uint8_t)protocol_;
    switch (phase_) {
        case Phase::Menu: {
            const int items = 6;
            if (down & KEY_DOWN) { cursor_ = (cursor_ + 1) % items; dirty_ = true; }
            if (down & KEY_UP) { cursor_ = (cursor_ + items - 1) % items; dirty_ = true; }
            if (down & KEY_A) {
                if (cursor_ == 0) { if (editText("Server  host:port (3724 by default)", server_, false)) saveConfig(); }
                else if (cursor_ == 1) { if (editText("Account name", account_, false)) saveConfig(); }
                else if (cursor_ == 2) { protocol_ = protocol_ == 3 ? 8 : 3; saveConfig(); dirty_ = true; }
                else if (cursor_ == 5) { logOn_ = !logOn_; saveConfig(); dirty_ = true; }
                else if (cursor_ == 3) {
                    if (account_.empty()) { setError("enter an account name first"); break; }
                    std::string password;
                    if (!editText("Password", password, true)) break;
                    status("Connecting to the login server...");
                    bool ok = auth_.start(server_, account_, password, ver);
                    std::fill(password.begin(), password.end(), '\0');
                    if (!ok) { setError(auth_.error()); break; }
                    phase_ = Phase::Authenticating;
                    dirty_ = true;
                } else if (cursor_ == 4) {
                    phase_ = Phase::Offline;
                    printf("\x1b[2J");
                    return false;
                }
            }
            break;
        }
        case Phase::Authenticating:
            auth_.update();
            if (auth_.state() == w3n::AuthClient::State::Failed) setError(auth_.error());
            else if (auth_.state() == w3n::AuthClient::State::Done) { phase_ = Phase::RealmSelect; cursor_ = 0; dirty_ = true; }
            break;
        case Phase::RealmSelect: {
            int n = (int)auth_.realms().size();
            if (down & KEY_DOWN) { cursor_ = (cursor_ + 1) % n; dirty_ = true; }
            if (down & KEY_UP) { cursor_ = (cursor_ + n - 1) % n; dirty_ = true; }
            if (down & KEY_B) { phase_ = Phase::Menu; cursor_ = 3; dirty_ = true; }
            if (down & KEY_A) {
                const w3n::Realm& r = auth_.realms()[cursor_];
                status(("Connecting to " + r.name + "...").c_str());
                if (!world_.start(r.address, auth_.account(), auth_.sessionKey(), ver)) { setError(world_.error()); break; }
                phase_ = Phase::WorldConnecting;
                dirty_ = true;
            }
            break;
        }
        case Phase::WorldConnecting:
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) setError(world_.error());
            else if (world_.state() == w3n::WorldClient::State::InQueue) dirty_ = true;
            else if (world_.state() == w3n::WorldClient::State::CharSelect) { phase_ = Phase::CharSelect; cursor_ = 0; dirty_ = true; }
            break;
        case Phase::CharSelect: {
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) { setError(world_.error()); break; }
            int n = (int)world_.characters().size();
            if (n > 0) {
                if (down & KEY_DOWN) { cursor_ = (cursor_ + 1) % n; dirty_ = true; }
                if (down & KEY_UP) { cursor_ = (cursor_ + n - 1) % n; dirty_ = true; }
                if (down & KEY_A) {
                    charName_ = world_.characters()[cursor_].name;
                    world_.login(world_.characters()[cursor_].guid);
                    phase_ = Phase::EnteringWorld;
                    dirty_ = true;
                }
            }
            if (down & KEY_B) { world_.close(); phase_ = Phase::Menu; cursor_ = 3; dirty_ = true; }
            break;
        }
        case Phase::EnteringWorld:
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) setError(world_.error());
            else if (world_.state() == w3n::WorldClient::State::InWorld) {
                sessionT0_ = w3n::nowMs();
                if (logOn_ && log_.open("sdmc:/3ds/w3dproto/session.w3dl")) {
                    // LOGIN_VERIFY_WORLD is consumed by WorldClient: keep a copy for replays.
                    w3n::Writer w;
                    const float* p = world_.position();
                    w.u32(world_.mapId());
                    w.f32(p[0]); w.f32(p[1]); w.f32(p[2]); w.f32(world_.orientation());
                    log_.write(0, 0, 0x236, w.b);
                    game_.tap = [this](uint32_t ms, int dir, uint16_t op, const w3n::Bytes& d) { log_.write(ms, dir, op, d); };
                } else {
                    game_.tap = nullptr;
                }
                game_.spellNames.load("sdmc:/3ds/w3dproto/spells.txt");   // optional, from w3dconv
                // stage 5.12: reactions (hostile / neutral / friendly), optional, from w3dconv; loaded once (~200 KB)
                if (!game_.factions.loaded()) game_.factions.load("sdmc:/3ds/w3dproto/factions.txt");
                if (!game_.talents.loaded()) game_.talents.load("sdmc:/3ds/w3dproto/talents.txt");   // UI stage 4
                if (!game_.taxiTable.loaded()) game_.taxiTable.load("sdmc:/3ds/w3dproto/taxi.txt");  // UI stage 6
                if (!game_.locks.loaded()) game_.locks.load("sdmc:/3ds/w3dproto/locks.txt");         // herbs, ore, chests
                if (!game_.emotes.loaded()) game_.emotes.load("sdmc:/3ds/w3dproto/emotes.txt");      // emote animations
                game_.objects.setTransportsFile("sdmc:/3ds/w3dproto/transports.txt");                 // ships, read on first use
                game_.attach(world_, sessionMs());
                phase_ = Phase::InWorld;
                entered_ = true;
                printf("\x1b[2J");
                return false;
            }
            break;
        case Phase::InWorld:
            world_.update();   // keep-alive pings, drain server packets
            if (world_.state() == w3n::WorldClient::State::Failed) setError("disconnected: " + world_.error());
            return phase_ != Phase::InWorld;
        case Phase::Offline:
            return false;
        case Phase::Error:
            if (down & (KEY_A | KEY_B)) { phase_ = Phase::Menu; cursor_ = 3; dirty_ = true; }
            break;
    }
    if (dirty_) draw();
    return true;
}

void LoginFlow::draw() {
    dirty_ = false;
    printf("\x1b[2J\x1b[1;1H");
    printf("w3dproto  stage 5: login\n\n");
    switch (phase_) {
        case Phase::Menu: {
            const char* items[6] = {"Server", "Account", "Auth protocol", "Log in", "Play offline (fly around)",
                                    "Packet log"};
            std::string vals[6] = {server_, account_.empty() ? "(not set)" : account_, protocol_ == 3 ? "3 (stock 1.12.1)" : "8 (vMaNGOS)", "", "",
                                   logOn_ ? "on (session.w3dl)" : "off"};
            for (int i = 0; i < 6; ++i)
                printf("%s %-14s %s\n", i == cursor_ ? ">" : " ", items[i], vals[i].c_str());
            printf("\nUp/Down select, A edit/confirm\nThe password is asked every time\nand never saved.\n");
            break;
        }
        case Phase::Authenticating:
            printf("Logging in as %s ...\n", account_.c_str());
            break;
        case Phase::RealmSelect: {
            const w3n::Timings& t = auth_.timings();
            printf("Login OK: connect %.0f ms, SRP %.0f ms,\ntotal %.0f ms\n\nChoose a realm (A), B back:\n\n",
                   t.authConnect, t.srp, t.authTotal);
            for (size_t i = 0; i < auth_.realms().size(); ++i) {
                const w3n::Realm& r = auth_.realms()[i];
                printf("%s %-22.22s %s\n", (int)i == cursor_ ? ">" : " ", r.name.c_str(), (r.flags & 2) ? "offline" : "");
            }
            break;
        }
        case Phase::WorldConnecting:
            if (world_.state() == w3n::WorldClient::State::InQueue)
                printf("In the login queue: position %u\n", (unsigned)world_.queuePosition());
            else
                printf("Authenticating with the realm...\n");
            break;
        case Phase::CharSelect: {
            printf("Choose a character (A), B back:\n\n");
            if (world_.characters().empty()) printf("  (no characters on this realm)\n");
            for (size_t i = 0; i < world_.characters().size(); ++i) {
                const w3n::Character& c = world_.characters()[i];
                printf("%s %-12s %2u %s %s\n", (int)i == cursor_ ? ">" : " ", c.name.c_str(), c.level,
                       w3n::raceName(c.race), w3n::className(c.cls));
            }
            break;
        }
        case Phase::EnteringWorld:
            printf("Entering the world as %s ...\n", charName_.c_str());
            break;
        case Phase::Error:
            printf("Error:\n%s\n\nA: back\n", error_.c_str());
            break;
        default:
            break;
    }
}

void LoginFlow::shutdown() {
    if (world_.state() == w3n::WorldClient::State::InWorld) {
        game_.mover.stopAll(sessionMs());
        game_.update(sessionMs());
        game_.detach();
        world_.logout();
        u64 until = osGetTime() + 1000;   // give the request a moment to leave
        while (osGetTime() < until) { world_.update(); svcSleepThread(10 * 1000000LL); }
    }
    log_.close();
    world_.close();
    auth_.close();
    w3n::netShutdown();
}
