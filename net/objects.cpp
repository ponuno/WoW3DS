#include "objects.hpp"

#include <cmath>
#include <cstring>

#include "inflate.hpp"

namespace w3n {

namespace {

// Classic UpdateFlags (u8). 1.12 values: these differ from TBC/WotLK (see WoWee comments).
enum : uint8_t {
    UPDATEFLAG_SELF = 0x01, UPDATEFLAG_TRANSPORT = 0x02, UPDATEFLAG_MELEE_ATTACKING = 0x04,
    UPDATEFLAG_HIGHGUID = 0x08, UPDATEFLAG_ALL = 0x10, UPDATEFLAG_LIVING = 0x20, UPDATEFLAG_HAS_POSITION = 0x40,
};
enum : uint8_t { UPD_VALUES = 0, UPD_MOVEMENT = 1, UPD_CREATE = 2, UPD_CREATE2 = 3, UPD_OUT_OF_RANGE = 4, UPD_NEAR = 5 };
// Pre-WotLK spline flags.
enum : uint32_t {
    SPLINE_FLYING = 0x00000200, SPLINE_FINAL_POINT = 0x00010000, SPLINE_FINAL_TARGET = 0x00020000,
    SPLINE_FINAL_ANGLE = 0x00040000, SPLINE_CATMULLROM = 0x00080000, SPLINE_CYCLIC = 0x00100000,
    SPLINE_ENTER_CYCLE = 0x00200000,
};

// mangos appendPackXYZ: 11-bit x, 11-bit y, 10-bit z, signed, quarter-yard units,
// stored as (middle - point).
Vec3f unpackDelta(uint32_t p, const Vec3f& mid) {
    int32_t x = (int32_t)(p & 0x7FF); if (x & 0x400) x -= 0x800;
    int32_t y = (int32_t)((p >> 11) & 0x7FF); if (y & 0x400) y -= 0x800;
    int32_t z = (int32_t)((p >> 22) & 0x3FF); if (z & 0x200) z -= 0x400;
    return {mid.x - x * 0.25f, mid.y - y * 0.25f, mid.z - z * 0.25f};
}

Vec3f readVec(Reader& r) {
    Vec3f v;
    v.x = r.f32(); v.y = r.f32(); v.z = r.f32();
    return v;
}

bool finite3(const Vec3f& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

const char* objTypeName(ObjType t) {
    static const char* n[] = {"object", "item", "container", "unit", "player", "gameobject", "dynobject", "corpse"};
    return (uint8_t)t < 8 ? n[(uint8_t)t] : "?";
}

float WorldObject::fieldF(uint16_t i) const {
    uint32_t v = field(i);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

float WorldObject::groundSpeedAt(uint32_t nowMs) const {
    if (pathActive && path.size() >= 2 && pathDurMs) {
        int32_t dt = (int32_t)(nowMs - pathStartMs);
        if (dt < 0 || (uint32_t)dt >= pathDurMs) return 0;
        float total = 0;
        for (size_t i = 1; i < path.size(); ++i) total += dist3(path[i - 1], path[i]);
        return total * 1000.0f / (float)pathDurMs;
    }
    if (type == ObjType::Player && (mv.flags & MoveFlag::MaskMoving) && !(mv.flags & MoveFlag::Root))
        return speeds.forFlags(mv.flags);
    return 0;
}

void WorldObject::positionAt(uint32_t nowMs, float out[3], float* orient) const {
    out[0] = mv.x; out[1] = mv.y; out[2] = mv.z;
    float o = mv.o;
    if (carried) {   // on a transport: tickTransports already put mv where the unit is now
        if (orient) *orient = normalizeAngle(o);
        return;
    }
    if (pathActive && pathSmooth && path.size() >= 3) {
        const size_t segs = path.size() - 1;
        std::vector<float> cum(segs + 1, 0.0f);
        for (size_t s = 0; s < segs; ++s) cum[s + 1] = cum[s] + crSegLength(path, s);
        float t = pathDurMs ? (float)(int32_t)(nowMs - pathStartMs) / (float)pathDurMs : 1.0f;
        t = std::fmin(std::fmax(t, 0.0f), 1.0f);
        const float want = t * cum[segs];
        size_t seg = 0;
        while (seg + 1 < segs && cum[seg + 1] < want) ++seg;
        float len = cum[seg + 1] - cum[seg];
        float u = len > 1e-5f ? std::fmin(1.0f, std::fmax(0.0f, (want - cum[seg]) / len)) : 1.0f;
        Vec3f p[4];
        crSegment(path, seg, p);
        float w[4], dw[4];
        crWeights(u, w);
        crDerivWeights(u, dw);
        Vec3f at = crEval(p, w), d = crEval(p, dw);
        out[0] = at.x; out[1] = at.y; out[2] = at.z;
        if (std::fabs(d.x) + std::fabs(d.y) > 1e-4f) o = std::atan2(d.y, d.x);
        if (t >= 1.0f && pathHasFinalO) o = pathFinalO;
    } else if (pathActive && path.size() >= 2) {
        float total = 0;
        for (size_t i = 1; i < path.size(); ++i)
            total += dist3(path[i - 1], path[i]);
        float t = pathDurMs ? (float)(int32_t)(nowMs - pathStartMs) / (float)pathDurMs : 1.0f;
        if (t < 0) t = 0;
        float want = t >= 1.0f ? total : t * total;
        size_t seg = 1;
        float acc = 0;
        for (; seg < path.size(); ++seg) {
            const Vec3f &a = path[seg - 1], &b = path[seg];
            float len = dist3(a, b);
            if (acc + len >= want || seg + 1 == path.size()) {
                float k = len > 1e-4f ? std::fmin(1.0f, (want - acc) / len) : 1.0f;
                out[0] = a.x + (b.x - a.x) * k;
                out[1] = a.y + (b.y - a.y) * k;
                out[2] = a.z + (b.z - a.z) * k;
                if (std::fabs(b.x - a.x) + std::fabs(b.y - a.y) > 1e-3f) o = std::atan2(b.y - a.y, b.x - a.x);
                break;
            }
            acc += len;
        }
        if (t >= 1.0f && pathHasFinalO) o = pathFinalO;
    } else if (type == ObjType::Player && (mv.flags & MoveFlag::MaskMoving) && !(mv.flags & MoveFlag::Root)) {
        // Players: dead-reckon from the last relayed key state (heartbeats arrive every 0.5 s).
        uint32_t dtMs = nowMs - posMs;
        if ((int32_t)dtMs < 0) dtMs = 0;
        if (dtMs > 1500) dtMs = 1500;
        float speed = speeds.forFlags(mv.flags);
        float fx = (mv.flags & MoveFlag::Forward) ? 1.f : (mv.flags & MoveFlag::Backward) ? -1.f : 0.f;
        float sx = (mv.flags & MoveFlag::StrafeLeft) ? 1.f : (mv.flags & MoveFlag::StrafeRight) ? -1.f : 0.f;
        float a = mv.o + std::atan2(sx, fx);
        float d = speed * dtMs / 1000.0f;
        out[0] += std::cos(a) * d;
        out[1] += std::sin(a) * d;
    }
    if (orient) *orient = normalizeAngle(o);
}

bool WorldObject::goRotation(float q[4]) const {
    if (type != ObjType::GameObject || fields.size() <= (size_t)UF::GAMEOBJECT_ROTATION + 3) return false;
    for (int i = 0; i < 4; ++i) {
        q[i] = fieldF((uint16_t)(UF::GAMEOBJECT_ROTATION + i));
        if (!std::isfinite(q[i]) || std::fabs(q[i]) > 1.5f) return false;
    }
    const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (n2 < 0.5f || n2 > 1.5f) return false;   // not a unit quaternion (all zero before the server sets it)
    const float inv = 1.0f / std::sqrt(n2);
    for (int i = 0; i < 4; ++i) q[i] *= inv;
    return true;
}

bool WorldObject::goTilt(float q[4]) const {
    if (!goRotation(q)) return false;
    return std::fabs(q[0]) > 0.02f || std::fabs(q[1]) > 0.02f;   // ~2.3 degrees
}

void ObjectTable::tickTransports(uint32_t nowMs) {
    if (!transports.loaded() && !transportsTried_ && !transportsFile_.empty()) {
        transportsTried_ = true;
        transports.load(transportsFile_);
    }
    poses_.clear();
    if (!transports.loaded()) return;
    for (auto& [g, o] : objs_) {
        if (o.type != ObjType::GameObject || !o.hasTransportProgress) continue;
        if (o.goType() == 11) {   // elevators and trams (stage 5.41): the key points of TransportAnimation.dbc, not a taxi path
            const ElevatorAnim* ea = transports.elevator(o.entry());
            if (!ea) continue;
            float q[4] = {0, 0, 0, 1};
            o.goRotation(q);
            TransportPose p = elevatorPose(*ea, o.transportProgress + (nowMs - o.posMs), o.stationary, q);
            if (!p.valid) continue;
            poses_[g] = p;
            o.mv.x = p.x; o.mv.y = p.y; o.mv.z = p.z; o.mv.o = p.o;
            o.hasPos = true;
            continue;
        }
        const GoInfo* gi = goInfo(o.entry());
        if (!gi || gi->type != 15) continue;
        float speed, accel;
        { float s = (float)gi->data[1], a = (float)gi->data[2]; speed = s; accel = a; }
        const TransportPath* path = transports.path(gi->data[0], speed, accel);
        if (!path) continue;
        TransportPose p = path->at(o.transportProgress + (nowMs - o.posMs));
        if (!p.valid) continue;
        poses_[g] = p;
        o.mv.x = p.x; o.mv.y = p.y; o.mv.z = p.z; o.mv.o = p.o;
        o.hasPos = true;
    }
    if (poses_.empty()) {
        for (auto& [g, o] : objs_) o.carried = false;
        return;
    }
    for (auto& [g, o] : objs_) {
        if (g == self_ || o.self) continue;
        if (!(o.mv.flags & MoveFlag::OnTransport) || !o.isUnitLike()) { o.carried = false; continue; }
        auto it = poses_.find(o.mv.transportGuid);
        if (it == poses_.end()) { o.carried = false; continue; }
        if (!o.carried || o.carriedStamp != o.mv.time) {   // a fresh packet: its offsets are the new base
            o.carriedStamp = o.mv.time;
            o.carriedBaseMs = nowMs;
            o.carriedBase[0] = o.mv.tx; o.carriedBase[1] = o.mv.ty; o.carriedBase[2] = o.mv.tz;
            o.carriedO = o.mv.to;
            o.carried = true;
        }
        float lx = o.carriedBase[0], ly = o.carriedBase[1];
        if (o.mv.flags & MoveFlag::MaskMoving) {   // walking on deck between packets
            float dtMs = (float)std::min<uint32_t>(nowMs - o.carriedBaseMs, 1500);
            float speed = o.speeds.forFlags(o.mv.flags);
            float fx = (o.mv.flags & MoveFlag::Forward) ? 1.f : (o.mv.flags & MoveFlag::Backward) ? -1.f : 0.f;
            float sx = (o.mv.flags & MoveFlag::StrafeLeft) ? 1.f : (o.mv.flags & MoveFlag::StrafeRight) ? -1.f : 0.f;
            float a = o.carriedO + std::atan2(sx, fx);
            lx += std::cos(a) * speed * dtMs / 1000.0f;
            ly += std::sin(a) * speed * dtMs / 1000.0f;
        }
        float w[3], wo;
        transportToWorld(it->second, lx, ly, o.carriedBase[2], o.carriedO, w, &wo);
        o.mv.x = w[0]; o.mv.y = w[1]; o.mv.z = w[2]; o.mv.o = wo;
    }
}

void ObjectTable::clear() {
    poses_.clear();
    objs_.clear();
    created_.clear();
    stats = ObjectStats{};
}

WorldObject* ObjectTable::find(uint64_t g) {
    auto it = objs_.find(g);
    return it == objs_.end() ? nullptr : &it->second;
}

const WorldObject* ObjectTable::find(uint64_t g) const {
    auto it = objs_.find(g);
    return it == objs_.end() ? nullptr : &it->second;
}

size_t ObjectTable::count(ObjType t) const {
    size_t n = 0;
    for (auto& [g, o] : objs_) n += o.type == t;
    return n;
}

void ObjectTable::remove(uint64_t g) {
    if (g != self_) objs_.erase(g);
}

uint32_t ObjectTable::fieldCap(const WorldObject& o) const {
    if (o.self) return UF::PLAYER_END;
    switch (o.type) {
        case ObjType::Item: return 64;
        case ObjType::Container: return 128;
        case ObjType::Unit: return UF::UNIT_END;
        case ObjType::Player: return maxFieldsOther;
        case ObjType::GameObject: return 32;
        case ObjType::DynamicObject: return 32;
        case ObjType::Corpse: return 64;
        default: return 8;
    }
}

std::string ObjectTable::nameOf(uint64_t g) const {
    auto pn = playerNames_.find(g);
    if (pn != playerNames_.end()) return pn->second;
    auto it = objs_.find(g);
    return it != objs_.end() ? it->second.name : std::string();
}

void ObjectTable::applyNames(WorldObject& o) {
    if (o.type == ObjType::Player) {
        auto it = playerNames_.find(o.guid);
        if (it != playerNames_.end()) o.name = it->second;
    } else if (o.type == ObjType::Unit) {
        auto it = creatureNames_.find(o.entry());
        if (it != creatureNames_.end()) { o.name = it->second.first; o.subname = it->second.second; }
        auto rk = creatureRank_.find(o.entry());
        if (rk != creatureRank_.end()) { o.rank = rk->second.first; o.creatureType = rk->second.second; }
    } else if (o.type == ObjType::GameObject) {
        auto it = goNames_.find(o.entry());
        if (it != goNames_.end()) o.name = it->second;
    }
}

bool ObjectTable::parseFields(Reader& r, WorldObject* o) {
    uint8_t maskCount = r.u8();
    if (r.bad) return false;
    if (maskCount > 64) { stats.lastError = "update mask too long"; return false; }
    uint32_t mask[64];
    for (int i = 0; i < maskCount; ++i) mask[i] = r.u32();
    if (r.bad) return false;
    uint32_t cap = o ? fieldCap(*o) : 0;
    for (int b = 0; b < maskCount; ++b) {
        uint32_t m = mask[b];
        while (m) {
            int bit = __builtin_ctz(m);
            m &= m - 1;
            uint32_t idx = (uint32_t)b * 32 + (uint32_t)bit;
            uint32_t v = r.u32();
            if (o && idx < cap) {
                const bool known = o->fields.size() > UF::GAMEOBJECT_TYPE_ID;   // false while a create block is still being read
                if (o->fields.size() <= idx) o->fields.resize(idx + 1, 0);
                if (idx == UF::GAMEOBJECT_STATE && o->type == ObjType::GameObject && known && o->fields[idx] != v)
                    o->stateChangeMs = nowMs_ ? nowMs_ : 1;   // a change we saw happen (not the first value of a new object)
                o->fields[idx] = v;
            }
        }
    }
    return !r.bad;
}

bool ObjectTable::parseMovementBlock(Reader& r, WorldObject& o, uint32_t nowMs) {
    uint8_t uf = r.u8();
    if (uf & UPDATEFLAG_SELF) o.self = true;
    if (uf & UPDATEFLAG_LIVING) {
        MovementInfo& m = o.mv;
        m.flags = r.u32();
        m.time = r.u32();
        m.x = r.f32(); m.y = r.f32(); m.z = r.f32(); m.o = r.f32();
        if (m.flags & MoveFlag::OnTransport) {
            m.transportGuid = r.packedGuid();
            m.tx = r.f32(); m.ty = r.f32(); m.tz = r.f32(); m.to = r.f32();
        }
        if (m.flags & MoveFlag::Swimming) m.pitch = r.f32();
        m.fallTime = r.u32();
        if (m.flags & MoveFlag::Jumping) {
            m.jumpZSpeed = r.f32(); m.jumpSin = r.f32(); m.jumpCos = r.f32(); m.jumpXYSpeed = r.f32();
        }
        if (m.flags & MoveFlag::SplineElevation) m.splineElevation = r.f32();
        o.speeds.walk = r.f32(); o.speeds.run = r.f32(); o.speeds.runBack = r.f32();
        o.speeds.swim = r.f32(); o.speeds.swimBack = r.f32(); o.speeds.turn = r.f32();
        o.hasPos = true;
        o.posMs = nowMs;
        o.pathActive = false;
        if (m.flags & MoveFlag::SplineEnabled) {
            uint32_t sf = r.u32();
            o.pathHasFinalO = false;
            if (sf & SPLINE_FINAL_POINT) r.skip(12);
            else if (sf & SPLINE_FINAL_TARGET) r.skip(8);
            else if (sf & SPLINE_FINAL_ANGLE) { o.pathFinalO = r.f32(); o.pathHasFinalO = true; }
            uint32_t passed = r.u32(), dur = r.u32();
            r.u32();   // spline id
            uint32_t count = r.u32();
            if (r.bad || count > 256) { stats.lastError = "bad spline in movement block"; return false; }
            std::vector<Vec3f> pts;
            pts.reserve(count + 1);
            for (uint32_t i = 0; i < count; ++i) pts.push_back(readVec(r));
            Vec3f end = readVec(r);
            if (r.bad) return false;
            // Path: where the unit is now, then the remaining points to the end point.
            o.path.clear();
            o.path.push_back({m.x, m.y, m.z});
            for (auto& p : pts) if (finite3(p)) o.path.push_back(p);
            if (finite3(end)) o.path.push_back(end);
            if (o.path.size() >= 2 && dur > passed) {
                // The unit is already 'passed' ms into the spline; from its current position
                // we only need the rest. Approximate by running the full path over the remaining time
                // from the reported position (the first point is the current position).
                o.pathStartMs = nowMs;
                o.pathDurMs = dur - passed;
                o.pathSmooth = (sf & SPLINE_FLYING) != 0;
                o.pathActive = true;
            }
        }
    } else if (uf & UPDATEFLAG_HAS_POSITION) {
        o.mv.x = r.f32(); o.mv.y = r.f32(); o.mv.z = r.f32(); o.mv.o = r.f32();
        o.hasPos = true;
        o.posMs = nowMs;
    }
    if (uf & UPDATEFLAG_HIGHGUID) r.u32();
    if (uf & UPDATEFLAG_ALL) r.u32();
    if (uf & UPDATEFLAG_MELEE_ATTACKING) r.packedGuid();
    if (uf & UPDATEFLAG_TRANSPORT) {
        o.transportProgress = r.u32();
        o.hasTransportProgress = true;
        o.stationary[0] = o.mv.x; o.stationary[1] = o.mv.y; o.stationary[2] = o.mv.z; o.stationary[3] = o.mv.o;   // the create block carries the resting place
    }
    return !r.bad;
}

bool ObjectTable::parseUpdate(const uint8_t* p, size_t n, uint32_t nowMs) {
    nowMs_ = nowMs;
    Reader r(p, n);
    uint32_t count = r.u32();
    r.u8();   // has transport
    if (r.bad || count > 4096) { stats.lastError = "bad update header"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t type = r.u8();
        if (r.bad || type > UPD_NEAR) { stats.lastError = "bad update block type " + std::to_string(type); return false; }
        stats.blocks[type]++;
        switch (type) {
            case UPD_VALUES: {
                uint64_t g = r.packedGuid();
                WorldObject* o = find(g);
                if (!parseFields(r, o)) { stats.lastError = "VALUES block truncated"; return false; }
                if (o) applyNames(*o);
                break;
            }
            case UPD_MOVEMENT: {
                uint64_t g = r.u64();   // full GUID here, unlike every other block
                WorldObject tmp;
                WorldObject* o = find(g);
                if (!parseMovementBlock(r, o ? *o : tmp, nowMs)) { stats.lastError = "MOVEMENT block truncated"; return false; }
                break;
            }
            case UPD_CREATE:
            case UPD_CREATE2: {
                uint64_t g = r.packedGuid();
                uint8_t ot = r.u8();
                if (r.bad || ot > 7) { stats.lastError = "bad object type"; return false; }
                bool existed = objs_.count(g) != 0;
                WorldObject& o = objs_[g];
                if (existed && o.type != (ObjType)ot) o = WorldObject{};   // GUID reused
                o.guid = g;
                o.type = (ObjType)ot;
                if (g == self_) o.self = true;
                if (!parseMovementBlock(r, o, nowMs) || !parseFields(r, &o)) {
                    if (!existed) objs_.erase(g);
                    if (stats.lastError.empty()) stats.lastError = "CREATE block truncated";
                    return false;
                }
                applyNames(o);
                if (!existed) created_.push_back(g);
                break;
            }
            case UPD_OUT_OF_RANGE:
            case UPD_NEAR: {
                uint32_t k = r.u32();
                if (r.bad || k > 65536) return false;
                for (uint32_t j = 0; j < k; ++j) {
                    uint64_t g = r.packedGuid();
                    if (type == UPD_OUT_OF_RANGE) remove(g);
                }
                if (r.bad) return false;
                break;
            }
        }
    }
    return true;
}

bool ObjectTable::parseMonsterMove(Reader& r, uint32_t nowMs) {
    uint64_t g = r.packedGuid();
    Vec3f start = readVec(r);
    r.u32();   // spline id / server tick
    uint8_t kind = r.u8();
    if (r.bad || !finite3(start)) return false;
    WorldObject tmp;
    WorldObject* op = find(g);
    WorldObject& o = op ? *op : tmp;
    o.mv.x = start.x; o.mv.y = start.y; o.mv.z = start.z;
    o.posMs = nowMs;
    o.hasPos = true;
    o.pathHasFinalO = false;
    switch (kind) {
        case 0: break;                                   // normal
        case 1: o.pathActive = false; return true;       // stop: nothing follows
        case 2: r.skip(12); break;                       // face a spot
        case 3: r.skip(8); break;                        // face a target
        case 4: o.pathFinalO = r.f32(); o.pathHasFinalO = true; break;
        default: return false;
    }
    uint32_t flags = r.u32(), dur = r.u32(), count = r.u32();
    if (r.bad || count == 0 || count > 1000) return false;
    // Uncompressed (Catmull-Rom/flying/cyclic): count absolute points.
    // Linear: destination, then count-1 packed offsets from the midpoint.
    size_t left = r.left();
    bool uncompressed;
    if (left == (size_t)count * 12) uncompressed = true;
    else if (left == 12 + (size_t)(count - 1) * 4) uncompressed = false;
    else uncompressed = (flags & (SPLINE_FLYING | SPLINE_CATMULLROM | SPLINE_CYCLIC | SPLINE_ENTER_CYCLE)) != 0;
    std::vector<Vec3f> path;
    path.push_back(start);
    if (uncompressed) {
        for (uint32_t i = 0; i < count; ++i) path.push_back(readVec(r));
    } else {
        Vec3f dest = readVec(r);
        Vec3f mid{(start.x + dest.x) * 0.5f, (start.y + dest.y) * 0.5f, (start.z + dest.z) * 0.5f};
        for (uint32_t i = 1; i < count; ++i) path.push_back(unpackDelta(r.u32(), mid));
        path.push_back(dest);
    }
    if (r.bad) return false;
    for (auto& p : path) if (!finite3(p)) return false;
    o.path = std::move(path);
    o.pathSmooth = (flags & SPLINE_FLYING) != 0;
    o.pathStartMs = nowMs;
    o.pathDurMs = dur;
    o.pathActive = true;
    return true;
}

bool ObjectTable::handle(uint16_t op, const Bytes& data, uint32_t nowMs) {
    switch (op) {
        case ObjOp::SMSG_UPDATE_OBJECT:
            stats.updatePackets++;
            if (!parseUpdate(data.data(), data.size(), nowMs)) stats.failedPackets++;
            return true;
        case ObjOp::SMSG_COMPRESSED_UPDATE_OBJECT: {
            stats.compressedPackets++;
            Reader r(data);
            uint32_t size = r.u32();
            Bytes out;
            if (r.bad || !zlibInflate(data.data() + 4, data.size() - 4, out, size)) {
                stats.failedPackets++;
                stats.lastError = "inflate failed";
                return true;
            }
            stats.inflatedIn += data.size();
            stats.inflatedOut += out.size();
            if (!parseUpdate(out.data(), out.size(), nowMs)) stats.failedPackets++;
            return true;
        }
        case ObjOp::SMSG_DESTROY_OBJECT: {
            Reader r(data);
            uint64_t g = r.u64();
            if (!r.bad) { remove(g); stats.destroys++; }
            return true;
        }
        case ObjOp::SMSG_MONSTER_MOVE: {
            Reader r(data);
            if (parseMonsterMove(r, nowMs)) stats.monsterMoves++;
            else stats.monsterMoveFails++;
            return true;
        }
        case ObjOp::SMSG_COMPRESSED_MOVES: {
            Reader r(data);
            uint32_t size = r.u32();
            Bytes out;
            if (r.bad || !zlibInflate(data.data() + 4, data.size() - 4, out, size)) {
                stats.lastError = "inflate failed (moves)";
                return true;
            }
            stats.compressedMoves++;
            Reader in(out);
            while (in.left() >= 3) {
                uint8_t len = in.u8();
                uint16_t sub = in.u16();
                if (len < 2 || in.left() < (size_t)len - 2) break;
                Bytes body(out.begin() + (long)in.pos, out.begin() + (long)(in.pos + len - 2));
                in.skip(len - 2);
                if (sub == ObjOp::SMSG_MONSTER_MOVE) handle(sub, body, nowMs);
            }
            return true;
        }
        case ObjOp::SMSG_NAME_QUERY_RESPONSE: {
            Reader r(data);
            uint64_t g = r.u64();
            std::string name = r.cstr();
            if (name.empty()) return true;
            playerNames_[g] = name;
            if (WorldObject* o = find(g)) o->name = name;
            stats.nameAnswers++;
            return true;
        }
        case ObjOp::SMSG_CREATURE_QUERY_RESPONSE: {
            Reader r(data);
            uint32_t entry = r.u32();
            if (r.bad || (entry & 0x80000000u)) return true;   // unknown entry
            std::string name = r.cstr();
            r.cstr(); r.cstr(); r.cstr();
            std::string sub = r.cstr();
            creatureNames_[entry] = {name, sub};
            r.u32();   // type flags
            const uint32_t ctype = r.u32();
            r.u32();   // family
            const uint32_t rank = r.u32();
            const uint8_t rk = r.bad || rank > 4 ? 0 : (uint8_t)rank, ct = r.bad || ctype > 255 ? 0 : (uint8_t)ctype;
            creatureRank_[entry] = {rk, ct};
            for (auto& [g, o] : objs_)
                if (o.type == ObjType::Unit && o.entry() == entry) { o.name = name; o.subname = sub; o.rank = rk; o.creatureType = ct; }
            stats.creatureAnswers++;
            return true;
        }
        case ObjOp::SMSG_GAMEOBJECT_QUERY_RESPONSE: {
            Reader r(data);
            uint32_t entry = r.u32();
            if (r.bad || (entry & 0x80000000u)) return true;
            GoInfo gi;
            gi.type = r.u32();
            r.u32();   // display id
            std::string name = r.cstr();
            goNames_[entry] = name;
            r.cstr(); r.cstr(); r.cstr();   // name2..4
            if (r.left() > 96) r.cstr();     // 1.12.1: the icon name
            for (int k = 0; k < 24 && r.left() >= 4; ++k) gi.data[k] = r.u32();
            goInfo_[entry] = gi;
            for (auto& [g, o] : objs_)
                if (o.type == ObjType::GameObject && o.entry() == entry) o.name = name;
            stats.gameobjectAnswers++;
            return true;
        }
        default:
            break;
    }
    if (MoveOp::isRelayedMove(op)) {
        Reader r(data);
        uint64_t g = r.packedGuid();
        MovementInfo m;
        if (!readMovementInfo(r, m)) return true;
        if (WorldObject* o = find(g)) {
            if (o->self) return true;   // our own echo (should not happen)
            o->mv = m;
            o->posMs = nowMs;
            o->hasPos = true;
            o->pathActive = false;
            stats.playerMoves++;
        }
        return true;
    }
    return false;
}

}  // namespace w3n
