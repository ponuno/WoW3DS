#include "fupdate.hpp"

#include <cstring>

namespace wf {
namespace {

struct Rd {
    In in;
    Rd(const uint8_t* d, size_t n) : in(d, n) {}
    uint8_t u8() { return in.u8(); }
    uint16_t u16() { uint16_t lo = in.u8(); return (uint16_t)(lo | (in.u8() << 8)); }
    uint32_t u32() { return in.u32(); }
    int32_t i32() { return (int32_t)in.u32(); }
    uint64_t u64() { return in.u64(); }
    float f() { uint32_t u = in.u32(); float v; memcpy(&v, &u, 4); return v; }
    bool bit() { return in.bit(); }
    uint32_t bits(int n) { return in.bits(n); }
    void flush() { in.resetBits(); }
    void skip(size_t n) { in.resetBits(); if (in.pos + n > in.n) { in.bad = true; in.pos = in.n; } else in.pos += n; }
    bool ok() const { return !in.bad; }
    FGuid guid() {
        FGuid g;
        flush();
        const uint32_t lo = in.u8(), hi = in.u8();
        const uint32_t mask = lo | (hi << 8);
        for (int i = 0; i < 16; ++i) if (mask & (1u << i)) g.b[i] = in.u8();
        return g;
    }
};

constexpr int kBitMovementUpdate = 5, kBitTransport = 6, kBitStationary = 7, kBitCombatVictim = 8, kBitServerTime = 9, kBitVehicle = 10,
              kBitAnimKit = 11, kBitRotation = 12, kBitGameObject = 13, kBitSmooth = 14, kBitScene = 15, kBitActivePlayer = 16, kBitConversation = 17,
              kBitRoom = 18, kBitDecor = 19, kBitMesh = 20, kBitThisIsYou = 4;
inline bool has(const FObject& o, int bit) { return (o.moveBits >> (20 - bit)) & 1; }

bool readMovement(Rd& r, FObject& o, std::string& err) {
    for (int i = 0; i < 21; ++i) o.moveBits = (o.moveBits << 1) | (r.bit() ? 1u : 0u);
    r.flush();
    o.thisIsYou = has(o, kBitThisIsYou);
    const uint32_t pause = r.u32();
    if (pause > 4096) { err = "implausible pause time count"; return false; }
    r.skip((size_t)pause * 4);
    if (has(o, kBitMovementUpdate)) {
        r.guid();
        o.moveFlags = r.u64();
        r.u32();
        o.x = r.f(); o.y = r.f(); o.z = r.f(); o.o = r.f();
        o.hasPos = true;
        r.f(); r.f();
        const uint32_t nForces = r.u32();
        r.u32(); r.f();
        if (nForces > 64) { err = "implausible remove-forces count"; return false; }
        for (uint32_t i = 0; i < nForces; ++i) r.guid();
        const bool standOn = r.bit(), transport = r.bit(), fall = r.bit(), spline = r.bit();
        r.bit(); r.bit();
        const bool inertia = r.bit(), adv = r.bit(), drive = r.bit();
        r.flush();
        if (standOn) r.guid();
        if (transport) { err = "unit on a transport (layout not verified yet)"; return false; }
        if (fall) {
            r.u32(); r.f();
            if (r.bit()) { r.f(); r.f(); r.f(); }
            r.flush();
        }
        if (inertia || adv || drive) { err = "inertia / advanced flying / drive status (layout not verified yet)"; return false; }
        for (int i = 0; i < 9; ++i) o.speeds[i] = r.f();
        o.hasSpeeds = true;
        const uint32_t nMove = r.u32();
        r.f();
        for (int i = 0; i < 17; ++i) r.f();
        if (nMove) { err = "movement forces present (layout not verified yet)"; return false; }
        const bool hasSpline = r.bit();
        r.flush();
        (void)spline;
        if (hasSpline) {
            FSpline& s = o.spline;
            s.has = true;
            s.id = r.u32();
            s.dest[0] = r.f(); s.dest[1] = r.f(); s.dest[2] = r.f();
            const bool move = r.bit();
            r.flush();
            if (move) {
                s.moving = true;
                s.flags = r.u32();
                s.facing = r.u8();
                s.elapsed = r.i32();
                s.duration = r.u32();
                r.f(); r.f();
                if (s.facing == 1) { r.f(); r.f(); r.f(); }
                else if (s.facing == 2) r.guid();
                else if (s.facing == 3) s.faceAngle = r.f();
                const bool fade = r.bit();
                const uint32_t n = r.bits(16);
                const bool filter = r.bit(), spellExtra = r.bit(), jump = r.bit(), turn = r.bit(), tier = r.bit(), visual = r.bit();
                r.flush();
                if (filter || visual) { err = "spline filter / visual data (layout not verified yet)"; return false; }
                if (n > 4096) { err = "implausible spline path length"; return false; }
                if (fade) r.u32();
                s.path.resize(n);
                for (uint32_t i = 0; i < n && r.ok(); ++i) { s.path[i][0] = r.f(); s.path[i][1] = r.f(); s.path[i][2] = r.f(); }
                if (spellExtra) { r.guid(); r.u32(); r.u32(); r.u32(); r.f(); }
                if (jump) { r.f(); r.u32(); r.u32(); }
                if (turn) { r.f(); r.f(); r.f(); }
                if (tier) { r.i32(); r.u8(); r.u32(); r.u32(); }
            }
        }
    }
    if (has(o, kBitTransport)) { err = "object on a transport (layout not verified yet)"; return false; }
    if (has(o, kBitStationary)) { o.x = r.f(); o.y = r.f(); o.z = r.f(); o.o = r.f(); o.hasPos = true; }
    if (has(o, kBitCombatVictim)) r.guid();
    if (has(o, kBitServerTime)) r.u32();
    if (has(o, kBitVehicle)) { r.u32(); r.f(); }
    if (has(o, kBitAnimKit)) { r.u16(); r.u16(); r.u16(); }
    if (has(o, kBitRotation)) o.rotation = r.u64();
    if (has(o, kBitGameObject)) {
        r.u32();
        const bool b8 = r.bit(), isTransport = r.bit(), progress = r.bit();
        r.flush();
        if (isTransport) { r.u32(); r.u32(); r.bit(); r.bit(); r.bit(); r.flush(); }
        if (b8) r.u32();
        if (progress) r.f();
    }
    if (has(o, kBitSmooth) || has(o, kBitScene) || has(o, kBitConversation) || has(o, kBitRoom) || has(o, kBitDecor) || has(o, kBitMesh)) {
        err = "smooth phasing / scene / conversation object (layout not verified yet)";
        return false;
    }
    if (has(o, kBitActivePlayer)) {
        const bool scenes = r.bit(), runes = r.bit();
        r.flush();
        if (scenes) {
            const uint32_t n = r.u32();
            if (n > 1024) { err = "implausible scene count"; return false; }
            r.skip((size_t)n * 4);
        }
        if (runes) {
            r.u8(); r.u8();
            const uint32_t n = r.u32();
            if (n > 16) { err = "implausible rune count"; return false; }
            r.skip(n);
        }
    }
    return r.ok();
}

// ObjectData + UnitData of a creature as another player sees it (field flags 0; the owner / empath conditional fields are not there).
void readUnitFields(Rd& r, FObject& o) {
    r.u8();   // the indirect fragment is active
    o.entry = r.i32(); o.dynFlags = r.u32(); o.scale = r.f();
    o.display = r.i32(); o.npcFlags = r.u32();
    r.u32(); r.u32(); r.u32(); r.u32();
    const uint32_t nEffects = r.u32();
    r.u32(); r.i32();
    if (nEffects > 256) { r.in.bad = true; return; }
    r.skip((size_t)nEffects * 4);
    for (int i = 0; i < 9; ++i) r.guid();     // charm, summon, charmed by, summoned by, created by, demon creator, look-at, target, battle pet
    r.u64(); r.guid(); r.guid();
    r.i32(); r.i32(); r.i32(); r.u32(); r.u32();   // channel
    r.u8();
    r.u32();
    o.race = r.u8(); o.classId = r.u8(); r.u8(); o.sex = r.u8(); o.creatureType = r.u8(); r.u8();
    r.u32();
    o.health = (int64_t)r.u64();
    r.skip(20 * 4);
    o.maxHealth = (int64_t)r.u64();
    o.level = r.i32();
    r.skip(5 * 4);
    r.u8();
    o.faction = r.i32();
}

void readGoFields(Rd& r, FObject& o) {
    r.u8();
    o.entry = r.i32(); o.dynFlags = r.u32(); o.scale = r.f();
    o.display = r.i32();
    r.u32(); r.u32(); r.u32(); r.u32();
    const uint32_t nEffects = r.u32();
    r.u32();
    if (nEffects > 256) { r.in.bad = true; return; }
    r.skip((size_t)nEffects * 4);
    r.guid(); r.guid();
    o.goFlags = r.u32();
    r.u32();
    r.skip(16);
    o.faction = r.i32();
    r.i32();
    o.goState = (int8_t)r.u8();
    o.goType = (int8_t)r.u8();
}

}  // namespace

bool parseUpdateObject(const uint8_t* data, size_t size, FUpdate& out, std::string& err) {
    out = FUpdate();
    err.clear();
    Rd r(data, size);
    out.map = r.u16();
    const uint32_t count = r.u32();
    r.bit();
    const bool hasDestroy = r.bit();
    r.flush();
    if (hasDestroy) {
        r.u16();
        const uint32_t total = r.u32();
        if (total > 65536) { err = "implausible destroy list"; return false; }
        for (uint32_t i = 0; i < total && r.ok(); ++i) out.destroyed.push_back(r.guid());
    }
    const uint32_t dataSize = r.u32();
    if (!r.ok()) { err = "short header"; return false; }
    if (dataSize > r.in.left()) { err = "data size " + std::to_string(dataSize) + " exceeds the packet"; return false; }
    const size_t end = r.in.pos + dataSize;
    if (count > 1000000) { err = "implausible block count"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t type = r.u8();
        if (!r.ok()) { err = "block " + std::to_string(i) + ": truncated"; return false; }
        if (type == FUPD_VALUES) {
            FValues v;
            v.guid = r.guid();
            const uint32_t sz = r.u32();
            if (!r.ok() || sz > r.in.left()) { err = "values block " + std::to_string(i) + ": bad size"; return false; }
            v.data.assign(r.in.p + r.in.pos, r.in.p + r.in.pos + sz);
            r.skip(sz);
            out.values.push_back(std::move(v));
            continue;
        }
        if (type != FUPD_CREATE && type != FUPD_CREATE2) { err = "block " + std::to_string(i) + ": unknown update type " + std::to_string(type); return false; }
        FObject o;
        o.updateType = type;
        o.guid = r.guid();
        o.objType = r.u8();
        std::string e;
        if (!r.ok() || !readMovement(r, o, e)) { err = "block " + std::to_string(i) + ": " + (e.empty() ? "truncated movement" : e); return false; }
        const uint32_t sz = r.u32();
        if (!r.ok() || sz < 2 || sz > r.in.left()) { err = "block " + std::to_string(i) + ": bad field size"; return false; }
        const size_t fieldsEnd = r.in.pos + sz;
        o.fieldFlags = r.u8();
        for (;;) {
            const uint8_t f = r.u8();
            if (!r.ok()) { err = "block " + std::to_string(i) + ": truncated fragment list"; return false; }
            if (f == 0xFF) break;
            o.frags.push_back(f);
            if (o.frags.size() > 64) { err = "block " + std::to_string(i) + ": too many fragments"; return false; }
        }
        // only creatures and game objects as other players see them are decoded; items, players and owner views stay raw
        bool hasObjectData = false;
        for (uint8_t f : o.frags) if (f == 3) hasObjectData = true;
        if (hasObjectData && o.fieldFlags == 0 && (o.objType == FOBJ_UNIT || o.objType == FOBJ_GAMEOBJECT)) {
            if (o.objType == FOBJ_UNIT) readUnitFields(r, o); else readGoFields(r, o);
            o.fieldsDecoded = r.ok() && r.in.pos <= fieldsEnd;
            if (!r.ok()) { err = "block " + std::to_string(i) + ": truncated fields"; return false; }
        }
        r.in.resetBits();
        r.in.pos = fieldsEnd;
        out.objects.push_back(std::move(o));
    }
    if (r.in.pos != end) { err = "blocks end at " + std::to_string(r.in.pos) + ", data size says " + std::to_string(end); return false; }
    return true;
}

}  // namespace wf
