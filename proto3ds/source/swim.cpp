#include "swim.hpp"

#include <math.h>

#include <algorithm>

namespace swim {

namespace {
constexpr float kDead = 0.25f;        // camera pitch that still strokes level (the default camera looks 0.1 down)
constexpr float kMaxPitch = 1.4f;
constexpr float kRiseSpeed = 2.5f;    // floating back up after falling in
constexpr float kStep = 1.5f;         // what a swimmer climbs over at the bottom (as a walker's step)
}

float strokePitch(float p) {
    if (fabsf(p) <= kDead) return 0.0f;
    const float m = (fabsf(p) - kDead) * kMaxPitch / (kMaxPitch - kDead);
    return std::clamp(p > 0 ? m : -m, -kMaxPitch, kMaxPitch);
}

Result step(State& s, const World& w, const Input& in) {
    Result r;
    r.pos[0] = in.pos[0]; r.pos[1] = in.pos[1]; r.pos[2] = in.pos[2];
    float level;
    uint8_t flags;
    if (!w.liquid(in.pos[0], in.pos[1], in.pos[2], &level, &flags)) {   // the liquid is gone under us
        r.stop = true;
        return r;
    }
    const float top = level - W3D_SWIM_DEPTH;   // highest the feet go
    if (in.jump && !in.rooted && in.pos[2] >= top - 0.15f) {
        r.jumpOut = true;
        return r;
    }
    const uint32_t dir = in.rooted ? 0 : in.dir;
    const float fx = (dir & kForward) ? 1.f : (dir & kBackward) ? -1.f : 0.f;
    const float sx = (dir & kStrafeLeft) ? 1.f : (dir & kStrafeRight) ? -1.f : 0.f;
    const bool stroke = fx != 0;                       // forward / backward follow the camera's pitch
    const float pitch = stroke ? strokePitch(in.camPitch) : 0.0f;
    r.pitch = pitch;
    float vz = stroke ? in.speed * sinf(pitch) * fx : 0.0f;
    if (in.ascend && !in.rooted) vz = std::max(vz, in.speed);
    // momentum of the fall and floating back up
    const float decay = expf(-3.0f * in.dt);
    s.vz *= decay;
    if (fabsf(s.vz) < 0.05f) s.vz = 0;
    if (vz != 0) s.surfacing = false;
    float rise = 0;
    if (s.surfacing) {
        rise = kRiseSpeed;
        if (in.pos[2] >= top - 0.01f) s.surfacing = false;
    }
    float z = in.pos[2] + (vz + s.vz + rise) * in.dt;

    // horizontal stroke: at most the swim speed (the pitched part goes into the height)
    float nx = in.pos[0], ny = in.pos[1];
    if (fx != 0 || sx != 0) {
        const float a = in.yaw + atan2f(sx, fx);
        const float h = in.speed * (stroke ? cosf(pitch) : 1.0f) * in.dt;
        nx += cosf(a) * h;
        ny += sinf(a) * h;
        w.walls(in.pos[0], in.pos[1], in.pos[2], &nx, &ny);
    }
    float ground;
    bool haveGround = w.floor(nx, ny, std::max(z, in.pos[2]), &ground);
    if (!haveGround || ground > std::max(z, in.pos[2]) + kStep) {   // not loaded, or a bank too steep: stay
        if (nx != in.pos[0] || ny != in.pos[1]) r.blocked = true;
        nx = in.pos[0];
        ny = in.pos[1];
        haveGround = w.floor(nx, ny, in.pos[2], &ground);
    }
    float levelHere = level;
    uint8_t f2;
    if (!w.liquid(nx, ny, z, &levelHere, &f2)) {   // swam out of the liquid's cells (a bank, the end of a canal)
        r.pos[0] = nx; r.pos[1] = ny; r.pos[2] = std::max(in.pos[2], haveGround ? ground : in.pos[2]);
        r.stop = true;
        return r;
    }
    z = std::min(z, levelHere - W3D_SWIM_DEPTH);
    if (haveGround && z < ground) {
        z = ground;
        if (s.vz < 0) s.vz = 0;
    }
    r.pos[0] = nx; r.pos[1] = ny; r.pos[2] = z;
    // standing up: on a bottom where the water is no deeper than W3D_SWIM_EXIT
    if (haveGround && z <= ground + 0.05f && levelHere - ground < W3D_SWIM_EXIT) r.stop = true;
    return r;
}

}  // namespace swim
