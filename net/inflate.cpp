// inflate.cpp — canonical-Huffman inflate in the style of zlib's "puff":
// decodes one bit at a time, which is plenty for a few KB of update packets per
// second and keeps the code small and easy to check.
#include "inflate.hpp"

#include <cstring>

namespace w3n {

namespace {

constexpr int kMaxBits = 15;

struct BitIn {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool err = false;
    int bits(int k) {
        while (cnt < k) {
            if (pos >= n) { err = true; return 0; }
            buf |= (uint32_t)p[pos++] << cnt;
            cnt += 8;
        }
        int v = (int)(buf & ((1u << k) - 1));
        buf >>= k;
        cnt -= k;
        return v;
    }
};

struct Huff {
    uint16_t count[kMaxBits + 1];
    uint16_t sym[288];
};

// Returns 0 for a complete code, >0 for an incomplete one, <0 for an over-subscribed one.
int build(Huff& h, const uint8_t* len, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int s = 0; s < n; ++s) h.count[len[s]]++;
    if (h.count[0] == n) return 0;   // no codes: complete, but decoding fails
    int left = 1;
    for (int l = 1; l <= kMaxBits; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return left;
    }
    uint16_t offs[kMaxBits + 1];
    offs[1] = 0;
    for (int l = 1; l < kMaxBits; ++l) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
    for (int s = 0; s < n; ++s)
        if (len[s]) h.sym[offs[len[s]]++] = (uint16_t)s;
    return left;
}

int decode(BitIn& in, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= kMaxBits; ++l) {
        code |= in.bits(1);
        if (in.err) return -1;
        int count = h.count[l];
        if (code - count < first) return h.sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(BitIn& in, Bytes& out, size_t base, const Huff& lencode, const Huff& distcode, size_t maxOut) {
    for (;;) {
        int sym = decode(in, lencode);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() - base >= maxOut) return false;
            out.push_back((uint8_t)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLenBase[sym] + (size_t)in.bits(kLenExtra[sym]);
            int ds = decode(in, distcode);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = kDistBase[ds] + (size_t)in.bits(kDistExtra[ds]);
            if (in.err) return false;
            if (dist > out.size() - base) return false;   // reaches before the stream start
            if (out.size() - base + len > maxOut) return false;
            size_t from = out.size() - dist;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);   // may overlap
        }
    }
}

bool fixedBlock(BitIn& in, Bytes& out, size_t base, size_t maxOut) {
    static Huff lencode, distcode;
    static bool built = false;
    if (!built) {
        uint8_t l[288];
        int s = 0;
        for (; s < 144; ++s) l[s] = 8;
        for (; s < 256; ++s) l[s] = 9;
        for (; s < 280; ++s) l[s] = 7;
        for (; s < 288; ++s) l[s] = 8;
        build(lencode, l, 288);
        for (s = 0; s < 30; ++s) l[s] = 5;
        build(distcode, l, 30);
        built = true;
    }
    return codes(in, out, base, lencode, distcode, maxOut);
}

bool dynamicBlock(BitIn& in, Bytes& out, size_t base, size_t maxOut) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = in.bits(5) + 257, ndist = in.bits(5) + 1, ncode = in.bits(4) + 4;
    if (in.err || nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320];
    int idx;
    for (idx = 0; idx < ncode; ++idx) lengths[order[idx]] = (uint8_t)in.bits(3);
    for (; idx < 19; ++idx) lengths[order[idx]] = 0;
    if (in.err) return false;
    Huff lencode, distcode;
    if (build(lencode, lengths, 19) != 0) return false;   // code-length code must be complete
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(in, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[idx++] = (uint8_t)sym;
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (idx == 0) return false;
                len = lengths[idx - 1];
                rep = 3 + in.bits(2);
            } else if (sym == 17) {
                rep = 3 + in.bits(3);
            } else {
                rep = 11 + in.bits(7);
            }
            if (in.err || idx + rep > nlen + ndist) return false;
            while (rep--) lengths[idx++] = (uint8_t)len;
        }
    }
    if (lengths[256] == 0) return false;   // no end-of-block code
    int err = build(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = build(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return codes(in, out, base, lencode, distcode, maxOut);
}

}  // namespace

bool inflateRaw(const uint8_t* src, size_t n, Bytes& out, size_t maxOut, size_t* consumed) {
    BitIn in{src, n};
    size_t base = out.size();
    int last;
    do {
        last = in.bits(1);
        int type = in.bits(2);
        if (in.err) return false;
        if (type == 0) {   // stored
            in.buf = 0;
            in.cnt = 0;    // discard to byte boundary
            if (in.pos + 4 > n) return false;
            unsigned len = src[in.pos] | (src[in.pos + 1] << 8);
            unsigned nlen = src[in.pos + 2] | (src[in.pos + 3] << 8);
            in.pos += 4;
            if (len != (~nlen & 0xFFFFu) || in.pos + len > n) return false;
            if (out.size() - base + len > maxOut) return false;
            out.insert(out.end(), src + in.pos, src + in.pos + len);
            in.pos += len;
        } else if (type == 1) {
            if (!fixedBlock(in, out, base, maxOut)) return false;
        } else if (type == 2) {
            if (!dynamicBlock(in, out, base, maxOut)) return false;
        } else {
            return false;
        }
    } while (!last);
    if (consumed) *consumed = in.pos;   // bits left in 'buf' belong to the last byte read
    return true;
}

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

bool zlibInflate(const uint8_t* src, size_t n, Bytes& out, size_t expected, size_t maxOut) {
    out.clear();
    if (n < 6) return false;
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;
    if (expected) {
        if (expected > maxOut) return false;
        out.reserve(expected);
        maxOut = expected;
    }
    size_t used = 0;
    if (!inflateRaw(src + 2, n - 2, out, maxOut, &used)) return false;
    if (expected && out.size() != expected) return false;
    size_t at = 2 + used;
    if (at + 4 > n) return false;   // Adler-32 missing
    uint32_t want = ((uint32_t)src[at] << 24) | ((uint32_t)src[at + 1] << 16) | ((uint32_t)src[at + 2] << 8) | src[at + 3];
    return want == adler32(out.data(), out.size());
}

}  // namespace w3n
