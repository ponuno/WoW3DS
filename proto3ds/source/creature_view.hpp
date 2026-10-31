// creature_view.hpp — world units -> animated creature instances (stage 5.3/5.4, the animation stage).
// Keeps one streamed CreatureDisplay (and its mount's) and an animation state per unit; independent of the
// network code so the PC harness can drive it with synthetic units (tests/anim_test.cpp).
#pragma once
#include <stdint.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

#include "world.h"

// Weapon in the main hand: picks the attack, ready, parry animations.
enum class CreatureWeapon : uint8_t { Unarmed = 0, OneHand, TwoHand, TwoHandLoose, Bow, Rifle, Thrown };
// What a spell release looks like: a spell (hands), or a shot / throw.
enum class CastLook : uint8_t { Spell = 0, Bow, Rifle, Thrown };

struct CreatureSeen {
    uint64_t guid = 0;
    uint32_t displayId = 0;
    uint64_t look = 0;           // player in its own shape: w3d_look_key (drawn from CHRS), else 0
    W3dLook lookParts{};         // valid when look != 0
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    float scale = 1;             // object scale (OBJECT_FIELD_SCALE_X)
    // what the unit is doing (main.cpp fills it from the session; creature_view.cpp picks the animation)
    float speed = 0;             // current ground speed, yd/s
    uint32_t moveFlags = 0;      // w3n::MoveFlag: 0x2 backward, 0x4 / 0x8 strafe, 0x2000 jumping, 0x4000 falling far
    bool dead = false;
    bool swimming = false;
    bool inCombat = false;       // UNIT_FLAG_IN_COMBAT: ready stance
    bool stunned = false;        // UNIT_FLAG_STUNNED
    bool looting = false;        // our own character with a loot window open
    uint8_t standState = 0;      // UNIT_FIELD_BYTES_1 byte 0: 1 sit, 2/4/5/6 chairs, 3 sleep, 7 dead, 8 kneel
    uint16_t loopAnim = 0;       // UNIT_NPC_EMOTESTATE as an animation (working, dancing ...), 0 = none
    uint16_t emoteAnim = 0;      // the last one-shot emote (SMSG_EMOTE) as an animation ...
    uint32_t emoteMs = 0;        // ... and when it came (0 = never)
    CreatureWeapon weapon = CreatureWeapon::Unarmed;
    uint32_t swingMs = 0;        // local time of the unit's last melee swing (0 = none): plays its attack animation
    bool swingOff = false;       // with the off hand
    uint32_t hitMs = 0;          // struck in melee ...
    uint8_t hitReact = 0;        // ... 1 wound, 2 critical, 3 dodge, 4 parry, 5 block
    uint8_t casting = 0;         // 0 no, 1 cast directed, 2 cast omni, 3 channel directed, 4 channel omni, 5 at a game object (kneels)
    uint32_t castGoMs = 0;       // a spell went off ...
    bool castGoDirected = false;
    bool castGoObject = false;   // ... at a game object (opening, gathering): the unit gets up from its knees, no spell release
    CastLook castGoLook = CastLook::Spell;
    uint32_t mountDisplay = 0;   // UNIT_FIELD_MOUNTDISPLAYID: riding (the mount is drawn under the unit)
    bool flying = false;         // on a taxi flight (UNIT_FLAG_TAXI_FLIGHT): the mount flies
    float farFade = 1.0f;        // stage 6.12: < 1 at the far edge of the distance units are drawn to (main.cpp)
    // Worn items (stage 5.19): ItemDisplayInfo id and inventory type per W3dEquipSlot (0 = empty / not known yet).
    // Players: PLAYER_VISIBLE_ITEM + the item query (hidden helmet / cloak left out); creatures: the three
    // UNIT_VIRTUAL_ITEM slots (main hand, off hand, ranged).
    uint32_t equip[W3D_EQ_SLOTS] = {};
    uint8_t equipInv[W3D_EQ_SLOTS] = {};
    // Game objects (stage 5.41): doors, chests, levers play Open / Close when GAMEOBJECT_STATE changes, and rest in the pose of
    // the state; objects placed at an angle (GAMEOBJECT_ROTATION with x / y) are drawn with the full rotation.
    bool isGo = false;
    uint8_t goState = 1;         // 0 active (open, pressed), 1 ready (closed), 2 alternative
    uint32_t goStateMs = 0;      // when the state changed in front of us (0 = it was like that when first seen)
    bool tilted = false;
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // model -> render rotation when tilted (row-major)
};

// Sequence for a simple state (kept for the old callers and tests): death, attack, run / walk / swim, stand.
int creaturePickSeq(const uint8_t* blob, bool dead, float speed, bool swimming, bool attacking, float* rate, bool* loop);

// One unit's animation over time: events (swings, hits, casts, emotes, sitting down, jumping) start one-shot
// animations, the state (moving, casting, sitting, in combat ...) picks the loop under them.
struct AnimChoice { int seq = -1; float rate = 1; bool loop = true; uint16_t animId = 0; };
class UnitAnimator {
public:
    // blob = the unit's model; riding = drawn on a mount (rider pose). Returns the sequence to draw now.
    AnimChoice update(const uint8_t* blob, const CreatureSeen& c, bool riding, uint32_t nowMs);
    float timeMs() const { return timeMs_; }
    void reset() { *this = UnitAnimator(); }
    // A game object: the sequence for its state (Closed / Opened, or the end of Close / Open), the transition while one plays.
    AnimChoice updateGo(const uint8_t* blob, const CreatureSeen& c, uint32_t nowMs);
private:
    void startOneShot(const uint8_t* blob, int seq, uint8_t prio, uint32_t nowMs);
    int oneSeq_ = -1;            // one-shot playing (-1 none)
    uint8_t onePrio_ = 0;
    uint32_t oneStart_ = 0, oneDur_ = 0;
    bool oneCancelOnMove_ = false;
    int seq_ = -2;
    float timeMs_ = 0;
    uint32_t lastMs_ = 0;
    bool started_ = false, wasDead_ = false, firstSeenDead_ = false, wasAirborne_ = false;
    uint8_t lastStand_ = 0xFF, lastCasting_ = 0;
    uint32_t lastSwing_ = 0, lastHit_ = 0, lastEmote_ = 0, lastCastGo_ = 0;
};

// One item model of a unit (helmet, shoulders, a weapon): the ITEM entry and the models acquired from it.
struct UnitWorn {
    uint32_t disp = 0;                      // ItemDisplayInfo id wanted
    ItemDisplay* item = nullptr;
    bool resolved = false;                  // models taken from the ITEM entry
    Model* model[2] = {nullptr, nullptr};   // shoulders: left, right; the others: [0]
};

class CreatureView {
public:
    // Once per frame with the units to show (the caller picks them: in range, nearest first).
    // Units missing from 'seen' are dropped and their displays released.
    void update(const std::vector<CreatureSeen>& seen, uint32_t nowMs, std::vector<CreatureInstance>& out);
    // True if the unit was drawn as a model this frame (no marker needed).
    bool drawn(uint64_t guid) const;
    // Stage 6.12: the unit is tracked and its display is not in the pack (the only case a marker stands in for it).
    bool missing(uint64_t guid) const;
    bool fadeIn = true;          // stage 6.12: a unit appears over kFadeMs (the "Fade in" setting)
    // Bind-pose bounds of the model a unit is drawn with, already multiplied by the display scale (not the object scale).
    bool bounds(uint64_t guid, float bmin[3], float bmax[3]) const;
    void clear();
    // 6.36: models drawn for the first time that cannot animate (no sequences, no Stand, or a Stand that is a still pose):
    // one line each, for perf_events.txt (the caller drains them)
    std::vector<std::string> takeDiag() { std::vector<std::string> d; d.swap(diag_); return d; }
    int tracked() const { return (int)units_.size(); }
    int ready() const { return ready_; }
    // Where a mount's rider sits (world space) at this pose; false without a seat.
    static bool seatPosition(const uint8_t* mountBlob, int seq, float timeMs, bool loop, const float pos[3], float yaw,
                             float scale, float out[3]);

private:
    static constexpr int kWornSlots = 5;
    struct Unit {
        CreatureDisplay* disp = nullptr;
        CreatureDisplay* next = nullptr;        // armour changed: the new display is built while the old one is drawn
        uint64_t eqKey = 0;
        UnitWorn worn[kWornSlots];   // helmet, shoulders, main hand, off hand, ranged
        CreatureDisplay* mount = nullptr;
        uint32_t displayId = 0, mountDisplay = 0;
        uint64_t look = 0;
        bool missing = false;    // not in the pack: marker (until the display changes)
        uint32_t shownMs = 0;    // stage 6.12: first drawn (with this display) - the fade-in starts there; 0 = not yet
        uint32_t frame = 0;
        bool drawn = false;
        UnitAnimator anim, mountAnim;
    };
    std::unordered_map<uint64_t, Unit> units_;
    std::vector<std::string> diag_;
    std::unordered_set<const uint8_t*> noted_;
    void noteModel(const uint8_t* blob, uint32_t displayId);
    void noteUnit(const CreatureSeen& c);
    void notePlay(const CreatureSeen& c, const uint8_t* blob, const AnimChoice& a, bool riding);   // 6.43c: what a model plays (log)
    std::set<uint32_t> notedPlay_;
    std::unordered_set<uint32_t> notedDisplays_;
    uint32_t frame_ = 0;
    int ready_ = 0;
};
