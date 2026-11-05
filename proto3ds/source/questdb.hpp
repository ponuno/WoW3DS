// questdb.hpp — 6.63: where the objectives of a quest are (questdb.bin, made by tools/questdb/questie_import.py from Questie's
// database): the spawns of what a quest wants killed / used / collected, as blobs in zone percent, and where it is given and handed
// in. Pure (no 3DS headers), shared with the PC test.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace questdb {

// version 1 (Questie): area = zone (AreaTable id), x / y / r in zone percent x 100. Version 2 (tools/questdb/tdb_import.py, from the
// Forever database): area = map id, x / y = world position_x / position_y in yards (signed), r in yards. Quest::world says which.
struct Pt { uint16_t area; int16_t x, y; };
struct Blob { uint16_t area; int16_t x, y; uint16_t r; };
enum Kind : uint8_t { kKill = 0, kObject = 1, kItem = 2, kExplore = 3 };
struct Objective { uint8_t kind = 0; uint32_t id = 0; std::vector<Blob> blobs; };   // id: creature / object / item (explore: 0)
struct Quest { std::vector<Pt> start, end; std::vector<Objective> obj; bool world = false; };

class Db {
public:
    // Reads the index (8 bytes a quest); the records are read when asked. False: no file / not a questdb.
    bool open(const char* path);
    bool opened() const { return !path_.empty(); }
    size_t count() const { return ids_.size(); }
    bool has(uint32_t questId) const;
    bool find(uint32_t questId, Quest* out) const;
    // The record bytes -> a quest (also for tests); false when it is cut short or lies.
    static bool decode(const uint8_t* p, size_t n, Quest* out);
    uint32_t version() const { return version_; }

private:
    std::string path_;
    std::vector<uint32_t> ids_, offs_;
    uint32_t fileSize_ = 0, version_ = 0;
};

}  // namespace questdb
