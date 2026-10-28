#include "login.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>

#include "ui_style.hpp"
#include "world.h"

static const char* kCfgPath = "sdmc:/3ds/w3dproto/login.cfg";
static const char* kGluePath = "sdmc:/3ds/w3dproto/glue.txt";

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
            else if (k == "realm") lastRealm_ = v;
            else if (k == "character") lastChar_ = v;
        }
        fclose(f);
    }
    texts_.load(kGluePath);   // optional: the game's own texts, clothes of new characters, zone names
    phase_ = Phase::Menu;
}

void LoginFlow::saveConfig() {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/w3dproto", 0777);
    FILE* f = fopen(kCfgPath, "w");
    if (!f) return;
    // The password is never stored.
    fprintf(f, "server=%s\naccount=%s\nprotocol=%d\nlog=%d\nrealm=%s\ncharacter=%s\n", server_.c_str(), account_.c_str(),
            protocol_, logOn_ ? 1 : 0, lastRealm_.c_str(), lastChar_.c_str());
    fclose(f);
}

bool LoginFlow::editText(const char* hint, std::string& value, bool password, int maxLen) {
    SwkbdState kb;
    char buf[128];
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, std::min<int>(maxLen, (int)sizeof(buf) - 1));
    swkbdSetHintText(&kb, hint);
    if (!password) swkbdSetInitialText(&kb, value.c_str());
    swkbdSetPasswordMode(&kb, password ? SWKBD_PASSWORD_HIDE_DELAY : SWKBD_PASSWORD_NONE);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    SwkbdButton b = swkbdInputText(&kb, buf, sizeof buf);
    ui_.touched();
    if (b != SWKBD_BUTTON_RIGHT) return false;
    value = buf;
    memset(buf, 0, sizeof buf);
    return true;
}

void LoginFlow::message(const std::string& text) {
    model_.message = text;
    wait_.clear();
    waitFixed_ = false;
    busy_ = Busy::None;
    ui_.touched();
}

// ----------------------------------------------------------------- what the screens ask for
void LoginFlow::act(GlueAction a) {
    using ui::tr;
    switch (a) {
        case GlueAction::None: break;
        case GlueAction::EditServer:
            if (editText(tr("Server: host or host:port (3724 by default)", "Сервер: адрес или адрес:порт (по умолчанию 3724)"), server_, false))
                saveConfig();
            break;
        case GlueAction::EditAccount:
            if (editText(tr("Account name", "Учётная запись"), account_, false, 32)) saveConfig();
            break;
        case GlueAction::EditPassword: {
            std::string p;
            if (editText(tr("Password", "Пароль"), p, true, 32)) { std::fill(password_.begin(), password_.end(), '\0'); password_ = p; }
            std::fill(p.begin(), p.end(), '\0');
            break;
        }
        case GlueAction::Login:
            if (account_.empty() && !editText(tr("Account name", "Учётная запись"), account_, false, 32)) break;
            saveConfig();
            if (password_.empty()) {
                std::string p;
                if (!editText(tr("Password", "Пароль"), p, true, 32)) break;
                password_ = p;
                std::fill(p.begin(), p.end(), '\0');
            }
            wait_ = tr("Connecting...", "Соединение...");
            waitFixed_ = false;
            pending_ = Pending::Auth;   // next frame: the dialog is on the screen before the blocking connect
            break;
        case GlueAction::Offline:
            phase_ = Phase::Offline;
            break;
        case GlueAction::Quit:
            quit_ = true;
            break;
        case GlueAction::ToggleProtocol:
            protocol_ = protocol_ == 3 ? 8 : 3;
            saveConfig();
            break;
        case GlueAction::ToggleLog:
            logOn_ = !logOn_;
            saveConfig();
            break;
        case GlueAction::DismissMessage:
            model_.message.clear();
            break;
        case GlueAction::CancelWait:
            pending_ = Pending::None;
            wait_.clear();
            if (phase_ == Phase::Authenticating || phase_ == Phase::Menu) { auth_.close(); phase_ = Phase::Menu; }
            else if (phase_ == Phase::WorldConnecting || phase_ == Phase::RealmSelect) { world_.close(); phase_ = Phase::RealmSelect; }
            break;
        case GlueAction::RealmAccept: {
            const auto& rs = auth_.realms();
            if (ui_.realm < 0 || ui_.realm >= (int)rs.size()) break;
            const w3n::Realm& r = rs[(size_t)ui_.realm];
            if (r.flags & 0x02) { message(tr("That realm is offline.", "Этот мир сейчас не в сети.")); break; }
            lastRealm_ = r.name;
            saveConfig();
            wait_ = std::string(tr("Connecting to ", "Соединение с ")) + r.name + "...";
            waitFixed_ = false;
            pending_ = Pending::World;
            break;
        }
        case GlueAction::RealmCancel:
            auth_.close();
            phase_ = Phase::Menu;
            break;
        case GlueAction::EnterWorld: {
            const auto& cs = world_.characters();
            if (ui_.character < 0 || ui_.character >= (int)cs.size() || busy_ != Busy::None) break;
            enterWorld();
            break;
        }
        case GlueAction::CreateNew: {
            const auto& cs = world_.characters();
            uint8_t race = 1, sex = 0;
            if (ui_.character >= 0 && ui_.character < (int)cs.size()) { race = cs[(size_t)ui_.character].race; sex = cs[(size_t)ui_.character].gender; }
            phase_ = Phase::CharCreate;
            model_.appearanceKnown = false;
            ui_.startCreate(race, sex, model_);
            lookPending_ = true;
            break;
        }
        case GlueAction::DeleteCharacter: {
            const auto& cs = world_.characters();
            if (ui_.character < 0 || ui_.character >= (int)cs.size()) break;
            world_.deleteCharacter(cs[(size_t)ui_.character].guid);
            busy_ = Busy::Delete;
            wait_ = tr("Deleting character...", "Удаление персонажа...");
            waitFixed_ = true;
            waitSince_ = w3n::nowMs();
            break;
        }
        case GlueAction::ChangeRealm:
            world_.close();
            phase_ = Phase::RealmSelect;
            break;
        case GlueAction::Back:
            world_.close();
            auth_.close();
            phase_ = Phase::Menu;
            break;
        case GlueAction::EditName: {
            std::string n = ui_.create.name;
            if (editText(tr("Name (2 to 12 letters)", "Имя (от 2 до 12 букв)"), n, false, 12)) {
                // the game's form: the first letter capital, the rest small (Latin)
                for (size_t i = 0; i < n.size(); ++i)
                    if ((unsigned char)n[i] < 128) n[i] = (char)(i ? tolower((unsigned char)n[i]) : toupper((unsigned char)n[i]));
                ui_.create.name = n;
            }
            break;
        }
        case GlueAction::CreateAccept: {
            if (ui_.create.name.empty()) { act(GlueAction::EditName); if (ui_.create.name.empty()) break; }
            w3n::CharCreateInfo ci;
            ci.name = ui_.create.name;
            ci.race = ui_.create.race;
            ci.cls = ui_.create.cls;
            ci.gender = ui_.create.sex;
            ci.skin = ui_.create.skin;
            ci.face = ui_.create.face;
            ci.hairStyle = ui_.create.hairStyle;
            ci.hairColor = ui_.create.hairColor;
            ci.facialHair = ui_.create.facial;
            world_.createCharacter(ci);
            busy_ = Busy::Create;
            wait_ = tr("Creating character...", "Создание персонажа...");
            waitFixed_ = true;
            waitSince_ = w3n::nowMs();
            break;
        }
        case GlueAction::CreateBack:
            phase_ = Phase::CharSelect;
            break;
    }
}

void LoginFlow::selectByName(const std::string& name) {
    const auto& cs = world_.characters();
    for (size_t i = 0; i < cs.size(); ++i)
        if (cs[i].name == name) { ui_.character = (int)i; ui_.modelYaw = 0; ui_.touched(); return; }
}

void LoginFlow::enterWorld() {
    const w3n::Character& c = world_.characters()[(size_t)ui_.character];
    charName_ = c.name;
    lastChar_ = c.name;
    saveConfig();
    world_.login(c.guid);
    phase_ = Phase::EnteringWorld;
    wait_ = ui::tr("Entering world...", "Вход в мир...");
    waitFixed_ = true;
}

// ----------------------------------------------------------------- the network
void LoginFlow::network() {
    using ui::tr;
    w3n::ClientVersion ver;
    ver.authProtocol = (uint8_t)protocol_;
    if (pending_ == Pending::Auth) {   // the "Connecting..." dialog was drawn last frame
        pending_ = Pending::None;
        bool ok = auth_.start(server_, account_, password_, ver);
        std::fill(password_.begin(), password_.end(), '\0');
        password_.clear();
        if (!ok) { message(auth_.error()); phase_ = Phase::Menu; return; }
        phase_ = Phase::Authenticating;
        wait_ = tr("Authenticating...", "Проверка учётной записи...");
    } else if (pending_ == Pending::World) {
        pending_ = Pending::None;
        const w3n::Realm& r = auth_.realms()[(size_t)ui_.realm];
        if (!world_.start(r.address, auth_.account(), auth_.sessionKey(), ver)) {
            message(world_.error());
            phase_ = Phase::RealmSelect;
            return;
        }
        phase_ = Phase::WorldConnecting;
        model_.realmName = r.name;
    }
    switch (phase_) {
        case Phase::Authenticating:
            auth_.update();
            if (auth_.state() == w3n::AuthClient::State::Failed) { message(auth_.error()); phase_ = Phase::Menu; }
            else if (auth_.state() == w3n::AuthClient::State::Done) {
                wait_.clear();
                phase_ = Phase::RealmSelect;
                ui_.realm = 0;
                const auto& rs = auth_.realms();
                int online = -1, onlineCount = 0;
                for (size_t i = 0; i < rs.size(); ++i) {
                    if (rs[i].name == lastRealm_) ui_.realm = (int)i;
                    if (!(rs[i].flags & 0x02)) { online = (int)i; ++onlineCount; }
                }
                // as the game does: straight to the realm played last (or the only one); the list otherwise
                bool known = false;
                for (const auto& r : rs) known |= r.name == lastRealm_ && !(r.flags & 0x02);
                if (!known && onlineCount == 1) { ui_.realm = online; known = true; }
                if (known) act(GlueAction::RealmAccept);
            }
            break;
        case Phase::WorldConnecting:
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) { message(world_.error()); phase_ = Phase::RealmSelect; }
            else if (world_.state() == w3n::WorldClient::State::InQueue) {
                char b[96];
                snprintf(b, sizeof b, "%s %u", tr("Position in queue:", "Место в очереди:"), (unsigned)world_.queuePosition());
                wait_ = b;
            } else if (world_.state() == w3n::WorldClient::State::CharSelect) {
                wait_.clear();
                phase_ = Phase::CharSelect;
                ui_.character = 0;
                selectByName(lastChar_);
            } else {
                wait_ = tr("Retrieving character list...", "Получение списка персонажей...");
            }
            break;
        case Phase::CharSelect:
        case Phase::CharCreate: {
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) {
                message(std::string(tr("Disconnected: ", "Соединение потеряно: ")) + world_.error());
                phase_ = Phase::Menu;
                break;
            }
            int r = world_.takeCreateResult();
            if (r >= 0 && busy_ == Busy::Create) {
                if (r == w3n::CHAR_CREATE_SUCCESS) {
                    selectAfterList_ = ui_.create.name;
                    busy_ = Busy::List;
                    wait_ = tr("Retrieving character list...", "Получение списка персонажей...");
                } else {
                    message(w3n::charCreateResultText((uint8_t)r));
                }
            }
            r = world_.takeDeleteResult();
            if (r >= 0 && busy_ == Busy::Delete) {
                if (r == w3n::CHAR_DELETE_SUCCESS) {
                    busy_ = Busy::List;
                    selectAfterList_.clear();
                    ui_.character = 0;
                } else {
                    message(w3n::charDeleteResultText((uint8_t)r));
                }
            }
            if (busy_ == Busy::List && world_.state() == w3n::WorldClient::State::CharSelect) {
                busy_ = Busy::None;
                wait_.clear();
                waitFixed_ = false;
                phase_ = Phase::CharSelect;
                if (!selectAfterList_.empty()) selectByName(selectAfterList_);
                selectAfterList_.clear();
                ui_.touched();
            }
            if ((busy_ == Busy::Create || busy_ == Busy::Delete) && w3n::nowMs() - waitSince_ > 15000)
                message(tr("The server did not answer.", "Сервер не ответил."));
            break;
        }
        case Phase::EnteringWorld:
            world_.update();
            if (world_.state() == w3n::WorldClient::State::Failed) {
                message(world_.error());
                phase_ = Phase::Menu;
            } else if (world_.state() == w3n::WorldClient::State::InWorld) {
                wait_.clear();
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
                if (!game_.areaTriggers.loaded()) game_.areaTriggers.load("sdmc:/3ds/w3dproto/areatriggers.txt");   // dungeon doors, the tram
                if (!game_.emotes.loaded()) game_.emotes.load("sdmc:/3ds/w3dproto/emotes.txt");      // emote animations
                game_.objects.setTransportsFile("sdmc:/3ds/w3dproto/transports.txt");                 // ships, read on first use
                game_.attach(world_, sessionMs());
                phase_ = Phase::InWorld;
                entered_ = true;
            }
            break;
        case Phase::InWorld:
            world_.update();   // keep-alive pings, drain server packets
            if (world_.state() == w3n::WorldClient::State::Failed) {
                game_.detach();
                log_.close();
                message(std::string(tr("Disconnected: ", "Соединение потеряно: ")) + world_.error());
                phase_ = Phase::Menu;
            }
            break;
        default:
            break;
    }
}

// ----------------------------------------------------------------- the screens
void LoginFlow::fillModel() {
    switch (phase_) {
        case Phase::Menu: case Phase::Authenticating: model_.screen = GlueScreen::Login; break;
        case Phase::RealmSelect: model_.screen = GlueScreen::RealmList; break;
        case Phase::WorldConnecting:
            model_.screen = world_.state() == w3n::WorldClient::State::CharSelect ? GlueScreen::CharSelect : GlueScreen::RealmList;
            break;
        case Phase::CharSelect: case Phase::EnteringWorld: model_.screen = GlueScreen::CharSelect; break;
        case Phase::CharCreate: model_.screen = GlueScreen::CharCreate; break;
        default: break;
    }
    model_.server = server_;
    model_.account = account_;
    model_.havePassword = !password_.empty();
    model_.protocol = protocol_;
    model_.packetLog = logOn_;
    model_.wait = wait_;
    model_.waitFixed = waitFixed_;
    model_.realms = &auth_.realms();
    model_.chars = &world_.characters();
    model_.texts = &texts_;
    if (phase_ == Phase::CharCreate) {   // the choices of the chosen race / sex, from its CHRS entry
        const uint8_t* blob = nullptr;
        uint32_t size = 0;
        const int st = streamerCharInfo(ui_.create.race, ui_.create.sex, &blob, &size);
        model_.appearanceKnown = st == 1;
        if (st == 1) {
            glue::appearanceFrom(blob, size, ui_.create.skin, ui_.create.hairStyle, model_.appearance);
            if (lookPending_) {   // as the game: a new character starts with a random look
                ui_.randomize(model_);
                glue::appearanceFrom(blob, size, ui_.create.skin, ui_.create.hairStyle, model_.appearance);
                lookPending_ = false;
            }
            ui_.clampLook(model_.appearance);
            glue::appearanceFrom(blob, size, ui_.create.skin, ui_.create.hairStyle, model_.appearance);   // after the clamp
        }
    }
}

bool LoginFlow::update(u32 down, u32 held, bool touching, int tx, int ty, float dt) {
    if (phase_ == Phase::Offline) return false;
    network();
    if (phase_ == Phase::InWorld) return false;
    fillModel();
    GlueInput in;
    in.down = down;
    in.held = held;
    in.touching = touching;
    in.touchDown = touching && !touchWas_;
    touchWas_ = touching;
    in.tx = tx;
    in.ty = ty;
    in.dt = dt;
    const GlueAction a = ui_.update(model_, in);
    if (a != GlueAction::None) {
        act(a);
        fillModel();
    }
    if (phase_ == Phase::Offline || phase_ == Phase::InWorld) return false;
    ui_.buildTop(top_, model_);
    ui_.buildBottom(bottom_, model_);
    return true;
}

LoginFlow::Scene LoginFlow::scene() const {
    Scene s;
    const auto& cs = world_.characters();
    if (phase_ == Phase::CharCreate) {
        const GlueCreate& c = ui_.create;
        s.id = c.race;
        s.character = true;
        s.look = W3dLook{c.race, c.sex, c.skin, c.face, c.hairStyle, c.hairColor, c.facial};
        uint32_t disp[19];
        uint8_t inv[19];
        if (texts_.outfit(c.race, c.cls, c.sex, disp, inv))
            for (int k = 0; k < W3D_EQ_SLOTS && k < 19; ++k) { s.equip[k] = disp[k]; s.equipInv[k] = inv[k]; }
        s.yaw = ui_.modelYaw;
    } else if ((phase_ == Phase::CharSelect || phase_ == Phase::EnteringWorld) && model_.screen == GlueScreen::CharSelect) {
        if (ui_.character >= 0 && ui_.character < (int)cs.size()) {
            const w3n::Character& c = cs[(size_t)ui_.character];
            s.id = c.race;
            s.character = true;
            s.look = W3dLook{c.race, c.gender, c.skin, c.face, c.hairStyle, c.hairColor, c.facialHair};
            for (int k = 0; k < W3D_EQ_SLOTS; ++k) { s.equip[k] = c.equipDisplay[k]; s.equipInv[k] = c.equipInv[k]; }
            s.yaw = ui_.modelYaw;
        } else {
            s.id = 1;   // an empty list: the human scene without anyone
        }
    }
    return s;
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
