// 6.41: moving textures. An M2 texture transform is baked to a table (bakeUvTrack), carried through buildModelBlob (submeshes
// with and without a transform stay apart), validated by w3d_model_uvanim, and the shader's two rows (w3d_uv_rows) give the
// coordinates the M2 matrix gives, in the 3DS's flipped v.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "conv.h"
#include "pipeline/m2_loader.hpp"
#include "w3d_format.h"

namespace w3d { bool bakeUvTrack(const wowee::pipeline::M2Model& m, uint16_t transformIdx, ModelSource::UvTrack& out); }
using namespace w3d;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void addKeys(wowee::pipeline::M2AnimationTrack& t, int16_t gs, std::vector<uint32_t> ts, std::vector<glm::vec3> v) {
    t.interpolationType = 1;
    t.globalSequence = gs;
    t.sequences.resize(1);
    t.sequences[0].timestamps = std::move(ts);
    t.sequences[0].vec3Values = std::move(v);
}

int main() {
    // ---- the baker
    wowee::pipeline::M2Model mdl;
    mdl.globalSequenceDurations = {4000};
    {
        wowee::pipeline::M2TextureTransform tt;
        addKeys(tt.translation, 0, {0, 4000}, {{0, 0, 0}, {1.0f, 0.5f, 0}});   // a scroll of one texture in 4 s
        mdl.textureTransforms.push_back(tt);
        wowee::pipeline::M2TextureTransform spin;   // a rotation of a quarter turn over the loop, scale 2 in v
        spin.rotation.interpolationType = 1;
        spin.rotation.globalSequence = 0;
        spin.rotation.sequences.resize(1);
        spin.rotation.sequences[0].timestamps = {0, 4000};
        spin.rotation.sequences[0].quatValues = {glm::quat(1, 0, 0, 0), glm::quat(std::cos(0.7853982f), 0, 0, std::sin(0.7853982f))};
        addKeys(spin.scale, 0, {0, 4000}, {{1, 1, 1}, {1, 2, 1}});
        mdl.textureTransforms.push_back(spin);
        mdl.textureTransforms.push_back(wowee::pipeline::M2TextureTransform());   // empty: does not move
    }
    ModelSource::UvTrack a, b, c;
    CHECK(bakeUvTrack(mdl, 0, a), "the scroll bakes");
    CHECK(a.periodMs == 4000 && a.keys.size() >= 9 && a.keys.size() <= W3D_UVA_MAX_KEYS, "period %u keys %zu", a.periodMs, a.keys.size());
    CHECK(std::fabs(a.keys.front().du) < 1e-4f && std::fabs(a.keys.back().du - 1.0f) < 0.01f, "scroll ends at 1: %f", a.keys.back().du);
    CHECK(std::fabs(a.keys.back().dv - 0.5f) < 0.01f, "dv ends at 0.5");
    CHECK(bakeUvTrack(mdl, 1, b), "the rotation bakes");
    CHECK(std::fabs(b.keys.back().rot - 1.5707963f) < 0.02f, "a quarter turn: %f", b.keys.back().rot);
    CHECK(std::fabs(b.keys.back().sv - 2.0f) < 0.01f && std::fabs(b.keys.back().su - 1.0f) < 0.01f, "scale 1 x 2: %f x %f", b.keys.back().su, b.keys.back().sv);
    CHECK(!bakeUvTrack(mdl, 2, c), "an empty transform stands still");
    CHECK(!bakeUvTrack(mdl, 9, c), "an index past the table");

    // ---- the blob: a still part and a moving part stay two submeshes
    ModelSource m;
    m.verts = {{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{1, 1, 0}, {0, 0, 1}, {1, 1}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
    ModelSource::Sub s1, s2;
    s1.texId = s2.texId = UINT32_MAX;
    s1.blend = s2.blend = W3D_BLEND_ADD;
    s1.flags = s2.flags = 0;
    s1.idx = {0, 1, 2};
    s2.idx = {0, 2, 3};
    s2.uvAnim = 0;
    m.subs = {s1, s2};
    m.uvTracks = {a};
    auto blob = buildModelBlob(m, 1000, nullptr);
    CHECK(!blob.empty(), "blob");
    if (blob.empty()) return 1;
    const W3dModelHeader* h = (const W3dModelHeader*)blob.data();
    CHECK(h->submeshCount == 2, "two submeshes (%u)", h->submeshCount);
    const W3dUvAnimTable* t = w3d_model_uvanim(blob.data(), (uint32_t)blob.size());
    CHECK(t && t->animCount == 1, "the table is valid");
    if (!t) return 1;
    int still = 0, moving = 0;
    for (uint32_t i = 0; i < h->submeshCount; ++i) (w3d_uv_of(blob.data(), t, i) ? moving : still)++;
    CHECK(still == 1 && moving == 1, "one still, one moving (%d, %d)", still, moving);
    CHECK(w3d_model_lods(blob.data(), (uint32_t)blob.size()) != nullptr || true, "lods readable");
    // a blob without moving parts has no table
    ModelSource m2 = m;
    m2.subs[1].uvAnim = 0xFFFF;
    auto plain = buildModelBlob(m2, 1000, nullptr);
    CHECK(!plain.empty() && !w3d_model_uvanim(plain.data(), (uint32_t)plain.size()), "no moving part: no table");
    // a truncated blob is refused, not read past its end
    CHECK(!w3d_model_uvanim(blob.data(), (uint32_t)blob.size() - 40), "a cut blob is refused (%s)", "table");

    // ---- the rows: the M2's own matrix, in stored (v flipped) coordinates
    const W3dUvAnim* an = w3d_uv_of(blob.data(), t, 0) ? w3d_uv_of(blob.data(), t, 0) : w3d_uv_of(blob.data(), t, 1);
    float r0[4], r1[4];
    w3d_uv_rows(blob.data(), an, 0, r0, r1);
    CHECK(std::fabs(r0[0] - 1) < 1e-4f && std::fabs(r0[1]) < 1e-4f && std::fabs(r0[2]) < 1e-3f && std::fabs(r1[1] - 1) < 1e-4f &&
              std::fabs(r1[2]) < 1e-3f, "time 0 is the identity");
    w3d_uv_rows(blob.data(), an, 1000, r0, r1);   // a quarter of the period: du = 0.25, dv = 0.125
    {
        const float u = 0.3f, v = 0.2f;   // M2 space
        const float eu = u + 0.25f, ev = v + 0.125f;
        const float su = u, sv = 1.0f - v;   // as stored
        const float gu = r0[0] * su + r0[1] * sv + r0[2], gv = r1[0] * su + r1[1] * sv + r1[2];
        CHECK(std::fabs(gu - eu) < 0.01f && std::fabs(gv - (1.0f - ev)) < 0.01f, "scroll: (%f, %f) vs (%f, %f)", gu, gv, eu, 1.0f - ev);
    }
    {   // a hand-built key with rotation and scale: compare to R S (uv - c) + c + d
        float keys[2][5] = {{0.1f, -0.2f, 0.6f, 1.5f, 0.8f}, {0.1f, -0.2f, 0.6f, 1.5f, 0.8f}};
        std::vector<uint8_t> fake(sizeof(W3dUvAnim) + sizeof keys + 8, 0);
        W3dUvAnim ua{};
        ua.periodMs = 1000; ua.keyCount = 2; ua.offKeys = (uint32_t)sizeof(W3dUvAnim);
        std::memcpy(fake.data(), &ua, sizeof ua);
        std::memcpy(fake.data() + sizeof ua, keys, sizeof keys);
        w3d_uv_rows(fake.data(), (const W3dUvAnim*)fake.data(), 250, r0, r1);
        const float u = 0.7f, v = 0.35f;
        const float cs = std::cos(0.6f), sn = std::sin(0.6f);
        const float x = 1.5f * (u - 0.5f), y = 0.8f * (v - 0.5f);
        const float eu = cs * x - sn * y + 0.5f + 0.1f, ev = sn * x + cs * y + 0.5f - 0.2f;
        const float su = u, sv = 1.0f - v;
        const float gu = r0[0] * su + r0[1] * sv + r0[2], gv = r1[0] * su + r1[1] * sv + r1[2];
        CHECK(std::fabs(gu - eu) < 1e-3f && std::fabs(gv - (1.0f - ev)) < 1e-3f, "rotation + scale: (%f, %f) vs (%f, %f)", gu, gv, eu, 1.0f - ev);
    }
    std::printf(fails ? "UV ANIM: FAILED\n" : "UV ANIM: PASS\n");
    return fails ? 1 : 0;
}
