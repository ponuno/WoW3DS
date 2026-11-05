// fworld.cpp — see fworld.hpp
#include "fworld.hpp"

#include <chrono>
#include <cstdio>

#include "fnet.hpp"
#include "miniz.h"

namespace wf {

namespace {
struct FOpcodeName { uint32_t op; const char* name; };
#include "fopcodes.inc"

const uint8_t kAuthCheckSeed[32] = {0xDE, 0x3A, 0x2A, 0x8E, 0x6B, 0x89, 0x52, 0x66, 0x88, 0x9D, 0x7E, 0x7A, 0x77, 0x1D, 0x5D, 0x1F,
                                    0x4E, 0xD9, 0x0C, 0x23, 0x9B, 0xCD, 0x0E, 0xDC, 0xD2, 0xE8, 0x04, 0x3A, 0x68, 0x64, 0xC7, 0xB0};
const uint8_t kSessionKeySeed[32] = {0xE8, 0x1E, 0x8B, 0x59, 0x27, 0x62, 0x1E, 0xAA, 0x86, 0x15, 0x18, 0xEA, 0xC0, 0xBF, 0x66, 0x8C,
                                     0x6D, 0xBF, 0x83, 0x93, 0xBC, 0xAA, 0x80, 0x52, 0x5B, 0x1E, 0xDC, 0x23, 0xA0, 0x12, 0xB7, 0x50};
const uint8_t kContinuedSessionSeed[32] = {0x56, 0x5C, 0x61, 0x9C, 0x48, 0x3A, 0x52, 0x1F, 0x61, 0x5D, 0x05, 0x49, 0xB2, 0x9A, 0x39, 0xBF,
                                           0x4B, 0x97, 0xB0, 0x1B, 0xF9, 0x6C, 0xDE, 0xD6, 0x80, 0x1D, 0xAB, 0x26, 0x02, 0xA9, 0x9B, 0x9D};
const uint8_t kEncryptionKeySeed[32] = {0x71, 0xC9, 0xED, 0x5A, 0xA7, 0x0E, 0x4D, 0xFF, 0x4C, 0x36, 0xA6, 0x5A, 0x3E, 0x46, 0x8A, 0x4A,
                                        0x5D, 0xA1, 0x48, 0xC8, 0x30, 0x47, 0x4A, 0xDE, 0xF6, 0x0D, 0x6C, 0xBE, 0x6F, 0xE4, 0x55, 0x73};
const char* kServerHello = "WORLD OF WARCRAFT CONNECTION - SERVER TO CLIENT - V2\n";
const char* kClientHello = "WORLD OF WARCRAFT CONNECTION - CLIENT TO SERVER - V2\n";
constexpr uint32_t kMagicServer = 0x52565253, kMagicClient = 0x544E4C43;

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void cat(Bytes& b, const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
}  // namespace

const char* serverOpcodeName(uint32_t o) {
    for (const FOpcodeName& e : kServerOpcodes) if (e.op == o) return e.name;
    return nullptr;
}
const char* clientOpcodeName(uint32_t o) {
    for (const FOpcodeName& e : kClientOpcodes) if (e.op == o) return e.name;
    return nullptr;
}

void deriveWorldKeys(const Bytes& clientSecret, const Bytes& joinSecret, const uint8_t serverChallenge[32], const uint8_t localChallenge[32],
                     WorldKeys& out, const uint8_t* buildKey16) {
    Bytes keyData = clientSecret;
    keyData.insert(keyData.end(), joinSecret.begin(), joinSecret.end());
    // the digest the server would check with the build's auth key (unknown for 1.60.1: an all-zero key)
    {
        Bytes dk = keyData;
        dk.resize(dk.size() + 16, 0);
        if (buildKey16) memcpy(dk.data() + keyData.size(), buildKey16, 16);
        uint8_t digestKey[64], h[64];
        crypto::sha512(dk.data(), dk.size(), digestKey);
        Bytes m;
        cat(m, localChallenge, 32);
        cat(m, serverChallenge, 32);
        cat(m, kAuthCheckSeed, 32);
        crypto::hmacSha512(digestKey, 64, m, h);
        memcpy(out.digest, h, 24);
    }
    uint8_t kd[64], skh[64];
    crypto::sha512(keyData.data(), keyData.size(), kd);
    Bytes m;
    cat(m, serverChallenge, 32);
    cat(m, localChallenge, 32);
    cat(m, kSessionKeySeed, 32);
    crypto::hmacSha512(kd, 64, m, skh);
    // SessionKeyGenerator<SHA512>: o1 = H(first half), o2 = H(second half), o0 = H(o1 | o0 | o2) with o0 = 0 first
    uint8_t o0[64] = {}, o1[64], o2[64];
    crypto::sha512(skh, 32, o1);
    crypto::sha512(skh + 32, 32, o2);
    size_t used = 64;
    for (int i = 0; i < 40; ++i) {
        if (used == 64) {
            Bytes in;
            cat(in, o1, 64);
            cat(in, o0, 64);
            cat(in, o2, 64);
            crypto::sha512(in.data(), in.size(), o0);
            used = 0;
        }
        out.sessionKey[i] = o0[used++];
    }
    Bytes e;
    cat(e, localChallenge, 32);
    cat(e, serverChallenge, 32);
    cat(e, kEncryptionKeySeed, 32);
    uint8_t ek[64];
    crypto::hmacSha512(out.sessionKey, 40, e, ek);
    memcpy(out.encryptKey, ek, 32);
}

void deriveInstanceKeys(const uint8_t sessionKey[40], uint64_t connectKey, const uint8_t serverChallenge[32], const uint8_t localChallenge[32],
                        uint8_t digest[24], uint8_t encryptKey[32]) {
    Bytes m;
    for (int i = 0; i < 8; ++i) m.push_back((uint8_t)(connectKey >> (8 * i)));
    cat(m, localChallenge, 32);
    cat(m, serverChallenge, 32);
    cat(m, kContinuedSessionSeed, 32);
    uint8_t h[64];
    crypto::hmacSha512(sessionKey, 40, m, h);
    memcpy(digest, h, 24);
    Bytes e;
    cat(e, localChallenge, 32);
    cat(e, serverChallenge, 32);
    cat(e, kEncryptionKeySeed, 32);
    crypto::hmacSha512(sessionKey, 40, e, h);
    memcpy(encryptKey, h, 32);
}

bool readGuid(In& in, uint8_t out[16]) {
    memset(out, 0, 16);
    in.resetBits();
    const uint32_t lo = in.u8(), hi = in.u8();
    const uint32_t mask = lo | (hi << 8);
    for (int i = 0; i < 16; ++i)
        if (mask & (1u << i)) out[i] = in.u8();
    return !in.bad && (mask & 0x8000);
}
void writeGuid(Out& o, const uint8_t g[16]) {
    uint32_t mask = 0;
    for (int i = 0; i < 16; ++i) if (g[i]) mask |= 1u << i;
    o.u8((uint8_t)mask);
    o.u8((uint8_t)(mask >> 8));
    for (int i = 0; i < 16; ++i) if (g[i]) o.u8(g[i]);
}

void parseEnumCharacters(const Bytes& d, std::vector<EnumCharacter>& out, std::string& summary, uint8_t* firstGuid, uint32_t* count) {
    In in(d);
    const uint32_t flags = in.bits(8);
    const bool hasMask = in.bit();
    const bool forceSort = in.bit();
    in.resetBits();
    const uint32_t nChars = in.u32(), nRegion = in.u32();
    const int32_t maxLevel = (int32_t)in.u32();
    const uint32_t nRaces = in.u32(), nCond = in.u32(), nLimit = in.u32(), nWarband = in.u32();
    uint32_t mask = hasMask ? in.u32() : 0;
    char b[256];
    snprintf(b, sizeof b, "flags 0x%02x (success %d)%s%s, %u characters, %u region-wide, max level %d, %u race unlocks, %u appearances, %u race limits, %u warband groups",
             flags, (flags >> 7) & 1, hasMask ? (", class mask 0x" + std::to_string(mask)).c_str() : "", forceSort ? ", sorted" : "", nChars, nRegion,
             maxLevel, nRaces, nCond, nLimit, nWarband);
    summary = b;
    if (in.bad || nChars > 64) { summary += " (layout differs from the retail one)"; return; }
    if (count) *count = nChars;
    if (nChars && firstGuid) {   // the list of characters opens with the first one's GUID
        In g(d.data() + in.pos, d.size() - in.pos);
        if (!readGuid(g, firstGuid)) memset(firstGuid, 0, 16);
    }
    // names: the printable runs after the counts (a guess until the Classic CharacterInfo layout is read)
    size_t i = in.pos;
    while (i < d.size() && out.size() < nChars) {
        size_t j = i;
        while (j < d.size() && ((d[j] >= 'A' && d[j] <= 'Z') || (d[j] >= 'a' && d[j] <= 'z') || d[j] >= 0x80)) ++j;
        if (j - i >= 2 && j - i <= 24 && d[i] >= 'A' && d[i] <= 'Z') {
            EnumCharacter c;
            c.name.assign((const char*)d.data() + i, j - i);
            c.note = "guessed from the bytes at " + std::to_string(i);
            out.push_back(c);
        }
        i = j > i ? j : i + 1;
    }
}

// ---------------------------------------------------------------- the connection
struct WorldProbe::Impl {
    Conn c;
    bool enc = false;
    uint8_t key[32] = {};
    uint64_t recvCounter = 0, sendCounter = 0, t0 = 0;
    mz_stream z;
    bool zInit = false;
    std::function<void(const WorldPacket&)> onPacket;
    Impl() { memset(&z, 0, sizeof z); }
    ~Impl() { if (zInit) mz_inflateEnd(&z); }

    bool send(uint32_t opcode, const Bytes& payload, std::string& err) {
        Bytes data;
        for (int i = 0; i < 4; ++i) data.push_back((uint8_t)(opcode >> (8 * i)));
        data.insert(data.end(), payload.begin(), payload.end());
        uint8_t tag[12] = {};
        if (enc) {
            uint8_t iv[12];
            memcpy(iv, &sendCounter, 8);
            memcpy(iv + 8, &kMagicClient, 4);
            if (!crypto::gcmEncrypt(key, iv, data.data(), data.size(), tag)) { err = "encrypt failed"; return false; }
        }
        ++sendCounter;
        Bytes frame;
        const uint32_t size = (uint32_t)data.size();
        for (int i = 0; i < 4; ++i) frame.push_back((uint8_t)(size >> (8 * i)));
        frame.insert(frame.end(), tag, tag + 12);
        frame.insert(frame.end(), data.begin(), data.end());
        if (onPacket) {
            WorldPacket p;
            p.fromServer = false;
            p.opcode = opcode;
            p.data = payload;
            p.ms = (uint32_t)(nowMs() - t0);
            onPacket(p);
        }
        return c.writeAll(frame.data(), frame.size(), err);
    }
    // One packet from the server (a compressed one unpacked); false on error / timeout ('timedOut' set)
    bool recv(WorldPacket& p, int timeoutMs, bool& timedOut, std::string& err) {
        timedOut = false;
        uint8_t hdr[16];
        if (!c.readExact(hdr, 16, timeoutMs, err)) { timedOut = err == "timeout"; return false; }
        uint32_t size;
        memcpy(&size, hdr, 4);
        if (size < 4 || size > (16u << 20)) { err = "bad packet size " + std::to_string(size); return false; }
        Bytes data(size);
        if (!c.readExact(data.data(), size, 30000, err)) return false;
        bool tagZero = true;
        for (int i = 4; i < 16; ++i) tagZero = tagZero && hdr[i] == 0;
        if (enc) {
            uint8_t iv[12];
            memcpy(iv, &recvCounter, 8);
            memcpy(iv + 8, &kMagicServer, 4);
            Bytes copy = data;
            if (!crypto::gcmDecrypt(key, iv, data.data(), data.size(), hdr + 4)) {
                if (!tagZero) { err = "decrypt failed at server packet " + std::to_string(recvCounter); return false; }
                data = copy;   // (sent before the server switched encryption on)
            }
        }
        ++recvCounter;
        p = WorldPacket();
        memcpy(&p.opcode, data.data(), 4);
        p.data.assign(data.begin() + 4, data.end());
        p.ms = (uint32_t)(nowMs() - t0);
        if (p.opcode == op::SMSG_COMPRESSED_PACKET) {
            if (p.data.size() < 12) { err = "short compressed packet"; return false; }
            uint32_t usize;
            memcpy(&usize, p.data.data(), 4);
            if (usize < 4 || usize > (16u << 20)) { err = "bad uncompressed size"; return false; }
            if (!zInit) { if (mz_inflateInit2(&z, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) { err = "inflate init"; return false; } zInit = true; }
            Bytes out(usize);
            z.next_in = p.data.data() + 12;
            z.avail_in = (unsigned)(p.data.size() - 12);
            z.next_out = out.data();
            z.avail_out = usize;
            const int rc = mz_inflate(&z, MZ_SYNC_FLUSH);
            if ((rc != MZ_OK && rc != MZ_STREAM_END && rc != MZ_BUF_ERROR) || z.avail_out != 0) {
                err = "inflate of a compressed packet failed (rc " + std::to_string(rc) + ", " + std::to_string(usize - z.avail_out) + " of " +
                      std::to_string(usize) + " bytes)";
                return false;
            }
            memcpy(&p.opcode, out.data(), 4);
            p.data.assign(out.begin() + 4, out.end());
            p.compressed = true;
        }
        if (onPacket) onPacket(p);
        return true;
    }
};

WorldProbe::WorldProbe() : d_(new Impl) {}
WorldProbe::~WorldProbe() = default;

bool WorldProbe::run(const JoinInfo& join, int staySec, std::string& err) {
    Impl& w = *d_;
    w.onPacket = onPacket;
    const std::string host = join.addresses[0].first;
    const int port = join.addresses[0].second;
    say("world: TCP to " + host + ":" + std::to_string(port));
    if (!w.c.open(host, port, false, err)) return false;
    w.t0 = nowMs();
    std::string line;
    if (!w.c.readLine(line, 15000, 200, err)) { err = "world greeting: " + err; return false; }
    if (line != kServerHello) { err = "unexpected world greeting: " + line; return false; }
    if (!w.c.writeAll(kClientHello, strlen(kClientHello), err)) return false;
    say("world: greeting exchanged");
    WorldKeys keys{};
    uint8_t local[32];
    const uint64_t until = nowMs() + 30000 + (uint64_t)staySec * 1000;
    uint64_t lastPing = nowMs(), stayFrom = 0;
    uint32_t pingSerial = 0;
    uint64_t sessionSentAt = 0;
    uint8_t guid[16] = {};
    bool haveGuid = false, enumSeen = false, creating = false, loginSent = false, waitRelist = false;
    std::unique_ptr<Impl> inst;   // the instance connection (after SMSG_CONNECT_TO)
    uint8_t instLocal[32] = {}, instEnc[32] = {}, instDigest[24] = {};
    uint64_t instKey = 0;
    uint32_t instNative = 0, instKey3 = 0;
    uint64_t loginAt = 0;
    int worldPackets = 0;
    auto enterWith = [&](std::string& e) -> bool {
        Out o;
        writeGuid(o, guid);
        o.u32(0x43480000u);   // FarClip 200.0
        o.bit(false);         // RPE
        o.flushBits();
        say("world: CMSG_PLAYER_LOGIN");
        loginSent = true;
        loginAt = nowMs();
        stayFrom = loginAt;   // the watch starts now
        return w.send(op::CMSG_PLAYER_LOGIN, o.b, e);
    };
    for (;;) {
        const uint64_t now = nowMs();
        if (stayFrom && now - stayFrom >= (uint64_t)staySec * 1000) break;
        if (now >= until + (uint64_t)staySec * 1000) { if (!gotChars_) err = "no character list within the time"; break; }
        if (now >= until && !stayFrom) { if (!gotChars_) err = "no character list within the time"; break; }
        if (w.enc && now - lastPing > 10000) {   // keep the connection alive while we watch
            Out o;
            o.u32(++pingSerial);
            o.u32(0);
            if (!w.send(op::CMSG_PING, o.b, err)) return false;
            lastPing = now;
        }
        if (inst) {   // the instance connection: its packets (the world itself arrives mostly here)
            WorldPacket q;
            bool qt = false;
            std::string ierr;
            if (inst->recv(q, 30, qt, ierr)) {
                ++packetsIn_;
                if (loginSent) ++worldPackets;
                if (q.opcode == op::SMSG_AUTH_CHALLENGE) {
                    if (q.data.size() < 65) { err = "short AUTH_CHALLENGE on the instance connection"; return false; }
                    crypto::random(instLocal, 32);
                    deriveInstanceKeys(keys.sessionKey, instKey, q.data.data() + 32, instLocal, instDigest, instEnc);
                    Out o;
                    o.u64(0);
                    o.raw(instLocal, 32);
                    o.raw(instDigest, 24);
                    o.u64(instKey);
                    o.u32(instNative);
                    o.u32(instKey3);
                    say("world: instance connection: AUTH_CHALLENGE -> AUTH_CONTINUED_SESSION");
                    if (!inst->send(op::CMSG_AUTH_CONTINUED_SESSION, o.b, err)) return false;
                } else if (q.opcode == op::SMSG_ENTER_ENCRYPTED_MODE) {
                    In in(q.data);
                    in.u32();
                    uint8_t sig[64];
                    in.raw(sig, 64);
                    const bool on = in.bit();
                    say(std::string("world: instance connection: ENTER_ENCRYPTED_MODE (") + (on ? "on" : "off") + ")");
                    if (!inst->send(op::CMSG_ENTER_ENCRYPTED_MODE_ACK, {}, err)) return false;
                    if (on) { memcpy(inst->key, instEnc, 32); inst->enc = true; }
                } else if (q.opcode == op::SMSG_RESUME_COMMS) {
                    Out o;
                    o.u32((uint32_t)(nowMs() - w.t0));
                    say("world: instance connection: RESUME_COMMS -> QUEUED_MESSAGES_END");
                    if (!inst->send(op::CMSG_QUEUED_MESSAGES_END, o.b, err)) return false;
                } else if (q.opcode == op::SMSG_AUTH_RESPONSE) {
                    In in(q.data);
                    say("world: instance connection: AUTH_RESPONSE result " + std::to_string(in.u32()) + " (an error: the continued session was refused)");
                } else if (q.opcode == op::SMSG_LOGIN_VERIFY_WORLD) {
                    In in(q.data);
                    const int32_t map = (int32_t)in.u32();
                    float pos[4];
                    for (float& f : pos) { uint32_t u = in.u32(); memcpy(&f, &u, 4); }
                    if (!in.bad) {
                        enteredWorld_ = true;
                        char b[160];
                        snprintf(b, sizeof b, "world: SMSG_LOGIN_VERIFY_WORLD: map %d at (%.1f, %.1f, %.1f), facing %.2f", map, pos[0], pos[1], pos[2], pos[3]);
                        say(b);
                    }
                } else if (q.opcode == op::SMSG_CHARACTER_LOGIN_FAILED) {
                    say("world: SMSG_CHARACTER_LOGIN_FAILED, reason " + std::to_string(q.data.empty() ? -1 : (int)q.data[0]));
                }
            } else if (!qt) { say("world: instance connection ended: " + ierr); inst.reset(); }
        }
        WorldPacket p;
        bool timedOut = false;
        if (!w.recv(p, inst ? 30 : 1000, timedOut, err)) {
            if (timedOut) continue;
            if (stayFrom) { say("world: connection ended: " + err); err.clear(); break; }
            if (sessionSentAt && authResult_ == 0xFFFFFFFF) {
                closedAfterMs = (uint32_t)(nowMs() - sessionSentAt);
                err += " " + std::to_string(closedAfterMs) + " ms after AUTH_SESSION, without an AUTH_RESPONSE (the session was refused: most likely the digest - the server checks it with the client build's auth key)";
            }
            return false;
        }
        ++packetsIn_;
        if (p.opcode == op::SMSG_AUTH_CHALLENGE) {
            if (p.data.size() < 65) { err = "short AUTH_CHALLENGE (" + std::to_string(p.data.size()) + " bytes)"; return false; }
            const uint8_t* serverChallenge = p.data.data() + 32;
            crypto::random(local, 32);
            deriveWorldKeys(join.clientSecret, join.joinSecret, serverChallenge, local, keys, buildKey.size() == 16 ? buildKey.data() : nullptr);
            sessionSentAt = nowMs();
            Out o;
            o.u64(0);                                    // DosResponse (not checked)
            o.u32(join.realmAddress >> 24);              // RegionID
            o.u32((join.realmAddress >> 16) & 0xFF);     // BattlegroupID
            o.u32(join.realmAddress & 0xFFFF);           // RealmID
            o.raw(local, 32);
            o.raw(keys.digest, 24);
            o.bit(false);                                // UseIPv6
            o.flushBits();
            o.u32((uint32_t)join.joinTicket.size());
            o.raw(join.joinTicket);
            say("world: AUTH_CHALLENGE -> AUTH_SESSION (realm " + std::to_string(join.realmAddress >> 24) + "-" +
                std::to_string((join.realmAddress >> 16) & 0xFF) + "-" + std::to_string(join.realmAddress & 0xFFFF) + ")");
            if (!w.send(op::CMSG_AUTH_SESSION, o.b, err)) return false;
        } else if (p.opcode == op::SMSG_ENTER_ENCRYPTED_MODE) {
            In in(p.data);
            const int32_t regionGroup = (int32_t)in.u32();
            uint8_t sig[64];
            in.raw(sig, 64);
            const bool on = in.bit();
            say("world: ENTER_ENCRYPTED_MODE (region group " + std::to_string(regionGroup) + ", " + (on ? "on" : "off") +
                ", Ed25519 signature not checked)");
            if (!w.send(op::CMSG_ENTER_ENCRYPTED_MODE_ACK, {}, err)) return false;
            if (on) { memcpy(w.key, keys.encryptKey, 32); w.enc = true; }
        } else if (p.opcode == op::SMSG_AUTH_RESPONSE) {
            In in(p.data);
            authResult_ = in.u32();
            say("world: AUTH_RESPONSE result " + std::to_string(authResult_) + (authResult_ == 0 ? " (OK)" : " (refused)") +
                ", " + std::to_string(p.data.size()) + " bytes");
            if (authResult_ != 0) { err = "the world server refused the session (result " + std::to_string(authResult_) + ")"; return false; }
            if (!w.send(op::CMSG_ENUM_CHARACTERS, {}, err)) return false;
        } else if (p.opcode == op::SMSG_ENUM_CHARACTERS_RESULT) {
            std::string summary;
            chars_.clear();
            uint32_t nChars = 0;
            bool guidOk = false;
            uint8_t g0[16] = {};
            parseEnumCharacters(p.data, chars_, summary, g0, &nChars);
            for (int i = 0; i < 16; ++i) if (g0[i]) guidOk = true;
            if (guidOk && !haveGuid) memcpy(guid, g0, 16);
            gotChars_ = true;
            say("world: ENUM_CHARACTERS_RESULT, " + std::to_string(p.data.size()) + " bytes: " + summary);
            if (!enumSeen) {
                enumSeen = true;
                if (nChars == 0 && !createName.empty()) {
                    // CMSG_CREATE_CHARACTER (Classic 1.60 layout, TrinityCore forever branch CreateCharacter::Read): bits - name length (6),
                    // template set (1), trial boost (1), NPE (1), hardcore self-found (1), 2 unknown, surname length (6); then race, class, sex,
                    // customization count (u32, 0: the server keeps the defaults), an int32 (-1), the timerunning season, the name
                    Out o;
                    for (int i = 5; i >= 0; --i) o.bit((createName.size() >> i) & 1);
                    for (int i = 0; i < 6; ++i) o.bit(false);
                    for (int i = 0; i < 6; ++i) o.bit(false);   // surname: none
                    o.flushBits();
                    o.u8(createRace); o.u8(createClass); o.u8(createSex);
                    o.u32(0);
                    o.u32(0xFFFFFFFFu);
                    o.u32(0);
                    o.raw(createName.data(), createName.size());
                    say("world: CMSG_CREATE_CHARACTER " + createName + " (race " + std::to_string(createRace) + ", class " + std::to_string(createClass) +
                        ", sex " + std::to_string(createSex) + ")");
                    creating = true;
                    if (!w.send(op::CMSG_CREATE_CHARACTER, o.b, err)) return false;
                } else {
                    if (nChars && enter) {
                        haveGuid = guidOk;
                        if (!haveGuid) say("world: the first character's GUID could not be read from the list");
                    }
                    if (haveGuid && enter && !loginSent) { if (!enterWith(err)) return false; }
                    else if (!stayFrom) stayFrom = nowMs();
                }
            } else if (waitRelist && !loginSent) {   // the list after the creation: the server only lets a character in that it has listed
                waitRelist = false;
                if (haveGuid && enter) { if (!enterWith(err)) return false; }
            }
        } else if (p.opcode == op::SMSG_CREATE_CHAR) {
            In in(p.data);
            createResult_ = in.u32();
            uint8_t g[16];
            const bool gok = readGuid(in, g);
            say("world: SMSG_CREATE_CHAR code " + std::to_string(createResult_) + (createResult_ == 24 ? " (success)" : " (refused: see CHAR_CREATE_* / CHAR_NAME_* in SharedDefines.h)"));
            creating = false;
            if (createResult_ == 24 && gok) {
                memcpy(guid, g, 16);
                haveGuid = true;
                if (enter) {   // (a freshly created character is not "legit" for the session until a character list was sent after it)
                    waitRelist = true;
                    say("world: asking for the character list again (the server lets in only listed characters)");
                    if (!w.send(op::CMSG_ENUM_CHARACTERS, {}, err)) return false;
                } else if (!stayFrom) stayFrom = nowMs();
            } else if (!stayFrom) stayFrom = nowMs();
        } else if (p.opcode == op::SMSG_SUSPEND_COMMS) {
            In in(p.data);
            const uint32_t serial = in.u32();
            Out o;
            o.u32(serial);
            if (!w.send(op::CMSG_SUSPEND_COMMS_ACK, o.b, err)) return false;
        } else if (p.opcode == op::SMSG_CONNECT_TO) {
            // u32 payload count, u32 serial, u8 connection, u64 key, u32 native realm address, u32 key3; payload: u8 address type (1 IPv4, 2 IPv6),
            // the address, u16 port, then a token we ignore
            In in(p.data);
            const uint32_t n = in.u32(), serial = in.u32();
            const uint8_t con = in.u8();
            instKey = in.u64();
            instNative = in.u32();
            instKey3 = in.u32();
            std::string addr;
            int port2 = 0;
            if (n >= 1) {
                const uint8_t type = in.u8();
                if (type == 1) {
                    char b[32];
                    const uint8_t a0 = in.u8(), a1 = in.u8(), a2 = in.u8(), a3 = in.u8();
                    snprintf(b, sizeof b, "%u.%u.%u.%u", a0, a1, a2, a3);
                    addr = b;
                } else if (type == 2) {
                    uint8_t a[16];
                    in.raw(a, 16);
                    char b[64];
                    snprintf(b, sizeof b, "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
                    addr = b;
                }
                { const int lo = in.u8(); port2 = lo | (in.u8() << 8); }
            }
            if (in.bad || addr.empty()) { say("world: SMSG_CONNECT_TO could not be read (" + std::to_string(p.data.size()) + " bytes)"); }
            else {
                // an address the server reports for itself may be unusable from here (0.0.0.0 / a Docker name): the one we dialled is the safer
                const std::string useHost = (addr == "0.0.0.0" || addr.rfind("172.", 0) == 0) ? host : addr;
                say("world: SMSG_CONNECT_TO (serial " + std::to_string(serial) + ", connection " + std::to_string(con) + ") -> " + addr + ":" + std::to_string(port2) +
                    (useHost != addr ? ", dialling " + useHost + " instead" : ""));
                inst.reset(new Impl);
                inst->onPacket = onPacket;
                inst->t0 = nowMs();
                std::string ierr2, l2;
                bool opened = inst->c.open(useHost, port2, false, ierr2);
                if (!opened && useHost != host) { say("world: " + useHost + " is not reachable (" + ierr2 + "), trying " + host); ierr2.clear(); opened = inst->c.open(host, port2, false, ierr2); }
                if (!opened || !inst->c.readLine(l2, 15000, 200, ierr2) || l2 != kServerHello ||
                    !inst->c.writeAll(kClientHello, strlen(kClientHello), ierr2)) {
                    say("world: the instance connection could not be opened: " + ierr2);
                    inst.reset();
                } else say("world: instance connection: greeting exchanged");
            }
        } else if (p.opcode == op::SMSG_CHARACTER_LOGIN_FAILED) {
            say("world: SMSG_CHARACTER_LOGIN_FAILED, reason " + std::to_string(p.data.empty() ? -1 : (int)p.data[0]));
        } else if (p.opcode == op::SMSG_LOGIN_VERIFY_WORLD) {
            In in(p.data);
            const int32_t map = (int32_t)in.u32();
            float pos[4];
            for (float& f : pos) { uint32_t u = in.u32(); memcpy(&f, &u, 4); }
            if (!in.bad) {
                enteredWorld_ = true;
                char b[160];
                snprintf(b, sizeof b, "world: SMSG_LOGIN_VERIFY_WORLD: map %d at (%.1f, %.1f, %.1f), facing %.2f", map, pos[0], pos[1], pos[2], pos[3]);
                say(b);
            }
        }
        if (loginSent && p.opcode != op::SMSG_PONG) ++worldPackets;
    }
    if (loginSent) say("world: " + std::to_string(worldPackets) + " packets after the login");
    Out o;
    o.u32(14);   // CMSG_LOG_DISCONNECT reason: a plain logout
    w.send(op::CMSG_LOG_DISCONNECT, o.b, err);
    w.c.close();
    return gotChars_;
}

bool PacketLogFile::open(const std::string& path) {
    f_ = fopen(path.c_str(), "wb");
    if (f_) fwrite("FWL1", 1, 4, f_);
    return f_ != nullptr;
}
void PacketLogFile::write(const WorldPacket& p) {
    if (!f_) return;
    const uint8_t dir = p.fromServer ? 0 : 1;
    const uint32_t size = (uint32_t)p.data.size();
    fwrite(&dir, 1, 1, f_);
    fwrite(&p.opcode, 4, 1, f_);
    fwrite(&p.ms, 4, 1, f_);
    fwrite(&size, 4, 1, f_);
    if (size) fwrite(p.data.data(), 1, size, f_);
    fflush(f_);
}
PacketLogFile::~PacketLogFile() { if (f_) fclose(f_); }

}  // namespace wf
