#include "movement.hpp"

#include <cmath>

namespace w3n {

float normalizeAngle(float a) {
    const float tau = 6.28318531f;
    a = std::fmod(a, tau);
    if (a < 0) a += tau;
    if (a >= tau) a = 0;
    return a;
}

bool MoveOp::isRelayedMove(uint16_t op) {
    return (op >= StartForward && op <= SetWalkMode) || op == FallLand || op == StartSwim || op == StopSwim ||
           op == SetFacing || op == SetPitch || op == Heartbeat || op == Root || op == Unroot;
}

const char* MoveOp::name(uint16_t op) {
    switch (op) {
        case StartForward: return "START_FORWARD";
        case StartBackward: return "START_BACKWARD";
        case Stop: return "STOP";
        case StartStrafeLeft: return "START_STRAFE_LEFT";
        case StartStrafeRight: return "START_STRAFE_RIGHT";
        case StopStrafe: return "STOP_STRAFE";
        case Jump: return "JUMP";
        case FallLand: return "FALL_LAND";
        case SetFacing: return "SET_FACING";
        case Heartbeat: return "HEARTBEAT";
        case TeleportAck: return "TELEPORT_ACK";
        default: return "MOVE";
    }
}

void writeMovementInfo(Writer& w, const MovementInfo& m) {
    w.u32(m.flags);
    w.u32(m.time);
    w.f32(m.x); w.f32(m.y); w.f32(m.z); w.f32(m.o);
    if (m.flags & MoveFlag::OnTransport) {
        w.packedGuid(m.transportGuid);
        w.f32(m.tx); w.f32(m.ty); w.f32(m.tz); w.f32(m.to);
    }
    if (m.flags & MoveFlag::Swimming) w.f32(m.pitch);
    w.u32(m.fallTime);
    if (m.flags & MoveFlag::Jumping) {
        w.f32(m.jumpZSpeed); w.f32(m.jumpSin); w.f32(m.jumpCos); w.f32(m.jumpXYSpeed);
    }
    if (m.flags & MoveFlag::SplineElevation) w.f32(m.splineElevation);
}

bool readMovementInfo(Reader& r, MovementInfo& m) {
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
    return !r.bad;
}

float MoveSpeeds::forFlags(uint32_t f) const {
    if (!(f & MoveFlag::MaskMoving) || (f & MoveFlag::Root)) return 0;
    // As the server picks it (vMaNGOS MovementAnticheat::GetMoveTypeForMovementInfo): swimming ignores walk mode,
    // on land walk mode wins over backpedalling (walking backwards is walk speed, not run-back speed).
    if (f & MoveFlag::Swimming) return (f & MoveFlag::Backward) ? swimBack : swim;
    if (f & MoveFlag::WalkMode) return walk;
    return (f & MoveFlag::Backward) ? runBack : run;
}

// ------------------------------------------------------------------ Mover
void Mover::reset(float x, float y, float z, float o, uint32_t nowMs) {
    // Water walking / safe fall / hover are states the server toggled (and we acknowledged): a teleport keeps them.
    const uint32_t sticky = m_.flags & (kStickyFlags | MoveFlag::WalkMode);   // and walk mode
    m_ = MovementInfo{};
    m_.flags = sticky;
    m_.x = x; m_.y = y; m_.z = z; m_.o = normalizeAngle(o);
    m_.time = nowMs;
    lastSentO_ = m_.o;
    lastSendMs_ = nowMs;
    out_.clear();
}

MovementInfo Mover::snapshot(uint32_t nowMs) const {
    MovementInfo s = m_;
    s.time = nowMs;
    if (s.flags & MoveFlag::Jumping) s.fallTime = nowMs - jumpStartMs_;
    return s;
}

void Mover::emit(uint16_t op, uint32_t nowMs) {
    Writer w;
    writeMovementInfo(w, snapshot(nowMs));
    out_.push_back({op, std::move(w.b)});
    lastSendMs_ = nowMs;
    lastSentO_ = m_.o;
    ++packetsSent;
}

void Mover::update(uint32_t dir, float x, float y, float z, float o, uint32_t nowMs) {
    dir &= MoveFlag::MaskMoving;
    if (rooted()) dir = 0;
    // The client never holds both opposite keys: keep one of each pair.
    if ((dir & MoveFlag::Forward) && (dir & MoveFlag::Backward)) dir &= ~MoveFlag::Backward;
    if ((dir & MoveFlag::StrafeLeft) && (dir & MoveFlag::StrafeRight)) dir &= ~MoveFlag::StrafeRight;

    const uint32_t fb = MoveFlag::Forward | MoveFlag::Backward, lr = MoveFlag::StrafeLeft | MoveFlag::StrafeRight;
    uint32_t prev = m_.flags & MoveFlag::MaskMoving;
    m_.x = x; m_.y = y; m_.z = z;
    m_.o = normalizeAngle(o);

    bool sent = false;
    if ((prev & fb) != (dir & fb)) {
        m_.flags = (m_.flags & ~fb) | (dir & fb);
        emit((dir & MoveFlag::Forward) ? MoveOp::StartForward : (dir & MoveFlag::Backward) ? MoveOp::StartBackward
                                                                                          : MoveOp::Stop, nowMs);
        sent = true;
    }
    if ((prev & lr) != (dir & lr)) {
        m_.flags = (m_.flags & ~lr) | (dir & lr);
        emit((dir & MoveFlag::StrafeLeft) ? MoveOp::StartStrafeLeft : (dir & MoveFlag::StrafeRight)
                                                                          ? MoveOp::StartStrafeRight
                                                                          : MoveOp::StopStrafe, nowMs);
        sent = true;
    }
    if (sent) return;

    float d = std::fabs(m_.o - lastSentO_);
    if (d > 3.14159265f) d = 6.28318531f - d;
    if (d > facingEpsilon && nowMs - lastSendMs_ >= facingIntervalMs) {
        emit(MoveOp::SetFacing, nowMs);
        return;
    }
    bool moving = (m_.flags & MoveFlag::MaskMoving) || airborne();
    if (moving && nowMs - lastSendMs_ >= heartbeatMs) {
        emit(MoveOp::Heartbeat, nowMs);
        ++heartbeats;
    }
}

void Mover::setTransport(uint64_t guid, float lx, float ly, float lz, float lo, uint32_t nowMs) {
    m_.flags |= MoveFlag::OnTransport;
    m_.transportGuid = guid;
    setTransportOffset(lx, ly, lz, lo);
    emit(MoveOp::Heartbeat, nowMs);
}

void Mover::setTransportOffset(float lx, float ly, float lz, float lo) {
    m_.tx = lx; m_.ty = ly; m_.tz = lz;
    m_.to = normalizeAngle(lo);
}

void Mover::clearTransport(uint32_t nowMs) {
    if (!onTransport()) return;
    m_.flags &= ~(uint32_t)MoveFlag::OnTransport;
    m_.transportGuid = 0;
    m_.tx = m_.ty = m_.tz = m_.to = 0;
    emit(MoveOp::Heartbeat, nowMs);
}

void Mover::jump(uint32_t nowMs) {
    if (airborne() || rooted()) return;
    m_.flags |= MoveFlag::Jumping;
    jumpStartMs_ = nowMs;
    float speed = speeds.forFlags(m_.flags);
    // Direction of travel: facing, turned by the strafe/backward keys.
    float a = m_.o;
    uint32_t f = m_.flags;
    float fx = (f & MoveFlag::Forward) ? 1.f : (f & MoveFlag::Backward) ? -1.f : 0.f;
    float sx = (f & MoveFlag::StrafeLeft) ? 1.f : (f & MoveFlag::StrafeRight) ? -1.f : 0.f;
    if (fx != 0 || sx != 0) a += std::atan2(sx, fx);
    m_.jumpZSpeed = -7.9555473f;            // the stock client's initial jump velocity (up is negative)
    m_.jumpSin = std::sin(a);
    m_.jumpCos = std::cos(a);
    m_.jumpXYSpeed = speed;
    emit(MoveOp::Jump, nowMs);
}

void Mover::land(float z, uint32_t nowMs) {
    if (!airborne()) return;
    m_.z = z;
    // Landing: the falling flag is already cleared, fallTime says how long the
    // fall lasted (the server derives fall damage from it).
    m_.flags &= ~(MoveFlag::Jumping | MoveFlag::FallingFar);
    m_.fallTime = nowMs - jumpStartMs_;
    Writer w;
    MovementInfo s = m_;
    s.time = nowMs;
    writeMovementInfo(w, s);
    out_.push_back({MoveOp::FallLand, std::move(w.b)});
    lastSendMs_ = nowMs;
    ++packetsSent;
    m_.fallTime = 0;
    m_.jumpZSpeed = m_.jumpSin = m_.jumpCos = m_.jumpXYSpeed = 0;
}

void Mover::stopAll(uint32_t nowMs) {
    update(0, m_.x, m_.y, m_.z, m_.o, nowMs);
}

void Mover::setWalk(bool on, uint32_t nowMs) {
    if (walking() == on) return;
    if (on) m_.flags |= MoveFlag::WalkMode;
    else m_.flags &= ~MoveFlag::WalkMode;
    emit(on ? MoveOp::SetWalkMode : MoveOp::SetRunMode, nowMs);
}

void Mover::setFlag(uint32_t flag, bool on) {
    flag &= kStickyFlags;
    if (on) m_.flags |= flag;
    else m_.flags &= ~flag;
}

void Mover::setRooted(bool r) {
    if (r) m_.flags = (m_.flags & ~MoveFlag::MaskMoving) | MoveFlag::Root;
    else m_.flags &= ~MoveFlag::Root;
}

}  // namespace w3n
