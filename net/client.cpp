#include "client.hpp"

#include <cstring>

#include "wire.hpp"

namespace w3n {

namespace {

// Vanilla 1.12.1 opcodes (WoWee Data/expansions/classic/opcodes.json).
enum : uint16_t {
    CMSG_CHAR_ENUM = 0x037, SMSG_CHAR_ENUM = 0x03B, CMSG_PLAYER_LOGIN = 0x03D, SMSG_CHARACTER_LOGIN_FAILED = 0x041,
    CMSG_LOGOUT_REQUEST = 0x04B, SMSG_LOGOUT_RESPONSE = 0x04C, SMSG_LOGOUT_COMPLETE = 0x04D,
    CMSG_PING = 0x1DC, SMSG_PONG = 0x1DD, SMSG_AUTH_CHALLENGE = 0x1EC, CMSG_AUTH_SESSION = 0x1ED,
    SMSG_AUTH_RESPONSE = 0x1EE, SMSG_LOGIN_VERIFY_WORLD = 0x236, SMSG_WARDEN_DATA = 0x2E6,
};
const uint8_t AUTH_OK = 0x0C, AUTH_WAIT_QUEUE = 0x1B;

bool pump(TcpSocket& s, Bytes& buf) {
    uint8_t tmp[4096];
    for (;;) {
        int n = s.recvSome(tmp, sizeof tmp);
        if (n > 0) { buf.insert(buf.end(), tmp, tmp + n); continue; }
        return n == 0;   // false: connection closed
    }
}

const uint64_t kTimeoutMs = 15000;

}  // namespace

// ================================================================== realmd
void AuthClient::fail(const std::string& e) {
    err_ = e;
    state_ = State::Failed;
    sock_.close();
}

bool AuthClient::start(const std::string& hostPort, const std::string& account, const std::string& password,
                       const ClientVersion& ver) {
    sock_.close();
    state_ = State::Idle;
    err_.clear(); buf_.clear(); K_.clear(); M2_.clear(); realms_.clear();
    t_ = Timings{};
    ver_ = ver;
    account_ = upper(account);
    password_ = password;
    std::string host;
    uint16_t port;
    if (!splitHostPort(hostPort, 3724, host, port)) { fail("bad server address"); return false; }
    t0_ = nowMs();
    if (!sock_.connect(host, port, err_)) { state_ = State::Failed; return false; }
    t_.authConnect = (double)(nowMs() - t0_);

    // CMD_AUTH_LOGON_CHALLENGE
    Writer w;
    w.u8(0x00);
    w.u8(ver_.authProtocol);
    w.u16((uint16_t)(30 + account_.size()));
    w.fourcc("WoW");
    w.u8(ver_.major); w.u8(ver_.minor); w.u8(ver_.patch);
    w.u16(ver_.build);
    w.fourcc("x86");
    w.fourcc("Win");
    w.fourcc(ver_.locale);
    w.u32(0);            // timezone bias
    w.u32(0);            // client IP (servers don't rely on it)
    w.u8((uint8_t)account_.size());
    w.raw(account_.data(), account_.size());
    if (!sock_.sendAll(w.b.data(), w.b.size())) { fail("connection lost"); return false; }
    state_ = State::WaitChallenge;
    deadline_ = nowMs() + kTimeoutMs;
    return true;
}

void AuthClient::update() {
    if (state_ == State::Idle || state_ == State::Done || state_ == State::Failed) return;
    bool alive = pump(sock_, buf_);
    for (;;) {
        bool progressed = false;
        switch (state_) {
            case State::WaitChallenge: progressed = handleChallenge(); break;
            case State::WaitProof: progressed = handleProof(); break;
            case State::WaitRealms: progressed = handleRealms(); break;
            default: break;
        }
        if (!progressed || state_ == State::Failed || state_ == State::Done) break;
    }
    if (state_ == State::Failed || state_ == State::Done) return;
    if (!alive) fail("server closed the connection");
    else if (nowMs() > deadline_) fail("timed out waiting for the login server");
}

bool AuthClient::handleChallenge() {
    if (buf_.size() < 3) return false;
    if (buf_[0] != 0x00) { fail("unexpected reply to logon challenge"); return true; }
    uint8_t result = buf_[2];
    if (result != 0) { fail(std::string("login refused: ") + authResultText(result)); return true; }
    // cmd, unk, result, B[32], gLen, g, nLen, N, salt[32], crcSalt[16], securityFlags [+extra]
    if (buf_.size() < 36) return false;
    size_t gLen = buf_[35];
    if (buf_.size() < 37 + gLen) return false;
    size_t nLen = buf_[36 + gLen];
    size_t base = 37 + gLen + nLen + 32 + 16 + 1;
    if (buf_.size() < base) return false;
    uint8_t sec = buf_[base - 1];
    size_t extra = ((sec & 1) ? 20 : 0) + ((sec & 2) ? 12 : 0) + ((sec & 4) ? 1 : 0);
    if (buf_.size() < base + extra) return false;
    if (sec) { fail("the account needs a PIN/authenticator, not supported yet"); return true; }

    Reader r(buf_, 3);
    Bytes B = r.bytes(32);
    r.skip(1);
    Bytes g = r.bytes(gLen);
    r.skip(1);
    Bytes N = r.bytes(nLen);
    Bytes salt = r.bytes(32);
    buf_.erase(buf_.begin(), buf_.begin() + (long)(base + extra));

    uint64_t s0 = nowMs();
    Bytes a = forcedA;
    if (a.empty()) {
        a.resize(19);
        if (!randomBytes(a.data(), a.size())) { fail("no random source"); return true; }
    }
    SrpResult srp;
    bool ok = srpCompute(account_, password_, B, g, N, salt, a, srp);
    std::fill(password_.begin(), password_.end(), '\0');
    password_.clear();
    t_.srp = (double)(nowMs() - s0);
    if (!ok) { fail("invalid SRP parameters from server"); return true; }
    K_ = srp.K;
    M2_ = srp.M2;

    // CMD_AUTH_LOGON_PROOF: A, M1, crc_hash[20], number_of_keys, securityFlags
    Writer w;
    w.u8(0x01);
    w.raw(srp.A);
    w.raw(srp.M1);
    for (int i = 0; i < 20; ++i) w.u8(0);
    w.u8(0);
    w.u8(0);
    if (!sock_.sendAll(w.b.data(), w.b.size())) { fail("connection lost"); return true; }
    tProof_ = nowMs();
    state_ = State::WaitProof;
    deadline_ = nowMs() + kTimeoutMs;
    return true;
}

bool AuthClient::handleProof() {
    if (buf_.size() < 2) return false;
    if (buf_[0] != 0x01) { fail("unexpected reply to logon proof"); return true; }
    if (buf_[1] != 0) { fail(std::string("login refused: ") + authResultText(buf_[1])); return true; }
    // build < 6299: cmd, error, M2[20], surveyId(u32) = 26 bytes
    if (buf_.size() < 26) return false;
    Bytes M2(buf_.begin() + 2, buf_.begin() + 22);
    buf_.erase(buf_.begin(), buf_.begin() + 26);
    if (M2 != M2_) { fail("server proof mismatch (wrong server?)"); return true; }
    t_.authTotal = (double)(nowMs() - t0_);

    Writer w;   // CMD_REALM_LIST
    w.u8(0x10);
    w.u32(0);
    if (!sock_.sendAll(w.b.data(), w.b.size())) { fail("connection lost"); return true; }
    tRealms_ = nowMs();
    state_ = State::WaitRealms;
    deadline_ = nowMs() + kTimeoutMs;
    return true;
}

bool AuthClient::handleRealms() {
    if (buf_.size() < 3) return false;
    if (buf_[0] != 0x10) { fail("unexpected reply to realm list"); return true; }
    size_t size = buf_[1] | (buf_[2] << 8);
    if (buf_.size() < 3 + size) return false;
    Bytes body(buf_.begin() + 3, buf_.begin() + 3 + (long)size);
    buf_.erase(buf_.begin(), buf_.begin() + 3 + (long)size);
    Reader r(body);
    r.u32();
    // Vanilla layout: uint8 count; per realm uint32 icon, uint8 flags, name, address,
    // float population, uint8 characters, uint8 timezone, uint8 id
    uint8_t count = r.u8();
    realms_.clear();
    for (uint8_t i = 0; i < count && !r.bad; ++i) {
        Realm rl;
        rl.icon = r.u32();
        rl.flags = r.u8();
        rl.name = r.cstr();
        rl.address = r.cstr();
        rl.population = r.f32();
        rl.characters = r.u8();
        rl.timezone = r.u8();
        rl.id = r.u8();
        if (rl.flags & 0x04) r.skip(5);   // version info (build-specific realms)
        if (!r.bad) realms_.push_back(rl);
    }
    t_.realmList = (double)(nowMs() - tRealms_);
    sock_.close();   // realmd is done; the world server is a new connection
    state_ = realms_.empty() ? State::Failed : State::Done;
    if (realms_.empty()) err_ = "the server lists no realms";
    return true;
}

// ================================================================== world
void WorldClient::fail(const std::string& e) {
    err_ = wardenSeen_ ? e + " (the server uses Warden anticheat, which this client does not answer)" : e;
    state_ = State::Failed;
    sock_.close();
}

bool WorldClient::start(const std::string& hostPort, const std::string& account, const Bytes& sessionKey,
                        const ClientVersion& ver) {
    sock_.close();
    state_ = State::Idle;
    err_.clear(); inBuf_.clear(); chars_.clear(); opCounts_.clear();
    cipher_ = HeaderCipher{};
    headerDecrypted_ = 0;
    queuePos_ = map_ = pingSeq_ = latency_ = 0;
    lastPing_ = pingSent_ = 0;
    bytesIn_ = 0;
    wardenSeen_ = false;
    t_ = Timings{};
    ver_ = ver;
    account_ = upper(account);
    K_ = sessionKey;
    std::string host;
    uint16_t port;
    if (!splitHostPort(hostPort, 8085, host, port)) { fail("bad realm address"); return false; }
    t0_ = nowMs();
    if (!sock_.connect(host, port, err_)) { state_ = State::Failed; return false; }
    t_.worldConnect = (double)(nowMs() - t0_);
    state_ = State::WaitChallenge;
    deadline_ = nowMs() + kTimeoutMs;
    return true;
}

void WorldClient::send(uint32_t opcode, const Bytes& payload) {
    if (!sock_.isOpen()) return;
    Bytes pkt(6);
    uint16_t size = (uint16_t)(payload.size() + 4);
    pkt[0] = (uint8_t)(size >> 8);   // size is big-endian
    pkt[1] = (uint8_t)size;
    pkt[2] = (uint8_t)opcode; pkt[3] = (uint8_t)(opcode >> 8);
    pkt[4] = (uint8_t)(opcode >> 16); pkt[5] = (uint8_t)(opcode >> 24);
    cipher_.encrypt(pkt.data(), 6);
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    if (!sock_.sendAll(pkt.data(), pkt.size())) fail("connection lost");
}

void WorldClient::requestCharacters() {
    tStep_ = nowMs();
    send(CMSG_CHAR_ENUM, {});
    state_ = State::WaitCharEnum;
    deadline_ = nowMs() + kTimeoutMs;
}

void WorldClient::login(uint64_t guid) {
    Writer w;
    w.u64(guid);
    playerGuid_ = guid;
    tStep_ = nowMs();
    send(CMSG_PLAYER_LOGIN, w.b);
    state_ = State::WaitLogin;
    deadline_ = nowMs() + 30000;
}

void WorldClient::logout() {
    if (state_ != State::InWorld) { sock_.close(); state_ = State::Done; return; }
    send(CMSG_LOGOUT_REQUEST, {});
    state_ = State::LoggingOut;
    deadline_ = nowMs() + 25000;   // 20 s logout timer outside inns, plus slack
}

void WorldClient::update() {
    if (state_ == State::Idle || state_ == State::Done || state_ == State::Failed) return;
    bool alive = pump(sock_, inBuf_);
    // Frame packets: server header = uint16 size (BE, includes opcode) + uint16 opcode,
    // the 4 header bytes encrypted once the session is authenticated.
    for (;;) {
        if (inBuf_.size() < 4) break;
        if (headerDecrypted_ < 4) {
            cipher_.decrypt(inBuf_.data() + headerDecrypted_, 4 - headerDecrypted_);
            headerDecrypted_ = 4;
        }
        size_t size = ((size_t)inBuf_[0] << 8) | inBuf_[1];
        if (size < 2) { fail("corrupt packet header"); return; }
        if (inBuf_.size() < 2 + size) break;
        Packet p;
        p.opcode = (uint16_t)(inBuf_[2] | (inBuf_[3] << 8));
        p.data.assign(inBuf_.begin() + 4, inBuf_.begin() + 2 + (long)size);
        inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 2 + (long)size);
        headerDecrypted_ = 0;
        bytesIn_ += 2 + size;
        opCounts_[p.opcode]++;
        const State before = state_;
        handle(p);
        if (state_ == State::Failed || state_ == State::Done) return;
        // Entering the world: the server sends the first update objects right behind
        // LOGIN_VERIFY_WORLD, often in the same TCP read. Stop here so the caller can
        // attach the game layer (onPacket) before they are dispatched.
        if (before != State::InWorld && state_ == State::InWorld) return;
    }
    uint64_t now = nowMs();
    if (state_ == State::InWorld && now - lastPing_ > pingIntervalMs) {   // keep-alive, as the real client
        Writer w;
        w.u32(++pingSeq_);
        w.u32(latency_);
        send(CMSG_PING, w.b);
        lastPing_ = pingSent_ = now;
    }
    if (!alive) { fail(state_ == State::LoggingOut ? "logged out" : "server closed the connection"); return; }
    bool waiting = state_ == State::WaitChallenge || state_ == State::WaitAuthResponse ||
                   state_ == State::WaitCharEnum || state_ == State::WaitLogin || state_ == State::LoggingOut;
    if (waiting && now > deadline_) {
        if (state_ == State::LoggingOut) { sock_.close(); state_ = State::Done; }
        else fail("timed out waiting for the world server");
    }
}

void WorldClient::handle(Packet& p) {
    Reader r(p.data);
    switch (p.opcode) {
        case SMSG_AUTH_CHALLENGE: {
            if (state_ != State::WaitChallenge) break;
            uint32_t serverSeed = r.u32();
            uint32_t clientSeed;
            randomBytes((uint8_t*)&clientSeed, 4);
            // digest = SHA1(account | 0u32 | clientSeed | serverSeed | K)
            Writer hin;
            hin.raw(account_.data(), account_.size());
            hin.u32(0);
            hin.u32(clientSeed);
            hin.u32(serverSeed);
            hin.raw(K_);
            Bytes digest = sha1(hin.b);
            // Addon list as the stock 1.12 client sends it (CMaNGOS fingerprints it).
            static const char* addons[] = {"Blizzard_AuctionUI", "Blizzard_BattlefieldMinimap", "Blizzard_BindingUI",
                                           "Blizzard_CombatText", "Blizzard_CraftUI", "Blizzard_GMSurveyUI",
                                           "Blizzard_InspectUI", "Blizzard_MacroUI", "Blizzard_RaidUI",
                                           "Blizzard_TalentUI", "Blizzard_TradeSkillUI", "Blizzard_TrainerUI"};
            Writer ad;
            for (const char* a : addons) { ad.cstr(a); ad.u8(1); ad.u32(0x4C1C776D); ad.u32(0); }
            Writer w;
            w.u32(ver_.build);
            w.u32(0);   // server id
            w.cstr(account_);
            w.u32(clientSeed);
            w.raw(digest);
            w.u32((uint32_t)ad.b.size());
            w.raw(zlibStored(ad.b));
            send(CMSG_AUTH_SESSION, w.b);
            cipher_.init(K_);   // everything after AUTH_SESSION has encrypted headers
            tStep_ = nowMs();
            state_ = State::WaitAuthResponse;
            deadline_ = nowMs() + kTimeoutMs;
            return;
        }
        case SMSG_AUTH_RESPONSE: {
            uint8_t code = r.u8();
            if (code == AUTH_OK) {
                t_.worldAuth = (double)(nowMs() - tStep_);
                requestCharacters();
            } else if (code == AUTH_WAIT_QUEUE) {
                queuePos_ = r.u32();   // position in the login queue
                state_ = State::InQueue;
            } else {
                fail(std::string("world server refused: ") + worldAuthResultText(code));
            }
            return;
        }
        case SMSG_CHAR_ENUM: {
            uint8_t count = r.u8();
            chars_.clear();
            for (uint8_t i = 0; i < count && !r.bad; ++i) {
                Character c;
                c.guid = r.u64();
                c.name = r.cstr();
                c.race = r.u8(); c.cls = r.u8(); c.gender = r.u8();
                r.skip(4 + 1);   // skin/face/hair style/hair colour, facial hair
                c.level = r.u8();
                c.zone = r.u32(); c.map = r.u32();
                c.x = r.f32(); c.y = r.f32(); c.z = r.f32();
                c.guild = r.u32(); c.flags = r.u32();
                r.skip(1 + 12 + 20 * 5);   // first login, pet, 20 x (display id, inventory type)
                if (!r.bad) chars_.push_back(c);
            }
            t_.charEnum = (double)(nowMs() - tStep_);
            state_ = State::CharSelect;
            return;
        }
        case SMSG_CHARACTER_LOGIN_FAILED:
            fail("character login failed (code " + std::to_string(r.u8()) + ")");
            return;
        case SMSG_LOGIN_VERIFY_WORLD:
            map_ = r.u32();
            pos_[0] = r.f32(); pos_[1] = r.f32(); pos_[2] = r.f32();
            orient_ = r.f32();
            t_.enterWorld = (double)(nowMs() - tStep_);
            state_ = State::InWorld;
            lastPing_ = nowMs();
            return;
        case SMSG_WARDEN_DATA:
            wardenSeen_ = true;
            break;   // passed on to onPacket; deliberately not answered
        case SMSG_PONG:
            if (pingSent_) latency_ = (uint32_t)(nowMs() - pingSent_);
            return;
        case SMSG_LOGOUT_RESPONSE:
            if (r.u32() != 0) { state_ = State::InWorld; err_ = "logout refused (in combat?)"; }
            return;
        case SMSG_LOGOUT_COMPLETE:
            // Back at character select on the same connection; we simply close.
            sock_.close();
            state_ = State::Done;
            return;
        default:
            break;
    }
    if (onPacket) onPacket(p);
}

// ================================================================== text
const char* raceName(uint8_t r) {
    static const char* n[] = {"?", "Human", "Orc", "Dwarf", "Night Elf", "Undead", "Tauren", "Gnome", "Troll"};
    return r < 9 ? n[r] : "?";
}

const char* className(uint8_t c) {
    static const char* n[] = {"?", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "?", "Shaman", "Mage", "Warlock", "?", "Druid"};
    return c < 12 ? n[c] : "?";
}

const char* authResultText(uint8_t code) {
    switch (code) {
        case 0x03: return "account banned";
        case 0x04: return "unknown account";
        case 0x05: return "incorrect password";
        case 0x06: return "account already online";
        case 0x07: return "no game time";
        case 0x08: return "server busy";
        case 0x09: return "client version not accepted (build 5875)";
        case 0x0A: return "client needs an update";
        case 0x0C: return "account suspended";
        case 0x0D: return "access denied";
        default: return "login failed";
    }
}

const char* worldAuthResultText(uint8_t code) {
    switch (code) {
        case 0x0D: return "auth failed";
        case 0x0E: return "rejected";
        case 0x0F: return "bad server proof";
        case 0x10: return "realm unavailable";
        case 0x11: return "system error";
        case 0x14: return "version mismatch";
        case 0x15: return "unknown account";
        case 0x16: return "incorrect password";
        case 0x17: return "session expired";
        case 0x18: return "server shutting down";
        case 0x19: return "already logging in";
        case 0x1C: return "banned";
        case 0x1D: return "already online";
        case 0x1F: return "database busy";
        case 0x20: return "suspended";
        default: return "refused";
    }
}

}  // namespace w3n
