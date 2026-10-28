// Stage 5.32: the particle / glow core (source/fx.cpp) on the PC: population, budget, ranges, transforms, glows,
// forgetting a freed model, hitches. Blobs are built here with common/w3d_fx.h's layout and read back through
// w3d_model_fx (the footer the converter writes).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "fx.hpp"
#include "w3d_format.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// a "model blob": 64 bytes of mesh stand-in, then the fx section and its footer
static std::vector<uint8_t> blobWith(const std::vector<W3dFxEmitter>& em, const std::vector<W3dFxGlow>& gl, int texCount) {
    std::vector<uint8_t> b(64, 0xAB);
    W3dFxHeader h{};
    auto put = [&](const void* p, size_t n) { const uint8_t* s = (const uint8_t*)p; b.insert(b.end(), s, s + n); };
    h.texCount = (uint16_t)texCount;
    h.offTexIds = (uint32_t)b.size();
    for (int i = 0; i < texCount; ++i) { uint32_t id = 1000 + i; put(&id, 4); }
    h.emitterCount = (uint16_t)em.size();
    h.offEmitters = (uint32_t)b.size();
    if (!em.empty()) put(em.data(), em.size() * sizeof(W3dFxEmitter));
    h.glowCount = (uint16_t)gl.size();
    h.offGlows = (uint32_t)b.size();
    if (!gl.empty()) put(gl.data(), gl.size() * sizeof(W3dFxGlow));
    const uint32_t off = (uint32_t)b.size();
    put(&h, sizeof h);
    const uint32_t foot[2] = {off, W3D_FX_MAGIC};
    put(foot, 8);
    return b;
}

static W3dFxEmitter flame() {
    W3dFxEmitter e{};
    e.pos[0] = 1; e.pos[1] = 0; e.pos[2] = 2;   // 1 yd along the model's +X, 2 up
    e.rate = 20; e.life = 1.0f; e.speed = 1.0f; e.gravity = -0.5f;
    e.scale[0] = 0.3f; e.scale[1] = 0.25f; e.scale[2] = 0.1f;
    e.color[0] = 0xFF50C8FF; e.color[1] = 0xC82878FF; e.color[2] = 0x00142878;
    e.mid = 0.5f; e.tex = 0; e.blend = W3D_BLEND_ADD; e.type = W3D_FX_PLANE; e.rows = 2; e.cols = 2;
    e.flags = W3D_FXF_FIRE;
    return e;
}

int main() {
    // the view: camera at the origin looking along +X (world -> view rows: x right = -Y, y up = +Z, z = -X)
    const float view[3][3] = {{0, -1, 0}, {0, 0, 1}, {-1, 0, 0}};
    const float cam[3] = {0, 0, 0};
    static_assert(sizeof(W3dFxEmitter) == 88 && sizeof(W3dFxGlow) == 20 && sizeof(W3dFxHeader) == 20, "fx layout");

    // ---- the footer
    std::vector<uint8_t> b1 = blobWith({flame()}, {}, 1);
    const W3dFxHeader* h1 = w3d_model_fx(b1.data(), (uint32_t)b1.size());
    CHECK(h1 && h1->emitterCount == 1 && h1->texCount == 1, "footer found");
    std::vector<uint8_t> bad = b1;
    bad[bad.size() - 1] ^= 1;
    CHECK(!w3d_model_fx(bad.data(), (uint32_t)bad.size()), "broken magic rejected");
    std::vector<uint8_t> plain(200, 0);
    CHECK(!w3d_model_fx(plain.data(), (uint32_t)plain.size()), "no footer: no fx");
    std::vector<uint8_t> evil = b1;   // header claiming more emitters than the blob holds
    W3dFxHeader* eh = (W3dFxHeader*)(evil.data() + (evil.size() - 8 - sizeof(W3dFxHeader)));
    eh->emitterCount = 5000;
    CHECK(!w3d_model_fx(evil.data(), (uint32_t)evil.size()), "oversized counts rejected");

    // ---- stage 5.35: the bone animation block is validated against the blob
    {
        std::vector<uint8_t> b = b1;   // inject a block before the header: 1 bone, 2 frames, 0 parts, 1 emitter, 0 glows
        const size_t hdrAt = b.size() - 8 - sizeof(W3dFxHeader);
        W3dFxHeader fh;
        memcpy(&fh, b.data() + hdrAt, sizeof fh);
        std::vector<uint8_t> blk;
        W3dFxAnim a{};
        a.magic = W3D_FXA_MAGIC; a.boneCount = 1; a.frames = 2; a.durationMs = 1000; a.subCount = 0; a.emitterCount = 1; a.glowCount = 0;
        auto put = [&](const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; blk.insert(blk.end(), q, q + n); };
        put(&a, sizeof a);
        const uint8_t eb[4] = {0, 0, 0, 0};
        put(eb, 4);
        float m[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,   1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 2};   // frame 1: lifted 2 yd
        put(m, sizeof m);
        std::vector<uint8_t> out(b.begin(), b.begin() + hdrAt);
        while (out.size() % 4) out.push_back(0);
        fh.animOff4 = (uint16_t)(out.size() / 4);
        out.insert(out.end(), blk.begin(), blk.end());
        const uint32_t off = (uint32_t)out.size();
        out.insert(out.end(), (const uint8_t*)&fh, (const uint8_t*)&fh + sizeof fh);
        const uint32_t foot[2] = {off, W3D_FX_MAGIC};
        out.insert(out.end(), (const uint8_t*)foot, (const uint8_t*)foot + 8);
        const W3dFxHeader* fx = w3d_model_fx(out.data(), (uint32_t)out.size());
        CHECK(fx != nullptr, "fx header with an animation block");
        W3dFxAnimView v = w3d_fx_anim(out.data(), (uint32_t)out.size(), fx);
        CHECK(v.a && v.a->frames == 2, "animation block found");
        float r[12];
        if (v.a) {
            w3d_fx_anim_matrix(&v, 0, 0, r);
            CHECK(fabsf(r[11]) < 1e-5f, "frame 0: not lifted");
            w3d_fx_anim_matrix(&v, 0, 250, r);
            CHECK(fabsf(r[11] - 1.0f) < 1e-3f, "a quarter of the loop: halfway between the two samples (%.3f)", r[11]);
            w3d_fx_anim_matrix(&v, 0, 1000, r);
            CHECK(fabsf(r[11]) < 1e-5f, "the loop wraps");
            w3d_fx_anim_matrix(&v, 7, 0, r);
            CHECK(fabsf(r[0] - 1) < 1e-5f && fabsf(r[11]) < 1e-5f, "a bone out of range: no motion");
        }
        std::vector<uint8_t> trunc = out;   // a block that claims more frames than the blob holds
        ((W3dFxAnim*)(trunc.data() + (size_t)fh.animOff4 * 4))->frames = 32;
        CHECK(!w3d_fx_anim(trunc.data(), (uint32_t)trunc.size(), w3d_model_fx(trunc.data(), (uint32_t)trunc.size())).a, "oversized block rejected");
        std::vector<uint8_t> zero = out;
        ((W3dFxAnim*)(zero.data() + (size_t)fh.animOff4 * 4))->durationMs = 0;
        CHECK(!w3d_fx_anim(zero.data(), (uint32_t)zero.size(), w3d_model_fx(zero.data(), (uint32_t)zero.size())).a, "zero loop rejected");
        CHECK(!w3d_fx_anim(b1.data(), (uint32_t)b1.size(), h1).a, "no block: no animation");
    }

    // ---- one torch 10 yd in front: rotated 90 deg about Z (the emitter's +X goes to world +Y), scale 2
    int texHandle = 7;
    const void* texs[1] = {&texHandle};
    fx::Source s;
    s.key = &s; s.owner = (const void*)0x1234; s.blob = b1.data(); s.h = h1; s.tex = texs;
    s.pos[0] = 10; s.pos[1] = 0; s.pos[2] = 0;
    s.rot[2] = 1.5707963f;
    s.scale = 2;
    fx::System sys;
    sys.setBudget(fx::budgetFor(2));
    std::vector<fx::Quad> q;
    std::vector<fx::Source> src{s};
    size_t peak = 0;
    double sum = 0;
    int n = 0;
    for (int f = 0; f < 300; ++f) {   // 10 s at 30 fps
        sys.frame(1.0f / 30, f / 30.0f, cam, view, src, q);
        if (f > 90) { sum += sys.alive(); ++n; }
        peak = std::max(peak, sys.alive());
    }
    const double avg = sum / n;
    CHECK(avg > 14 && avg < 26, "steady population ~ rate * life = 20: %.1f", avg);
    printf("one torch: %.1f alive on average (peak %zu), %zu quads now\n", avg, peak, q.size());
    bool allTex = !q.empty(), placed = true, sizes = true;
    for (const fx::Quad& x : q) {
        allTex &= x.tex == &texHandle && x.blend == W3D_BLEND_ADD;
        // world: (10, 0, 0) + Rz(90) * (2 * (1, 0, 2)) = (10, 2, 4), rising: view x = -world y ~ -2, z = -world x ~ -10
        placed &= fabsf(x.c[0] + 2.0f) < 2.5f && x.c[2] < -7.0f && x.c[2] > -13.0f && x.c[1] > 3.0f;
        sizes &= x.half > 0.04f && x.half <= 0.45f;   // a flame: 0.1 .. 0.9 yd across
    }
    CHECK(allTex, "particles carry the emitter's texture and blend");
    CHECK(placed, "particles start at the transformed emitter (rotation, scale)");
    CHECK(sizes, "flame sizes kept small");
    bool cells = true;
    for (const fx::Quad& x : q) cells &= (x.u1 - x.u0) > 0.49f && (x.u1 - x.u0) < 0.51f && (x.v1 - x.v0) > 0.49f;
    CHECK(cells, "2x2 atlas: a cell per quad");

    // ---- forgetting the model
    sys.forget(s.owner);
    CHECK(sys.alive() == 0, "forget drops the model's particles: %zu left", sys.alive());

    // ---- out of range and behind the camera
    fx::Source far = s;
    far.pos[0] = 80;   // beyond the 50 yd emit range
    src = {far};
    for (int f = 0; f < 60; ++f) sys.frame(1.0f / 30, 0, cam, view, src, q);
    CHECK(sys.alive() == 0, "no emission beyond the range");
    fx::Source behind = s;
    behind.pos[0] = -10;
    src = {behind};
    for (int f = 0; f < 60; ++f) sys.frame(1.0f / 30, 0, cam, view, src, q);
    CHECK(sys.alive() > 0 && q.empty(), "behind the camera: alive (it will be seen when turning) but no quads");
    sys.clear();

    // ---- the budget: 200 torches next to each other
    std::vector<fx::Source> many;
    std::vector<int> keys(200);
    for (int i = 0; i < 200; ++i) {
        fx::Source t = s;
        t.key = &keys[i];
        t.pos[0] = 8 + (i % 20) * 0.5f;
        t.pos[1] = (i / 20) * 0.5f - 2.5f;
        many.push_back(t);
    }
    size_t maxAlive = 0;
    for (int f = 0; f < 200; ++f) {
        sys.frame(1.0f / 30, f / 30.0f, cam, view, many, q);
        maxAlive = std::max(maxAlive, sys.alive());
    }
    printf("200 torches: at most %zu alive, %zu quads, %u dropped\n", maxAlive, q.size(), (unsigned)sys.stats().dropped);
    CHECK(maxAlive <= 500, "never more than the budget alive: %zu", maxAlive);
    CHECK(maxAlive > 350, "the budget is used: %zu", maxAlive);
    sys.setBudget(fx::budgetFor(1));
    sys.frame(1.0f / 30, 0, cam, view, many, q);
    CHECK(sys.alive() <= 220, "low level: at most 220 (%zu)", sys.alive());
    sys.setBudget(fx::budgetFor(0));
    sys.frame(1.0f / 30, 0, cam, view, many, q);
    CHECK(sys.alive() == 0 && q.empty(), "off: nothing");
    sys.clear();

    // ---- glows: one per lamp, facing the camera, fading at the edge of the range
    W3dFxGlow g{};
    g.pos[2] = 2.2f; g.size = 0.3f; g.color = 0xE675D1FFu;
    std::vector<uint8_t> b2 = blobWith({}, {g}, 0);
    fx::Source lamp;
    lamp.key = &lamp; lamp.owner = (const void*)0x99; lamp.blob = b2.data(); lamp.h = w3d_model_fx(b2.data(), (uint32_t)b2.size());
    lamp.pos[0] = 30;
    sys.setBudget(fx::budgetFor(2));
    src = {lamp};
    sys.frame(1.0f / 30, 0, cam, view, src, q);
    CHECK(q.size() == 1 && !q[0].tex && q[0].blend == W3D_BLEND_ADD, "a glow: one additive quad of the soft dot");
    if (q.size() == 1) {
        CHECK(fabsf(q[0].c[1] - 2.2f) < 0.01f && q[0].c[2] > -30.0f && q[0].c[2] < -29.0f, "glow at the lamp, a little toward the eye");
        CHECK(fabsf(q[0].half - 0.45f) < 0.01f, "glow size 1.5 x its radius: %.2f", q[0].half);
        CHECK((q[0].color >> 24) > 150, "full strength near: %u", q[0].color >> 24);
    }
    lamp.pos[0] = 130;   // 75%..100% of the 140 yd glow range: fading
    src = {lamp};
    sys.frame(1.0f / 30, 0, cam, view, src, q);
    CHECK(q.size() == 1 && (q[0].color >> 24) < 120, "fading near the range's end");
    lamp.pos[0] = 150;
    src = {lamp};
    sys.frame(1.0f / 30, 0, cam, view, src, q);
    CHECK(q.empty(), "no glow beyond the range");

    // ---- hitches and odd data: a 2 s frame, zero / huge values
    W3dFxEmitter odd = flame();
    odd.speed = 0; odd.flags = 0; odd.scale[0] = odd.scale[1] = odd.scale[2] = 9.0f; odd.blend = W3D_BLEND_ALPHA;
    odd.type = W3D_FX_SPHERE; odd.areaL = 2; odd.tex = 0xFFFF;
    std::vector<uint8_t> b3 = blobWith({odd}, {}, 0);
    fx::Source o = s;
    o.blob = b3.data(); o.h = w3d_model_fx(b3.data(), (uint32_t)b3.size()); o.tex = nullptr;
    src = {o};
    sys.frame(2.0f, 0, cam, view, src, q);
    size_t burst = sys.alive();
    for (int f = 0; f < 90; ++f) sys.frame(1.0f / 30, 0, cam, view, src, q);
    bool smokeOk = !q.empty();
    for (const fx::Quad& x : q) smokeOk &= x.half <= 0.75f && !x.tex && x.blend == W3D_BLEND_ALPHA && isfinite(x.c[0]);
    CHECK(burst <= 4, "a long frame does not burst (%zu)", burst);
    CHECK(smokeOk, "big alpha particles capped at 1.5 yd, soft dot, finite");

    printf("FX: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
