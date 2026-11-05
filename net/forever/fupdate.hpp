// fupdate.hpp - SMSG_UPDATE_OBJECT of the WoW Forever server (Classic 1.60.1, TrinityCore "forever" fork), as verified against a live
// server (docs/FOREVER_WORLD_PROTOCOL.md). Parses the header and every block: positions, movement splines, entry / display id / level /
// health / faction of units and game objects. Values-update blocks are kept raw. No exceptions, every read is bounds-checked; a malformed
// packet gives false and an error text (the objects parsed up to then stay in the result).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "fbase.hpp"

namespace wf {

struct FGuid {
    uint8_t b[16] = {};
    bool empty() const { for (uint8_t v : b) if (v) return false; return true; }
    bool operator==(const FGuid& o) const { for (int i = 0; i < 16; ++i) if (b[i] != o.b[i]) return false; return true; }
    bool operator<(const FGuid& o) const { for (int i = 0; i < 16; ++i) if (b[i] != o.b[i]) return b[i] < o.b[i]; return false; }
};

enum FObjType : uint8_t { FOBJ_OBJECT = 0, FOBJ_ITEM = 1, FOBJ_CONTAINER = 2, FOBJ_UNIT = 5, FOBJ_PLAYER = 6, FOBJ_ACTIVE_PLAYER = 7, FOBJ_GAMEOBJECT = 8,
                          FOBJ_DYNAMICOBJECT = 9, FOBJ_CORPSE = 10 };
enum FUpdateType : uint8_t { FUPD_VALUES = 0, FUPD_CREATE = 1, FUPD_CREATE2 = 2 };

struct FSpline {
    bool has = false;            // the object has a spline block
    uint32_t id = 0;
    float dest[3] = {};          // final destination
    bool moving = false;         // a running spline (the path below is valid)
    uint32_t flags = 0, duration = 0;
    int32_t elapsed = 0;
    uint8_t facing = 0;          // 0 none, 1 spot, 2 target, 3 angle
    float faceAngle = 0;
    std::vector<std::array<float, 3>> path;
};

struct FObject {
    uint8_t updateType = 0, objType = 0;
    FGuid guid;
    uint32_t moveBits = 0;       // the 21 movement flag bits, bit i = i-th (MSB first)
    bool thisIsYou = false;
    bool hasPos = false;         // x y z o valid (MovementUpdate or Stationary)
    float x = 0, y = 0, z = 0, o = 0;
    uint64_t moveFlags = 0;      // MovementUpdate only
    bool hasSpeeds = false;
    float speeds[9] = {};        // walk run runBack swim swimBack flight flightBack turn pitch
    FSpline spline;
    uint64_t rotation = 0;       // game objects: packed local rotation
    std::vector<uint8_t> frags;  // fragment ids on the wire
    uint8_t fieldFlags = 0;
    bool fieldsDecoded = false;  // entry.. below are valid
    int32_t entry = 0;
    uint32_t dynFlags = 0;
    float scale = 1.0f;
    // units
    int32_t display = 0, level = 0, faction = 0;
    uint32_t npcFlags = 0;
    uint8_t race = 0, classId = 0, sex = 0, creatureType = 0;
    int64_t health = 0, maxHealth = 0;
    // game objects
    uint32_t goFlags = 0;
    int8_t goState = 0, goType = 0;
};

struct FValues { FGuid guid; std::vector<uint8_t> data; };

struct FUpdate {
    uint16_t map = 0;
    std::vector<FObject> objects;
    std::vector<FValues> values;
    std::vector<FGuid> destroyed;     // destroyed and out-of-range, as the server lists them
};

bool parseUpdateObject(const uint8_t* data, size_t size, FUpdate& out, std::string& err);
inline bool parseUpdateObject(const Bytes& d, FUpdate& out, std::string& err) { return parseUpdateObject(d.data(), d.size(), out, err); }

}  // namespace wf
