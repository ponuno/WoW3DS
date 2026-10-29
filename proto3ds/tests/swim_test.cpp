// Stage 5.26: swimming on the PC - swim.cpp (the movement rules) with net/movement.cpp (the packets), driven in the
// order main.cpp uses, over a test lake; every packet goes through a checker that applies the server's rules
// (vMaNGOS MovementAnticheat + MovementHandler):
//   * the swimming flag appears only with MSG_MOVE_START_SWIM and leaves only with MSG_MOVE_STOP_SWIM (which must not
//     carry it); START_SWIM ends a jump / fall (no JUMPING flag in it, the fall time set), no FALL_LAND in the water;
//   * no packet is swimming and jumping at once; the horizontal distance between two packets stays within the speed
//     of the earlier packet's flags (+10 %, the server's overspeed tolerance);
//   * a swimming packet carries a pitch, SET_PITCH follows the camera.
// Scenarios: wade in and swim out, dive and come up, a plunge from a cliff, jumping out at the surface, water walking,
// swimming off the edge of a liquid area, standing still (no packets).
//   g++ -std=c++20 -I common -I net -I proto3ds/source proto3ds/tests/swim_test.cpp proto3ds/source/swim.cpp net/movement.cpp
#include "swim.hpp"
#include "movement.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace w3n;

namespace {
int g_fail = 0, g_checks = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++g_fail; }
}

// The lake: a beach from x = 0 (z 10) down to z -10 at x = 40, flat beyond; water at z 0 for 4 <= x < 120 (its cells
// end at x = 120 where a low dry bank at z -0.5 begins: low enough to climb out). Beyond y = 500 a cliff (z 25) stands
// right at the water (x < 50, water from x 50 over a bottom at -10): jumping in from there.
bool g_cliff = false;
float groundAt(float x) {
    if (g_cliff) return x < 50 ? 25.0f : -10.0f;
    if (x >= 120) return -0.5f;
    return x < 40 ? 10.0f - 0.5f * x : -10.0f;
}
bool liquidAt(float x, float /*y*/, float* level) {
    if (g_cliff ? x < 50 : (x < 4 || x >= 120)) return false;
    *level = 0.0f;
    return true;
}

// The server's view of us, packet by packet.
struct Checker {
    MovementInfo last;
    bool have = false, swimming = false;
    std::vector<std::string> ops;
    MoveSpeeds sp;
    float speedFor(uint32_t f) const { return (f & MoveFlag::MaskMoving) ? sp.forFlags(f) : 0.0f; }
    void packet(uint16_t op, const Bytes& b) {
        Reader r(b);
        MovementInfo m;
        check(readMovementInfo(r, m) && r.left() == 0, std::string("parse ") + MoveOp::name(op));
        ops.push_back(MoveOp::name(op));
        const bool sw = (m.flags & MoveFlag::Swimming) != 0;
        if (op == MoveOp::StartSwim) {
            check(sw, "START_SWIM carries the flag");
            check(!(m.flags & (MoveFlag::Jumping | MoveFlag::FallingFar)), "START_SWIM ends the fall");
        } else if (op == MoveOp::StopSwim) {
            check(!sw, "STOP_SWIM without the flag");
        } else {
            check(sw == swimming, std::string("swimming flag changed by ") + MoveOp::name(op));
        }
        check(!(sw && (m.flags & MoveFlag::Jumping)), "swimming and jumping at once");
        check(op != MoveOp::FallLand || !sw, "FALL_LAND while swimming");
        if (have && m.time > last.time) {
            const float dt = (m.time - last.time) / 1000.0f;
            const float d = std::hypot(m.x - last.x, m.y - last.y);
            const float allowed = std::max(speedFor(last.flags), speedFor(m.flags)) * dt * 1.1f + 0.05f;
            const bool jump = (last.flags | m.flags) & MoveFlag::Jumping;
            if (!jump) check(d <= allowed, std::string("overspeed ") + MoveOp::name(op) + " " + std::to_string(d) + " > " + std::to_string(allowed));
        }
        if (op == MoveOp::StartSwim || op == MoveOp::StopSwim) swimming = sw;
        last = m;
        have = true;
    }
    bool saw(const char* name) const { for (auto& o : ops) if (o == name) return true; return false; }
    size_t count(const char* name) const { size_t n = 0; for (auto& o : ops) n += o == name; return n; }
};

// The character and the main loop's movement step (main.cpp, online walking, without ships and loading screens).
struct Sim {
    Mover mover;
    Checker server;
    float pl[3] = {0, 0, 10};
    float yaw = 0;              // facing +x (into the lake)
    float plVz = 0;
    bool airborne = false, jumped = false;
    uint32_t heldDir = 0;
    swim::State ss;
    uint32_t now = 1000;
    int starts = 0, stops = 0;
    void reset(float x, float z) {
        pl[0] = x; pl[1] = 0; pl[2] = z;
        mover.reset(x, 0, z, yaw, now);
        server = Checker();
    }
    void flush() {
        for (auto& p : mover.queue()) server.packet(p.opcode, p.data);
        mover.queue().clear();
    }
    // one frame of 1/30 s
    void frame(uint32_t keys, float camPitch, bool ascend, bool jumpPress) {
        const float dt = 1.0f / 30.0f;
        now += 33;
        swim::World w;
        w.floor = [](float x, float, float, float* g) { *g = groundAt(x); return true; };
        w.liquid = [&](float x, float y, float z, float* lv, uint8_t* f) {
            if (!liquidAt(x, y, lv)) return false;
            *f = 0;
            return z >= groundAt(x) - 2.0f;
        };
        w.walls = [](float, float, float, float*, float*) {};
        const bool waterWalker = (mover.info().flags & MoveFlag::WaterWalking) != 0;
        uint32_t dir = airborne ? heldDir : keys;
        if (!airborne) heldDir = dir;
        const uint32_t other = mover.info().flags & ~(uint32_t)MoveFlag::MaskMoving;
        float speed = mover.speeds.forFlags(dir | other);
        if (mover.swimming()) {
            if (!dir) speed = mover.speeds.swim;   // swimming straight up
            swim::Input in;
            in.dt = dt;
            for (int k = 0; k < 3; ++k) in.pos[k] = pl[k];
            in.yaw = yaw; in.dir = dir; in.speed = speed; in.camPitch = camPitch; in.ascend = ascend; in.jump = jumpPress;
            swim::Result r = swim::step(ss, w, in);
            if (r.jumpOut) {
                mover.stopSwim(now);
                mover.update(dir, pl[0], pl[1], pl[2], yaw, now);
                mover.jump(now);
                airborne = jumped = true;
                plVz = 7.9555473f;
                heldDir = dir;
                ++stops;
            } else {
                for (int k = 0; k < 3; ++k) pl[k] = r.pos[k];
                if (r.stop) {
                    mover.stopSwim(now);
                    ++stops;
                    float g = groundAt(pl[0]);
                    if (g < pl[2] - 1.0f) { airborne = true; jumped = false; plVz = 0; }
                    else pl[2] = g;
                    ss = swim::State();
                } else {
                    mover.setSwimPitch(r.pitch, now);
                }
            }
        } else {
            auto floorW = [&](float x, float z) {
                float g = groundAt(x), lv;
                if (waterWalker && liquidAt(x, 0, &lv) && lv > g && z >= lv - 1.0f) g = lv;
                return g;
            };
            if (dir) {
                const float fx = (dir & MoveFlag::Forward) ? 1.f : (dir & MoveFlag::Backward) ? -1.f : 0.f;
                pl[0] += std::cos(yaw) * fx * speed * dt;
                pl[1] += std::sin(yaw) * fx * speed * dt;
            }
            float ground = floorW(pl[0], pl[2]);
            if (jumpPress && !airborne) {
                mover.update(dir, pl[0], pl[1], pl[2], yaw, now);
                mover.jump(now);
                airborne = jumped = true;
                plVz = 7.9555473f;
            }
            if (!airborne && ground < pl[2] - 1.0f) { airborne = true; jumped = false; plVz = 0; }
            float lv = 0;
            auto deep = [&]() { return !waterWalker && liquidAt(pl[0], 0, &lv) && pl[2] >= groundAt(pl[0]) - 2 && swim::deepEnough(lv, pl[2]); };
            if (airborne) {
                plVz -= 19.291105f * dt;
                pl[2] += plVz * dt;
                if (deep()) {
                    ss = swim::State();
                    ss.vz = std::max(plVz * 0.5f, -12.0f);
                    ss.surfacing = true;
                    mover.update(dir, pl[0], pl[1], pl[2], yaw, now);
                    mover.startSwim(now);
                    ++starts;
                    airborne = jumped = false;
                    plVz = 0;
                } else if (plVz < 0 && pl[2] <= ground) {
                    pl[2] = ground;
                    airborne = false;
                    if (jumped) mover.land(ground, now);
                    jumped = false;
                }
            } else {
                pl[2] = ground;
                if (deep()) {
                    ss = swim::State();
                    ss.surfacing = true;
                    mover.update(dir, pl[0], pl[1], pl[2], yaw, now);
                    mover.startSwim(now);
                    ++starts;
                }
            }
        }
        mover.update(dir, pl[0], pl[1], pl[2], yaw, now);
        flush();
    }
    void run(float sec, uint32_t keys, float camPitch = -0.1f, bool ascend = false) {
        for (int i = 0; i < (int)(sec * 30); ++i) frame(keys, camPitch, ascend, false);
    }
};
}  // namespace

int main() {
    const float floatAt = 0.0f - W3D_SWIM_DEPTH;
    // ---- A: wade in from the beach, swim, turn round and come out
    {
        Sim s;
        s.reset(0, 10);
        s.run(6.0f, MoveFlag::Forward);   // 7 yd/s: from x 0 down the beach into the lake
        check(s.mover.swimming(), "A: swimming after wading in");
        check(s.starts == 1, "A: one START_SWIM");
        check(std::fabs(s.pl[2] - floatAt) < 0.05f, "A: floats at the surface (" + std::to_string(s.pl[2]) + ")");
        const float xIn = s.pl[0];
        check(xIn > 10 && xIn < 7 * 6.0f, "A: swam at swim speed, not run speed (x " + std::to_string(xIn) + ")");
        s.yaw = 3.14159265f;              // back to the beach
        s.run(8.0f, MoveFlag::Forward);
        check(!s.mover.swimming(), "A: stood up on the beach");
        check(s.stops == 1 && s.server.count("STOP_SWIM") == 1, "A: one STOP_SWIM");
        float lv;
        check(!liquidAt(s.pl[0], 0, &lv) || lv - groundAt(s.pl[0]) < W3D_SWIM_DEPTH, "A: stood where the water is shallow");
        check(std::fabs(s.pl[2] - groundAt(s.pl[0])) < 0.01f, "A: on the ground again");
        check(!s.server.saw("FALL_LAND"), "A: no landing");
    }
    // ---- B: dive (camera down), swim along the bottom, come up holding jump
    {
        Sim s;
        s.reset(60, floatAt);
        s.mover.startSwim(s.now);
        s.flush();
        s.run(1.0f, MoveFlag::Forward, -0.1f);
        check(std::fabs(s.pl[2] - floatAt) < 0.01f, "B: level strokes stay at the surface");
        s.run(2.0f, MoveFlag::Forward, -1.2f);
        check(s.pl[2] < floatAt - 3.0f, "B: dived (z " + std::to_string(s.pl[2]) + ")");
        check(s.server.saw("SET_PITCH"), "B: SET_PITCH sent when the camera tipped down");
        check(s.mover.info().pitch < -0.5f, "B: pitch in the packets");
        s.run(3.0f, MoveFlag::Forward, -1.2f);
        check(std::fabs(s.pl[2] - groundAt(s.pl[0])) < 0.01f, "B: on the bottom, not through it");
        check(s.mover.swimming(), "B: still swimming on a deep bottom");
        s.run(4.0f, 0, -0.1f, true);
        check(std::fabs(s.pl[2] - floatAt) < 0.01f, "B: back at the surface, no higher (z " + std::to_string(s.pl[2]) + ")");
        check(s.server.count("HEARTBEAT") > 3, "B: heartbeats while only rising");
        const size_t before = s.server.ops.size();
        s.run(3.0f, 0);
        check(s.server.ops.size() == before, "B: no packets floating still");
    }
    // ---- C: a plunge from the cliff: JUMP, fall, START_SWIM (no FALL_LAND, fall time set), back up by itself
    {
        Sim s;
        g_cliff = true;
        s.reset(47, 25);
        s.frame(MoveFlag::Forward, -0.1f, false, false);
        s.frame(MoveFlag::Forward, -0.1f, false, true);   // jump off
        for (int i = 0; i < 200 && !s.mover.swimming(); ++i) s.frame(MoveFlag::Forward, -0.1f, false, false);
        check(s.mover.swimming(), "C: swimming after the plunge");
        check(s.server.saw("JUMP") && !s.server.saw("FALL_LAND"), "C: JUMP, no FALL_LAND");
        check(s.pl[0] > 50, "C: landed in the lake");
        s.run(4.0f, 0);
        check(std::fabs(s.pl[2] - floatAt) < 0.05f, "C: floated back up (z " + std::to_string(s.pl[2]) + ")");
        g_cliff = false;
    }
    // ---- D: jump out at the surface and fall back in
    {
        Sim s;
        s.reset(60, floatAt);
        s.mover.startSwim(s.now);
        s.flush();
        s.run(0.5f, 0);
        s.frame(0, -0.1f, true, true);
        check(!s.mover.swimming() && s.airborne, "D: out of the water in a jump");
        check(s.server.ops.size() >= 2 && s.server.ops[s.server.ops.size() - 2] == "STOP_SWIM" &&
              s.server.ops.back() == "JUMP", "D: STOP_SWIM then JUMP");
        s.run(2.0f, 0);
        check(s.mover.swimming(), "D: back in the water");
        check(!s.server.saw("FALL_LAND"), "D: no landing in the water");
    }
    // ---- E: water walking (a ghost): along the surface, never swimming
    {
        Sim s;
        s.reset(0, 10);
        s.mover.setFlag(MoveFlag::WaterWalking, true);
        s.run(8.0f, MoveFlag::Forward);
        check(!s.mover.swimming() && s.starts == 0, "E: water walker does not swim");
        check(s.pl[0] > 30 && std::fabs(s.pl[2] - 0.0f) < 0.01f, "E: walks on the surface (z " + std::to_string(s.pl[2]) + ")");
    }
    // ---- F: swim off the end of the liquid (a bank at x 120): STOP_SWIM, standing on the bank
    {
        Sim s;
        s.reset(110, floatAt);
        s.mover.startSwim(s.now);
        s.flush();
        s.run(4.0f, MoveFlag::Forward);
        check(!s.mover.swimming(), "F: not swimming past the liquid's edge");
        check(s.pl[0] >= 120.0f && std::fabs(s.pl[2] + 0.5f) < 0.01f, "F: on the bank (x " + std::to_string(s.pl[0]) + " z " + std::to_string(s.pl[2]) + ")");
    }
    // ---- G: the stroke pitch: level within the dead zone, up to 1.4 rad
    check(swim::strokePitch(-0.1f) == 0.0f && swim::strokePitch(0.2f) == 0.0f, "G: dead zone");
    check(std::fabs(swim::strokePitch(-1.4f) + 1.4f) < 1e-4f && swim::strokePitch(0.8f) > 0.5f, "G: full range");
    std::printf("SWIM: %s (%d checks, %d failed)\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
