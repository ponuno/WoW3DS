// client.hpp — vanilla 1.12.1 (build 5875) login: realmd auth + world session.
//
// Both clients are polled state machines: call update() every frame, read
// state()/error(). Connecting is the only blocking step (TCP connect).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "socket.hpp"

namespace w3n {

struct ClientVersion {
    uint8_t major = 1, minor = 12, patch = 1;
    uint16_t build = 5875;
    uint8_t authProtocol = 3;     // what the real 1.12.1 client sends; vMaNGOS also accepts 8
    std::string locale = "enUS";
};

struct Realm {
    uint32_t icon = 0;
    uint8_t flags = 0;            // 0x02 offline
    std::string name, address;
    float population = 0;
    uint8_t characters = 0, timezone = 0, id = 0;
};

struct Timings {                  // milliseconds, for the stage-4 handshake measurements
    double authConnect = 0, srp = 0, authTotal = 0, realmList = 0;
    double worldConnect = 0, worldAuth = 0, charEnum = 0, enterWorld = 0;
};

// ------------------------------------------------------------------ realmd
class AuthClient {
public:
    enum class State { Idle, WaitChallenge, WaitProof, WaitRealms, Done, Failed };

    bool start(const std::string& hostPort, const std::string& account, const std::string& password,
               const ClientVersion& ver = ClientVersion());
    void update();
    State state() const { return state_; }
    const std::string& error() const { return err_; }
    const std::vector<Realm>& realms() const { return realms_; }
    const Bytes& sessionKey() const { return K_; }
    const std::string& account() const { return account_; }
    const Timings& timings() const { return t_; }
    void close() { sock_.close(); }
    // Testing hook: fixed SRP ephemeral instead of random bytes.
    Bytes forcedA;

private:
    void fail(const std::string& e);
    bool handleChallenge();
    bool handleProof();
    bool handleRealms();
    TcpSocket sock_;
    State state_ = State::Idle;
    std::string err_, account_, password_;
    ClientVersion ver_;
    Bytes buf_, K_, M2_;
    std::vector<Realm> realms_;
    Timings t_;
    uint64_t t0_ = 0, tProof_ = 0, tRealms_ = 0, deadline_ = 0;
};

// ------------------------------------------------------------------ world
struct Character {
    uint64_t guid = 0;
    std::string name;
    uint8_t race = 0, cls = 0, gender = 0, level = 0;
    uint32_t zone = 0, map = 0;
    float x = 0, y = 0, z = 0;
    uint32_t guild = 0, flags = 0;
};

struct Packet {
    uint16_t opcode = 0;
    Bytes data;
};

class WorldClient {
public:
    enum class State { Idle, WaitChallenge, WaitAuthResponse, InQueue, WaitCharEnum, CharSelect, WaitLogin,
                       InWorld, LoggingOut, Done, Failed };

    bool start(const std::string& hostPort, const std::string& account, const Bytes& sessionKey,
               const ClientVersion& ver = ClientVersion());
    void update();
    void requestCharacters();
    void login(uint64_t guid);
    void logout();
    void send(uint32_t opcode, const Bytes& payload);

    State state() const { return state_; }
    const std::string& error() const { return err_; }
    const std::vector<Character>& characters() const { return chars_; }
    uint32_t queuePosition() const { return queuePos_; }
    // Position from SMSG_LOGIN_VERIFY_WORLD (server coordinates == render coordinates).
    uint32_t mapId() const { return map_; }
    uint64_t playerGuid() const { return playerGuid_; }
    const float* position() const { return pos_; }
    float orientation() const { return orient_; }
    uint32_t lastLatencyMs() const { return latency_; }
    const Timings& timings() const { return t_; }
    const std::map<uint16_t, uint32_t>& opcodeCounts() const { return opCounts_; }
    uint64_t bytesReceived() const { return bytesIn_; }
    // True once the server sent SMSG_WARDEN_DATA (anticheat). We do not answer
    // it, so such servers usually disconnect the client shortly afterwards.
    bool wardenSeen() const { return wardenSeen_; }
    // Called for every packet not consumed by the session itself.
    std::function<void(const Packet&)> onPacket;
    uint32_t pingIntervalMs = 30000;   // keep-alive period (the real client uses 30 s)
    void close() { sock_.close(); }

private:
    void fail(const std::string& e);
    void handle(Packet& p);
    TcpSocket sock_;
    State state_ = State::Idle;
    std::string err_, account_;
    ClientVersion ver_;
    Bytes K_, inBuf_;
    HeaderCipher cipher_;
    size_t headerDecrypted_ = 0;   // bytes of the current header already decrypted
    std::vector<Character> chars_;
    uint64_t playerGuid_ = 0;
    uint32_t queuePos_ = 0, map_ = 0, pingSeq_ = 0, latency_ = 0;
    float pos_[3] = {0, 0, 0}, orient_ = 0;
    uint64_t t0_ = 0, tStep_ = 0, lastPing_ = 0, pingSent_ = 0, deadline_ = 0;
    Timings t_;
    std::map<uint16_t, uint32_t> opCounts_;
    uint64_t bytesIn_ = 0;
    bool wardenSeen_ = false;
};

const char* raceName(uint8_t r);
const char* className(uint8_t c);
const char* authResultText(uint8_t code);      // realmd LOGON_CHALLENGE / PROOF codes
const char* worldAuthResultText(uint8_t code);  // SMSG_AUTH_RESPONSE codes

}  // namespace w3n
