// objects.hpp — vanilla 1.12.1 world object table: SMSG_(COMPRESSED_)UPDATE_OBJECT,
// SMSG_DESTROY_OBJECT, SMSG_MONSTER_MOVE / SMSG_COMPRESSED_MOVES, relayed
// MSG_MOVE_* of other players, and name/creature/gameobject query answers.
//
// Layouts follow WoWee (ClassicPacketParsers / TbcPacketParsers::parseUpdateObject,
// parseClassicMoveUpdateSpline) and vMaNGOS PacketBuilder::WriteMonsterMove.
// Positions are server coordinates (== render coordinates in wow3ds).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "movement.hpp"
#include "spline.hpp"
#include "transport.hpp"

namespace w3n {

enum class ObjType : uint8_t { Object = 0, Item = 1, Container = 2, Unit = 3, Player = 4, GameObject = 5,
                               DynamicObject = 6, Corpse = 7 };
const char* objTypeName(ObjType t);

// Update field indices for 1.12.1 (WoWee Data/expansions/classic/update_fields.json).
namespace UF {
enum : uint16_t {
    OBJECT_GUID = 0, OBJECT_TYPE = 2, OBJECT_ENTRY = 3, OBJECT_SCALE_X = 4,
    UNIT_TARGET = 16, UNIT_HEALTH = 22, UNIT_POWER1 = 23, UNIT_MAXHEALTH = 28, UNIT_MAXPOWER1 = 29,
    UNIT_LEVEL = 34, UNIT_FACTIONTEMPLATE = 35, UNIT_BYTES_0 = 36, UNIT_VIRTUAL_ITEM_SLOT_DISPLAY = 37, UNIT_VIRTUAL_ITEM_INFO = 40, UNIT_FLAGS = 46,
    UNIT_FIELD_AURA = 47, UNIT_FIELD_AURAFLAGS = 95, UNIT_FIELD_AURALEVELS = 101, UNIT_FIELD_AURAAPPLICATIONS = 113,
    UNIT_BOUNDINGRADIUS = 129, UNIT_COMBATREACH = 130, UNIT_DISPLAYID = 131, UNIT_NATIVEDISPLAYID = 132,
    UNIT_FIELD_CHANNEL_OBJECT = 20,
    UNIT_MOUNTDISPLAYID = 133, UNIT_BYTES_1 = 138, UNIT_DYNAMIC_FLAGS = 143, UNIT_CHANNEL_SPELL = 144, UNIT_NPC_FLAGS = 147,
    UNIT_NPC_EMOTESTATE = 148,
    UNIT_END = 188,
    // PLAYER_BYTES/_2 are 193/194 (vMaNGOS UpdateFields_1_12_1.h: GUILDID 191 and GUILDRANK 192 come first;
    // WoWee's classic update_fields.json has 191/192, which reads the guild fields as the look - confirmed on
    // the real Stone Tavern log: 193 = 0x03080601 skin 1 face 6 hair 8 colour 3, 194 = facial hair 2).
    PLAYER_FLAGS = 190, PLAYER_BYTES = 193, PLAYER_BYTES_2 = 194,
    PLAYER_VISIBLE_ITEM_1_0 = 260, PLAYER_VISIBLE_ITEM_STRIDE = 12,   // entry of equipment slot k: 260 + 12 k
    PLAYER_FIELD_INV_SLOT_HEAD = 486,   // 23 equipped + 4 bags + 16 backpack slots, u64 GUID each (2 fields)
    PLAYER_XP = 716, PLAYER_NEXT_LEVEL_XP = 717, PLAYER_FIELD_COINAGE = 1176,
    PLAYER_END = 1282,
    // items and bags
    ITEM_FIELD_OWNER = 6, ITEM_FIELD_CONTAINED = 8, ITEM_FIELD_STACK_COUNT = 14, ITEM_FIELD_DURABILITY = 46,
    ITEM_FIELD_MAXDURABILITY = 47, CONTAINER_FIELD_NUM_SLOTS = 48, CONTAINER_FIELD_SLOT_1 = 50,
    // corpses
    CORPSE_FIELD_OWNER = 6,
    GAMEOBJECT_DISPLAYID = 8, GAMEOBJECT_FLAGS = 9, GAMEOBJECT_STATE = 14, GAMEOBJECT_DYN_FLAGS = 19, GAMEOBJECT_TYPE_ID = 21,
};
}


struct WorldObject {
    uint64_t guid = 0;
    ObjType type = ObjType::Object;
    bool self = false;
    bool hasPos = false;
    MovementInfo mv;                 // last authoritative movement (position, flags, orientation)
    uint32_t posMs = 0;              // local time when 'mv' was received
    MoveSpeeds speeds;
    // Server-driven spline (SMSG_MONSTER_MOVE or SPLINE_ENABLED in a movement block).
    std::vector<Vec3f> path;         // start point first
    uint32_t pathStartMs = 0, pathDurMs = 0;
    bool pathActive = false, pathHasFinalO = false;
    bool pathSmooth = false;         // flying spline (taxi flights): Catmull-Rom through the points, like the server's MoveSpline
    float pathFinalO = 0;
    // Transports (stage 5.21). A GO transport: the server's path progress when its create block arrived (UPDATEFLAG_TRANSPORT).
    uint32_t transportProgress = 0;
    bool hasTransportProgress = false;
    // A unit standing on a transport (MoveFlag::OnTransport): ObjectTable::tickTransports keeps mv at the world position
    // computed from the transport's pose and the unit's offset, and 'carried' turns the usual extrapolation off.
    bool carried = false;
    uint32_t carriedStamp = 0, carriedBaseMs = 0;   // mv.time of the last packet seen, local time it was seen
    float carriedBase[3] = {0, 0, 0}, carriedO = 0;               // offset (x, y, z) in that packet
    std::vector<uint32_t> fields;    // update fields (index -> value), grown on demand
    std::string name, subname;       // from the query caches
    // Melee (stage 5.7, from SMSG_ATTACKSTART/STOP/ATTACKERSTATEUPDATE): drives attack animations.
    uint64_t meleeVictim = 0;        // who it is swinging at (0 = not in melee)
    uint32_t swingMs = 0;            // local time of its last swing (0 = never)
    bool swingOff = false;           // that swing was the off hand (HITINFO_LEFTSWING)
    // Animation events (the animation stage; local times, 0 = never): drive creature_view.cpp
    uint32_t hitMs = 0;              // struck in melee
    uint8_t hitReact = 0;            // 1 wound, 2 critical, 3 dodge, 4 parry, 5 block
    uint32_t emoteMs = 0, emote = 0; // SMSG_EMOTE: a one-shot Emotes.dbc id
    uint32_t castGoMs = 0;           // a spell went off (SMSG_SPELL_GO)
    uint32_t castGoSpell = 0;
    bool castGoDirected = false;     // at someone else (the hands point) or around itself
    bool taxiFlight() const { return isUnitLike() && (field(UF::UNIT_FLAGS) & 0x00100000u) != 0; }   // UNIT_FLAG_TAXI_FLIGHT
    uint8_t standState() const { return (uint8_t)(field(UF::UNIT_BYTES_1) & 0xFF); }   // 0 stand, 1 sit, 3 sleep, 8 kneel ...
    uint32_t emoteState() const { return field(UF::UNIT_NPC_EMOTESTATE); }             // looping Emotes.dbc id (work, dance)

    uint32_t field(uint16_t i) const { return i < fields.size() ? fields[i] : 0; }
    float fieldF(uint16_t i) const;
    uint32_t entry() const { return field(UF::OBJECT_ENTRY); }
    uint32_t level() const { return field(UF::UNIT_LEVEL); }
    uint32_t health() const { return field(UF::UNIT_HEALTH); }
    uint32_t maxHealth() const { return field(UF::UNIT_MAXHEALTH); }
    uint32_t displayId() const { return type == ObjType::GameObject ? field(UF::GAMEOBJECT_DISPLAYID) : field(UF::UNIT_DISPLAYID); }
    bool isUnitLike() const { return type == ObjType::Unit || type == ObjType::Player; }
    bool dead() const { return isUnitLike() && fields.size() > UF::UNIT_HEALTH && health() == 0; }
    // Interpolated position at local time nowMs (splines, or players extrapolated from their key state).
    void positionAt(uint32_t nowMs, float out[3], float* orient = nullptr) const;
    // Ground speed (yd/s) at local time nowMs: spline speed while a server path runs,
    // key-state speed for players, else 0. Drives walk/run animation choice.
    float groundSpeedAt(uint32_t nowMs) const;
    bool swimming() const { return (mv.flags & MoveFlag::Swimming) != 0; }
};

struct ObjectStats {
    uint32_t updatePackets = 0, compressedPackets = 0, failedPackets = 0;
    uint32_t blocks[6] = {0, 0, 0, 0, 0, 0};   // VALUES, MOVEMENT, CREATE, CREATE2, OUT_OF_RANGE, NEAR
    uint32_t destroys = 0, monsterMoves = 0, monsterMoveFails = 0, compressedMoves = 0, playerMoves = 0;
    uint32_t nameAnswers = 0, creatureAnswers = 0, gameobjectAnswers = 0;
    uint64_t inflatedIn = 0, inflatedOut = 0;
    std::string lastError;
};

class ObjectTable {
public:
    void clear();
    void setSelf(uint64_t guid) { self_ = guid; }
    uint64_t self() const { return self_; }
    // Returns true when the opcode belongs to the object layer (consumed).
    bool handle(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    // Parse a raw (already inflated) SMSG_UPDATE_OBJECT body.
    bool parseUpdate(const uint8_t* p, size_t n, uint32_t nowMs);

    WorldObject* find(uint64_t g);
    const WorldObject* find(uint64_t g) const;
    const std::unordered_map<uint64_t, WorldObject>& all() const { return objs_; }
    size_t count(ObjType t) const;

    // Objects created since the last call (the session sends name/creature queries for them).
    std::vector<uint64_t> takeCreated() { std::vector<uint64_t> v; v.swap(created_); return v; }
    bool hasPlayerName(uint64_t g) const { return playerNames_.count(g) != 0; }
    bool hasCreatureInfo(uint32_t entry) const { return creatureNames_.count(entry) != 0; }
    bool hasGameObjectInfo(uint32_t entry) const { return goNames_.count(entry) != 0; }
    // SMSG_GAMEOBJECT_QUERY_RESPONSE: type and the 24 data values (chest / goober / quest giver: data[0] = lock id)
    struct GoInfo { uint32_t type = 0; uint32_t data[24] = {}; };
    void setGoInfo(uint32_t entry, const GoInfo& gi) { goInfo_[entry] = gi; }
    const GoInfo* goInfo(uint32_t entry) const { auto it = goInfo_.find(entry); return it == goInfo_.end() ? nullptr : &it->second; }
    // Best known name of any GUID (players from the name cache even when out of range); "" if unknown.
    std::string nameOf(uint64_t g) const;

    // Transports (stage 5.21): transports.txt is read on first use; call tickTransports once per frame.
    void setTransportsFile(const std::string& path) { transportsFile_ = path; transportsTried_ = false; }
    TransportTable transports;
    // Moves every known ship / zeppelin (GO type 15) along its path and carries the units standing on them.
    void tickTransports(uint32_t nowMs);
    // Pose of a transport as of the last tick (null for other objects).
    const TransportPose* transportPose(uint64_t guid) const {
        auto it = poses_.find(guid);
        return it == poses_.end() ? nullptr : &it->second;
    }
    const std::unordered_map<uint64_t, TransportPose>& transportPoses() const { return poses_; }

    ObjectStats stats;
    // Hook for self-movement packets that need an answer (teleports are handled by the session).
    uint32_t maxFieldsOther = UF::PLAYER_END;   // fields kept for objects other than self

private:
    bool parseMovementBlock(Reader& r, WorldObject& o, uint32_t nowMs);
    bool parseFields(Reader& r, WorldObject* o);
    bool parseMonsterMove(Reader& r, uint32_t nowMs);
    void remove(uint64_t g);
    void applyNames(WorldObject& o);
    uint32_t fieldCap(const WorldObject& o) const;

    uint64_t self_ = 0;
    std::unordered_map<uint64_t, WorldObject> objs_;
    std::unordered_map<uint64_t, std::string> playerNames_;
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> creatureNames_;
    std::unordered_map<uint32_t, std::string> goNames_;
    std::unordered_map<uint32_t, GoInfo> goInfo_;
    std::vector<uint64_t> created_;
    std::unordered_map<uint64_t, TransportPose> poses_;
    std::string transportsFile_;
    bool transportsTried_ = false;
};

// Object-layer opcodes.
namespace ObjOp {
enum : uint16_t {
    CMSG_NAME_QUERY = 0x050, SMSG_NAME_QUERY_RESPONSE = 0x051,
    CMSG_GAMEOBJECT_QUERY = 0x05E, SMSG_GAMEOBJECT_QUERY_RESPONSE = 0x05F,
    CMSG_CREATURE_QUERY = 0x060, SMSG_CREATURE_QUERY_RESPONSE = 0x061,
    SMSG_UPDATE_OBJECT = 0x0A9, SMSG_DESTROY_OBJECT = 0x0AA, SMSG_MONSTER_MOVE = 0x0DD,
    SMSG_COMPRESSED_UPDATE_OBJECT = 0x1F6, SMSG_COMPRESSED_MOVES = 0x2FB,
};
}

}  // namespace w3n
