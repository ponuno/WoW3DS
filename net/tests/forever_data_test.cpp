// forever_data_test — the modern-format readers of net/forever/fdata.cpp on gen_forever_data.py's files.
#include <cstdio>
#include "fdata.hpp"

using namespace wf;
static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static Bytes load(const std::string& p) {
    Bytes b;
    if (FILE* f = fopen(p.c_str(), "rb")) { fseek(f, 0, SEEK_END); b.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET); if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear(); fclose(f); }
    return b;
}
int main(int argc, char** argv) {
    const std::string d = argc > 1 ? argv[1] : ".";
    std::string err;
    Db2 db;
    CHECK(db.load(load(d + "/Map.db2"), err), "load: %s", err.c_str());
    CHECK(db.magic == "WDC5" && db.recordCount == 3 && db.fields.size() == 5 && db.records.size() == 3, "%s", db.describe().c_str());
    if (db.records.size() == 3) {
        CHECK(db.records[0].id == 0 && db.records[1].id == 1 && db.records[2].id == 530, "ids from the id list");
        CHECK(db.stringAt(db.records[0], 0) == "Azeroth" && db.stringAt(db.records[0], 1) == "Eastern Kingdoms", "strings of record 0: '%s' '%s'",
              db.stringAt(db.records[0], 0).c_str(), db.stringAt(db.records[0], 1).c_str());
        CHECK(db.stringAt(db.records[2], 0) == "Expansion01" && db.stringAt(db.records[2], 1) == "Outland", "strings of record 2");
        CHECK(db.records[0].values[2] == 1 && db.records[2].values[2] == 3, "bit-packed field: %u %u", db.records[0].values[2], db.records[2].values[2]);
        CHECK(db.records[0].values[3] == 30 && db.records[1].values[3] == 20 && db.records[2].values[3] == 10, "pallet field: %u %u %u",
              db.records[0].values[3], db.records[1].values[3], db.records[2].values[3]);
        CHECK(db.records[0].values[4] == 5 && db.records[1].values[4] == 99 && db.records[2].values[4] == 5, "common field: default and override");
        CHECK(db.stringAt(db.records[0], 2).empty(), "a number is not a string");
    }
    {   // two sections, the first one encrypted: string offsets cross the unreadable section's records and string block
        Db2 d2;
        CHECK(d2.load(load(d + "/Map2.db2"), err), "load Map2: %s", err.c_str());
        CHECK(d2.records.size() == 3 && !d2.skipped.empty(), "Map2: 3 readable records, encrypted section reported");
        if (d2.records.size() == 3) {
            CHECK(d2.stringAt(d2.records[0], 0) == "Azeroth" && d2.stringAt(d2.records[0], 1) == "Eastern Kingdoms", "Map2 strings of record 0: '%s' '%s'",
                  d2.stringAt(d2.records[0], 0).c_str(), d2.stringAt(d2.records[0], 1).c_str());
            CHECK(d2.stringAt(d2.records[2], 0) == "Expansion01" && d2.stringAt(d2.records[2], 1) == "Outland", "Map2 strings of record 2");
        }
    }
    std::vector<WdtTile> tiles;
    uint32_t mphd = 0;
    CHECK(readWdt(load(d + "/Test.wdt"), tiles, mphd, err) && tiles.size() == 2 && mphd == 0x200, "WDT: %s", err.c_str());
    if (tiles.size() == 2) CHECK(tiles[0].x == 32 && tiles[0].y == 48 && tiles[0].fdid[0] == 1001 && tiles[0].fdid[3] == 1004 && tiles[1].fdid[7] == 1108, "MAID ids");
    const Bytes adt = load(d + "/Test_32_48_obj0.adt");
    std::vector<Chunk> cs = chunks(adt);
    CHECK(cs.size() == 3 && cs[1].id == "MDDF" && cs[1].size == 72 && cs[2].id == "MODF" && cs[2].size == 64, "ADT chunks: %s", chunkSummary(cs).c_str());
    M2Info m;
    CHECK(readM2(load(d + "/Wolf.m2"), m, err), "M2: %s", err.c_str());
    CHECK(m.name == "Wolf" && m.version == 264 && m.vertices == 1000 && m.bones == 40 && m.sequences == 12 && m.views == 1 && m.textures == 2, "M2 header");
    CHECK(m.skinFileIds.size() == 1 && m.skinFileIds[0] == 2001 && m.textureFileIds.size() == 2 && m.animFileIds.size() == 2 && m.animFileIds[1] == 4001, "M2 file ids");
    std::printf("FOREVER DATA: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
