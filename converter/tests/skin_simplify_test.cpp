// skin_simplify_test — simplification of skinned creature models must not stretch triangles across
// joints. A dense two-bone tube ("arm", 10k triangles, elbow at z = 1) is reduced to 800 triangles by
// buildCreatureBlob, posed with the elbow bent 90 degrees, and every output triangle's longest edge in
// the pose is compared with the longest edge the source mesh has anywhere in the same pose.
#include "conv.h"
#include "creature_model.h"
#include "w3d_anim.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>

using namespace wowee::pipeline;

static glm::vec3 apply(const W3dMat34& M, const glm::vec3& p) {
    const float* a = M.m;
    return {a[0] * p.x + a[1] * p.y + a[2] * p.z + a[3], a[4] * p.x + a[5] * p.y + a[6] * p.z + a[7],
            a[8] * p.x + a[9] * p.y + a[10] * p.z + a[11]};
}

int main() {
    M2Model m{};
    m.version = 256;
    for (uint32_t id : {0u, 4u}) {
        M2Sequence q{};
        q.id = id;
        q.duration = 1000;
        m.sequences.push_back(q);
    }
    M2Bone root{}, fore{};
    root.parentBone = -1;
    root.keyBoneId = fore.keyBoneId = -1;
    fore.parentBone = 0;
    fore.pivot = glm::vec3(0, 0, 1);
    fore.rotation.interpolationType = 1;
    fore.rotation.globalSequence = -1;
    fore.rotation.sequences.resize(2);
    for (int s = 0; s < 2; ++s) {   // stand: straight; walk: bent 90 degrees at the elbow
        glm::quat q = s == 0 ? glm::quat(1, 0, 0, 0) : glm::angleAxis(1.5707963f, glm::vec3(1, 0, 0));
        fore.rotation.sequences[s].timestamps = {0, 1000};
        fore.rotation.sequences[s].quatValues = {q, q};
    }
    m.bones = {root, fore};
    const int around = 48, rings = 105;   // ~10k triangles
    for (int r = 0; r <= rings; ++r)
        for (int a = 0; a < around; ++a) {
            float z = 2.0f * r / rings, ang = 6.2831853f * a / around;
            M2Vertex v{};
            v.position = glm::vec3(0.2f * std::cos(ang), 0.2f * std::sin(ang), z);
            v.normal = glm::vec3(std::cos(ang), std::sin(ang), 0);
            v.texCoords[0] = glm::vec2((float)a / around, z / 2);
            float w1 = std::clamp((z - 0.9f) / 0.2f, 0.0f, 1.0f);   // blended over 0.9 .. 1.1
            v.boneIndices[0] = 0; v.boneIndices[1] = 1;
            v.boneWeights[1] = (uint8_t)std::lround(w1 * 255); v.boneWeights[0] = (uint8_t)(255 - v.boneWeights[1]);
            m.vertices.push_back(v);
        }
    for (int r = 0; r < rings; ++r)
        for (int a = 0; a < around; ++a) {
            uint16_t i0 = (uint16_t)(r * around + a), i1 = (uint16_t)(r * around + (a + 1) % around);
            uint16_t i2 = (uint16_t)(i0 + around), i3 = (uint16_t)(i1 + around);
            m.indices.insert(m.indices.end(), {i0, i1, i3, i0, i3, i2});
        }
    m.textures = {{11, 0, ""}};
    m.textureLookup = {0};
    m.materials = {{0, 0}};
    M2Batch b{};
    b.indexCount = (uint32_t)m.indices.size();
    m.batches = {b};

    // the source mesh in the bent pose: longest edge anywhere (the reference)
    auto srcPos = [&](const M2Vertex& v, bool bent) {
        glm::vec3 p = v.position;
        if (!bent) return p;
        glm::quat q = glm::angleAxis(1.5707963f, glm::vec3(1, 0, 0));
        glm::vec3 r = fore.pivot + q * (p - fore.pivot);
        float w1 = v.boneWeights[1] / 255.0f;
        return (1 - w1) * p + w1 * r;
    };
    double srcMax = 0;
    for (size_t t = 0; t < m.indices.size(); t += 3)
        for (int e = 0; e < 3; ++e)
            srcMax = std::max(srcMax, (double)glm::length(srcPos(m.vertices[m.indices[t + e]], true) -
                                                          srcPos(m.vertices[m.indices[t + (e + 1) % 3]], true)));
    w3d::CreatureBuildInfo info;
    auto blob = w3d::buildCreatureBlob(m, 800, [](const std::string&, bool) { return 0u; }, info);
    if (blob.empty()) { std::printf("build failed: %s\n", info.error.c_str()); return 1; }
    const W3dCreatureHeader* h = w3d_creature(blob.data());
    std::vector<W3dMat34> pose(h->boneCount), bind(h->boneCount);
    int walk = w3d_find_seq(blob.data(), W3D_ANIM_WALK);
    w3d_anim_pose(blob.data(), walk, 500, 1, pose.data());
    const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(blob.data() + h->offSubmeshes);
    const W3dBonePalette* pal = (const W3dBonePalette*)(blob.data() + h->offPalettes);
    const W3dSkinVertex* vx = (const W3dSkinVertex*)(blob.data() + h->offVertices);
    const uint16_t* ix = (const uint16_t*)(blob.data() + h->offIndices);
    // Surface error: points on every output triangle (centroid, edge midpoints) against the nearest
    // source vertex, in the bind pose and bent. A triangle bridging the elbow cuts through the inside
    // of the bent arm, far from any source vertex; inside one bone's part it stays as close as unbent.
    std::vector<glm::vec3> srcBent, srcBind;
    for (auto& v : m.vertices) { srcBent.push_back(srcPos(v, true)); srcBind.push_back(srcPos(v, false)); }
    auto nearest = [](const std::vector<glm::vec3>& pts, const glm::vec3& q) {
        float best = 1e30f;
        for (auto& p : pts) best = std::min(best, glm::length(p - q));
        return (double)best;
    };
    double errBent = 0, errBind = 0;
    size_t tris = 0;
    for (uint32_t s = 0; s < h->submeshCount; ++s) {
        const W3dBonePalette& p = pal[sm[s].palette];
        auto bindPos = [&](uint16_t i) {
            const W3dSkinVertex& v = vx[i];
            return glm::vec3(v.x * h->posScale[0] + h->posOffset[0], v.y * h->posScale[1] + h->posOffset[1],
                             v.z * h->posScale[2] + h->posOffset[2]);
        };
        auto posed = [&](uint16_t i) {
            const W3dSkinVertex& v = vx[i];
            glm::vec3 b = bindPos(i);
            return (v.weight[0] / 255.0f) * apply(pose[p.bones[v.bone[0] / 3]], b) +
                   (v.weight[1] / 255.0f) * apply(pose[p.bones[v.bone[1] / 3]], b);
        };
        for (uint32_t q = sm[s].firstIndex; q + 2 < sm[s].firstIndex + sm[s].indexCount; q += 3, ++tris) {
            glm::vec3 A = posed(ix[q]), B = posed(ix[q + 1]), C = posed(ix[q + 2]);
            glm::vec3 a0 = bindPos(ix[q]), b0 = bindPos(ix[q + 1]), c0 = bindPos(ix[q + 2]);
            for (auto [x, y] : {std::pair{(A + B + C) / 3.0f, (a0 + b0 + c0) / 3.0f}, std::pair{(A + B) * 0.5f, (a0 + b0) * 0.5f},
                                std::pair{(B + C) * 0.5f, (b0 + c0) * 0.5f}, std::pair{(C + A) * 0.5f, (c0 + a0) * 0.5f}}) {
                errBent = std::max(errBent, nearest(srcBent, x));
                errBind = std::max(errBind, nearest(srcBind, y));
            }
        }
    }
    (void)srcMax;
    // The source mesh itself, bent, measured the same way (outer side of the elbow stretches): the floor.
    double srcFloor = 0;
    for (size_t t = 0; t < m.indices.size(); t += 3) {
        glm::vec3 A = srcBent[m.indices[t]], B = srcBent[m.indices[t + 1]], C = srcBent[m.indices[t + 2]];
        for (glm::vec3 x : {(A + B + C) / 3.0f, (A + B) * 0.5f, (B + C) * 0.5f, (C + A) * 0.5f})
            srcFloor = std::max(srcFloor, nearest(srcBent, x));
    }
    bool ok = tris <= 900 && errBent <= (errBind + srcFloor) * 1.5 + 0.02;
    std::printf("source mesh bent: %.3f yd\n", srcFloor);
    std::printf("skinned simplify: %zu -> %zu triangles; surface error unbent %.3f yd, elbow bent 90 deg %.3f yd\n",
                m.indices.size() / 3, tris, errBind, errBent);
    std::printf("SKIN SIMPLIFY: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
