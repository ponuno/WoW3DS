// fdata.cpp — see fdata.hpp. Layouts as documented on wowdev.wiki (DB2 WDC3-WDC5, WDT, ADT, M2 chunks).
#include "fdata.hpp"

#include <cstdio>
#include <map>

namespace wf {

namespace {
uint32_t rd32(const Bytes& d, size_t at) { uint32_t v = 0; if (at + 4 <= d.size()) memcpy(&v, d.data() + at, 4); return v; }
uint16_t rd16(const Bytes& d, size_t at) { uint16_t v = 0; if (at + 2 <= d.size()) memcpy(&v, d.data() + at, 2); return v; }
// bits [bit, bit + n) of the bytes at 'base' (little-endian bit order), n <= 32
uint32_t bitsAt(const Bytes& d, size_t base, uint32_t bit, uint32_t n) {
    if (n == 0) return 0;
    uint64_t v = 0;
    const size_t first = base + bit / 8;
    for (size_t k = 0; k < 8 && first + k < d.size(); ++k) v |= (uint64_t)d[first + k] << (8 * k);
    v >>= (bit & 7);
    return (uint32_t)(n >= 32 ? v : v & ((1ull << n) - 1));
}
}  // namespace

bool Db2::load(const Bytes& file, std::string& err) {
    d_ = file;
    records.clear();
    skipped.clear();
    if (d_.size() < 76) { err = "too short for a DB2"; return false; }
    magic.assign((const char*)d_.data(), 4);
    size_t h = 4;
    if (magic == "WDC5") { version = rd32(d_, 4); h = 4 + 4 + 128; }
    else if (magic == "WDC4" || magic == "WDC3") { version = magic[3] - '0'; }
    else { err = "not a WDC3/4/5 table (magic '" + magic + "')"; return false; }
    recordCount = rd32(d_, h); fieldCount = rd32(d_, h + 4); recordSize = rd32(d_, h + 8);
    const uint32_t stringTableSize = rd32(d_, h + 12);
    (void)stringTableSize;
    tableHash = rd32(d_, h + 16); layoutHash = rd32(d_, h + 20); minId = rd32(d_, h + 24); maxId = rd32(d_, h + 28); locale = rd32(d_, h + 32);
    flags = rd16(d_, h + 36); idIndex = rd16(d_, h + 38);
    const uint32_t totalFieldCount = rd32(d_, h + 40);
    // bitpacked_data_offset h+44, lookup_column_count h+48
    const uint32_t storageInfoSize = rd32(d_, h + 52), commonSize = rd32(d_, h + 56), palletSize = rd32(d_, h + 60);
    sectionCount = rd32(d_, h + 64);
    size_t at = h + 68;
    struct Section { uint64_t keyHash; uint32_t fileOffset, records, stringSize, recordsEnd, idListSize, relSize, offsetMapCount, copyCount; };
    std::vector<Section> secs(sectionCount);
    if (sectionCount > 4096 || at + sectionCount * 40 > d_.size()) { err = "bad section count"; return false; }
    for (Section& s : secs) {
        memcpy(&s.keyHash, d_.data() + at, 8);
        s.fileOffset = rd32(d_, at + 8); s.records = rd32(d_, at + 12); s.stringSize = rd32(d_, at + 16); s.recordsEnd = rd32(d_, at + 20);
        s.idListSize = rd32(d_, at + 24); s.relSize = rd32(d_, at + 28); s.offsetMapCount = rd32(d_, at + 32); s.copyCount = rd32(d_, at + 36);
        at += 40;
    }
    at += (size_t)totalFieldCount * 4;   // field_structure: int16 size, uint16 position (the storage info says it again)
    const size_t nInfo = storageInfoSize / 24;
    if (at + storageInfoSize > d_.size() || nInfo > 4096) { err = "bad field storage info"; return false; }
    fields.resize(nInfo);
    for (Db2Field& f : fields) {
        f.offsetBits = rd16(d_, at); f.sizeBits = rd16(d_, at + 2); f.extraSize = rd32(d_, at + 4); f.storage = rd32(d_, at + 8);
        f.v1 = rd32(d_, at + 12); f.v2 = rd32(d_, at + 16); f.v3 = rd32(d_, at + 20);
        at += 24;
    }
    // pallet data (fields 3 / 4, in field order), then common data (fields 2)
    const size_t palletAt = at, commonAt = at + palletSize;
    if (commonAt + commonSize > d_.size()) { err = "pallet / common data past the end"; return false; }
    pallet_.clear();
    for (size_t i = 0; i + 4 <= palletSize; i += 4) pallet_.push_back(rd32(d_, palletAt + i));
    palletStart_.assign(fields.size(), 0);
    common_.assign(fields.size(), {});
    {
        size_t p = 0, c = commonAt;
        for (size_t f = 0; f < fields.size(); ++f) {
            if (fields[f].storage == 3 || fields[f].storage == 4) { palletStart_[f] = p; p += fields[f].extraSize / 4; }
            else if (fields[f].storage == 2) {
                for (size_t k = 0; k + 8 <= fields[f].extraSize; k += 8) common_[f].emplace_back(rd32(d_, c + k), rd32(d_, c + k + 4));
                c += fields[f].extraSize;
            }
        }
    }
    if (flags & 0x01) { skipped = "sparse (offset map) layout: records not read"; return true; }
    sectionStrings_.clear();
    strBlocks_.clear();
    totalRecords_ = 0;
    for (const Section& s : secs) totalRecords_ += s.records;
    size_t globalIndex = 0;
    int encrypted = 0;
    for (size_t si = 0; si < secs.size(); ++si) {
        const Section& s = secs[si];
        strBlocks_.emplace_back();
        strBlocks_.back().size = s.stringSize;
        const size_t firstIndex = globalIndex;
        globalIndex += s.records;
        if (s.keyHash != 0) { ++encrypted; continue; }
        const size_t recBase = s.fileOffset, strBase = recBase + (size_t)s.records * recordSize, idBase = strBase + s.stringSize;
        if (idBase + s.idListSize > d_.size()) { skipped += "section " + std::to_string(si) + " past the end; "; continue; }
        sectionStrings_.emplace_back(strBase, idBase);
        strBlocks_.back().begin = strBase;
        strBlocks_.back().readable = true;
        for (uint32_t r = 0; r < s.records; ++r) {
            Record rec;
            rec.sectionBase = sectionStrings_.size() - 1;
            rec.virtualPos = (firstIndex + r) * (size_t)recordSize;
            const size_t rb = recBase + (size_t)r * recordSize;
            for (size_t f = 0; f < fields.size(); ++f) {
                const Db2Field& fl = fields[f];
                uint32_t v = 0;
                switch (fl.storage) {
                    case 0: { const uint32_t n = fl.sizeBits >= 32 ? 32 : fl.sizeBits; v = bitsAt(d_, rb, fl.offsetBits, n); break; }
                    case 1: case 5: v = bitsAt(d_, rb, fl.offsetBits, fl.v2 ? fl.v2 : fl.sizeBits); break;
                    case 3: { const uint32_t idx = bitsAt(d_, rb, fl.offsetBits, fl.v2 ? fl.v2 : fl.sizeBits); const size_t pi = palletStart_[f] + idx;
                              v = pi < pallet_.size() ? pallet_[pi] : 0; break; }
                    case 4: { const uint32_t idx = bitsAt(d_, rb, fl.offsetBits, fl.v2 ? fl.v2 : fl.sizeBits);
                              const size_t pi = palletStart_[f] + (size_t)idx * (fl.v3 ? fl.v3 : 1); v = pi < pallet_.size() ? pallet_[pi] : 0; break; }
                    default: v = fl.v1; break;   // common: the default unless the id is listed (below)
                }
                rec.values.push_back(v);
                rec.fieldPos.push_back(rb + fl.offsetBits / 8);
            }
            rec.id = (flags & 0x04) && s.idListSize >= (r + 1) * 4 ? rd32(d_, idBase + r * 4) : (idIndex < rec.values.size() ? rec.values[idIndex] : 0);
            for (size_t f = 0; f < fields.size(); ++f)
                if (fields[f].storage == 2)
                    for (auto& kv : common_[f]) if (kv.first == rec.id) { rec.values[f] = kv.second; break; }
            records.push_back(std::move(rec));
        }
    }
    if (encrypted) skipped += std::to_string(encrypted) + " encrypted section(s) (hotfix keys) not read; ";
    return true;
}

std::string Db2::stringAt(const Record& r, size_t f) const {
    if (f >= r.values.size() || fields[f].storage != 0 || r.values[f] == 0) return "";
    // position in the virtual layout, relative to the start of the string blocks (after all records of all sections)
    const size_t total = totalRecords_ * (size_t)recordSize;
    const size_t at = r.virtualPos + fields[f].offsetBits / 8 + r.values[f];
    if (at < total) return "";
    size_t off = at - total;
    for (const StrBlock& b : strBlocks_) {
        if (off >= b.size) { off -= b.size; continue; }
        if (!b.readable) return "";
        std::string s;
        for (size_t i = b.begin + off; i < b.begin + b.size && i < d_.size() && d_[i]; ++i) {
            if (d_[i] < 0x20 && d_[i] != '\t') return "";
            s += (char)d_[i];
            if (s.size() > 200) break;
        }
        return s;
    }
    return "";
}

std::string Db2::describe() const {
    char b[300];
    snprintf(b, sizeof b, "%s%s: %u records, %u fields (%zu storage infos), record %u bytes, ids %u..%u, layout 0x%08X, table 0x%08X, flags 0x%x, %u section(s)",
             magic.c_str(), magic == "WDC5" ? (" v" + std::to_string(version)).c_str() : "", recordCount, fieldCount, fields.size(), recordSize, minId, maxId,
             layoutHash, tableHash, flags, sectionCount);
    return b;
}

std::vector<Chunk> chunks(const Bytes& d, size_t from, size_t to) {
    std::vector<Chunk> out;
    if (to > d.size()) to = d.size();
    size_t at = from;
    while (at + 8 <= to) {
        Chunk c;
        c.id = {(char)d[at + 3], (char)d[at + 2], (char)d[at + 1], (char)d[at]};
        c.size = rd32(d, at + 4);
        c.offset = at + 8;
        if (c.offset + c.size > to) break;
        out.push_back(c);
        at = c.offset + c.size;
    }
    return out;
}

std::string chunkSummary(const std::vector<Chunk>& c) {
    std::string s;
    for (size_t i = 0; i < c.size();) {
        size_t j = i;
        size_t total = 0;
        while (j < c.size() && c[j].id == c[i].id) { total += c[j].size; ++j; }
        char b[64];
        if (j - i > 1) snprintf(b, sizeof b, "%s x%zu (%zu) ", c[i].id.c_str(), j - i, total);
        else snprintf(b, sizeof b, "%s(%zu) ", c[i].id.c_str(), c[i].size);
        s += b;
        i = j;
    }
    return s;
}

bool readWdt(const Bytes& d, std::vector<WdtTile>& tiles, uint32_t& mphdFlags, std::string& err) {
    tiles.clear();
    mphdFlags = 0;
    bool haveMaid = false;
    std::vector<uint32_t> mainFlags;
    for (const Chunk& c : chunks(d)) {
        if (c.id == "MPHD" && c.size >= 4) mphdFlags = rd32(d, c.offset);
        if (c.id == "MAIN" && c.size >= 64 * 64 * 8)
            for (int i = 0; i < 64 * 64; ++i) mainFlags.push_back(rd32(d, c.offset + (size_t)i * 8));
        if (c.id == "MAID" && c.size >= 64 * 64 * 32) {
            haveMaid = true;
            for (int i = 0; i < 64 * 64; ++i) {
                WdtTile t;
                t.x = i % 64;
                t.y = i / 64;
                bool any = false;
                for (int k = 0; k < 8; ++k) { t.fdid[k] = rd32(d, c.offset + (size_t)i * 32 + (size_t)k * 4); any = any || t.fdid[k]; }
                if (any) tiles.push_back(t);
            }
        }
    }
    if (!haveMaid) {
        int n = 0;
        for (uint32_t f : mainFlags) n += (f & 1) ? 1 : 0;
        err = "no MAID chunk (an old-style WDT: " + std::to_string(n) + " tiles by name in MAIN)";
        return false;
    }
    return true;
}

bool readM2(const Bytes& d, M2Info& out, std::string& err) {
    out = M2Info();
    std::vector<Chunk> cs = chunks(d);
    out.chunks = chunkSummary(cs);
    size_t md = (size_t)-1, mdSize = 0;
    if (d.size() >= 4 && memcmp(d.data(), "MD20", 4) == 0) { md = 0; mdSize = d.size(); out.chunks = "(no chunks: an old-style MD20 file)"; }
    for (Chunk& c : cs) {
        // M2 chunk ids are stored in reading order (real files: "MD21"); chunks() reverses them like ADT/WDT ids, so un-reverse here
        c.id = std::string(c.id.rbegin(), c.id.rend());
        if (c.id == "MD21") { md = c.offset; mdSize = c.size; }
        auto ids = [&](std::vector<uint32_t>& v, size_t stride, size_t at) { for (size_t i = 0; i + stride <= c.size; i += stride) v.push_back(rd32(d, c.offset + i + at)); };
        if (c.id == "SFID") ids(out.skinFileIds, 4, 0);
        if (c.id == "TXID") ids(out.textureFileIds, 4, 0);
        if (c.id == "AFID") ids(out.animFileIds, 8, 4);
    }
    if (md == (size_t)-1 || mdSize < 0x58 || memcmp(d.data() + md, "MD20", 4) != 0) { err = "no MD21/MD20 header"; return false; }
    Bytes h(d.begin() + (long)md, d.begin() + (long)(md + mdSize));
    out.version = rd32(h, 4);
    const uint32_t nameLen = rd32(h, 8), nameOfs = rd32(h, 12);
    if (nameLen && nameOfs + nameLen <= h.size()) { out.name.assign((const char*)h.data() + nameOfs, nameLen); while (!out.name.empty() && !out.name.back()) out.name.pop_back(); }
    out.sequences = rd32(h, 28);
    out.bones = rd32(h, 44);
    out.vertices = rd32(h, 60);
    out.views = rd32(h, 68);
    out.textures = rd32(h, 80);
    return true;
}

}  // namespace wf
