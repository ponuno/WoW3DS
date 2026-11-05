// fbase.hpp — WoW Forever probe (Classic 1.60.1 protocol): bytes, little readers / writers, a minimal protobuf
// encoder / decoder and a minimal JSON parser. No dependencies. Used by the Battle.net login (fbnet) and the world
// connection (fworld); the protocol follows the TrinityCore Forever fork the private servers run (see docs/FOREVER_RU.md).
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wf {

using Bytes = std::vector<uint8_t>;

std::string hex(const uint8_t* p, size_t n, size_t max = 64);
inline std::string hex(const Bytes& b, size_t max = 64) { return hex(b.data(), b.size(), max); }
constexpr uint32_t fourcc(const char* s) {
    uint32_t v = 0;
    for (; *s; ++s) v = (v << 8) | (uint8_t)*s;
    return v;
}

// ---------------------------------------------------------------- byte buffers (little-endian, as the world protocol)
struct Out {
    Bytes b;
    uint8_t bitByte = 0;
    int bitPos = 8;   // 8 = no bits pending
    void u8(uint8_t v) { flushBits(); b.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { flushBits(); for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { flushBits(); for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void raw(const void* p, size_t n) { flushBits(); b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void raw(const Bytes& v) { raw(v.data(), v.size()); }
    // bits: most significant first, as the server's ByteBuffer::WriteBit
    void bit(bool v) {
        if (bitPos == 8) { bitByte = 0; bitPos = 0; }
        if (v) bitByte |= (uint8_t)(0x80 >> bitPos);
        if (++bitPos == 8) { b.push_back(bitByte); bitPos = 8; }
    }
    void flushBits() { if (bitPos != 8) { b.push_back(bitByte); bitPos = 8; } }
};

struct In {
    const uint8_t* p = nullptr;
    size_t n = 0, pos = 0;
    bool bad = false;
    uint8_t bitByte = 0;
    int bitPos = 8;
    In() = default;
    In(const uint8_t* d, size_t len) : p(d), n(len) {}
    explicit In(const Bytes& v) : p(v.data()), n(v.size()) {}
    size_t left() const { return pos <= n ? n - pos : 0; }
    void resetBits() { bitPos = 8; }
    uint8_t u8() { resetBits(); if (pos + 1 > n) { bad = true; return 0; } return p[pos++]; }
    uint32_t u32() { resetBits(); if (pos + 4 > n) { bad = true; pos = n; return 0; } uint32_t v; memcpy(&v, p + pos, 4); pos += 4; return v; }
    uint64_t u64() { resetBits(); if (pos + 8 > n) { bad = true; pos = n; return 0; } uint64_t v; memcpy(&v, p + pos, 8); pos += 8; return v; }
    bool raw(void* out, size_t len) { resetBits(); if (pos + len > n) { bad = true; pos = n; return false; } memcpy(out, p + pos, len); pos += len; return true; }
    bool bit() {
        if (bitPos == 8) { if (pos >= n) { bad = true; return false; } bitByte = p[pos++]; bitPos = 0; }
        return (bitByte >> (7 - bitPos++)) & 1;
    }
    uint32_t bits(int count) { uint32_t v = 0; for (int i = 0; i < count; ++i) v = (v << 1) | (bit() ? 1u : 0u); return v; }
};

// ---------------------------------------------------------------- protobuf (proto2 wire format)
namespace pb {
struct W {
    Bytes b;
    void varint(uint64_t v) { while (v >= 0x80) { b.push_back((uint8_t)(v | 0x80)); v >>= 7; } b.push_back((uint8_t)v); }
    void tag(uint32_t field, uint32_t wire) { varint((uint64_t)field << 3 | wire); }
    void u(uint32_t field, uint64_t v) { tag(field, 0); varint(v); }
    void boolean(uint32_t field, bool v) { u(field, v ? 1 : 0); }
    void f32(uint32_t field, uint32_t v) { tag(field, 5); for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void f64(uint32_t field, uint64_t v) { tag(field, 1); for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void bytes(uint32_t field, const void* p, size_t n) { tag(field, 2); varint(n); b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void str(uint32_t field, const std::string& s) { bytes(field, s.data(), s.size()); }
    void msg(uint32_t field, const W& m) { bytes(field, m.b.data(), m.b.size()); }
};
// One field of a message: number, wire type, the value (varint / fixed) or the bytes (length-delimited).
struct Field {
    uint32_t num = 0, wire = 0;
    uint64_t v = 0;
    const uint8_t* data = nullptr;
    size_t len = 0;
    std::string str() const { return std::string((const char*)data, len); }
    Bytes bytes() const { return Bytes(data, data + len); }
};
// Calls f(Field) for every field; false on a malformed message.
template <typename F>
bool each(const uint8_t* p, size_t n, F&& f) {
    size_t i = 0;
    auto varint = [&](uint64_t& out) {
        out = 0;
        for (int s = 0; s < 64; s += 7) {
            if (i >= n) return false;
            const uint8_t c = p[i++];
            out |= (uint64_t)(c & 0x7F) << s;
            if (!(c & 0x80)) return true;
        }
        return false;
    };
    while (i < n) {
        uint64_t key;
        if (!varint(key)) return false;
        Field fl;
        fl.num = (uint32_t)(key >> 3);
        fl.wire = (uint32_t)(key & 7);
        if (fl.wire == 0) { if (!varint(fl.v)) return false; }
        else if (fl.wire == 1) { if (i + 8 > n) return false; for (int k = 0; k < 8; ++k) fl.v |= (uint64_t)p[i + k] << (8 * k); i += 8; }
        else if (fl.wire == 5) { if (i + 4 > n) return false; for (int k = 0; k < 4; ++k) fl.v |= (uint64_t)p[i + k] << (8 * k); i += 4; }
        else if (fl.wire == 2) { uint64_t l; if (!varint(l) || i + l > n) return false; fl.data = p + i; fl.len = (size_t)l; i += (size_t)l; }
        else return false;
        f(fl);
    }
    return true;
}
template <typename F> bool each(const Bytes& b, F&& f) { return each(b.data(), b.size(), f); }
}  // namespace pb

// ---------------------------------------------------------------- JSON (enough for the login and the realm list)
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const char* key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double numOr(const char* key, double d) const { const Json* j = get(key); return j && j->type == Num ? j->num : d; }
    std::string strOr(const char* key, const std::string& d) const { const Json* j = get(key); return j && j->type == Str ? j->s : d; }
    // Parses one value; false and 'err' on bad input.
    static bool parse(const std::string& text, Json& out, std::string* err = nullptr);
};

// zlib-compressed JSON as the Battle.net realm list sends it: uint32 length, zlib stream; "Prefix:{...}" -> the JSON part
bool unpackJsonBlob(const Bytes& blob, std::string& json, std::string* prefix = nullptr);

}  // namespace wf
