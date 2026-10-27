// crypto.hpp — the small amount of cryptography a vanilla 1.12.1 login needs,
// with no external dependencies (builds for 3DS, Linux and Windows alike):
//   * SHA-1
//   * unsigned big integers (little-endian 32-bit limbs) with modular exponentiation
//   * WoW's SRP6 variant (k = 3, interleaved session key)
//   * the vanilla world-packet header cipher
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace w3n {

using Bytes = std::vector<uint8_t>;

// ------------------------------------------------------------------ SHA-1
class Sha1 {
public:
    Sha1() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void update(const Bytes& b) { update(b.data(), b.size()); }
    void update(const std::string& s) { update(s.data(), s.size()); }
    Bytes final();   // 20 bytes; the object must be reset() before reuse
private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint64_t len_ = 0;
    uint8_t buf_[64];
    size_t fill_ = 0;
};
Bytes sha1(const Bytes& b);
Bytes sha1(const std::string& s);

// ------------------------------------------------------------------ BigNum
// Unsigned arbitrary-precision integer. Simple and portable rather than fast:
// a WoW login needs three 256-bit modular exponentiations.
class BigNum {
public:
    BigNum() = default;
    explicit BigNum(uint32_t v);
    static BigNum fromLE(const uint8_t* p, size_t n);
    static BigNum fromLE(const Bytes& b) { return fromLE(b.data(), b.size()); }
    static BigNum fromHex(const std::string& hex);   // big-endian hex text (tests)
    // Little-endian bytes. minLen = 0 gives the natural length (no high zero
    // bytes, but at least 1 byte), like OpenSSL BN_bn2bin + reverse.
    Bytes toLE(size_t minLen = 0) const;
    std::string toHex() const;

    bool isZero() const { return w_.empty(); }
    size_t bits() const;
    bool bit(size_t i) const { return i / 32 < w_.size() && (w_[i / 32] >> (i % 32)) & 1u; }

    static int cmp(const BigNum& a, const BigNum& b);
    static BigNum add(const BigNum& a, const BigNum& b);
    static BigNum sub(const BigNum& a, const BigNum& b);   // requires a >= b
    static BigNum mul(const BigNum& a, const BigNum& b);
    static BigNum mod(const BigNum& a, const BigNum& m);
    static BigNum modPow(const BigNum& base, const BigNum& exp, const BigNum& m);

private:
    void trim();
    std::vector<uint32_t> w_;   // little-endian limbs, no high zero limbs
};

// ------------------------------------------------------------------ SRP6 (WoW)
// Client side of the realmd handshake. All byte arrays are little-endian as on
// the wire. Mirrors WoWee's auth/srp.cpp, which is known to work against
// MaNGOS/CMaNGOS/vMaNGOS/TrinityCore.
struct SrpResult {
    Bytes A;     // 32 bytes, client public ephemeral
    Bytes M1;    // 20 bytes, client proof
    Bytes M2;    // 20 bytes, expected server proof
    Bytes K;     // 40 bytes, session key (also keys the world header cipher)
};
// 'a' = 19 random bytes (client private ephemeral); injectable for tests.
bool srpCompute(const std::string& account, const std::string& password, const Bytes& B, const Bytes& g,
                const Bytes& N, const Bytes& salt, const Bytes& a, SrpResult& out);

// ------------------------------------------------------------------ header cipher
// Vanilla 1.12: header bytes only, keyed with the raw 40-byte session key.
class HeaderCipher {
public:
    void init(const Bytes& key) { key_ = key; si_ = sp_ = ri_ = rp_ = 0; on_ = !key.empty(); }
    bool enabled() const { return on_; }
    void encrypt(uint8_t* d, size_t n);
    void decrypt(uint8_t* d, size_t n);
private:
    Bytes key_;
    size_t si_ = 0, ri_ = 0;
    uint8_t sp_ = 0, rp_ = 0;
    bool on_ = false;
};

// ------------------------------------------------------------------ misc
// zlib stream made of one "stored" (uncompressed) deflate block: valid input
// for any inflate, so the client needs no compressor (used for the addon list).
Bytes zlibStored(const Bytes& data);
// Platform random bytes (3DS: ps:ps, Windows: BCrypt/rand_s, POSIX: /dev/urandom).
bool randomBytes(uint8_t* out, size_t n);
std::string upper(std::string s);
std::string toHex(const uint8_t* p, size_t n);

}  // namespace w3n
