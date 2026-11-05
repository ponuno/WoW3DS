// questdb.cpp — 6.63: see questdb.hpp.
#include "questdb.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace questdb {

static uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool Db::open(const char* path) {
    path_.clear();
    version_ = 0;
    ids_.clear();
    offs_.clear();
    FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return false;
    uint8_t h[16];
    bool ok = std::fread(h, 1, 16, f) == 16 && !std::memcmp(h, "WQDB", 4) && (u32(h + 4) == 1 || u32(h + 4) == 2);
    version_ = ok ? u32(h + 4) : 0;
    const uint32_t n = ok ? u32(h + 8) : 0, io = ok ? u32(h + 12) : 0;
    if (ok && (n > 200000 || io != 16)) ok = false;
    if (ok) {
        std::vector<uint8_t> idx((size_t)n * 8);
        ok = idx.empty() || std::fread(idx.data(), 1, idx.size(), f) == idx.size();
        if (ok) {
            std::fseek(f, 0, SEEK_END);
            fileSize_ = (uint32_t)std::ftell(f);
            for (uint32_t i = 0; i < n; ++i) {
                ids_.push_back(u32(&idx[i * 8]));
                offs_.push_back(u32(&idx[i * 8 + 4]));
                if (i && (ids_[i] <= ids_[i - 1] || offs_[i] <= offs_[i - 1] || offs_[i] > fileSize_)) { ok = false; break; }
            }
        }
    }
    std::fclose(f);
    if (!ok) { ids_.clear(); offs_.clear(); return false; }
    path_ = path;
    return true;
}

bool Db::has(uint32_t id) const { return std::binary_search(ids_.begin(), ids_.end(), id); }

bool Db::decode(const uint8_t* p, size_t n, Quest* out) {
    if (n < 4) return false;
    const unsigned ns = p[0], ne = p[1], no = p[2];
    size_t o = 4;
    if (o + (size_t)(ns + ne) * 6 > n) return false;
    out->start.clear(); out->end.clear(); out->obj.clear();
    for (unsigned i = 0; i < ns + ne; ++i, o += 6) {
        const Pt pt{u16(p + o), (int16_t)u16(p + o + 2), (int16_t)u16(p + o + 4)};
        (i < ns ? out->start : out->end).push_back(pt);
    }
    for (unsigned k = 0; k < no; ++k) {
        if (o + 8 > n) return false;
        Objective ob;
        ob.kind = p[o];
        const unsigned nb = p[o + 1];
        ob.id = u32(p + o + 4);
        o += 8;
        if (ob.kind > kExplore || o + (size_t)nb * 8 > n) return false;
        for (unsigned b = 0; b < nb; ++b, o += 8) ob.blobs.push_back({u16(p + o), (int16_t)u16(p + o + 2), (int16_t)u16(p + o + 4), u16(p + o + 6)});
        out->obj.push_back(std::move(ob));
    }
    return true;
}

bool Db::find(uint32_t id, Quest* out) const {
    auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
    if (it == ids_.end() || *it != id) return false;
    const size_t i = (size_t)(it - ids_.begin());
    const uint32_t off = offs_[i], end = i + 1 < offs_.size() ? offs_[i + 1] : fileSize_;
    if (end <= off || end - off > 65536) return false;
    FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b(end - off);
    const bool ok = std::fseek(f, (long)off, SEEK_SET) == 0 && std::fread(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    if (!ok || !decode(b.data(), b.size(), out)) return false;
    out->world = version_ == 2;
    return true;
}

}  // namespace questdb
