// fdata.hpp — readers of the modern client data formats the WoW Forever port needs, for the data probe (w3dfdata) and later
// the converter: DB2 tables WDC3 / WDC4 / WDC5 (the field storage: inline, bit-packed, common, pallet; the string block; the
// ID list; section 0 and the further unencrypted sections) and the chunked files (WDT with MAID FileDataIDs, ADT root / _obj0 /
// _tex0, M2 with MD21 / SFID / TXID ...). No dependencies; tested on synthetic files (net/tests/forever_data_test.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "fbase.hpp"

namespace wf {

struct Db2Field {
    uint16_t offsetBits = 0, sizeBits = 0;
    uint32_t extraSize = 0, storage = 0, v1 = 0, v2 = 0, v3 = 0;   // storage: 0 inline, 1 bit-packed, 2 common, 3 pallet, 4 pallet array, 5 signed
};

class Db2 {
public:
    bool load(const Bytes& file, std::string& err);
    std::string magic;
    uint32_t version = 0, recordCount = 0, fieldCount = 0, recordSize = 0, tableHash = 0, layoutHash = 0, minId = 0, maxId = 0, locale = 0;
    uint16_t flags = 0, idIndex = 0;
    uint32_t sectionCount = 0;
    std::vector<Db2Field> fields;
    // the records read (unencrypted sections, not the sparse layout): id and the raw 32-bit values of each field's first element
    struct Record { uint32_t id = 0; std::vector<uint32_t> values; std::vector<size_t> fieldPos; size_t sectionBase = 0; size_t virtualPos = 0; };
    std::vector<Record> records;
    std::string skipped;   // what was not read and why (sparse table, encrypted sections)
    // The string a field's value points at. WDC3+: the value is relative to the field's own position as if the records of ALL sections
    // (also the encrypted ones that are not read) came one after the other and the string blocks after them all.
    std::string stringAt(const Record& r, size_t field) const;
    std::string describe() const;   // a header line
private:
    Bytes d_;
    std::vector<uint32_t> pallet_;
    std::vector<size_t> palletStart_;   // per field, index into pallet_
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> common_;   // per field: (id, value)
    size_t stringsBegin_ = 0, stringsEnd_ = 0;   // of the section being read (kept per record through sectionBase)
    std::vector<std::pair<size_t, size_t>> sectionStrings_;   // (begin, end) per section index used
    struct StrBlock { size_t begin = 0, size = 0; bool readable = false; };
    std::vector<StrBlock> strBlocks_;   // every section, in file order
    size_t totalRecords_ = 0;
};

// Chunked files: 4-character id (stored reversed in the file: "REVM" = MVER), uint32 size, data.
struct Chunk { std::string id; size_t offset = 0, size = 0; };
std::vector<Chunk> chunks(const Bytes& file, size_t from = 0, size_t to = (size_t)-1);
std::string chunkSummary(const std::vector<Chunk>& c);   // "MVER(4) MPHD(32) MAIN(32768) ..." with repeats counted

// WDT MAID: per tile the FileDataIDs of root ADT, _obj0, _obj1, _tex0, _lod, map texture, map normal texture, minimap
struct WdtTile { int x = 0, y = 0; uint32_t fdid[8] = {}; };
bool readWdt(const Bytes& file, std::vector<WdtTile>& tiles, uint32_t& mphdFlags, std::string& err);

// M2 (MD21): the header numbers of the original layout inside MD21 and the file-id chunks
struct M2Info {
    uint32_t version = 0, vertices = 0, bones = 0, sequences = 0, textures = 0, views = 0;
    std::string name;
    std::vector<uint32_t> skinFileIds, textureFileIds, animFileIds;
    std::string chunks;
};
bool readM2(const Bytes& file, M2Info& out, std::string& err);

}  // namespace wf
