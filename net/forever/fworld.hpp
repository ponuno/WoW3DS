// fworld.hpp — the world connection of WoW Forever (Classic 1.60.1, builds 70291+) as the TrinityCore Forever fork serves it:
//   greeting lines ("WORLD OF WARCRAFT CONNECTION - SERVER TO CLIENT - V2\n" / "... CLIENT TO SERVER - V2\n");
//   every packet: uint32 size, 12-byte AES-GCM tag, then (encrypted once on) uint32 opcode + payload, size covering both;
//   SMSG_AUTH_CHALLENGE (32 DoS words, 32-byte challenge) -> CMSG_AUTH_SESSION (our 32-byte challenge, a 24-byte digest the
//   current servers do not check - no build key is known for 1.60.1 - and the join ticket);
//   keys: session key = 40 bytes from HMAC-SHA512(SHA512(client secret | join secret), server challenge | ours | seed);
//   encryption key = HMAC-SHA512(session key, ours | server challenge | seed)[0..32), AES-256-GCM, IV = packet counter (u64) +
//   "SRVR" / "CLNT" (the counters count every packet of the connection, the ones before encryption too);
//   SMSG_ENTER_ENCRYPTED_MODE (Ed25519-signed: not checked here) -> CMSG_ENTER_ENCRYPTED_MODE_ACK, both sides encrypt;
//   SMSG_AUTH_RESPONSE (uint32 result, 0 = OK) -> CMSG_ENUM_CHARACTERS -> SMSG_ENUM_CHARACTERS_RESULT;
//   SMSG_COMPRESSED_PACKET: uncompressed size, two Adler-32, one raw deflate stream for the whole connection (sync flush).
#pragma once
#include <cstdio>
#include <functional>
#include <string>

#include "fbase.hpp"
#include "fbnet.hpp"

namespace wf {

// Classic opcode numbers the probe uses (tools/forever/gen_opcodes.py prints them)
namespace op {
constexpr uint32_t SMSG_AUTH_CHALLENGE = 0x4E0000, SMSG_ENTER_ENCRYPTED_MODE = 0x4E0004, SMSG_PONG = 0x4E0009,
                   SMSG_COMPRESSED_PACKET = 0x4E000E, SMSG_MULTIPLE_PACKETS = 0x4E000D, SMSG_AUTH_RESPONSE = 0x470001,
                   SMSG_ENUM_CHARACTERS_RESULT = 0x470018, SMSG_CREATE_CHAR = 0x4701AC, SMSG_CHARACTER_LOGIN_FAILED = 0x4701B0,
                   SMSG_LOGIN_VERIFY_WORLD = 0x47002F, SMSG_SUSPEND_COMMS = 0x4E0005, SMSG_RESUME_COMMS = 0x4E0006, SMSG_CONNECT_TO = 0x4E0008;
constexpr uint32_t CMSG_AUTH_SESSION = 0x460001, CMSG_ENTER_ENCRYPTED_MODE_ACK = 0x460005, CMSG_PING = 0x460006,
                   CMSG_LOG_DISCONNECT = 0x460007, CMSG_ENUM_CHARACTERS = 0x440014, CMSG_KEEP_ALIVE = 0x4400AB,
                   CMSG_PLAYER_LOGIN = 0x440016, CMSG_CREATE_CHARACTER = 0x440070, CMSG_SUSPEND_COMMS_ACK = 0x460000,
                   CMSG_AUTH_CONTINUED_SESSION = 0x460003, CMSG_QUEUED_MESSAGES_END = 0x46000B;
}
const char* serverOpcodeName(uint32_t op);   // "SMSG_..." or nullptr
const char* clientOpcodeName(uint32_t op);

// The key derivation of the world connection (TrinityCore WorldSocket::HandleAuthSessionCallback), exposed for the tests.
struct WorldKeys {
    uint8_t sessionKey[40];
    uint8_t encryptKey[32];
    uint8_t digest[24];   // what we send: HMAC with an all-zero build key (the servers skip the check)
};
void deriveWorldKeys(const Bytes& clientSecret, const Bytes& joinSecret, const uint8_t serverChallenge[32], const uint8_t localChallenge[32],
                     WorldKeys& out, const uint8_t* buildKey16 = nullptr);

// The second ("instance") connection the server asks for with SMSG_CONNECT_TO after CMSG_PLAYER_LOGIN (WorldSocket::HandleAuthContinuedSession):
// digest = HMAC-SHA512(session key, connect key (u64) | our challenge | server challenge | seed)[0..24), encryption key as for the first one
// but from the new challenges. Only the session key from the first connection is needed - no build key.
void deriveInstanceKeys(const uint8_t sessionKey[40], uint64_t connectKey, const uint8_t serverChallenge[32], const uint8_t localChallenge[32],
                        uint8_t digest[24], uint8_t encryptKey[32]);

struct WorldPacket {
    bool fromServer = true;
    uint32_t opcode = 0;
    Bytes data;
    uint32_t ms = 0;            // since the connection opened
    bool compressed = false;    // came inside SMSG_COMPRESSED_PACKET
};

struct EnumCharacter { std::string name; uint32_t level = 0; std::string note; };

class WorldProbe {
public:
    WorldProbe();
    ~WorldProbe();
    std::function<void(const std::string&)> log;
    std::function<void(const WorldPacket&)> onPacket;   // every packet both ways (the packet log)
    Bytes buildKey;   // the client build's 16-byte auth key for the digest (empty: zeros - fine where the server skips the check)
    uint32_t closedAfterMs = 0;   // when the server closed after our AUTH_SESSION (0: it did not)
    // Connects, authenticates, enters encryption, asks for the characters; stays 'staySec' seconds logging what comes.
    bool run(const JoinInfo& join, int staySec, std::string& err);
    // 6.x probe step 2: with no character on the account, create one (createName non-empty); with 'enter', log in with it (the new one,
    // else the first of the list) and watch the world packets for 'staySec' seconds.
    std::string createName;
    uint8_t createRace = 1, createClass = 1, createSex = 0;   // human warrior, male
    bool enter = false;
    uint32_t createResult() const { return createResult_; }   // SMSG_CREATE_CHAR code (24 = success), 0xFFFFFFFF = none
    bool enteredWorld() const { return enteredWorld_; }
    uint32_t authResult() const { return authResult_; }
    const std::vector<EnumCharacter>& characters() const { return chars_; }
    bool gotCharacters() const { return gotChars_; }
    int packetsIn() const { return packetsIn_; }
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    uint32_t authResult_ = 0xFFFFFFFF;
    std::vector<EnumCharacter> chars_;
    bool gotChars_ = false;
    int packetsIn_ = 0;
    uint32_t createResult_ = 0xFFFFFFFF;
    bool enteredWorld_ = false;
    void say(const std::string& s) { if (log) log(s); }
};

// The probe's packet log (.fwl): "FWL1", then per packet u8 dir (0 server, 1 client), u32 opcode, u32 ms, u32 size, data.
class PacketLogFile {
public:
    bool open(const std::string& path);
    void write(const WorldPacket& p);
    ~PacketLogFile();
private:
    FILE* f_ = nullptr;
};

// What can be read of SMSG_ENUM_CHARACTERS_RESULT without the Classic layout: the counts in front, then names guessed from
// the printable runs (the probe's report says so).
// firstGuid (optional, 16 bytes): the GUID of the first character, which the list opens with (the retail layout of CharacterInfoBasic).
void parseEnumCharacters(const Bytes& data, std::vector<EnumCharacter>& out, std::string& summary, uint8_t* firstGuid = nullptr, uint32_t* count = nullptr);
// The 128-bit GUID of the Forever world protocol: u16 mask (bit i = byte i is present), then the present bytes; mask bit 15 = it is not empty.
bool readGuid(In& in, uint8_t out[16]);
void writeGuid(Out& o, const uint8_t g[16]);

}  // namespace wf
