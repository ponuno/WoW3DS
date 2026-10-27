#include "crypto.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#if defined(__3DS__)
#include <3ds.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace w3n {

// ================================================================== SHA-1
static inline uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

void Sha1::reset() {
    h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE; h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
    len_ = 0;
    fill_ = 0;
}

void Sha1::block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    len_ += len;
    while (len) {
        size_t n = std::min(len, 64 - fill_);
        memcpy(buf_ + fill_, p, n);
        fill_ += n; p += n; len -= n;
        if (fill_ == 64) { block(buf_); fill_ = 0; }
    }
}

Bytes Sha1::final() {
    uint64_t bitLen = len_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (fill_ != 56) update(&z, 1);
    uint8_t lenBE[8];
    for (int i = 0; i < 8; ++i) lenBE[i] = (uint8_t)(bitLen >> (56 - 8 * i));
    update(lenBE, 8);
    Bytes out(20);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t)(h_[i] >> 24); out[i * 4 + 1] = (uint8_t)(h_[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h_[i] >> 8); out[i * 4 + 3] = (uint8_t)h_[i];
    }
    return out;
}

Bytes sha1(const Bytes& b) { Sha1 s; s.update(b); return s.final(); }
Bytes sha1(const std::string& str) { Sha1 s; s.update(str); return s.final(); }

// ================================================================== BigNum
BigNum::BigNum(uint32_t v) { if (v) w_.push_back(v); }

void BigNum::trim() { while (!w_.empty() && w_.back() == 0) w_.pop_back(); }

BigNum BigNum::fromLE(const uint8_t* p, size_t n) {
    BigNum r;
    r.w_.assign((n + 3) / 4, 0);
    for (size_t i = 0; i < n; ++i) r.w_[i / 4] |= (uint32_t)p[i] << (8 * (i % 4));
    r.trim();
    return r;
}

BigNum BigNum::fromHex(const std::string& hex) {
    Bytes le;
    for (size_t i = hex.size(); i > 0;) {
        size_t s = i >= 2 ? i - 2 : 0;
        le.push_back((uint8_t)strtoul(hex.substr(s, i - s).c_str(), nullptr, 16));
        i = s;
    }
    return fromLE(le);
}

Bytes BigNum::toLE(size_t minLen) const {
    Bytes b(w_.size() * 4);
    for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)(w_[i / 4] >> (8 * (i % 4)));
    while (b.size() > 1 && b.back() == 0) b.pop_back();
    if (b.empty()) b.push_back(0);
    if (b.size() < minLen) b.resize(minLen, 0);
    return b;
}

std::string BigNum::toHex() const {
    Bytes le = toLE();
    std::string s;
    char t[3];
    for (size_t i = le.size(); i-- > 0;) { snprintf(t, sizeof t, "%02X", le[i]); s += t; }
    return s;
}

size_t BigNum::bits() const {
    if (w_.empty()) return 0;
    uint32_t top = w_.back();
    size_t b = (w_.size() - 1) * 32;
    while (top) { ++b; top >>= 1; }
    return b;
}

int BigNum::cmp(const BigNum& a, const BigNum& b) {
    if (a.w_.size() != b.w_.size()) return a.w_.size() < b.w_.size() ? -1 : 1;
    for (size_t i = a.w_.size(); i-- > 0;)
        if (a.w_[i] != b.w_[i]) return a.w_[i] < b.w_[i] ? -1 : 1;
    return 0;
}

BigNum BigNum::add(const BigNum& a, const BigNum& b) {
    BigNum r;
    size_t n = std::max(a.w_.size(), b.w_.size());
    r.w_.resize(n + 1);
    uint64_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c += (uint64_t)(i < a.w_.size() ? a.w_[i] : 0) + (i < b.w_.size() ? b.w_[i] : 0);
        r.w_[i] = (uint32_t)c;
        c >>= 32;
    }
    r.w_[n] = (uint32_t)c;
    r.trim();
    return r;
}

BigNum BigNum::sub(const BigNum& a, const BigNum& b) {
    BigNum r;
    r.w_.resize(a.w_.size());
    int64_t br = 0;
    for (size_t i = 0; i < a.w_.size(); ++i) {
        int64_t d = (int64_t)a.w_[i] - (i < b.w_.size() ? b.w_[i] : 0) - br;
        br = d < 0;
        r.w_[i] = (uint32_t)(d + (br ? ((int64_t)1 << 32) : 0));
    }
    r.trim();
    return r;
}

BigNum BigNum::mul(const BigNum& a, const BigNum& b) {
    BigNum r;
    if (a.isZero() || b.isZero()) return r;
    r.w_.assign(a.w_.size() + b.w_.size(), 0);
    for (size_t i = 0; i < a.w_.size(); ++i) {
        uint64_t c = 0;
        for (size_t j = 0; j < b.w_.size(); ++j) {
            c += (uint64_t)a.w_[i] * b.w_[j] + r.w_[i + j];
            r.w_[i + j] = (uint32_t)c;
            c >>= 32;
        }
        r.w_[i + b.w_.size()] = (uint32_t)c;
    }
    r.trim();
    return r;
}

// Bit-serial long division: simple, branch-light, plenty fast for 256-bit moduli.
BigNum BigNum::mod(const BigNum& a, const BigNum& m) {
    if (cmp(a, m) < 0) return a;
    const size_t n = m.w_.size();
    std::vector<uint32_t> r(n + 1, 0);
    for (size_t bi = a.bits(); bi-- > 0;) {
        // r = (r << 1) | bit
        uint32_t carry = a.bit(bi) ? 1u : 0u;
        for (size_t i = 0; i <= n; ++i) {
            uint32_t nc = r[i] >> 31;
            r[i] = (r[i] << 1) | carry;
            carry = nc;
        }
        // if r >= m: r -= m
        bool ge = r[n] != 0;
        if (!ge) {
            ge = true;
            for (size_t i = n; i-- > 0;) {
                if (r[i] != m.w_[i]) { ge = r[i] > m.w_[i]; break; }
            }
        }
        if (ge) {
            int64_t br = 0;
            for (size_t i = 0; i <= n; ++i) {
                int64_t d = (int64_t)r[i] - (i < n ? m.w_[i] : 0) - br;
                br = d < 0;
                r[i] = (uint32_t)(d + (br ? ((int64_t)1 << 32) : 0));
            }
        }
    }
    BigNum out;
    out.w_ = r;
    out.trim();
    return out;
}

BigNum BigNum::modPow(const BigNum& base, const BigNum& exp, const BigNum& m) {
    BigNum result(1), b = mod(base, m);
    for (size_t i = exp.bits(); i-- > 0;) {
        result = mod(mul(result, result), m);
        if (exp.bit(i)) result = mod(mul(result, b), m);
    }
    return mod(result, m);
}

// ================================================================== SRP6
bool srpCompute(const std::string& account, const std::string& password, const Bytes& Bb, const Bytes& gb,
                const Bytes& Nb, const Bytes& salt, const Bytes& ab, SrpResult& out) {
    const BigNum B = BigNum::fromLE(Bb), g = BigNum::fromLE(gb), N = BigNum::fromLE(Nb);
    const BigNum s = BigNum::fromLE(salt), a = BigNum::fromLE(ab), k(3);
    if (N.isZero() || g.isZero()) return false;
    if (BigNum::mod(B, N).isZero()) return false;   // malicious/broken server

    const std::string I = upper(account);
    const Bytes authHash = sha1(I + ":" + upper(password));
    // x = H(s | H(I:P)), salt bytes exactly as received
    Sha1 h;
    h.update(salt);
    h.update(authHash);
    const BigNum x = BigNum::fromLE(h.final());

    const BigNum A = BigNum::modPow(g, a, N);
    if (A.isZero()) return false;
    const Bytes An = A.toLE(), Bn = B.toLE();
    // u = H(A | B), natural sizes
    h.reset(); h.update(An); h.update(Bn);
    const BigNum u = BigNum::fromLE(h.final());
    // S = (B + kN - k*g^x mod N) ^ (a + u*x) mod N
    const BigNum kgx = BigNum::mul(k, BigNum::modPow(g, x, N));
    const BigNum base = BigNum::sub(BigNum::add(B, BigNum::mul(k, N)), kgx);
    const BigNum S = BigNum::modPow(base, BigNum::add(a, BigNum::mul(u, x)), N);
    // K: interleave SHA1(even bytes) and SHA1(odd bytes) of S (32 bytes LE)
    const Bytes Sb = S.toLE(32);
    Bytes s1, s2;
    for (int i = 0; i < 16; ++i) { s1.push_back(Sb[i * 2]); s2.push_back(Sb[i * 2 + 1]); }
    const Bytes h1 = sha1(s1), h2 = sha1(s2);
    out.K.clear();
    for (int i = 0; i < 20; ++i) { out.K.push_back(h1[i]); out.K.push_back(h2[i]); }
    // M1 = H(H(N) ^ H(g) | H(I) | s | A | B | K), natural sizes
    const Bytes hN = sha1(N.toLE()), hg = sha1(g.toLE());
    Bytes ng(20);
    for (int i = 0; i < 20; ++i) ng[i] = hN[i] ^ hg[i];
    h.reset();
    h.update(ng); h.update(sha1(I)); h.update(s.toLE()); h.update(An); h.update(Bn); h.update(out.K);
    out.M1 = h.final();
    // M2 = H(A | M1 | K)
    h.reset(); h.update(An); h.update(out.M1); h.update(out.K);
    out.M2 = h.final();
    out.A = A.toLE(32);
    return true;
}

// ================================================================== header cipher
void HeaderCipher::encrypt(uint8_t* d, size_t n) {
    if (!on_) return;
    for (size_t i = 0; i < n; ++i) {
        uint8_t x = (uint8_t)((d[i] ^ key_[si_]) + sp_);
        si_ = (si_ + 1) % key_.size();
        d[i] = sp_ = x;
    }
}

void HeaderCipher::decrypt(uint8_t* d, size_t n) {
    if (!on_) return;
    for (size_t i = 0; i < n; ++i) {
        uint8_t e = d[i];
        d[i] = (uint8_t)((e - rp_) ^ key_[ri_]);
        ri_ = (ri_ + 1) % key_.size();
        rp_ = e;
    }
}

// ================================================================== misc
Bytes zlibStored(const Bytes& data) {
    Bytes out = {0x78, 0x01};
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, data.size() - pos);
        bool last = pos + n >= data.size();
        out.push_back(last ? 1 : 0);
        out.push_back((uint8_t)n); out.push_back((uint8_t)(n >> 8));
        out.push_back((uint8_t)~n); out.push_back((uint8_t)(~n >> 8));
        out.insert(out.end(), data.begin() + pos, data.begin() + pos + n);
        pos += n;
    } while (pos < data.size());
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t c : data) { s1 = (s1 + c) % 65521; s2 = (s2 + s1) % 65521; }
    uint32_t adler = (s2 << 16) | s1;
    for (int i = 3; i >= 0; --i) out.push_back((uint8_t)(adler >> (8 * i)));
    return out;
}

bool randomBytes(uint8_t* out, size_t n) {
#if defined(__3DS__)
    static bool inited = false;
    if (!inited) inited = R_SUCCEEDED(psInit());
    return inited && R_SUCCEEDED(PS_GenerateRandomBytes(out, n));
#elif defined(_WIN32)
    return BCryptGenRandom(nullptr, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    bool ok = fread(out, 1, n, f) == n;
    fclose(f);
    return ok;
#endif
}

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string toHex(const uint8_t* p, size_t n) {
    std::string s;
    char t[3];
    for (size_t i = 0; i < n; ++i) { snprintf(t, sizeof t, "%02x", p[i]); s += t; }
    return s;
}

}  // namespace w3n
