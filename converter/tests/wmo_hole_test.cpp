// Stage 5.31: walls of Stormwind had triangular holes that never closed. A WMO group over the triangle budget
// (--wmo-tris, 3000) is simplified one material at a time; a material's patch is an open mesh, and with its border free
// the simplifier collapsed border corners away - cutting triangles out of the wall next to the other material.
// Here: a bumpy wall of two materials (vertices duplicated at the seam, as WMO batches have their own vertex ranges),
// 7200 triangles, budget 3000. Every point of the wall must still be covered after simplification.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "conv.h"
#include "w3d_format.h"

using namespace w3d;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// A big group (a district's canal: 300 yd across) made of small blocks (pillars 2 x 2 x 6 yd, faces subdivided): with an
// error bound relative to the group's size (2% = 6 yd) the simplifier folded whole faces of the blocks away. Every
// original surface point must stay within 0.5 yd of the simplified mesh.
static float pointTri(const float p[3], const float a[3], const float b[3], const float c[3]) {
    // distance from p to triangle abc (Ericson, closest point on triangle)
    float ab[3], ac[3], ap[3];
    for (int k = 0; k < 3; ++k) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; ap[k] = p[k] - a[k]; }
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    float q[3];
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    float bp[3], cp[3];
    for (int k = 0; k < 3; ++k) { bp[k] = p[k] - b[k]; cp[k] = p[k] - c[k]; }
    const float d3 = dot(ab, bp), d4 = dot(ac, bp), d5 = dot(ab, cp), d6 = dot(ac, cp);
    const float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
    if (d1 <= 0 && d2 <= 0) { for (int k = 0; k < 3; ++k) q[k] = a[k]; }
    else if (d3 >= 0 && d4 <= d3) { for (int k = 0; k < 3; ++k) q[k] = b[k]; }
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) { const float v = d1 / (d1 - d3); for (int k = 0; k < 3; ++k) q[k] = a[k] + v * ab[k]; }
    else if (d6 >= 0 && d5 <= d6) { for (int k = 0; k < 3; ++k) q[k] = c[k]; }
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) { const float w = d2 / (d2 - d6); for (int k = 0; k < 3; ++k) q[k] = a[k] + w * ac[k]; }
    else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; ++k) q[k] = b[k] + w * (c[k] - b[k]);
    } else {
        const float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
        for (int k = 0; k < 3; ++k) q[k] = a[k] + ab[k] * v + ac[k] * w;
    }
    const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static void pillars() {
    ModelSource m;
    ModelSource::Sub s;
    s.texId = 0;
    s.blend = W3D_BLEND_OPAQUE;
    s.flags = W3D_SUBF_BUILDING;
    const int S = 4;   // each face S x S quads
    std::vector<float> orig;   // sample points: centres of the original triangles
    for (int py = 0; py < 10; ++py)
        for (int px = 0; px < 10; ++px) {
            const float ox = px * 30.0f, oy = py * 30.0f, sx = 2, sy = 2, sz = 6;
            // 6 faces: axis, sign
            for (int ax = 0; ax < 3; ++ax)
                for (int sg = 0; sg < 2; ++sg) {
                    const uint32_t base = (uint32_t)m.verts.size();
                    const int u = (ax + 1) % 3, v = (ax + 2) % 3;
                    const float size[3] = {sx, sy, sz};
                    for (int j = 0; j <= S; ++j)
                        for (int i = 0; i <= S; ++i) {
                            ModelSource::V vv{};
                            float p[3];
                            p[ax] = sg ? size[ax] : 0;
                            p[u] = size[u] * i / S;
                            p[v] = size[v] * j / S;
                            vv.p[0] = ox + p[0]; vv.p[1] = oy + p[1]; vv.p[2] = p[2];
                            vv.n[ax] = sg ? 1.0f : -1.0f;
                            vv.uv[0] = p[u] / 2; vv.uv[1] = p[v] / 2;
                            m.verts.push_back(vv);
                        }
                    for (int j = 0; j < S; ++j)
                        for (int i = 0; i < S; ++i) {
                            const uint32_t a = base + j * (S + 1) + i, b = a + 1, c = a + S + 1, d = c + 1;
                            if (sg) s.idx.insert(s.idx.end(), {a, b, d, a, d, c});
                            else s.idx.insert(s.idx.end(), {a, d, b, a, c, d});
                        }
                }
        }
    for (size_t t = 0; t < s.idx.size(); t += 3)
        for (int k = 0; k < 3; ++k)
            orig.push_back((m.verts[s.idx[t]].p[k] + m.verts[s.idx[t + 1]].p[k] + m.verts[s.idx[t + 2]].p[k]) / 3);
    const size_t inTris = s.idx.size() / 3;
    m.subs.push_back(s);
    size_t tris = 0;
    auto blob = buildModelBlob(m, 3000, &tris);
    CHECK(!blob.empty(), "pillars: no blob");
    if (blob.empty()) return;
    W3dModelHeader h;
    std::memcpy(&h, blob.data(), sizeof h);
    std::vector<float> P(h.vertexCount * 3);
    for (uint32_t i = 0; i < h.vertexCount; ++i) {
        W3dModelVertex q;
        std::memcpy(&q, blob.data() + h.offVertices + i * sizeof q, sizeof q);
        P[i * 3] = q.x * h.posScale[0] + h.posOffset[0];
        P[i * 3 + 1] = q.y * h.posScale[1] + h.posOffset[1];
        P[i * 3 + 2] = q.z * h.posScale[2] + h.posOffset[2];
    }
    // the blob is centred on its own box: shift the samples the same way (buildModelBlob does not move the vertices,
    // the quantisation offset is the box centre, so decoded positions are in the input space)
    const uint16_t* idx = (const uint16_t*)(blob.data() + h.offIndices);
    float worst = 0;
    int far = 0;
    for (size_t k = 0; k < orig.size(); k += 3) {
        float best = 1e30f;
        for (uint32_t t = 0; t + 2 < h.indexCount && best > 0.01f; t += 3)
            best = std::min(best, pointTri(&orig[k], &P[idx[t] * 3], &P[idx[t + 1] * 3], &P[idx[t + 2] * 3]));
        worst = std::max(worst, best);
        far += best > 0.5f;
    }
    std::printf("pillars: %zu -> %zu triangles, surface points farther than 0.5 yd: %d of %zu (worst %.2f yd)\n", inTris, tris,
                far, orig.size() / 3, worst);
    CHECK(far == 0, "pillars: %d surface points moved away (worst %.2f yd)", far, worst);
}

int main() {
    const int N = 60;           // quads along x; 30 along y
    const float W = 60, H = 30;
    ModelSource m;
    auto bump = [](float x, float y) { return 0.15f * std::sin(x * 0.9f) * std::cos(y * 1.3f); };
    for (int part = 0; part < 2; ++part) {
        ModelSource::Sub s;
        s.texId = (uint32_t)part;
        s.blend = W3D_BLEND_OPAQUE;
        s.flags = W3D_SUBF_BUILDING;
        const uint32_t base = (uint32_t)m.verts.size();
        const int x0 = part ? N / 2 : 0, x1 = part ? N : N / 2;
        const int cols = x1 - x0 + 1, rows = N / 2 + 1;
        for (int j = 0; j < rows; ++j)
            for (int i = x0; i <= x1; ++i) {
                const float x = W * i / N, y = H * j / (N / 2);
                ModelSource::V v{};
                v.p[0] = x; v.p[1] = y; v.p[2] = bump(x, y);
                v.n[2] = 1;
                v.uv[0] = x / 4; v.uv[1] = y / 4;
                m.verts.push_back(v);
            }
        for (int j = 0; j + 1 < rows; ++j)
            for (int i = 0; i + 1 < cols; ++i) {
                const uint32_t a = base + j * cols + i, b = a + 1, c = a + cols, d = c + 1;
                s.idx.insert(s.idx.end(), {a, b, d, a, d, c});
            }
        m.subs.push_back(s);
    }
    size_t tris = 0;
    auto blob = buildModelBlob(m, 3000, &tris);
    CHECK(!blob.empty(), "no blob");
    if (blob.empty()) return 1;
    W3dModelHeader h;
    std::memcpy(&h, blob.data(), sizeof h);
    std::vector<float> P(h.vertexCount * 2);
    for (uint32_t i = 0; i < h.vertexCount; ++i) {
        W3dModelVertex q;
        std::memcpy(&q, blob.data() + h.offVertices + i * sizeof q, sizeof q);
        P[i * 2] = q.x * h.posScale[0] + h.posOffset[0];
        P[i * 2 + 1] = q.y * h.posScale[1] + h.posOffset[1];
    }
    const uint16_t* idx = (const uint16_t*)(blob.data() + h.offIndices);
    // coverage: sample the wall (inside a small margin) and look for a triangle under each sample
    int samples = 0, holes = 0;
    for (float y = 0.07f; y < H - 0.05f; y += 0.23f)
        for (float x = 0.05f; x < W - 0.05f; x += 0.23f) {
            ++samples;
            bool in = false;
            for (uint32_t t = 0; t + 2 < h.indexCount && !in; t += 3) {
                const float* a = &P[idx[t] * 2]; const float* b = &P[idx[t + 1] * 2]; const float* c = &P[idx[t + 2] * 2];
                const float d1 = (x - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (y - b[1]);
                const float d2 = (x - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (y - c[1]);
                const float d3 = (x - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (y - a[1]);
                const float e = 1e-4f;
                in = (d1 >= -e && d2 >= -e && d3 >= -e) || (d1 <= e && d2 <= e && d3 <= e);
            }
            holes += !in;
        }
    std::printf("wall: 7200 -> %zu triangles, %d of %d sample points uncovered\n", tris, holes, samples);
    CHECK(holes == 0, "%d holes in the wall", holes);
    CHECK(tris < 7200, "not simplified at all (%zu triangles)", tris);
    pillars();
    std::printf(fails ? "WMO HOLES: FAILED\n" : "WMO HOLES: PASS\n");
    return fails ? 1 : 0;
}
