// A long tiled quad (WMO street): UVs far past the old +-8 limit must survive quantisation (per-model uvScale).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "conv.h"
#include "w3d_format.h"

using namespace w3d;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static float decodedU(const std::vector<uint8_t>& blob, uint32_t vi, float* v) {
    W3dModelHeader h;
    std::memcpy(&h, blob.data(), sizeof h);
    W3dModelVertex q;
    std::memcpy(&q, blob.data() + h.offVertices + vi * sizeof q, sizeof q);
    *v = 1.0f - q.v * w3d_model_uv_scale((const W3dModelHeader*)blob.data());
    return q.u * w3d_model_uv_scale((const W3dModelHeader*)blob.data());
}

int main() {
    for (float span : {1.0f, 7.0f, 75.0f, 300.0f}) {
        ModelSource m;
        m.verts = {{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{100, 0, 0}, {0, 0, 1}, {span, 0}},
                   {{100, 20, 0}, {0, 0, 1}, {span, span / 5}}, {{0, 20, 0}, {0, 0, 1}, {0, span / 5}}};
        ModelSource::Sub s;
        s.texId = UINT32_MAX;
        s.blend = W3D_BLEND_OPAQUE;
        s.flags = 0;
        s.idx = {0, 1, 2, 0, 2, 3};
        m.subs.push_back(s);
        size_t tris = 0;
        auto blob = buildModelBlob(m, 1000, &tris);
        CHECK(!blob.empty(), "span %.0f: no blob", span);
        if (blob.empty()) continue;
        W3dModelHeader h;
        std::memcpy(&h, blob.data(), sizeof h);
        // find the vertex with the largest u: it must come back as ~span (v as ~span/5)
        float best = 0;
        for (uint32_t i = 0; i < h.vertexCount; ++i) {
            float v;
            float u = decodedU(blob, i, &v);
            best = std::max(best, u);
        }
        CHECK(std::fabs(best - span) < span * 0.002f + 0.002f, "span %.0f: largest u decodes as %f", span, best);
        std::printf("span %.0f: uvScale %g (default %g), largest u %f\n", span, h.uvScale, (float)W3D_MODEL_UV_SCALE, best);
    }
    std::printf(fails ? "UV RANGE: FAILED\n" : "UV RANGE: PASS\n");
    return fails ? 1 : 0;
}
