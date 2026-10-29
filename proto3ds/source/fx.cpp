// fx.cpp — see fx.hpp. Semantics follow WoWee's M2 particles (m2_renderer_particles.cpp: emission along the model's +Z
// with a spread, gravity down, colour / alpha / scale at birth -> mid -> death, a floor on flame density), simplified for
// static doodads and the 3DS's budget.
#include "fx.hpp"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <iterator>

#include "w3d_format.h"

namespace fx {

Budget budgetFor(int level) {
    Budget b;
    if (level <= 0) { b.maxParticles = 0; b.emitRange = 0; b.glowRange = 0; b.maxGlows = 0; }
    else if (level == 1) { b.maxParticles = 220; b.emitRange = 32.0f; b.glowRange = 90.0f; b.maxGlows = 96; }
    return b;
}

System& global() {
    static System s;
    return s;
}

float System::rnd() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (rng_ >> 8) * (1.0f / 16777216.0f);
}

namespace {
void rotation(const float rot[3], float R[3][3]) {   // Rz(c) * Ry(b) * Rx(a)
    const float ca = cosf(rot[0]), sa = sinf(rot[0]), cb = cosf(rot[1]), sb = sinf(rot[1]), cc = cosf(rot[2]), sc = sinf(rot[2]);
    R[0][0] = cc * cb; R[0][1] = cc * sb * sa - sc * ca; R[0][2] = cc * sb * ca + sc * sa;
    R[1][0] = sc * cb; R[1][1] = sc * sb * sa + cc * ca; R[1][2] = sc * sb * ca - cc * sa;
    R[2][0] = -sb;     R[2][1] = cb * sa;                R[2][2] = cb * ca;
}
void mul(const float R[3][3], const float v[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
}
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int k = 0; k < 32; k += 8) {
        const float ca = (float)((a >> k) & 255), cb = (float)((b >> k) & 255);
        r |= (uint32_t)std::clamp((int)(ca + (cb - ca) * t + 0.5f), 0, 255) << k;
    }
    return r;
}
uint32_t scaleColor(uint32_t c, float rgb, float a) {
    auto ch = [&](int k, float f) { return (uint32_t)std::clamp((int)(((c >> k) & 255) * f + 0.5f), 0, 255) << k; };
    return ch(0, rgb) | ch(8, rgb) | ch(16, rgb) | ch(24, a);
}
// what a quad needs to show at all: in front of the camera and roughly inside the view (60 deg vertical, 400:240)
bool inView(const float c[3], float half) {
    const float z = -c[2];
    if (z < 0.4f) return false;
    return fabsf(c[0]) - half < z * 1.0f + 1.0f && fabsf(c[1]) - half < z * 0.62f + 1.0f;
}
}  // namespace

void System::emit(const Source& s, const W3dFxEmitter& e, const float R[3][3], float scale, const void* tex, const float* bone) {
    Particle p;
    float local[3] = {e.pos[0], e.pos[1], e.pos[2]};
    if (bone)   // the bone's animated pose: the emitter's place moves with it
        for (int r = 0; r < 3; ++r)
            local[r] = bone[r * 4] * e.pos[0] + bone[r * 4 + 1] * e.pos[1] + bone[r * 4 + 2] * e.pos[2] + bone[r * 4 + 3];
    float dir[3];
    const float th = rnd() * e.spreadV, ph = rnd() * (e.spreadH > 0.01f ? e.spreadH : 6.2831853f);
    if (e.type == W3D_FX_SPHERE) {
        // outward from the centre: a full sphere when the emitter gives no cone
        const float cone = e.spreadV > 0.01f ? e.spreadV : 3.14159265f;
        const float t2 = acosf(1.0f - rnd() * (1.0f - cosf(cone))), p2 = rnd() * 6.2831853f;
        dir[0] = sinf(t2) * cosf(p2); dir[1] = sinf(t2) * sinf(p2); dir[2] = cosf(t2);
        const float r = e.areaL * rnd();
        for (int k = 0; k < 3; ++k) local[k] += dir[k] * r;
    } else {
        dir[0] = sinf(th) * cosf(ph); dir[1] = sinf(th) * sinf(ph); dir[2] = cosf(th);
        local[0] += rndS() * 0.5f * e.areaL;
        local[1] += rndS() * 0.5f * e.areaW;
    }
    if (bone) {   // ... and so does where it throws (the rotation / scale part of the pose)
        const float d0[3] = {dir[0], dir[1], dir[2]};
        float l = 0;
        for (int r = 0; r < 3; ++r) {
            dir[r] = bone[r * 4] * d0[0] + bone[r * 4 + 1] * d0[1] + bone[r * 4 + 2] * d0[2];
            l += dir[r] * dir[r];
        }
        l = sqrtf(l);
        if (l > 1e-4f) for (int r = 0; r < 3; ++r) dir[r] /= l;
        else for (int r = 0; r < 3; ++r) dir[r] = d0[r];
    }
    float w[3], d[3];
    for (int k = 0; k < 3; ++k) local[k] *= scale;
    mul(R, local, w);
    mul(R, dir, d);
    float speed = e.speed * (1.0f + rndS() * e.speedVar) * scale;
    for (int k = 0; k < 3; ++k) {
        p.p[k] = s.pos[k] + w[k];
        p.v[k] = d[k] * speed;
    }
    if (fabsf(e.speed) < 0.01f) {   // no speed of its own: a gentle drift instead of a pile (WoWee does the same)
        p.v[0] = rndS() * 0.3f * scale;
        p.v[1] = rndS() * 0.3f * scale;
        p.v[2] = (e.flags & W3D_FXF_FIRE) ? 0.4f * scale : rndS() * 0.15f * scale;
    }
    p.age = 0;
    p.life = e.life * (0.85f + 0.3f * rnd());
    p.e = &e;
    p.owner = s.owner;
    p.tex = tex;
    const int cells = e.rows * e.cols;
    p.tile = (uint8_t)((e.flags & W3D_FXF_RANDOM_TILE) && cells > 1 ? std::min(cells - 1, (int)(rnd() * cells)) : 0);
    parts_.push_back(p);
    ++stats_.emitted;
}

void System::frame(float dt, float timeSec, const float cam[3], const float view[3][3], const std::vector<Source>& sources,
                   std::vector<Quad>& out) {
    ++frame_;
    out.clear();
    stats_.glows = stats_.sources = 0;
    dt = std::clamp(dt, 0.0f, 0.1f);
    const int maxP = budget_.maxParticles;
    if (maxP <= 0 && budget_.maxGlows <= 0) { parts_.clear(); state_.clear(); stats_.alive = 0; stats_.quads = 0; return; }
    if ((int)parts_.size() > maxP) parts_.resize(maxP > 0 ? maxP : 0);

    auto toView = [&](const float w[3], float o[3]) {
        const float d[3] = {w[0] - cam[0], w[1] - cam[1], w[2] - cam[2]};
        for (int i = 0; i < 3; ++i) o[i] = view[i][0] * d[0] + view[i][1] * d[1] + view[i][2] * d[2];
    };

    // ---- emit and glow, per source
    int glows = 0;
    for (const Source& s : sources) {
        const W3dFxHeader* h = s.h;
        if (!h) continue;
        const float dx = s.pos[0] - cam[0], dy = s.pos[1] - cam[1], dz = s.pos[2] - cam[2];
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        ++stats_.sources;
        float R[3][3];
        rotation(s.rot, R);
        const W3dFxEmitter* em = (const W3dFxEmitter*)(s.blob + h->offEmitters);
        bool fire = false;
        for (unsigned k = 0; k < h->emitterCount; ++k) fire |= (em[k].flags & W3D_FXF_FIRE) != 0;
        if (h->emitterCount && maxP > 0 && (dist < budget_.emitRange || (s.priority && dist < 80.0f))) {
            EmitterState& st = state_[s.key];
            st.frame = frame_;
            // farther away: fewer (a third at the edge of the range); a filling pool thins every emitter alike
            float f = 1.0f - 0.65f * std::clamp((dist - 15.0f) / std::max(budget_.emitRange - 15.0f, 1.0f), 0.0f, 1.0f);
            const float fill = (float)parts_.size() / (float)maxP;
            if (fill > 0.75f) f *= std::max(0.0f, (1.0f - fill) / 0.25f);
            if (s.priority) f = 1.0f;   // spells: what the player is looking at (the pool's hard cap still holds)
            for (unsigned k = 0; k < h->emitterCount && k < 8; ++k) {
                const W3dFxEmitter& e = em[k];
                float rate = e.rate;
                // a flame reads as one only with enough particles alive at once (WoWee: kMinLiveParticles)
                if (e.flags & W3D_FXF_FIRE) rate = std::max(rate, 8.0f / std::max(e.life, 0.1f));
                st.acc[k] = std::min(st.acc[k] + rate * f * dt, 3.0f);
                const void* tex = e.tex < h->texCount && s.tex ? s.tex[e.tex] : nullptr;
                while (st.acc[k] >= 1.0f) {
                    st.acc[k] -= 1.0f;
                    if ((int)parts_.size() >= maxP) { ++stats_.dropped; continue; }
                    float bm[12];
                    if (s.anim.a) w3d_fx_anim_matrix(&s.anim, k < s.anim.a->emitterCount ? s.anim.emitterBone[k] : -1, s.animMs, bm);
                    emit(s, e, R, s.scale, tex, s.anim.a ? bm : nullptr);
                }
            }
        }
        if (h->glowCount && glows < budget_.maxGlows && dist < budget_.glowRange) {
            const W3dFxGlow* g = (const W3dFxGlow*)(s.blob + h->offGlows);
            const float fade = 1.0f - std::clamp((dist - budget_.glowRange * 0.75f) / (budget_.glowRange * 0.25f), 0.0f, 1.0f);
            for (unsigned k = 0; k < h->glowCount && glows < budget_.maxGlows; ++k) {
                float gp[3] = {g[k].pos[0], g[k].pos[1], g[k].pos[2]};
                if (s.anim.a) {
                    float bm[12];
                    w3d_fx_anim_matrix(&s.anim, k < s.anim.a->glowCount ? s.anim.glowBone[k] : -1, s.animMs, bm);
                    for (int r = 0; r < 3; ++r) gp[r] = bm[r * 4] * g[k].pos[0] + bm[r * 4 + 1] * g[k].pos[1] + bm[r * 4 + 2] * g[k].pos[2] + bm[r * 4 + 3];
                }
                float lw[3] = {gp[0] * s.scale, gp[1] * s.scale, gp[2] * s.scale}, w[3];
                mul(R, lw, w);
                for (int i = 0; i < 3; ++i) w[i] += s.pos[i];
                Quad q;
                toView(w, q.c);
                q.half = g[k].size * s.scale * 1.5f;
                if (!inView(q.c, q.half)) continue;
                q.c[2] += q.half * 0.6f;   // a little toward the camera: not cut by the lamp's own glass
                float flick = 1.0f;
                if (fire) flick = 0.9f + 0.1f * sinf(timeSec * 11.0f + (float)((uintptr_t)s.key % 97));
                const float nearFade = std::clamp((-q.c[2] - 0.4f) / 1.6f, 0.0f, 1.0f);
                q.color = scaleColor(g[k].color, flick * nearFade, 0.85f * fade * nearFade);
                q.u0 = 0; q.v0 = 0; q.u1 = 1; q.v1 = 1;
                q.tex = nullptr;
                q.blend = W3D_BLEND_ADD;
                out.push_back(q);
                ++glows;
            }
        }
    }
    stats_.glows = (uint32_t)glows;
    if ((frame_ & 63) == 0)   // emitters not seen for a while (the camera left, the tile went)
        for (auto it = state_.begin(); it != state_.end();) it = frame_ - it->second.frame > 120 ? state_.erase(it) : std::next(it);

    // ---- move, age, draw
    size_t w = 0;
    for (size_t i = 0; i < parts_.size(); ++i) {
        Particle p = parts_[i];
        p.age += dt;
        if (p.age >= p.life) continue;
        const W3dFxEmitter& e = *p.e;
        p.v[2] -= e.gravity * dt;
        if (e.drag > 0) {
            const float k = std::max(0.0f, 1.0f - e.drag * dt);
            for (int c = 0; c < 3; ++c) p.v[c] *= k;
        }
        for (int c = 0; c < 3; ++c) p.p[c] += p.v[c] * dt;
        parts_[w++] = p;
        const float t = p.age / p.life;
        float size;
        uint32_t col;
        if (t < e.mid) {
            const float u = t / e.mid;
            size = lerp(e.scale[0], e.scale[1], u);
            col = lerpColor(e.color[0], e.color[1], u);
        } else {
            const float u = (t - e.mid) / std::max(1.0f - e.mid, 0.01f);
            size = lerp(e.scale[1], e.scale[2], u);
            col = lerpColor(e.color[1], e.color[2], u);
        }
        if (e.flags & W3D_FXF_FIRE) size = std::clamp(size, 0.1f, 0.9f);
        else if (e.flags & W3D_FXF_SPELL) size = std::clamp(size, 0.12f, 2.5f);   // spells: few, short, may be big
        else size = std::min(size, 1.5f);   // big smoke is the fill-rate killer
        Quad q;
        toView(p.p, q.c);
        q.half = size * 0.5f;
        if (q.half < 0.01f || !inView(q.c, q.half)) continue;
        const int cells = e.rows * e.cols;
        int cell = p.tile;
        if (!(e.flags & W3D_FXF_RANDOM_TILE) && cells > 1) cell = std::min(cells - 1, (int)(t * cells));
        const int cx = cell % e.cols, cy = cell / e.cols;
        q.u0 = (float)cx / e.cols;
        q.u1 = (float)(cx + 1) / e.cols;
        q.v1 = 1.0f - (float)cy / e.rows;        // top of the cell
        q.v0 = 1.0f - (float)(cy + 1) / e.rows;
        // near the eye (our own hands in the first person, a spell right at the camera): fade out - a particle filling
        // the screen costs a whole frame of fill rate
        const float nearFade = std::clamp((-q.c[2] - 0.4f) / 1.6f, 0.0f, 1.0f);
        q.color = nearFade < 1.0f ? scaleColor(col, e.blend == W3D_BLEND_ADD ? nearFade : 1.0f, nearFade) : col;
        q.tex = p.tex;
        q.blend = e.blend == W3D_BLEND_ADD ? W3D_BLEND_ADD : W3D_BLEND_ALPHA;
        out.push_back(q);
    }
    parts_.resize(w);
    stats_.alive = (uint32_t)parts_.size();
    stats_.quads = (uint32_t)out.size();
}

void System::forget(const void* owner) {
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [&](const Particle& p) { return p.owner == owner; }), parts_.end());
}

void System::clear() {
    parts_.clear();
    state_.clear();
}

}  // namespace fx

void fxForgetModel(const void* model) { fx::global().forget(model); }
