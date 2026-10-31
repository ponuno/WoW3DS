#include "static_batch.hpp"

#include <math.h>

#include <algorithm>
#include <map>
#include <tuple>

namespace sbatch {

int bucketOf(float r) {
    if (!(r > 0) || r >= kMaxRadius) return -1;
    return r < 1.5f ? 0 : r < 4.0f ? 1 : 2;
}

bool eligibleModel(const uint8_t* blob, uint32_t size) {
    if (!blob || size < sizeof(W3dModelHeader)) return false;
    const W3dModelHeader* h = (const W3dModelHeader*)blob;
    if (!h->submeshCount || !h->vertexCount || h->vertexCount > kMaxModelVerts) return false;
    if ((uint64_t)h->offSubmeshes + h->submeshCount * sizeof(W3dSubmesh) > size) return false;
    if ((uint64_t)h->offVertices + h->vertexCount * sizeof(W3dModelVertex) > size) return false;
    if ((uint64_t)h->offIndices + h->indexCount * 2ull > size) return false;
    const W3dSubmesh* s = (const W3dSubmesh*)(blob + h->offSubmeshes);
    for (uint32_t i = 0; i < h->submeshCount; ++i) {
        if (s[i].blend != W3D_BLEND_OPAQUE && s[i].blend != W3D_BLEND_ALPHAKEY) return false;
        if (s[i].flags & W3D_SUBF_BUILDING) return false;
        if ((uint64_t)s[i].firstIndex + s[i].indexCount > h->indexCount) return false;
    }
    return true;
}

void turn(const float r[3], const float v[3], float o[3]) {
    const float cx = cosf(r[0]), sx = sinf(r[0]), cy = cosf(r[1]), sy = sinf(r[1]), cz = cosf(r[2]), sz = sinf(r[2]);
    const float y1 = cx * v[1] - sx * v[2], z1 = sx * v[1] + cx * v[2];   // Rx
    const float x2 = cy * v[0] + sy * z1, z2 = -sy * v[0] + cy * z1;       // Ry
    o[0] = cz * x2 - sz * y1;                                               // Rz
    o[1] = sz * x2 + cz * y1;
    o[2] = z2;
}

namespace {

struct Vtx { float p[3], n[3], uv[2]; };

static int16_t q16(float v) {
    const long r = lroundf(v);
    return (int16_t)(r < -32767 ? -32767 : r > 32767 ? 32767 : r);
}

// One batch being built: float vertices (quantised at the end) and the indices of each level.
struct Acc {
    uintptr_t tex;
    uint8_t blend, flags;
    std::vector<Vtx> v;
    std::vector<uint16_t> lv[kLevels];
};

}  // namespace

std::vector<BatchOut> build(const std::vector<MemberIn>& members, const float origin[3]) {
    typedef std::tuple<uintptr_t, uint8_t, uint8_t> Key;
    std::map<Key, std::vector<size_t>> open;   // key -> accumulators (the last one is filled; a full one starts another)
    std::vector<Acc> acc;
    for (const MemberIn& m : members) {
        const W3dModelHeader* h = (const W3dModelHeader*)m.blob;
        const W3dSubmesh* subs = (const W3dSubmesh*)(m.blob + h->offSubmeshes);
        const W3dModelVertex* mv = (const W3dModelVertex*)(m.blob + h->offVertices);
        const uint16_t* mi = (const uint16_t*)(m.blob + h->offIndices);
        const W3dSubLod* lods = w3d_lods_check(m.blob, 0xFFFFFFFFu, h->offTexIds >= W3D_MODEL_HDR_LOD_END ? h->lodCount : 0,
                                               h->offTexIds >= W3D_MODEL_HDR_LOD_END ? h->offLods : 0, h->submeshCount, h->indexCount);
        const float uvs = w3d_model_uv_scale(h);
        for (uint32_t s = 0; s < h->submeshCount; ++s) {
            const W3dSubmesh& sm = subs[s];
            if (!sm.indexCount) continue;
            // the index ranges of each level for this part (lightest existing up to k, else the full one)
            uint32_t f[kLevels], c[kLevels];
            f[0] = sm.firstIndex; c[0] = sm.indexCount;
            for (int k = 1; k < kLevels; ++k) {
                f[k] = sm.firstIndex; c[k] = sm.indexCount;
                if (lods)
                    for (int j = k; j >= 1; --j)
                        if (lods[s].indexCount[j - 1]) { f[k] = lods[s].firstIndex[j - 1]; c[k] = lods[s].indexCount[j - 1]; break; }
            }
            // the vertices this part uses (any level), remapped
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (int k = 0; k < kLevels; ++k)
                for (uint32_t i = f[k]; i < f[k] + c[k]; ++i) { lo = std::min<uint32_t>(lo, mi[i]); hi = std::max<uint32_t>(hi, mi[i]); }
            if (lo > hi || hi >= h->vertexCount) continue;
            const uint32_t nv = hi - lo + 1;
            const Key key(sm.tex == W3D_NO_TEXTURE ? 0 : m.texKeys[s], sm.blend, (uint8_t)(sm.flags & (W3D_SUBF_TWO_SIDED | W3D_SUBF_UNLIT)));
            auto& list = open[key];
            if (list.empty() || acc[list.back()].v.size() + nv > 65535) {
                acc.push_back(Acc{std::get<0>(key), std::get<1>(key), std::get<2>(key), {}, {}});
                list.push_back(acc.size() - 1);
            }
            Acc& a = acc[list.back()];
            const uint32_t base = (uint32_t)a.v.size();
            for (uint32_t i = lo; i <= hi; ++i) {
                const W3dModelVertex& q = mv[i];
                float p[3] = {(q.x * h->posScale[0] + h->posOffset[0]) * m.scale, (q.y * h->posScale[1] + h->posOffset[1]) * m.scale,
                              (q.z * h->posScale[2] + h->posOffset[2]) * m.scale};
                float n[3] = {q.nx / 127.0f, q.ny / 127.0f, q.nz / 127.0f};
                Vtx o;
                turn(m.rot, p, o.p);
                turn(m.rot, n, o.n);
                for (int k = 0; k < 3; ++k) o.p[k] += m.pos[k] - origin[k];
                o.uv[0] = q.u * uvs;
                o.uv[1] = q.v * uvs;
                a.v.push_back(o);
            }
            for (int k = 0; k < kLevels; ++k)
                for (uint32_t i = f[k]; i < f[k] + c[k]; ++i) a.lv[k].push_back((uint16_t)(base + mi[i] - lo));
        }
    }
    std::vector<BatchOut> out;
    out.reserve(acc.size());
    for (Acc& a : acc) {
        if (a.v.empty() || a.lv[0].empty()) continue;
        BatchOut b;
        b.tex = a.tex; b.blend = a.blend; b.flags = a.flags;
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f}, uvMax = 0;
        for (const Vtx& v : a.v) {
            for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], v.p[k]); mx[k] = std::max(mx[k], v.p[k]); }
            uvMax = std::max({uvMax, fabsf(v.uv[0]), fabsf(v.uv[1])});
        }
        for (int k = 0; k < 3; ++k) {
            b.offset[k] = 0.5f * (mn[k] + mx[k]);
            b.scale[k] = std::max((mx[k] - mn[k]) / 65532.0f, 1e-5f);
        }
        b.uvScale = std::max(uvMax / 32767.0f, 1e-6f);
        b.v.resize(a.v.size());
        for (size_t i = 0; i < a.v.size(); ++i) {
            const Vtx& v = a.v[i];
            W3dModelVertex& o = b.v[i];
            o.x = q16((v.p[0] - b.offset[0]) / b.scale[0]);
            o.y = q16((v.p[1] - b.offset[1]) / b.scale[1]);
            o.z = q16((v.p[2] - b.offset[2]) / b.scale[2]);
            o.w = 0;
            const float nl = sqrtf(v.n[0] * v.n[0] + v.n[1] * v.n[1] + v.n[2] * v.n[2]);
            const float s = nl > 1e-6f ? 127.0f / nl : 0.0f;
            o.nx = (int8_t)lroundf(v.n[0] * s); o.ny = (int8_t)lroundf(v.n[1] * s); o.nz = (int8_t)lroundf(v.n[2] * s); o.pad = 0;
            o.u = q16(v.uv[0] / b.uvScale);
            o.v = q16(v.uv[1] / b.uvScale);
        }
        for (int k = 0; k < kLevels; ++k) {
            b.first[k] = (uint32_t)b.idx.size();
            b.count[k] = (uint32_t)a.lv[k].size();
            b.idx.insert(b.idx.end(), a.lv[k].begin(), a.lv[k].end());
        }
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace sbatch
