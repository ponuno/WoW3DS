// M2 -> static low-poly W3D model.
//
// Prototype scope: bind pose only (no skinning), LOD 0 skin, one texture per
// submesh, simplification with meshoptimizer down to a triangle budget.
#include "conv.h"

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "meshoptimizer.h"
#include "w3d_fx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>

namespace w3d {

static int16_t q16(float v) { return (int16_t)std::clamp((int)std::lround(v), -32767, 32767); }

// Makes each submesh wound counter-clockwise when seen from the side its vertex
// normals point to (the terrain's convention, culled with GPU_CULL_BACK_CCW on the
// 3DS). Returns true if the submesh was flipped.
bool orientByNormals(const ModelSource& m, std::vector<uint32_t>& idx) {
    double agree = 0, disagree = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const auto &a = m.verts[idx[t]], &b = m.verts[idx[t + 1]], &c = m.verts[idx[t + 2]];
        float e1[3], e2[3], g[3];
        for (int k = 0; k < 3; ++k) { e1[k] = b.p[k] - a.p[k]; e2[k] = c.p[k] - a.p[k]; }
        g[0] = e1[1] * e2[2] - e1[2] * e2[1];
        g[1] = e1[2] * e2[0] - e1[0] * e2[2];
        g[2] = e1[0] * e2[1] - e1[1] * e2[0];
        float d = 0;
        for (int k = 0; k < 3; ++k) d += g[k] * (a.n[k] + b.n[k] + c.n[k]);
        double area = std::sqrt((double)g[0] * g[0] + (double)g[1] * g[1] + (double)g[2] * g[2]);
        if (d > 0) agree += area; else if (d < 0) disagree += area;
    }
    if (disagree <= agree) return false;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
    return true;
}

std::vector<uint8_t> buildModelBlob(ModelSource& m, int maxTris, size_t* trisOut, int* flippedOut) {
    // ---- 0a. sanitise the input. meshoptimizer is compiled without asserts in Release, so an index list that is not a
    // whole number of triangles (a WMO batch with an odd index count, a group with no batch table, ...) or an index past the
    // vertex array made it read and write beyond the buffer: heap corruption / access violation, no message
    // (this killed a --all run inside Ahn'Qiraj). Whole triangles with valid corners only.
    if (m.verts.empty()) return {};
    m.dropped = 0;
    m.outTags.clear();
    {
        const size_t nv = m.verts.size();
        for (auto& s : m.subs) {
            const size_t before = s.idx.size();
            size_t w = 0;
            for (size_t t = 0; t + 2 < before; t += 3) {
                if (s.idx[t] >= nv || s.idx[t + 1] >= nv || s.idx[t + 2] >= nv) continue;
                s.idx[w++] = s.idx[t]; s.idx[w++] = s.idx[t + 1]; s.idx[w++] = s.idx[t + 2];
            }
            s.idx.resize(w);
            m.dropped += before - w;
        }
    }

    // ---- 0. consistent winding (CCW = front, by the vertex normals)
    int flipped = 0;
    for (auto& s : m.subs) flipped += orientByNormals(m, s.idx);
    if (flippedOut) *flippedOut = flipped;

    // ---- 1. merge submeshes sharing material
    std::map<std::tuple<uint32_t, uint8_t, uint8_t, uint8_t>, ModelSource::Sub> merged;
    for (auto& s : m.subs) {
        auto& d = merged[{s.texId, s.blend, s.flags, s.tag}];
        d.texId = s.texId; d.blend = s.blend; d.flags = s.flags; d.tag = s.tag;
        d.idx.insert(d.idx.end(), s.idx.begin(), s.idx.end());
    }
    std::vector<ModelSource::Sub> subs;
    for (auto& [k, s] : merged) if (!s.idx.empty()) subs.push_back(std::move(s));
    if (subs.empty()) return {};

    // ---- 2. simplify each submesh proportionally to hit the budget
    size_t total = 0;
    for (auto& s : subs) total += s.idx.size() / 3;
    if (maxTris > 0 && total > (size_t)maxTris) {
        // Weld before simplifying (stage 5.31): WMO panels each carry their own vertices whose positions differ from the
        // neighbour's by float noise; seen as open borders, they were collapsed one panel at a time and cracks opened
        // between panels (Stormwind's walls: holes that never closed). Positions within 2 mm become identical (seams the
        // simplifier keeps together), and what is still a border is locked (meshopt_SimplifyLockBorder below).
        std::unordered_map<uint64_t, uint32_t> cell;
        auto q = [](float v) { return (uint64_t)(uint32_t)(int32_t)std::lround(v * 500.0f) & 0x1FFFFF; };
        for (uint32_t i = 0; i < m.verts.size(); ++i) {
            auto& v = m.verts[i];
            const uint64_t k = q(v.p[0]) | q(v.p[1]) << 21 | q(v.p[2]) << 42;
            auto [it, fresh] = cell.emplace(k, i);
            if (!fresh) { const auto& w = m.verts[it->second]; v.p[0] = w.p[0]; v.p[1] = w.p[1]; v.p[2] = w.p[2]; }
        }
    }
    const float* pos = &m.verts[0].p[0];
    const size_t stride = sizeof(ModelSource::V);
    if (maxTris > 0 && total > (size_t)maxTris) {
        float ratio = (float)maxTris / (float)total;
        for (auto& s : subs) {
            size_t target = std::max<size_t>(3, (size_t)(s.idx.size() * ratio) / 3 * 3);
            std::vector<uint32_t> out(s.idx.size());
            float err = 0;
            // Error-bounded only (2% of the mesh size), UVs kept: the old "sloppy" fallback (100% error
            // allowed) tore big buildings (Stormwind) into slabs. A mesh that cannot reach the budget
            // within the error keeps more triangles.
            static const float attrW[2] = {0.5f, 0.5f};
            size_t n = meshopt_simplifyWithAttributes(out.data(), s.idx.data(), s.idx.size(), pos, m.verts.size(), stride,
                                                      &m.verts[0].uv[0], stride, attrW, 2, nullptr, target, 0.02f,
                                                      meshopt_SimplifyLockBorder, &err);
            out.resize(n);
            if (n >= 3) s.idx.swap(out);
        }
    }

    // ---- 3. single index buffer, vertex cache + fetch optimisation
    std::vector<uint32_t> all;
    std::vector<W3dSubmesh> outSubs;
    for (auto& s : subs) {
        if (s.idx.empty()) continue;
        meshopt_optimizeVertexCache(s.idx.data(), s.idx.data(), s.idx.size(), m.verts.size());
        W3dSubmesh d{};
        d.tex = s.texId == UINT32_MAX ? (uint16_t)W3D_NO_TEXTURE : (uint16_t)s.texId;
        d.blend = s.blend;
        d.flags = s.flags;
        d.firstIndex = (uint32_t)all.size();
        d.indexCount = (uint32_t)s.idx.size();
        all.insert(all.end(), s.idx.begin(), s.idx.end());
        outSubs.push_back(d);
        m.outTags.push_back(s.tag);
    }
    std::vector<ModelSource::V> verts(m.verts.size());
    size_t vcount = meshopt_optimizeVertexFetch(verts.data(), all.data(), all.size(), m.verts.data(),
                                                m.verts.size(), sizeof(ModelSource::V));
    verts.resize(vcount);
    if (vcount > 65535 || all.empty()) return {};

    // ---- 4. quantise
    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f}, r2 = 0;
    for (auto& v : verts)
        for (int c = 0; c < 3; ++c) { bmin[c] = std::min(bmin[c], v.p[c]); bmax[c] = std::max(bmax[c], v.p[c]); }
    for (auto& v : verts) r2 = std::max(r2, v.p[0] * v.p[0] + v.p[1] * v.p[1] + v.p[2] * v.p[2]);

    W3dModelHeader h{};
    for (int c = 0; c < 3; ++c) {
        h.posOffset[c] = 0.5f * (bmin[c] + bmax[c]);
        float half = std::max(0.5f * (bmax[c] - bmin[c]), 1e-3f);
        h.posScale[c] = half / 32767.0f;
        h.bmin[c] = bmin[c];
        h.bmax[c] = bmax[c];
    }
    // UVs: the default range is +-8; long tiled surfaces (WMO streets) go further, so the scale is per model
    float uvMax = 0;
    for (auto& v : verts) uvMax = std::max({uvMax, std::fabs(v.uv[0]), std::fabs(1.0f - v.uv[1])});
    h.uvScale = W3D_MODEL_UV_SCALE;
    if (uvMax / W3D_MODEL_UV_SCALE > 32000.0f) h.uvScale = uvMax / 32000.0f;
    h.radius = std::sqrt(r2);
    h.vertexCount = (uint32_t)verts.size();
    h.indexCount = (uint32_t)all.size();
    h.submeshCount = (uint32_t)outSubs.size();
    h.texCount = (uint32_t)m.texIds.size();

    Blob b;
    b.put(h);
    b.align(4);
    size_t offTex = b.putArray(m.texIds);
    b.align(4);
    size_t offSub = b.putArray(outSubs);
    b.align(16);
    size_t offV = b.data.size();
    for (auto& v : verts) {
        W3dModelVertex o{};
        o.x = q16((v.p[0] - h.posOffset[0]) / h.posScale[0]);
        o.y = q16((v.p[1] - h.posOffset[1]) / h.posScale[1]);
        o.z = q16((v.p[2] - h.posOffset[2]) / h.posScale[2]);
        float nl = std::sqrt(v.n[0] * v.n[0] + v.n[1] * v.n[1] + v.n[2] * v.n[2]);
        if (nl < 1e-6f) nl = 1;
        o.nx = (int8_t)std::lround(v.n[0] / nl * 127);
        o.ny = (int8_t)std::lround(v.n[1] / nl * 127);
        o.nz = (int8_t)std::lround(v.n[2] / nl * 127);
        o.u = q16(v.uv[0] / h.uvScale);
        o.v = q16((1.0f - v.uv[1]) / h.uvScale);  // PICA t=1 is the top row
        b.put(o);
    }
    b.align(4);
    size_t offI = b.data.size();
    for (uint32_t i : all) b.put((uint16_t)i);
    b.align(4);

    auto* hp = b.at<W3dModelHeader>(0);
    hp->offTexIds = (uint32_t)offTex;
    hp->offSubmeshes = (uint32_t)offSub;
    hp->offVertices = (uint32_t)offV;
    hp->offIndices = (uint32_t)offI;
    if (trisOut) *trisOut = all.size() / 3;
    return b.data;
}

static uint8_t mapBlend(uint16_t m2Blend) {
    switch (m2Blend) {
        case 0: return W3D_BLEND_OPAQUE;
        case 1: return W3D_BLEND_ALPHAKEY;
        case 3: case 6: case 7: return W3D_BLEND_ADD;
        default: return W3D_BLEND_ALPHA;   // 2 alpha, 4/5 modulate approximated
    }
}


// ------------------------------------------------------------------ effects (stage 5.32)
namespace {
std::string lowerStr(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool hasAnyOf(const std::string& s, std::initializer_list<const char*> toks) {
    for (const char* t : toks) if (s.find(t) != std::string::npos) return true;
    return false;
}
// One value for an animated track of a doodad (it stands in its bind pose): the mean of the first sequence's keys.
float trackMean(const wowee::pipeline::M2AnimationTrack& t, float def) {
    for (const auto& sq : t.sequences) {
        if (sq.floatValues.empty()) continue;
        double sum = 0;
        for (float v : sq.floatValues) sum += v;
        const float m = (float)(sum / sq.floatValues.size());
        return std::isfinite(m) ? m : def;
    }
    return def;
}
// 5.35f: emission rate / life: the first sequence that says something positive (a spell's hand glow keys its rate to 0
// in some sequences: the mean of the first one could be 0 and the emitter was dropped)
float trackPositive(const wowee::pipeline::M2AnimationTrack& t, float def) {
    for (const auto& sq : t.sequences) {
        if (sq.floatValues.empty()) continue;
        double sum = 0;
        for (float v : sq.floatValues) sum += v;
        const float m = (float)(sum / sq.floatValues.size());
        if (std::isfinite(m) && m > 0.01f) return m;
    }
    return def;
}
uint32_t rgba8(const glm::vec3& c, float a) {
    auto b = [](float v) { return (uint32_t)std::clamp((int)std::lround(v * 255.0f), 0, 255); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(a) << 24;
}
}  // namespace

// Fire-like doodads (their particles are flames: dense, small, bright, never dimmed by the day light).
static bool fireLike(const std::string& lowerPath) {
    return hasAnyOf(lowerPath, {"torch", "fire", "flame", "candle", "brazier", "lamp", "lantern", "chandelier", "candelabra",
                                "sconce", "forge", "stove", "oven", "fireplace", "campfire", "bonfire", "lightpost"});
}

// Appends the W3dFx section (and its footer) to a finished model blob. glows: from the glow cards taken out of the mesh.
static void appendFx(std::vector<uint8_t>& blob, std::vector<W3dFxEmitter>& em, std::vector<W3dFxGlow>& glows,
                     const std::vector<uint32_t>& texIds, const std::vector<uint8_t>& anim = {}) {
    while (blob.size() % 4) blob.push_back(0);
    W3dFxHeader h{};
    auto put = [&](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; blob.insert(blob.end(), b, b + n); };
    h.texCount = (uint16_t)texIds.size();
    h.offTexIds = (uint32_t)blob.size();
    if (!texIds.empty()) put(texIds.data(), texIds.size() * 4);
    h.emitterCount = (uint16_t)em.size();
    h.offEmitters = (uint32_t)blob.size();
    if (!em.empty()) put(em.data(), em.size() * sizeof(W3dFxEmitter));
    h.glowCount = (uint16_t)glows.size();
    h.offGlows = (uint32_t)blob.size();
    if (!glows.empty()) put(glows.data(), glows.size() * sizeof(W3dFxGlow));
    if (!anim.empty() && blob.size() / 4 < 65535 - anim.size() / 4) {   // stage 5.35 (the offset is in words, 16 bits)
        h.animOff4 = (uint16_t)(blob.size() / 4);
        put(anim.data(), anim.size());
    }
    const uint32_t off = (uint32_t)blob.size();
    put(&h, sizeof h);
    const uint32_t footer[2] = {off, W3D_FX_MAGIC};
    put(footer, sizeof footer);
}


// Stage 5.35: the animation of an effect model, baked per bone (see common/w3d_fx.h, W3dFxAnim). used = the M2 bones
// that something hangs on (index = the tag the parts got); the loop is the first Stand sequence (else the first), or the
// longest global sequence when the tracks run on those. Empty: nothing moves.
static std::vector<uint8_t> bakeFxAnim(const wowee::pipeline::M2Model& m, const std::vector<int>& used,
                                       const std::vector<uint8_t>& subBone, const std::vector<uint8_t>& emBone,
                                       const std::vector<uint8_t>& glBone) {
    if (used.empty() || used.size() > W3D_FXA_MAX_BONES || m.bones.empty() || m.sequences.empty()) return {};
    int seq = 0;
    for (size_t i = 0; i < m.sequences.size(); ++i)
        if (m.sequences[i].id == 0 && (m.sequences[seq].id != 0 || m.sequences[i].variationIndex < m.sequences[seq].variationIndex))
            seq = (int)i;
    uint32_t loop = m.sequences[seq].duration;
    if (loop < 100)
        for (uint32_t g : m.globalSequenceDurations) loop = std::max(loop, g);
    if (loop < 100) return {};
    loop = std::min<uint32_t>(loop, 8000);
    const uint32_t frames = std::clamp<uint32_t>((uint32_t)std::ceil(loop / 1000.0f * 16.0f), 2, W3D_FXA_MAX_FRAMES);
    const size_t nb = m.bones.size();
    std::vector<float> mats((size_t)frames * used.size() * 12);
    bool moves = false;
    for (uint32_t f = 0; f < frames; ++f) {
        const float tms = loop * (float)f / (float)frames;
        std::vector<glm::mat4> world(nb);
        std::vector<char> done(nb, 0), busy(nb, 0);
        std::function<const glm::mat4&(size_t)> get = [&](size_t b) -> const glm::mat4& {
            if (done[b]) return world[b];
            const auto& sb = m.bones[b];
            using namespace wowee::rendering::m2_track;
            const glm::vec3 tr = sampleVec3(sb.translation, seq, tms, tms, m.globalSequenceDurations, glm::vec3(0.0f));
            const glm::quat rq = sampleQuat(sb.rotation, seq, tms, tms, m.globalSequenceDurations);
            const glm::vec3 sc = sampleVec3(sb.scale, seq, tms, tms, m.globalSequenceDurations, glm::vec3(1.0f));
            glm::mat4 local = glm::translate(glm::mat4(1.0f), sb.pivot) * glm::translate(glm::mat4(1.0f), tr) *
                              glm::mat4_cast(rq) * glm::scale(glm::mat4(1.0f), sc) * glm::translate(glm::mat4(1.0f), -sb.pivot);
            busy[b] = 1;
            if (sb.parentBone >= 0 && (size_t)sb.parentBone < nb && !busy[(size_t)sb.parentBone])
                local = get((size_t)sb.parentBone) * local;
            busy[b] = 0;
            done[b] = 1;
            return world[b] = local;
        };
        for (size_t u = 0; u < used.size(); ++u) {
            glm::mat4 M(1.0f);
            if (used[u] >= 0 && (size_t)used[u] < nb) M = get((size_t)used[u]);
            float* o = &mats[((size_t)f * used.size() + u) * 12];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) {
                    o[r * 4 + c] = M[c][r];
                    const float id = r == c ? 1.0f : 0.0f;
                    if (std::fabs(M[c][r] - id) > 1e-3f) moves = true;
                }
        }
    }
    if (!moves) return {};
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); };
    W3dFxAnim a{};
    a.magic = W3D_FXA_MAGIC;
    a.boneCount = (uint16_t)used.size();
    a.frames = (uint16_t)frames;
    a.durationMs = loop;
    a.subCount = (uint16_t)subBone.size();
    a.emitterCount = (uint16_t)emBone.size();
    a.glowCount = (uint16_t)glBone.size();
    put(&a, sizeof a);
    for (const std::vector<uint8_t>* v : {&subBone, &emBone, &glBone}) {
        if (!v->empty()) put(v->data(), v->size());
        while (out.size() % 4) out.push_back(0);
    }
    put(mats.data(), mats.size() * 4);
    return out;
}

static uint32_t addModelFromGameImpl(PackWriter& pack, const DataDir& data, const std::string& m2PathIn, const Options& o,
                                     ConvStats& st);

// Adds MODL (and its textures). Returns model id or 0. A model that throws (out of memory, broken tables) is skipped.
uint32_t addModelFromGame(PackWriter& pack, const DataDir& data, const std::string& m2PathIn,
                          const Options& o, ConvStats& st) {
    try {
        return addModelFromGameImpl(pack, data, m2PathIn, o, st);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  model FAILED, skipped: %s (%s)\n", m2PathIn.c_str(), e.what());
        ++st.modelsFailed;
        return 0;
    }
}

static uint32_t addModelFromGameImpl(PackWriter& pack, const DataDir& data, const std::string& m2PathIn,
                                     const Options& o, ConvStats& st) {
    std::string m2Path = wowee::pipeline::modelPathToM2(m2PathIn);
    uint32_t id = pack.keyId(W3D_ENTRY_MODEL, DataDir::normalize(m2Path));
    if (pack.has(W3D_ENTRY_MODEL, id)) return id;
    trace("model %s", m2Path.c_str());

    progress(false, "tile %d_%d: models %zu, buildings %zu, textures %zu", o.tileX, o.tileY, st.modelsOk, st.wmoOk,
             st.texturesOk);
    std::vector<uint8_t> bytes;
    if (!data.read(m2Path, bytes)) {
        if (o.verbose) std::fprintf(stderr, "  model missing: %s\n", m2Path.c_str());
        ++st.modelsFailed;
        return 0;
    }
    wowee::pipeline::M2Model mdl = wowee::pipeline::M2Loader::load(bytes);
    // 5.35f: most spell effects are particles only - no vertex at all (Fire_Precast_Hand, MoltenBlast_Impact_Chest,
    // LesserHeal_Base ...): they were refused as broken models, so Fireball and Heal showed nothing. With effects on, such a
    // model becomes one degenerate triangle carrying its emitters.
    const bool particlesOnly = !o.noFx && mdl.vertices.empty() && !mdl.particleEmitters.empty();
    if (mdl.vertices.empty() && !particlesOnly) {
        ++st.modelsFailed;
        return 0;
    }
    if (!particlesOnly && (mdl.batches.empty() || mdl.indices.empty())) {
        // Post-vanilla layout keeps LOD 0 in an external .skin.
        std::vector<uint8_t> skin;
        if (data.read(wowee::pipeline::skinPathForM2(m2Path), skin)) wowee::pipeline::M2Loader::loadSkin(skin, mdl);
    }
    if (particlesOnly) {
        mdl.vertices.resize(1);
        mdl.vertices[0] = {};
        mdl.vertices[0].boneWeights[0] = 255;
        mdl.vertices[0].normal = glm::vec3(0, 0, 1);
        mdl.batches.clear();
        mdl.indices.clear();
        ++st.fxParticleOnly;
    } else if (!mdl.isValid() || mdl.batches.empty()) {
        if (o.verbose) std::fprintf(stderr, "  model invalid: %s\n", m2Path.c_str());
        ++st.modelsFailed;
        return 0;
    }

    ModelSource src;
    src.verts.resize(mdl.vertices.size());
    for (size_t i = 0; i < mdl.vertices.size(); ++i) {
        const auto& v = mdl.vertices[i];
        auto& d = src.verts[i];
        d.p[0] = v.position.x; d.p[1] = v.position.y; d.p[2] = v.position.z;
        d.n[0] = v.normal.x;   d.n[1] = v.normal.y;   d.n[2] = v.normal.z;
        d.uv[0] = v.texCoords[0].x; d.uv[1] = v.texCoords[0].y;
    }

    std::map<uint32_t, uint32_t> texSlot;  // TEX entry id -> slot
    const std::string lowerPath = lowerStr(m2Path);
    const bool fire = !o.fxSpell && fireLike(lowerPath);   // "Spells\\Fireball..." is not a torch
    std::vector<W3dFxGlow> glows;
    // stage 5.35: effect models animate their bones; parts, glows and emitters ride the bone they mostly belong to
    bool animOk = o.fxSpell && !o.noFx && !mdl.bones.empty() && !mdl.sequences.empty();
    std::vector<int> animBones;            // M2 bones something hangs on; the tag of a part is its index here
    std::vector<uint8_t> glowBone, emitterBone;
    auto tagFor = [&](int bone) -> uint8_t {
        for (size_t i = 0; i < animBones.size(); ++i) if (animBones[i] == bone) return (uint8_t)i;
        if (animBones.size() >= W3D_FXA_MAX_BONES) { animOk = false; return 0; }
        animBones.push_back(bone);
        return (uint8_t)(animBones.size() - 1);
    };
    auto dominantBone = [&](const wowee::pipeline::M2Batch& b) -> int {
        std::map<int, float> w;
        for (uint32_t k = b.indexStart; k < b.indexStart + b.indexCount && k < mdl.indices.size(); ++k) {
            const uint16_t vi = mdl.indices[k];
            if (vi >= mdl.vertices.size()) continue;
            const auto& v = mdl.vertices[vi];
            for (int j = 0; j < 4; ++j)
                if (v.boneWeights[j]) w[v.boneIndices[j]] += v.boneWeights[j];
        }
        int best = 0;
        float bw = -1;
        for (auto& [bone, ww] : w) if (ww > bw) { bw = ww; best = bone; }
        return best < (int)mdl.bones.size() ? best : 0;
    };
    for (const auto& b : mdl.batches) {
        if (b.submeshLevel != 0) continue;
        if (b.indexStart + b.indexCount > mdl.indices.size() || b.indexCount < 3) continue;
        if (!o.noFx && glows.size() < 16) {   // a glow card: drawn by the client as a camera-facing glow instead
            uint16_t bm = b.materialIndex < mdl.materials.size() ? mdl.materials[b.materialIndex].blendMode : 0;
            std::string tn;
            if (b.textureIndex < mdl.textureLookup.size() && mdl.textureLookup[b.textureIndex] < mdl.textures.size())
                tn = lowerStr(mdl.textures[mdl.textureLookup[b.textureIndex]].filename);
            // additive in either reading of the M2 blend table (3 add, 4 add-with-alpha / modulate, 6 blend-add)
            const bool additive = bm == 3 || bm == 4 || bm == 6 || mapBlend(bm) == W3D_BLEND_ADD;
            if (additive && b.indexCount <= 36 && hasAnyOf(tn, {"glow", "halo", "flare", "lensflare", "lightbeam", "t_vfx"})) {
                glm::vec3 lo(1e30f), hi(-1e30f);
                for (uint32_t k = b.indexStart; k < b.indexStart + b.indexCount; ++k) {
                    const uint16_t vi = mdl.indices[k];
                    if (vi >= mdl.vertices.size()) continue;
                    lo = glm::min(lo, mdl.vertices[vi].position);
                    hi = glm::max(hi, mdl.vertices[vi].position);
                }
                if (lo.x <= hi.x) {
                    const glm::vec3 e = hi - lo;
                    W3dFxGlow g{};
                    const glm::vec3 c = 0.5f * (lo + hi);
                    g.pos[0] = c.x; g.pos[1] = c.y; g.pos[2] = c.z;
                    g.size = std::clamp(0.5f * std::max({e.x, e.y, e.z}), 0.15f, 4.0f);
                    // the batch's colour track (Orgrimmar's bonfire glow is authored white and coloured there, see WoWee
                    // m2_loader.hpp colorRGB), else a tint by the texture's name like WoWee's classifyBatchTexture
                    glm::vec3 col(1.0f, 0.82f, 0.46f);
                    float a = 0.9f;
                    if (hasAnyOf(tn, {"blue", "arcane", "nightelf"})) col = glm::vec3(0.48f, 0.72f, 1.0f);
                    else if (hasAnyOf(tn, {"red", "ruby", "scarlet"})) col = glm::vec3(1.0f, 0.28f, 0.22f);
                    if (b.colorIndex < mdl.colorRGB.size()) {
                        const glm::vec3 cr = mdl.colorRGB[b.colorIndex];
                        if (cr.r + cr.g + cr.b > 0.05f && !(cr.r > 0.99f && cr.g > 0.99f && cr.b > 0.99f)) col = cr;
                        if (b.colorIndex < mdl.colorAlphas.size()) a = std::clamp(mdl.colorAlphas[b.colorIndex], 0.2f, 1.0f);
                    }
                    g.color = rgba8(col, a);
                    glows.push_back(g);
                    glowBone.push_back(animOk ? tagFor(dominantBone(b)) : 0);
                    continue;   // not in the mesh: a flat card seen from the side was a bright line
                }
            }
        }
        ModelSource::Sub s;
        s.texId = UINT32_MAX;
        uint16_t blendMode = 0, mflags = 0;
        if (b.materialIndex < mdl.materials.size()) {
            blendMode = mdl.materials[b.materialIndex].blendMode;
            mflags = mdl.materials[b.materialIndex].flags;
        }
        s.blend = mapBlend(blendMode);
        s.flags = (uint8_t)(((mflags & 0x4) ? W3D_SUBF_TWO_SIDED : 0) | ((mflags & 0x1) ? W3D_SUBF_UNLIT : 0));
        if (b.textureIndex < mdl.textureLookup.size()) {
            uint16_t ti = mdl.textureLookup[b.textureIndex];
            if (ti < mdl.textures.size() && mdl.textures[ti].type == 0 && !mdl.textures[ti].filename.empty()) {
                uint32_t tid = addTextureFromGame(pack, data, mdl.textures[ti].filename, o.modelTex, o.texFormat,
                                                  s.blend != W3D_BLEND_OPAQUE, o.verbose);
                if (tid) {
                    ++st.texturesOk;
                    auto [it, fresh] = texSlot.emplace(tid, (uint32_t)src.texIds.size());
                    if (fresh) src.texIds.push_back(tid);
                    s.texId = it->second;
                } else {
                    ++st.texturesFailed;
                }
            }
            // type != 0 = replaceable (creature skin, hair...) -> untextured in the prototype
        }
        s.idx.assign(mdl.indices.begin() + b.indexStart, mdl.indices.begin() + b.indexStart + b.indexCount);
        if (animOk) s.tag = tagFor(dominantBone(b));
        src.subs.push_back(std::move(s));
    }
    if (src.subs.empty() && (!glows.empty() || particlesOnly)) {   // nothing but glow cards / particles: one empty triangle carries them
        ModelSource::Sub s;
        s.texId = UINT32_MAX;
        s.blend = W3D_BLEND_ADD;
        s.flags = 0;
        s.idx = {0, 0, 0};
        src.subs.push_back(std::move(s));
    }
    if (src.subs.empty()) { ++st.modelsFailed; return 0; }

    size_t tris = 0;
    int flipped = 0;
    auto blob = buildModelBlob(src, o.maxModelTris, &tris, &flipped);
    st.m2SubsFlipped += flipped;
    if (blob.empty()) { ++st.modelsFailed; return 0; }
    // ---- particle emitters (stage 5.32)
    std::vector<W3dFxEmitter> emitters;
    std::vector<uint32_t> fxTex;
    if (!o.noFx) {
        for (const auto& pe : mdl.particleEmitters) {
            if (emitters.size() >= (o.fxSpell ? 12u : 8u)) break;   // a doodad with more is a showpiece: the first eight
            W3dFxEmitter e{};
            e.pos[0] = pe.position.x; e.pos[1] = pe.position.y; e.pos[2] = pe.position.z;
            // (bind pose: the bone matrix is the identity, the position is in model space - as WoWee emitParticles)
            e.rate = trackPositive(pe.emissionRate, 0);
            e.life = trackPositive(pe.lifespan, 0);
            if (!(e.rate > 0.01f) || !(e.life > 0.01f)) { ++st.fxEmittersDropped; continue; }
            e.rate = std::min(e.rate, 60.0f);
            e.life = std::min(e.life, 8.0f);
            e.speed = trackMean(pe.emissionSpeed, 0);
            e.speedVar = std::clamp(trackMean(pe.speedVariation, 0), 0.0f, 1.0f);
            e.spreadV = std::clamp(trackMean(pe.verticalRange, 0), 0.0f, 3.2f);
            e.spreadH = std::clamp(trackMean(pe.horizontalRange, 0), 0.0f, 6.3f);
            e.gravity = std::clamp(trackMean(pe.gravity, 0), -30.0f, 30.0f);
            e.areaL = std::clamp(trackMean(pe.emissionAreaLength, 0), 0.0f, 20.0f);
            e.areaW = std::clamp(trackMean(pe.emissionAreaWidth, 0), 0.0f, 20.0f);
            e.drag = std::clamp(trackMean(pe.deceleration, 0), 0.0f, 10.0f);
            const auto& sc = pe.particleScale.floatValues;
            const auto& co = pe.particleColor.vec3Values;
            const auto& al = pe.particleAlpha.floatValues;
            for (int k = 0; k < 3; ++k) {
                e.scale[k] = std::clamp(k < (int)sc.size() ? sc[k] : 1.0f, 0.0f, 10.0f);
                e.color[k] = rgba8(k < (int)co.size() ? co[k] : glm::vec3(1.0f), k < (int)al.size() ? al[k] : 1.0f);
            }
            e.mid = pe.particleScale.timestamps.size() == 3 ? std::clamp(pe.particleScale.timestamps[1], 0.05f, 0.95f) : 0.5f;
            e.blend = (pe.blendingType == 1 || pe.blendingType == 2 || pe.blendingType == 0) ? W3D_BLEND_ALPHA : W3D_BLEND_ADD;
            e.type = pe.emitterType == 2 ? W3D_FX_SPHERE : W3D_FX_PLANE;
            e.rows = (uint8_t)std::clamp<int>(pe.textureRows, 1, 16);
            e.cols = (uint8_t)std::clamp<int>(pe.textureCols, 1, 16);
            e.flags = (uint16_t)(((pe.flags & 0x40) ? W3D_FXF_RANDOM_TILE : 0) | (fire ? W3D_FXF_FIRE : 0) |
                                 (o.fxSpell ? W3D_FXF_SPELL : 0) |
                                 ((pe.flags & 0x1) && !fire ? W3D_FXF_LIT : 0));
            e.tex = 0xFFFF;
            if (pe.texture < mdl.textures.size() && mdl.textures[pe.texture].type == 0 && !mdl.textures[pe.texture].filename.empty()) {
                // small: a particle is a few pixels to a few dozen on the 400x240 screen
                const uint32_t tid = addTextureFromGame(pack, data, mdl.textures[pe.texture].filename, std::min(o.modelTex, 64),
                                                        o.texFormat, true, o.verbose);
                if (tid) {
                    auto it = std::find(fxTex.begin(), fxTex.end(), tid);
                    e.tex = (uint16_t)(it - fxTex.begin());
                    if (it == fxTex.end()) fxTex.push_back(tid);
                    ++st.texturesOk;
                }
            }
            emitters.push_back(e);
            emitterBone.push_back(animOk ? tagFor(pe.bone < mdl.bones.size() ? pe.bone : 0) : 0);
        }
    }
    std::vector<uint8_t> anim;
    if (animOk && (!emitters.empty() || !glows.empty() || !src.outTags.empty())) {
        anim = bakeFxAnim(mdl, animBones, src.outTags, emitterBone, glowBone);
        if (!anim.empty()) ++st.fxAnimated;
    }
    if (!emitters.empty() || !glows.empty() || !anim.empty()) {
        appendFx(blob, emitters, glows, fxTex, anim);
        ++st.fxModels;
        st.fxEmitters += emitters.size();
        st.fxGlows += glows.size();
        if (o.verbose)
            std::printf("    fx %s: %zu emitters, %zu glows%s%s\n", m2Path.c_str(), emitters.size(), glows.size(), fire ? " (fire)" : "",
                        anim.empty() ? "" : " (animated)");
    }
    pack.add(W3D_ENTRY_MODEL, id, std::move(blob));
    // Bounding (collision) triangles for walls/floors of every placement of this model.
    if (!o.noFloors && !o.noDoodadColl && !mdl.collisionIndices.empty() && !mdl.collisionVertices.empty()) {
        std::vector<uint32_t> ci;
        for (size_t k = 0; k + 2 < mdl.collisionIndices.size(); k += 3) {
            uint16_t i0 = mdl.collisionIndices[k], i1 = mdl.collisionIndices[k + 1], i2 = mdl.collisionIndices[k + 2];
            if (i0 >= mdl.collisionVertices.size() || i1 >= mdl.collisionVertices.size() ||
                i2 >= mdl.collisionVertices.size()) continue;
            ci.insert(ci.end(), {i0, i1, i2});
        }
        // Budget per model (COLL lives in the 3DS heap for every resident tile): simplify with
        // a small error and no sloppy fallback, so walls never get holes.
        size_t srcTris = ci.size() / 3;
        if (o.doodadCollTris > 0 && srcTris > (size_t)o.doodadCollTris) {
            std::vector<uint32_t> out(ci.size());
            float err = 0;
            size_t n = meshopt_simplify(out.data(), ci.data(), ci.size(), &mdl.collisionVertices[0].x,
                                        mdl.collisionVertices.size(), sizeof(mdl.collisionVertices[0]),
                                        (size_t)o.doodadCollTris * 3, 0.02f, 0, &err);
            if (n >= 3) {
                out.resize(n);
                ci.swap(out);
                ++st.m2CollSimplified;
            }
        }
        std::vector<float> c;
        c.reserve(ci.size() * 3);
        for (size_t k = 0; k + 2 < ci.size(); k += 3) {
            for (uint32_t vi : {ci[k], ci[k + 1], ci[k + 2]}) {
                const auto& v = mdl.collisionVertices[vi];
                c.insert(c.end(), {v.x, v.y, v.z});
            }
        }
        if (!c.empty()) {
            st.m2Coll[id] = std::move(c);
            ++st.m2CollModels;
        }
    }
    st.modelTrisTotal += tris;
    ++st.modelsOk;
    if (o.verbose) std::printf("  model %-60s %5zu tris (src %zu)\n", m2Path.c_str(), tris, mdl.indices.size() / 3);
    return id;
}

uint32_t addItemModel(PackWriter& pack, const DataDir& data, const std::string& m2PathIn, const std::string& texPath,
                      const Options& o, ConvStats& st) {
    using wowee::pipeline::M2Model;
    std::string m2Path = wowee::pipeline::modelPathToM2(m2PathIn);
    const std::string key = DataDir::normalize(m2Path) + "|" + DataDir::normalize(texPath);
    uint32_t id = pack.keyId(W3D_ENTRY_MODEL, key);
    if (pack.has(W3D_ENTRY_MODEL, id)) return id;
    std::vector<uint8_t> bytes;
    if (!data.read(m2Path, bytes)) {
        if (o.verbose) std::fprintf(stderr, "  item model missing: %s\n", m2Path.c_str());
        ++st.itemModelsFailed;
        return 0;
    }
    M2Model mdl = wowee::pipeline::M2Loader::load(bytes);
    if (mdl.batches.empty() || mdl.indices.empty()) {
        std::vector<uint8_t> skin;
        if (data.read(wowee::pipeline::skinPathForM2(m2Path), skin)) wowee::pipeline::M2Loader::loadSkin(skin, mdl);
    }
    if (mdl.vertices.empty() || mdl.batches.empty() || mdl.indices.empty()) { ++st.itemModelsFailed; return 0; }
    // Which texture slots the item's own texture replaces: the replaceable ones (object skin 2, blade 3, handle 4);
    // a model without any takes it in slot 0 (as WoWee attachWeapon does).
    bool anyReplaceable = false;
    for (const auto& t : mdl.textures) anyReplaceable |= t.type == 2 || t.type == 3 || t.type == 4;
    auto replaced = [&](size_t ti) {
        return anyReplaceable ? (mdl.textures[ti].type == 2 || mdl.textures[ti].type == 3 || mdl.textures[ti].type == 4) : ti == 0;
    };
    ModelSource src;
    src.verts.resize(mdl.vertices.size());
    for (size_t i = 0; i < mdl.vertices.size(); ++i) {
        const auto& v = mdl.vertices[i];
        auto& d = src.verts[i];
        d.p[0] = v.position.x; d.p[1] = v.position.y; d.p[2] = v.position.z;
        d.n[0] = v.normal.x;   d.n[1] = v.normal.y;   d.n[2] = v.normal.z;
        d.uv[0] = v.texCoords[0].x; d.uv[1] = v.texCoords[0].y;
    }
    std::map<uint32_t, uint32_t> texSlot;
    for (const auto& b : mdl.batches) {
        if (b.submeshLevel != 0) continue;
        if ((size_t)b.indexStart + b.indexCount > mdl.indices.size() || b.indexCount < 3) continue;
        ModelSource::Sub s;
        s.texId = UINT32_MAX;
        uint16_t blendMode = 0, mflags = 0;
        if (b.materialIndex < mdl.materials.size()) {
            blendMode = mdl.materials[b.materialIndex].blendMode;
            mflags = mdl.materials[b.materialIndex].flags;
        }
        s.blend = mapBlend(blendMode);
        s.flags = (uint8_t)(((mflags & 0x4) ? W3D_SUBF_TWO_SIDED : 0) | ((mflags & 0x1) ? W3D_SUBF_UNLIT : 0));
        if (b.textureIndex < mdl.textureLookup.size()) {
            uint16_t ti = mdl.textureLookup[b.textureIndex];
            if (ti < mdl.textures.size()) {
                std::string path;
                if (replaced(ti)) path = texPath;
                if (path.empty() && mdl.textures[ti].type == 0) path = mdl.textures[ti].filename;
                if (!path.empty()) {
                    uint32_t tid = addTextureFromGame(pack, data, path, o.itemTex, o.texFormat, s.blend != W3D_BLEND_OPAQUE, o.verbose);
                    if (tid) {
                        ++st.texturesOk;
                        auto [it, fresh] = texSlot.emplace(tid, (uint32_t)src.texIds.size());
                        if (fresh) src.texIds.push_back(tid);
                        s.texId = it->second;
                    } else {
                        ++st.texturesFailed;
                    }
                }
            }
        }
        s.idx.assign(mdl.indices.begin() + b.indexStart, mdl.indices.begin() + b.indexStart + b.indexCount);
        for (uint32_t& i : s.idx) if (i >= mdl.vertices.size()) i = 0;
        src.subs.push_back(std::move(s));
    }
    if (src.subs.empty()) { ++st.itemModelsFailed; return 0; }
    size_t tris = 0;
    int flipped = 0;
    auto blob = buildModelBlob(src, o.itemModelTris, &tris, &flipped);
    if (blob.empty()) { ++st.itemModelsFailed; return 0; }
    pack.add(W3D_ENTRY_MODEL, id, std::move(blob));
    ++st.itemModels;
    st.itemModelTris += tris;
    if (o.verbose) std::printf("  item model %-56s %4zu tris, texture %s\n", m2Path.c_str(), tris, texPath.c_str());
    return id;
}

}  // namespace w3d
