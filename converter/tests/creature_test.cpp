// creature_test — checks the CMDL path end to end without game data:
//   random M2Model (WoWee structs: bone hierarchy, linear tracks per sequence, a global
//   sequence, 4 weights per vertex, several geosets/materials)
//   -> buildCreatureBlob -> w3d_anim_pose + palette skinning (what the 3DS does)
//   vs. reference: WoWee computeBoneMatrices formula on WoWee's own track sampler.
// Reports: bone pose error at the baked samples and between them, skinned vertex error
// (2 strongest weights vs the original 4), palette/weight invariants, and the pack validator.
#include "conv.h"
#include "creature_model.h"
#include "rendering/m2_track_sampler.hpp"
#include "w3d_anim.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>

#include <cstdio>
#include <random>

using namespace wowee::pipeline;

static std::mt19937 rng(20261025);
static float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
static int I(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }

static glm::quat randQuat(float maxAngle) {
    glm::vec3 ax = glm::normalize(glm::vec3(U(-1, 1), U(-1, 1), U(-1, 1)) + glm::vec3(1e-3f));
    return glm::angleAxis(U(-maxAngle, maxAngle), ax);
}

static M2Model makeModel(int seed) {
    rng.seed(seed);
    M2Model m{};
    m.version = 256;
    const int nb = I(3, 60);
    m.globalSequenceDurations = {1700};
    const uint32_t ids[] = {0, 4, 5, 1, 17, 150, 0};   // 150 (Destroy) = not kept; second 0 = variation 1
    for (int s = 0; s < 7; ++s) {
        M2Sequence q{};
        q.id = ids[s];
        q.variationIndex = s == 6 ? 1 : 0;
        q.duration = (uint32_t)I(600, 3000);   // real loops: walk ~1 s, run ~0.7 s, stand 1-3 s
        q.movingSpeed = U(0, 8);
        q.boundRadius = U(1, 3);
        m.sequences.push_back(q);
    }
    for (int b = 0; b < nb; ++b) {
        M2Bone bo{};
        bo.keyBoneId = -1;
        bo.parentBone = (int16_t)(b == 0 ? -1 : I(std::max(-1, b - 4), b - 1));
        bo.pivot = glm::vec3(U(-1, 1), U(-1, 1), U(0, 2));
        auto fill = [&](M2AnimationTrack& tr, int kind) {
            int mode = I(0, 3);   // 0 none, 1 animated, 2 constant, 3 global sequence
            if (mode == 0) return;
            tr.interpolationType = 1;
            tr.globalSequence = mode == 3 ? 0 : -1;
            size_t nseq = mode == 3 ? 1 : m.sequences.size();
            tr.sequences.resize(nseq);
            for (size_t s = 0; s < nseq; ++s) {
                uint32_t dur = mode == 3 ? 1700 : m.sequences[s].duration;
                // smooth motion keyed every 33 ms like real exports: a few sine harmonics per axis
                int nk = mode == 2 ? 1 : (int)(dur / 33) + 1;
                auto& k = tr.sequences[s];
                float ph[3] = {U(0, 6.28f), U(0, 6.28f), U(0, 6.28f)}, am[3] = {U(0.2f, 1), U(0.2f, 1), U(0.2f, 1)};
                glm::vec3 ax = glm::normalize(glm::vec3(U(-1, 1), U(-1, 1), U(-1, 1)) + glm::vec3(1e-3f));
                for (int i = 0; i < nk; ++i) {
                    uint32_t ts = nk == 1 ? 0 : (uint32_t)((uint64_t)dur * i / (nk - 1));
                    float u = nk == 1 ? 0 : (float)i / (nk - 1) * 6.2831853f;   // one loop per sequence
                    k.timestamps.push_back(ts);
                    glm::vec3 w(am[0] * std::sin(u + ph[0]), am[1] * std::sin(2 * u + ph[1]), am[2] * std::sin(u + ph[2]));
                    if (kind == 0) k.vec3Values.push_back(0.3f * w);
                    if (kind == 1) k.quatValues.push_back(glm::angleAxis(0.8f * w.x, ax) * glm::angleAxis(0.4f * w.y, glm::vec3(0, 0, 1)));
                    if (kind == 2) k.vec3Values.push_back(glm::vec3(1.0f) + 0.2f * w);
                }
            }
        };
        fill(bo.translation, 0);
        fill(bo.rotation, 1);
        if (I(0, 3) == 0) fill(bo.scale, 2);
        m.bones.push_back(bo);
    }
    const int nv = I(30, 1500);
    for (int i = 0; i < nv; ++i) {
        M2Vertex v{};
        v.position = glm::vec3(U(-1, 1), U(-1, 1), U(0, 2)) + glm::vec3(i * 1e-4f, 0, 0);   // unique positions
        v.normal = glm::normalize(glm::vec3(U(-1, 1), U(-1, 1), U(-1, 1)) + glm::vec3(1e-3f));
        v.texCoords[0] = glm::vec2(U(0, 1), U(0, 1));
        int nInf = I(1, 4), left = 255;
        for (int k = 0; k < 4; ++k) {
            v.boneIndices[k] = (uint8_t)I(0, nb - 1);
            int w = k + 1 == nInf ? left : (k < nInf ? I(0, left) : 0);
            v.boneWeights[k] = (uint8_t)w;
            left -= w;
        }
        m.vertices.push_back(v);
    }
    const int nt = I(10, 2000);
    for (int t = 0; t < nt * 3; ++t) m.indices.push_back((uint16_t)I(0, nv - 1));
    m.textures = {{11, 0, ""}, {0, 0, "creature\\test\\eye.blp"}, {1, 0, ""}};
    m.textureLookup = {0, 1, 2};
    m.materials = {{0, 0}, {4, 1}, {0, 2}};
    uint32_t start = 0;
    for (int b = 0; b < 3; ++b) {
        M2Batch ba{};
        ba.submeshId = (uint16_t)(b == 2 ? 101 : b);
        ba.materialIndex = (uint16_t)b;
        ba.textureIndex = (uint16_t)b;
        ba.indexStart = start;
        ba.indexCount = (uint32_t)((b == 2 ? nt - (int)start / 3 : nt / 3) * 3);
        start += ba.indexCount;
        m.batches.push_back(ba);
    }
    return m;
}

// WoWee computeBoneMatrices (no billboards), on WoWee's sampler.
static void refPose(const M2Model& m, int seq, float t, std::vector<glm::mat4>& out) {
    using namespace wowee::rendering::m2_track;
    out.resize(m.bones.size());
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const auto& b = m.bones[i];
        glm::vec3 tr = sampleVec3(b.translation, seq, t, t, m.globalSequenceDurations, glm::vec3(0.0f));
        glm::quat q = sampleQuat(b.rotation, seq, t, t, m.globalSequenceDurations);
        glm::vec3 s = sampleVec3(b.scale, seq, t, t, m.globalSequenceDurations, glm::vec3(1.0f));
        if (s.x < 0.001f) s.x = 1; if (s.y < 0.001f) s.y = 1; if (s.z < 0.001f) s.z = 1;
        glm::mat4 l = glm::translate(glm::mat4(1.0f), b.pivot);
        l = glm::translate(l, tr);
        l *= glm::toMat4(q);
        l = glm::scale(l, s);
        l = glm::translate(l, -b.pivot);
        out[i] = (b.parentBone >= 0 && (size_t)b.parentBone < i) ? out[b.parentBone] * l : l;
    }
}

static glm::vec3 apply(const W3dMat34& M, const glm::vec3& p) {
    const float* a = M.m;
    return {a[0] * p.x + a[1] * p.y + a[2] * p.z + a[3], a[4] * p.x + a[5] * p.y + a[6] * p.z + a[7],
            a[8] * p.x + a[9] * p.y + a[10] * p.z + a[11]};
}

int main() {
    int bad = 0;
    double worstSample = 0, worstBetween = 0, worstSkin2 = 0, worst4vs2 = 0;
    size_t models = 0, verts = 0, palettes = 0;
    for (int seed = 1; seed <= 150; ++seed) {
        M2Model m = makeModel(seed);
        w3d::CreatureBuildInfo info;
        auto blob = w3d::buildCreatureBlob(m, 0, [](const std::string&, bool) { return 0u; }, info);
        if (blob.empty()) { std::printf("seed %d: build failed: %s\n", seed, info.error.c_str()); ++bad; continue; }
        ++models;
        const W3dCreatureHeader* h = w3d_creature(blob.data());
        palettes += h->paletteCount;
        if (h->seqCount != 5) { std::printf("seed %d: %u sequences kept, want 5\n", seed, h->seqCount); ++bad; }
        if (seed <= 20) {   // a 1-byte animation budget keeps the essential animations only (stand, walk, run, death, attack)
            w3d::CreatureBuildInfo i2;
            m.sequences.push_back(m.sequences[0]);
            m.sequences.back().id = 69;   // a dance: not essential
            auto b2 = w3d::buildCreatureBlob(m, 0, [](const std::string&, bool) { return 0u; }, i2, 1);
            m.sequences.pop_back();
            const W3dCreatureHeader* h2 = w3d_creature(b2.data());
            if (h2->seqCount != 5 || i2.droppedSeqs != 1 || w3d_find_seq(b2.data(), 69) >= 0) {
                std::printf("seed %d: budget kept %u sequences, dropped %zu\n", seed, h2->seqCount, i2.droppedSeqs);
                ++bad;
            }
        }
        if (h->seatBone >= (int)h->boneCount) { std::printf("seed %d: seat bone %d\n", seed, h->seatBone); ++bad; }
        std::vector<W3dMat34> pose(h->boneCount);
        std::vector<glm::mat4> ref;
        // probe points spread over the model
        std::vector<glm::vec3> probes;
        for (int k = 0; k < 16; ++k) probes.push_back(glm::vec3(U(-1, 1), U(-1, 1), U(0, 2)));
        for (int si = 0; si < (int)m.sequences.size(); ++si) {
            int mine = w3d_find_seq(blob.data(), (uint16_t)m.sequences[si].id);
            if (mine < 0 || m.sequences[si].id == 150 || m.sequences[si].variationIndex) continue;
            const W3dAnimSeq* sq = (const W3dAnimSeq*)(blob.data() + h->offSeqs) + mine;
            for (int k = 0; k <= 40; ++k) {
                bool atSample = k % 2 == 0;
                float t = atSample ? sq->durationMs * (float)(k / 2 % sq->frames) / (float)std::max(1, sq->frames - 1)
                                   : U(0, (float)sq->durationMs);
                w3d_anim_pose(blob.data(), mine, t, 0, pose.data());   // clamp: t may equal the duration
                refPose(m, si, t, ref);
                for (size_t b = 0; b < m.bones.size(); ++b)
                    for (auto& p : probes) {
                        glm::vec3 r = glm::vec3(ref[b] * glm::vec4(p, 1.0f));
                        double e = glm::length(apply(pose[b], p) - r) / (1.0 + glm::length(r));   // relative
                        (atSample ? worstSample : worstBetween) = std::max(atSample ? worstSample : worstBetween, e);
                    }
            }
        }
        // skinning: a mid-animation pose of 'walk', every output vertex
        int seqW = w3d_find_seq(blob.data(), W3D_ANIM_WALK);
        const W3dAnimSeq* sw = (const W3dAnimSeq*)(blob.data() + h->offSeqs) + seqW;
        float tw = sw->durationMs * (float)(sw->frames / 3) / (float)std::max(1, sw->frames - 1);   // a baked sample
        w3d_anim_pose(blob.data(), seqW, tw, 1, pose.data());
        int refSeq = 0;
        for (size_t s = 0; s < m.sequences.size(); ++s) if (m.sequences[s].id == W3D_ANIM_WALK) refSeq = (int)s;
        refPose(m, refSeq, tw, ref);
        const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(blob.data() + h->offSubmeshes);
        const W3dBonePalette* pal = (const W3dBonePalette*)(blob.data() + h->offPalettes);
        const W3dSkinVertex* vx = (const W3dSkinVertex*)(blob.data() + h->offVertices);
        const uint16_t* ix = (const uint16_t*)(blob.data() + h->offIndices);
        for (uint32_t s = 0; s < h->submeshCount; ++s) {
            const W3dBonePalette& p = pal[sm[s].palette];
            if (p.count > W3D_PALETTE_BONES) ++bad;
            for (uint32_t q = sm[s].firstIndex; q < sm[s].firstIndex + sm[s].indexCount; ++q) {
                const W3dSkinVertex& v = vx[ix[q]];
                glm::vec3 bind(v.x * h->posScale[0] + h->posOffset[0], v.y * h->posScale[1] + h->posOffset[1],
                               v.z * h->posScale[2] + h->posOffset[2]);
                // what the vertex shader computes
                glm::vec3 got = (v.weight[0] / 255.0f) * apply(pose[p.bones[v.bone[0] / 3]], bind) +
                                (v.weight[1] / 255.0f) * apply(pose[p.bones[v.bone[1] / 3]], bind);
                // the source vertex (positions are unique)
                size_t best = 0;
                float bd = 1e30f;
                for (size_t i = 0; i < m.vertices.size(); ++i) {
                    float d = glm::length(m.vertices[i].position - bind);
                    if (d < bd) { bd = d; best = i; }
                }
                const M2Vertex& sv = m.vertices[best];
                glm::vec3 r4(0.0f);
                int order[4] = {0, 1, 2, 3};
                std::sort(order, order + 4, [&](int a, int b) { return sv.boneWeights[a] > sv.boneWeights[b]; });
                float w01 = (float)sv.boneWeights[order[0]] + sv.boneWeights[order[1]];
                glm::vec3 r2(0.0f);
                for (int k = 0; k < 4; ++k) {
                    glm::vec3 bp = glm::vec3(ref[sv.boneIndices[k]] * glm::vec4(sv.position, 1.0f));
                    r4 += (sv.boneWeights[k] / 255.0f) * bp;
                }
                for (int k = 0; k < 2; ++k) {
                    glm::vec3 bp = glm::vec3(ref[sv.boneIndices[order[k]]] * glm::vec4(sv.position, 1.0f));
                    r2 += (w01 > 0 ? sv.boneWeights[order[k]] / w01 : (k == 0 ? 1.0f : 0.0f)) * bp;
                }
                worstSkin2 = std::max(worstSkin2, (double)glm::length(got - r2));
                worst4vs2 = std::max(worst4vs2, (double)glm::length(r4 - r2));
                ++verts;
            }
        }
        // the same blob through the pack validator
        if (seed <= 10) {
            w3d::PackWriter pw;
            pw.add(W3D_ENTRY_CREATURE, 1, blob);
            pw.write("/tmp/creature_test.w3dp");
            if (w3d::printPackInfo("/tmp/creature_test.w3dp") != 0) { std::printf("seed %d: validator failed\n", seed); ++bad; }
        }
    }
    std::printf("%zu models, %zu skinned vertices checked, %zu palettes\n", models, verts, palettes);
    std::printf("bone pose error vs WoWee (relative): at baked samples %.6f, between samples %.4f\n", worstSample, worstBetween);
    std::printf("skinned vertex error vs WoWee (2 strongest weights): %.5f yd; 2 vs all 4 weights: %.3f yd\n",
                worstSkin2, worst4vs2);
    // between samples: two piecewise-linear approximations (WoWee keys every 33 ms, ours every
    // 1/30 s) of fast synthetic motion, amplified along bone chains: ~1 degree, not a defect.
    bool ok = bad == 0 && worstSample < 0.005 && worstBetween < 0.12 && worstSkin2 < 0.02;
    std::printf("CREATURES: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
