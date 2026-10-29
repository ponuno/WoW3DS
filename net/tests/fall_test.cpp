// 6.18d: falling off a ledge. The stock client sends a heartbeat with the falling flag when the feet leave the ground and
// MSG_MOVE_FALL_LAND with the fall time when they touch it again; the server deals the fall damage from those two.
#include "movement.hpp"

#include <cstdio>

using namespace w3n;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    Mover m;
    m.reset(100, 200, 50, 1.0f, 1000);
    m.queue().clear();
    CHECK(!m.airborne(), "airborne after reset");
    m.update(MoveFlag::Forward, 101, 200, 50, 1.0f, 1100);   // (a start-forward packet)
    m.queue().clear();
    m.fall(1200);
    CHECK(m.airborne(), "not airborne after fall()");
    CHECK(m.queue().size() == 1 && m.queue()[0].opcode == MoveOp::Heartbeat, "fall() must send one heartbeat (%zu packets)", m.queue().size());
    MovementInfo sent;
    {
        Reader r(m.queue()[0].data);
        CHECK(readMovementInfo(r, sent), "heartbeat does not parse");
        CHECK(sent.flags & MoveFlag::Jumping, "the falling flag is missing in the heartbeat");
        CHECK(sent.flags & MoveFlag::Forward, "forward lost");
    }
    m.queue().clear();
    m.fall(1300);   // already falling: nothing
    CHECK(m.queue().empty(), "a second fall() sent something");
    m.update(MoveFlag::Forward, 103, 200, 40, 1.0f, 1700);
    m.queue().clear();
    m.land(20.0f, 2200);
    CHECK(!m.airborne(), "still airborne after land()");
    CHECK(m.queue().size() == 1 && m.queue()[0].opcode == MoveOp::FallLand, "land() must send FALL_LAND");
    if (!m.queue().empty()) {
        Reader r(m.queue().back().data);
        MovementInfo l;
        CHECK(readMovementInfo(r, l), "FALL_LAND does not parse");
        CHECK(!(l.flags & MoveFlag::Jumping), "the falling flag is still set on landing");
        CHECK(l.fallTime == 1000, "fall time %u ms, want 1000 (from 1200 to 2200)", l.fallTime);
        CHECK(l.z == 20.0f, "landing z %.1f", l.z);
    }
    // 6.19b: what vMaNGOS does with the packets (Player::UpdateFallInformationIfNeed + HandleFall): a 20-yard drop off a ledge
    // must pass every check of HandleFall; a plain jump must not.
    auto serverFall = [](Mover& mv, float zStart, float zGround, bool jumped, float* zDiffOut) -> bool {
        mv.queue().clear();
        uint32_t t = 10000;
        if (jumped) mv.jump(t); else mv.fall(t);
        float z = zStart, vz = jumped ? 7.9555473f : 0.0f;
        float fallStartZ = 0;
        uint32_t lastFlags = 0;
        bool land = false, ok = false;
        for (int f = 0; f < 2000 && !land; ++f) {
            const float dt = 1.0f / 60.0f;
            t += 16;
            vz -= 19.291105f * dt;
            z += vz * dt;
            if (vz < 0) { mv.update(0, 0, 0, z, 0, t); mv.descend(t); }
            if (z <= zGround) { z = zGround; mv.land(z, t); land = true; }
            else mv.update(0, 0, 0, z, 0, t);
            for (const auto& pk : mv.queue()) {
                Reader r(pk.data);
                MovementInfo mi;
                if (!readMovementInfo(r, mi)) continue;
                if (pk.opcode == MoveOp::FallLand) {
                    ok = fallStartZ != 0 && (lastFlags & 0x4000) && mi.fallTime >= 1229 && fallStartZ >= mi.z && fallStartZ - mi.z >= 14.57f;
                    *zDiffOut = fallStartZ - mi.z;
                } else if (mi.flags & (0x2000 | 0x4000)) {
                    if (fallStartZ == 0 || fallStartZ < mi.z) fallStartZ = mi.z;
                } else {
                    fallStartZ = 0;
                }
                lastFlags = mi.flags;
            }
            mv.queue().clear();
        }
        return ok;
    };
    {
        Mover a;
        a.reset(0, 0, 50, 0, 9000);
        float zd = 0;
        CHECK(serverFall(a, 50, 30, false, &zd), "a 20-yard fall gives no fall damage on vMaNGOS (z diff %.1f)", zd);
        Mover b;
        b.reset(0, 0, 50, 0, 9000);
        CHECK(!serverFall(b, 50, 50, true, &zd), "a jump on the spot would deal fall damage");
        Mover c;
        c.reset(0, 0, 50, 0, 9000);
        CHECK(serverFall(c, 50, 20, true, &zd), "jumping off a 30-yard cliff gives no fall damage (z diff %.1f)", zd);
    }
    // rooted / swimming characters do not start a fall
    Mover s;
    s.reset(0, 0, 0, 0, 0);
    s.startSwim(10);
    s.queue().clear();
    s.fall(20);
    CHECK(!s.airborne() && s.queue().empty(), "fall() while swimming");
    std::puts(fails ? "FALL TEST: FAIL" : "FALL TEST: PASS (heartbeat with the falling flag, FALLINGFAR on the way down, FALL_LAND with the fall time; vMaNGOS HandleFall checks)");
    return fails ? 1 : 0;
}
