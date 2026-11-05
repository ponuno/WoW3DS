// 6.45: poseDoodad - a doodad with a skeleton stands in its Stand pose: a "wing" bone turned 90 degrees in Stand's only key carries its
// vertices along; a model without a skeleton, or one whose Stand is its bind pose, is left alone.
#include "conv.h"

#include "pipeline/m2_loader.hpp"

#include <cmath>
#include <cstdio>

using wowee::pipeline::M2Model;

static int bad = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++bad; } }

static M2Model wing(float angleDeg) {
    M2Model m;
    m.bones.resize(2);
    m.bones[0].parentBone = -1; m.bones[0].pivot = glm::vec3(0.0f);
    m.bones[1].parentBone = 0;  m.bones[1].pivot = glm::vec3(1, 0, 0);   // the wing turns about (1, 0, 0)
    wowee::pipeline::M2Sequence st{};
    st.id = 0; st.duration = 1000;
    m.sequences.push_back(st);
    auto& r = m.bones[1].rotation;
    r.interpolationType = 1;
    r.sequences.resize(1);
    r.sequences[0].timestamps = {0};
    const float a = angleDeg * 3.14159265f / 180.0f;
    r.sequences[0].quatValues = {glm::quat(cosf(a / 2), 0, 0, sinf(a / 2))};   // about Z
    for (int i = 0; i < 3; ++i) {
        wowee::pipeline::M2Vertex v{};
        v.position = i == 0 ? glm::vec3(0, 0, 0) : i == 1 ? glm::vec3(3, 0, 0) : glm::vec3(2, 0, 1);
        v.normal = glm::vec3(0, 1, 0);
        v.boneWeights[0] = 255;
        v.boneIndices[0] = i == 0 ? 0 : 1;   // the body vertex on bone 0, the wing tip on bone 1
        m.vertices.push_back(v);
    }
    return m;
}

static w3d::ModelSource sourceOf(const M2Model& m) {
    w3d::ModelSource s;
    for (const auto& v : m.vertices) {
        w3d::ModelSource::V d{};
        d.p[0] = v.position.x; d.p[1] = v.position.y; d.p[2] = v.position.z;
        d.n[0] = v.normal.x; d.n[1] = v.normal.y; d.n[2] = v.normal.z;
        s.verts.push_back(d);
    }
    return s;
}

int main() {
    {
        M2Model m = wing(90);
        w3d::ModelSource s = sourceOf(m);
        check(w3d::poseDoodad(m, s), "a turned wing: the model is posed");
        // (3,0,0) about the pivot (1,0,0) by 90 degrees about Z -> (1, 2, 0)
        check(std::fabs(s.verts[1].p[0] - 1) < 1e-4f && std::fabs(s.verts[1].p[1] - 2) < 1e-4f, "the wing tip moves with its bone");
        check(std::fabs(s.verts[0].p[0]) < 1e-6f && std::fabs(s.verts[0].p[1]) < 1e-6f, "the body vertex stays");
        check(std::fabs(s.verts[2].p[2] - 1) < 1e-4f && std::fabs(s.verts[2].p[1] - 1) < 1e-4f, "a raised vertex turns about the pivot too");
        check(std::fabs(s.verts[1].n[0] + 1) < 1e-4f, "normals turn with it ((0,1,0) -> (-1,0,0))");
    }
    {
        M2Model m = wing(0);
        w3d::ModelSource s = sourceOf(m);
        check(!w3d::poseDoodad(m, s) && s.verts[1].p[0] == 3, "Stand equal to the bind pose: unchanged");
    }
    {
        M2Model m = wing(90);
        m.bones.clear();
        w3d::ModelSource s = sourceOf(m);
        check(!w3d::poseDoodad(m, s), "no skeleton: unchanged");
    }
    std::printf("POSE DOODAD: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
