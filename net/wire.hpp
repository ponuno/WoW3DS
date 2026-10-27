// wire.hpp — little-endian packet writer/reader shared by the network code.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "crypto.hpp"

namespace w3n {

struct Writer {
    Bytes b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
    void f32(float f) { uint32_t v; memcpy(&v, &f, 4); u32(v); }
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void raw(const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }
    void cstr(const std::string& s) { raw(s.c_str(), s.size() + 1); }
    // Packed GUID: a mask byte telling which of the 8 bytes are non-zero, then those bytes.
    void packedGuid(uint64_t g) {
        size_t at = b.size();
        u8(0);
        for (int i = 0; i < 8; ++i) {
            uint8_t byte = (uint8_t)(g >> (8 * i));
            if (byte) { b[at] |= (uint8_t)(1u << i); u8(byte); }
        }
    }
    // FourCC fields are sent reversed and zero padded ("Win" -> 'n','i','W',0)
    void fourcc(const std::string& s) {
        uint8_t t[4] = {0, 0, 0, 0};
        size_t n = std::min<size_t>(4, s.size());
        for (size_t i = 0; i < n; ++i) t[i] = (uint8_t)s[n - 1 - i];
        raw(t, 4);
    }
};

// Bounds-checked reader: reading past the end sets 'bad' and returns zeros.
struct Reader {
    const uint8_t* p;
    size_t n, pos = 0;
    bool bad = false;
    Reader(const Bytes& b, size_t start = 0) : p(b.data()), n(b.size()), pos(start) {}
    Reader(const uint8_t* data, size_t len) : p(data), n(len) {}
    size_t left() const { return pos < n ? n - pos : 0; }
    bool need(size_t k) { if (pos + k > n) { bad = true; pos = n; return false; } return true; }
    uint8_t u8() { return need(1) ? p[pos++] : 0; }
    uint16_t u16() { uint16_t v = u8(); return (uint16_t)(v | (u8() << 8)); }
    uint32_t u32() { uint32_t v = u16(); return v | ((uint32_t)u16() << 16); }
    uint64_t u64() { uint64_t v = u32(); return v | ((uint64_t)u32() << 32); }
    float f32() { uint32_t v = u32(); float f; memcpy(&f, &v, 4); return f; }
    uint64_t packedGuid() {
        uint8_t mask = u8();
        uint64_t g = 0;
        for (int i = 0; i < 8; ++i)
            if (mask & (1u << i)) g |= (uint64_t)u8() << (8 * i);
        return g;
    }
    Bytes bytes(size_t k) { Bytes out; if (need(k)) { out.assign(p + pos, p + pos + k); pos += k; } return out; }
    std::string cstr() {
        std::string s;
        while (pos < n && p[pos]) s += (char)p[pos++];
        if (pos < n) ++pos; else bad = true;
        return s;
    }
    void skip(size_t k) { if (need(k)) pos += k; }
};

}  // namespace w3n
