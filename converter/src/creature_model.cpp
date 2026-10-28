// M2 -> skinned, animated W3D creature model (CMDL). See creature_model.h / w3d_format.h.
#include "creature_model.h"

#include "conv.h"
#include "meshoptimizer.h"
#include "rendering/m2_track_sampler.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <map>
#include <tuple>

namespace w3d {

// AnimationData.dbc ids in the order they are kept (see creature_model.h).
static const uint16_t kEssential[] = {0, 1, 4, 5, 16, 17, 18, 19, 41, 42};
const std::vector<uint16_t>& creatureAnimIds() {
    static const std::vector<uint16_t> ids = {
        // stand, move, fight, die, swim: every unit needs these
        0, 1, 4, 5, 16, 17, 18, 19, 41, 42,
        // combat around them: dead pose, wounds, ready stances, backwards, strafing, stun, dodge / parry / block
        6, 9, 10, 25, 26, 27, 28, 13, 11, 12, 14, 30, 20, 21, 22, 23, 24, 36,
        // spells, jumping, swimming directions, off-hand, riding, looting
        31, 32, 51, 52, 53, 54, 124, 125, 37, 38, 39, 40, 43, 44, 45, 87, 91, 135, 50,
        // NPC life: sitting, sleeping, chairs, kneeling, talking and working
        96, 97, 98, 99, 100, 101, 102, 103, 104, 114, 115, 116, 60, 64, 65, 66, 67, 68, 34, 35, 62, 63, 61, 122, 123, 136, 138,
        // the rest of the emotes and special attacks
        69, 70, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 113, 55, 57, 58, 59, 95, 29, 46, 47, 48, 49, 107, 108,
        119, 120, 126, 130, 127, 7, 8, 117, 118, 85, 133, 134};
    return ids;
}

bool creatureAnimEssential(uint16_t id) {
    for (uint16_t e : kEssential)
        if (e == id) return true;
    return false;
}

namespace {

int16_t q16(float v) { return (int16_t)std::clamp((int)std::lround(v), -32767, 32767); }

uint8_t mapBlendM2(uint16_t b) {
    switch (b) {
        case 0: return W3D_BLEND_OPAQUE;
        case 1: return W3D_BLEND_ALPHAKEY;
        case 3: case 6: case 7: return W3D_BLEND_ADD;
        default: return W3D_BLEND_ALPHA;
    }
}

uint8_t texKindFor(uint32_t m2Type) {
    switch (m2Type) {
        case 0: return W3D_CTEX_FIXED;
        case 1: return W3D_CTEX_BODY;     // character skin (NPCs: baked texture)
        case 6: return W3D_CTEX_HAIR;
        case 8: return W3D_CTEX_BODY;     // skin extra: WoWee falls back to the body/underwear art
        case 2: return W3D_CTEX_CAPE;     // cloak (worn items, stage 5.19)
        case 11: return W3D_CTEX_SKIN1;   // monster skins from CreatureDisplayInfo
        case 12: return W3D_CTEX_SKIN2;
        case 13: return W3D_CTEX_SKIN3;
        default: return W3D_CTEX_NONE;    // fur, armour pieces...: untextured for now
    }
}

struct Influence { uint16_t bone[2]; uint8_t w[2]; };

// Two strongest influences, renormalised to 255.
Influence reduceWeights(const wowee::pipeline::M2Vertex& v, size_t boneCount) {
    int order[4] = {0, 1, 2, 3};
    std::sort(order, order + 4, [&](int a, int b) { return v.boneWeights[a] > v.boneWeights[b]; });
    Influence r{};
    int w0 = v.boneWeights[order[0]], w1 = v.boneWeights[order[1]];
    uint16_t b0 = v.boneIndices[order[0]], b1 = v.boneIndices[order[1]];
    if (b0 >= boneCount) { b0 = 0; }
    if (b1 >= boneCount || w1 == 0) { b1 = b0; w1 = 0; }
    if (w0 + w1 == 0) { w0 = 255; }
    int s = w0 + w1;
    r.bone[0] = b0; r.bone[1] = b1;
    r.w[0] = (uint8_t)std::lround(255.0 * w0 / s);
    r.w[1] = (uint8_t)(255 - r.w[0]);
    if (r.w[1] == 0) r.bone[1] = b0;
    return r;
}

}  // namespace

std::vector<uint8_t> buildCreatureBlob(const wowee::pipeline::M2Model& m, int maxTris,
                                       const std::function<uint32_t(const std::string&, bool)>& fixedTex,
                                       CreatureBuildInfo& info, size_t animBudgetBytes) {
    using wowee::pipeline::M2Model;
    if (m.vertices.empty() || m.indices.empty() || m.batches.empty()) { info.error = "no geometry"; return {}; }
    const size_t boneCount = std::max<size_t>(1, m.bones.size());

    // ---- 1. submeshes from render batches (LOD 0)
    struct Sub { uint16_t texSlot, geoset; uint8_t blend, flags; std::vector<uint32_t> idx; };
    std::vector<Sub> subs;
    std::vector<W3dCreatureTex> texTable;
    std::map<std::pair<uint8_t, uint32_t>, uint16_t> texIndex;
    for (const auto& b : m.batches) {
        if (b.submeshLevel != 0) continue;
        if ((size_t)b.indexStart + b.indexCount > m.indices.size() || b.indexCount < 3) continue;
        Sub s{};
        uint16_t blend = 0, mflags = 0;
        if (b.materialIndex < m.materials.size()) {
            blend = m.materials[b.materialIndex].blendMode;
            mflags = m.materials[b.materialIndex].flags;
        }
        s.blend = mapBlendM2(blend);
        s.flags = (uint8_t)(((mflags & 0x4) ? W3D_SUBF_TWO_SIDED : 0) | ((mflags & 0x1) ? W3D_SUBF_UNLIT : 0));
        s.geoset = b.submeshId;
        s.texSlot = W3D_NO_TEXTURE;
        if (b.textureIndex < m.textureLookup.size()) {
            uint16_t ti = m.textureLookup[b.textureIndex];
            if (ti < m.textures.size()) {
                uint8_t kind = texKindFor(m.textures[ti].type);
                uint32_t id = 0;
                bool alpha = s.blend != W3D_BLEND_OPAQUE;
                if (kind == W3D_CTEX_FIXED) {
                    id = m.textures[ti].filename.empty() ? 0 : fixedTex(m.textures[ti].filename, alpha);
                    if (!id) kind = W3D_CTEX_NONE;
                }
                if (kind != W3D_CTEX_NONE) {
                    if (kind < W3D_CTEX_KINDS) info.needsAlpha[kind] |= alpha;
                    auto key = std::make_pair(kind, id);
                    auto it = texIndex.find(key);
                    if (it == texIndex.end()) {
                        W3dCreatureTex t{};
                        t.kind = kind; t.blendAlpha = alpha; t.texId = id;
                        it = texIndex.emplace(key, (uint16_t)texTable.size()).first;
                        texTable.push_back(t);
                    } else if (alpha) {
                        texTable[it->second].blendAlpha = 1;
                    }
                    s.texSlot = it->second;
                }
            }
        }
        s.idx.assign(m.indices.begin() + b.indexStart, m.indices.begin() + b.indexStart + b.indexCount);
        for (uint32_t& i : s.idx) if (i >= m.vertices.size()) i = 0;
        s.idx.resize(s.idx.size() / 3 * 3);   // whole triangles only (meshoptimizer reads past the end otherwise)
        if (s.idx.empty()) continue;
        subs.push_back(std::move(s));
        info.geosets.insert(b.submeshId);
    }
    if (subs.empty()) { info.error = "no LOD-0 batches"; return {}; }

    // ---- 2. winding (CCW from the normals' side) and simplification to the budget
    ModelSource src;
    src.verts.resize(m.vertices.size());
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        const auto& v = m.vertices[i];
        auto& d = src.verts[i];
        d.p[0] = v.position.x; d.p[1] = v.position.y; d.p[2] = v.position.z;
        d.n[0] = v.normal.x; d.n[1] = v.normal.y; d.n[2] = v.normal.z;
        d.uv[0] = v.texCoords[0].x; d.uv[1] = v.texCoords[0].y;
    }
    for (auto& s : subs) info.flippedSubs += orientByNormals(src, s.idx);
    size_t total = 0;
    for (auto& s : subs) total += s.idx.size() / 3;
    if (maxTris > 0 && total > (size_t)maxTris) {
        // Skinned meshes must not collapse an edge between parts that move differently: a vertex of
        // the forearm merged into the upper arm stretches a triangle across the elbow ("planks" in
        // every pose but the bind pose). Vertices on triangles whose corners have different dominant
        // bones are locked; only regions skinned to the same bone simplify. No sloppy fallback.
        auto topBone = [&](uint32_t vi) {
            const auto& v = m.vertices[vi];
            int best = 0;
            for (int k = 1; k < 4; ++k) if (v.boneWeights[k] > v.boneWeights[best]) best = k;
            return (int)v.boneIndices[best];
        };
        std::vector<unsigned char> lock(m.vertices.size(), 0);
        static const bool noLock = std::getenv("W3D_NO_SKIN_LOCK") != nullptr;   // test switch: the old behaviour
        for (auto& s : subs)
            if (!noLock)
            for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
                int a = topBone(s.idx[t]), b = topBone(s.idx[t + 1]), c = topBone(s.idx[t + 2]);
                if (a != b || b != c) lock[s.idx[t]] = lock[s.idx[t + 1]] = lock[s.idx[t + 2]] = 1;
            }
        // UVs as attributes so texture seams survive too.
        std::vector<float> attr(m.vertices.size() * 2);
        for (size_t i = 0; i < m.vertices.size(); ++i) { attr[i * 2] = src.verts[i].uv[0]; attr[i * 2 + 1] = src.verts[i].uv[1]; }
        const float attrW[2] = {0.5f, 0.5f};
        float ratio = (float)maxTris / (float)total;
        for (auto& s : subs) {
            size_t target = std::max<size_t>(3, (size_t)(s.idx.size() * ratio) / 3 * 3);
            std::vector<uint32_t> out(s.idx.size());
            float err = 0;
            size_t n = meshopt_simplifyWithAttributes(out.data(), s.idx.data(), s.idx.size(), &src.verts[0].p[0],
                                                      src.verts.size(), sizeof(ModelSource::V), attr.data(),
                                                      2 * sizeof(float), attrW, 2, lock.data(), target, 0.03f, 0, &err);
            out.resize(n);
            if (n >= 3) s.idx.swap(out);
        }
    }

    // ---- 3. bone palettes: split each submesh so no piece needs more than W3D_PALETTE_BONES bones
    std::vector<Influence> infl(m.vertices.size());
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        infl[i] = reduceWeights(m.vertices[i], boneCount);
        const auto& v = m.vertices[i];
        int w[4] = {v.boneWeights[0], v.boneWeights[1], v.boneWeights[2], v.boneWeights[3]};
        std::sort(w, w + 4);
        int sum = w[0] + w[1] + w[2] + w[3];
        if (sum > 0 && (w[0] + w[1]) * 10 > sum) ++info.lossyVerts;
        ++info.srcVerts;
    }
    std::vector<W3dBonePalette> palettes;
    std::vector<W3dSkinSubmesh> outSubs;
    std::vector<W3dSkinVertex> outVerts;
    std::vector<uint16_t> outIdx;
    // bind-pose bounds for quantisation
    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f}, r2 = 0;
    for (auto& v : m.vertices) {
        const float p[3] = {v.position.x, v.position.y, v.position.z};
        for (int c = 0; c < 3; ++c) { bmin[c] = std::min(bmin[c], p[c]); bmax[c] = std::max(bmax[c], p[c]); }
        r2 = std::max(r2, p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    }
    W3dCreatureHeader h{};
    for (int c = 0; c < 3; ++c) {
        h.posOffset[c] = 0.5f * (bmin[c] + bmax[c]);
        h.posScale[c] = std::max(0.5f * (bmax[c] - bmin[c]), 1e-3f) / 32767.0f;
        h.bmin[c] = bmin[c]; h.bmax[c] = bmax[c];
    }
    float radius = std::sqrt(r2);
    for (const auto& s : subs) {
        size_t t = 0;
        while (t + 2 < s.idx.size()) {
            W3dBonePalette pal{};
            std::map<uint16_t, uint8_t> local;   // global bone -> local index
            std::map<uint32_t, uint16_t> vmap;   // source vertex -> output vertex (this piece)
            W3dSkinSubmesh sm{};
            sm.texSlot = s.texSlot; sm.blend = s.blend; sm.flags = s.flags; sm.geoset = s.geoset;
            sm.palette = (uint16_t)palettes.size();
            sm.firstIndex = (uint32_t)outIdx.size();
            for (; t + 2 < s.idx.size(); t += 3) {
                std::set<uint16_t> need;
                for (int k = 0; k < 3; ++k) {
                    const Influence& f = infl[s.idx[t + k]];
                    need.insert(f.bone[0]);
                    if (f.w[1]) need.insert(f.bone[1]);
                }
                size_t fresh = 0;
                for (uint16_t b : need) fresh += local.count(b) == 0;
                if (local.size() + fresh > W3D_PALETTE_BONES) break;            // next piece
                if (outVerts.size() + 3 > 65535) { info.error = "too many vertices"; return {}; }
                for (uint16_t b : need)
                    if (!local.count(b)) { local[b] = (uint8_t)pal.count; pal.bones[pal.count++] = b; }
                for (int k = 0; k < 3; ++k) {
                    uint32_t si = s.idx[t + k];
                    auto it = vmap.find(si);
                    if (it == vmap.end()) {
                        const auto& v = m.vertices[si];
                        const Influence& f = infl[si];
                        W3dSkinVertex o{};
                        o.x = q16((v.position.x - h.posOffset[0]) / h.posScale[0]);
                        o.y = q16((v.position.y - h.posOffset[1]) / h.posScale[1]);
                        o.z = q16((v.position.z - h.posOffset[2]) / h.posScale[2]);
                        float nl = std::sqrt(v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z);
                        if (nl < 1e-6f) nl = 1;
                        o.nx = (int8_t)std::lround(v.normal.x / nl * 127);
                        o.ny = (int8_t)std::lround(v.normal.y / nl * 127);
                        o.nz = (int8_t)std::lround(v.normal.z / nl * 127);
                        o.u = q16(v.texCoords[0].x / W3D_MODEL_UV_SCALE);
                        o.v = q16((1.0f - v.texCoords[0].y) / W3D_MODEL_UV_SCALE);
                        o.bone[0] = (uint8_t)(local[f.bone[0]] * 3);
                        o.bone[1] = (uint8_t)(local[f.w[1] ? f.bone[1] : f.bone[0]] * 3);
                        o.weight[0] = f.w[0];
                        o.weight[1] = f.w[1];
                        it = vmap.emplace(si, (uint16_t)outVerts.size()).first;
                        outVerts.push_back(o);
                    }
                    outIdx.push_back(it->second);
                }
            }
            sm.indexCount = (uint32_t)outIdx.size() - sm.firstIndex;
            if (sm.indexCount == 0) { info.error = "triangle needs more bones than a palette holds"; return {}; }
            palettes.push_back(pal);
            outSubs.push_back(sm);
        }
    }

    // ---- 4. skeleton and baked animation
    std::vector<W3dBone> bones(boneCount);
    for (size_t b = 0; b < boneCount; ++b) {
        W3dBone& o = bones[b];
        o.parent = -1;
        if (b < m.bones.size()) {
            const auto& sb = m.bones[b];
            o.parent = (sb.parentBone >= 0 && (size_t)sb.parentBone < b) ? sb.parentBone : -1;
            o.flags = (uint16_t)sb.flags;
            o.pivot[0] = sb.pivot.x; o.pivot[1] = sb.pivot.y; o.pivot[2] = sb.pivot.z;
        }
    }
    // Sequences: the animations the 3DS plays (creatureAnimIds, in priority order), first variation of each id.
    // Long ones (> 1.5 s: emotes, sitting, sleeping) are sampled at half rate; when the key budget runs out the
    // remaining ones are dropped - the order puts what every unit needs first (stand, move, fight, die).
    std::vector<int> chosen;
    std::vector<bool> essential;
    for (uint16_t id : creatureAnimIds()) {
        int best = -1;
        for (size_t s = 0; s < m.sequences.size(); ++s)
            if (m.sequences[s].id == id && (best < 0 || m.sequences[s].variationIndex < m.sequences[best].variationIndex))
                best = (int)s;
        if (best >= 0) {
            chosen.push_back(best);
            essential.push_back(creatureAnimEssential(id));
        }
    }
    struct Sampled { std::vector<glm::quat> r; std::vector<glm::vec3> t, s; };
    std::vector<std::vector<Sampled>> samples(chosen.size(), std::vector<Sampled>(boneCount));
    std::vector<uint16_t> frames(chosen.size());
    float maxT = 0;
    for (size_t c = 0; c < chosen.size(); ++c) {
        const auto& sq = m.sequences[chosen[c]];
        const float fps = sq.duration > 1500 ? W3D_ANIM_FPS * 0.5f : W3D_ANIM_FPS;
        uint32_t n = sq.duration ? (uint32_t)std::ceil(sq.duration / 1000.0f * fps) : 0;
        n = std::min<uint32_t>(n, 600);
        frames[c] = (uint16_t)(n + 1);
        for (size_t b = 0; b < m.bones.size(); ++b) {
            const auto& sb = m.bones[b];
            Sampled& out = samples[c][b];
            for (uint32_t k = 0; k <= n; ++k) {
                float tms = n ? sq.duration * (float)k / (float)n : 0.0f;
                using namespace wowee::rendering::m2_track;
                glm::vec3 tr = sampleVec3(sb.translation, chosen[c], tms, tms, m.globalSequenceDurations, glm::vec3(0.0f));
                glm::quat rq = sampleQuat(sb.rotation, chosen[c], tms, tms, m.globalSequenceDurations);
                glm::vec3 sc = sampleVec3(sb.scale, chosen[c], tms, tms, m.globalSequenceDurations, glm::vec3(1.0f));
                out.t.push_back(tr); out.r.push_back(rq); out.s.push_back(sc);
                maxT = std::max({maxT, std::fabs(tr.x), std::fabs(tr.y), std::fabs(tr.z)});
            }
        }
    }
    h.transScale = std::max(maxT / 32000.0f, 1e-5f);
    std::vector<int16_t> keys;
    std::vector<W3dAnimSeq> seqs;
    std::vector<uint32_t> channels;
    for (size_t c = 0; c < chosen.size(); ++c) {
        const auto& sq = m.sequences[chosen[c]];
        const size_t keysBefore = keys.size(), chanBefore = channels.size();
        W3dAnimSeq o{};
        o.animId = (uint16_t)sq.id;
        o.frames = frames[c];
        o.durationMs = sq.duration;
        o.moveSpeed = sq.movingSpeed;
        o.offChannels = (uint32_t)channels.size();   // patched to a byte offset below
        for (size_t b = 0; b < boneCount; ++b) {
            if (b >= m.bones.size()) { channels.insert(channels.end(), {W3D_CHAN_NONE, W3D_CHAN_NONE, W3D_CHAN_NONE}); continue; }
            Sampled& sm = samples[c][b];
            // rotation: consistent hemisphere so neighbouring keys interpolate the short way
            for (size_t k = 1; k < sm.r.size(); ++k)
                if (glm::dot(sm.r[k], sm.r[k - 1]) < 0) sm.r[k] = -sm.r[k];
            auto emit = [&](int comps, auto get, auto isDefault) -> uint32_t {
                bool constant = true, def = true;
                for (size_t k = 0; k < sm.t.size(); ++k) {
                    for (int j = 0; j < comps; ++j)
                        if (get(k, j) != get(0, j)) constant = false;
                    if (!isDefault(k)) def = false;
                }
                if (constant && def) return W3D_CHAN_NONE;
                uint32_t off = (uint32_t)keys.size();
                size_t count = constant ? 1 : sm.t.size();
                for (size_t k = 0; k < count; ++k)
                    for (int j = 0; j < comps; ++j) keys.push_back(get(k, j));
                return constant ? off : (off | W3D_CHAN_ANIM);
            };
            uint32_t cr = emit(4, [&](size_t k, int j) { const glm::quat& q = sm.r[k];
                                   float v = j == 0 ? q.x : j == 1 ? q.y : j == 2 ? q.z : q.w; return q16(v * 32767.0f); },
                               // identity only if the stored key would be identity (x, y, z quantise to 0)
                               [&](size_t k) { return q16(sm.r[k].x * 32767.0f) == 0 && q16(sm.r[k].y * 32767.0f) == 0 &&
                                                      q16(sm.r[k].z * 32767.0f) == 0; });
            uint32_t ct = emit(3, [&](size_t k, int j) { return q16(sm.t[k][j] / h.transScale); },
                               [&](size_t k) { return q16(sm.t[k].x / h.transScale) == 0 && q16(sm.t[k].y / h.transScale) == 0 &&
                                                      q16(sm.t[k].z / h.transScale) == 0; });
            uint32_t cs = emit(3, [&](size_t k, int j) { return q16(sm.s[k][j] / W3D_SCALE_KEY); },
                               [&](size_t k) { return q16(sm.s[k].x / W3D_SCALE_KEY) == 4096 && q16(sm.s[k].y / W3D_SCALE_KEY) == 4096 &&
                                                      q16(sm.s[k].z / W3D_SCALE_KEY) == 4096; });
            channels.insert(channels.end(), {cr, ct, cs});
        }
        if (animBudgetBytes && !essential[c] && keys.size() * 2 > animBudgetBytes) {   // over budget: drop it
            keys.resize(keysBefore);
            channels.resize(chanBefore);
            ++info.droppedSeqs;
            continue;
        }
        // the animation's own bounds can exceed the bind pose (wings, tails)
        radius = std::max(radius, sq.boundRadius);
        seqs.push_back(o);
    }
    // the seat of a mount: the first of the attachments 0, 5, 6, 7, 8 the model has (as WoWee looks for it)
    h.seatBone = -1;
    for (uint32_t want : {0u, 5u, 6u, 7u, 8u}) {
        if (h.seatBone >= 0) break;
        for (const auto& a : m.attachments)
            if (a.id == want && a.bone < boneCount) {
                h.seatBone = (int16_t)a.bone;
                h.seat[0] = a.position.x; h.seat[1] = a.position.y; h.seat[2] = a.position.z;
                break;
            }
    }

    // attachment points worn items hang on (hands, shoulders, helm, back, sheaths); first of each id
    std::vector<W3dAttach> attach;
    static const uint16_t kAttachIds[] = {0, 1, 2, 5, 6, 11, 12, 26, 27, 28, 30, 31, 32, 33};
    for (uint16_t want : kAttachIds)
        for (const auto& a : m.attachments)
            if (a.id == want && a.bone < boneCount) {
                W3dAttach o{};
                o.id = want;
                o.bone = (int16_t)a.bone;
                o.pos[0] = a.position.x; o.pos[1] = a.position.y; o.pos[2] = a.position.z;
                attach.push_back(o);
                break;
            }

    // ---- 5. blob
    h.radius = radius;
    h.vertexCount = (uint32_t)outVerts.size();
    h.indexCount = (uint32_t)outIdx.size();
    h.submeshCount = (uint32_t)outSubs.size();
    h.texCount = (uint32_t)texTable.size();
    h.boneCount = (uint32_t)boneCount;
    h.seqCount = (uint32_t)seqs.size();
    h.paletteCount = (uint32_t)palettes.size();
    h.keyCount = (uint32_t)keys.size();
    Blob out;
    out.put(h);
    out.align(4);
    size_t offTex = out.putArray(texTable);
    out.align(4);
    size_t offSub = out.putArray(outSubs);
    out.align(16);
    size_t offV = out.putArray(outVerts);
    out.align(4);
    size_t offI = out.putArray(outIdx);
    out.align(4);
    size_t offP = out.putArray(palettes);
    out.align(4);
    size_t offB = out.putArray(bones);
    out.align(4);
    size_t offChan = out.putArray(channels);
    out.align(4);
    for (auto& s : seqs) s.offChannels = (uint32_t)(offChan + s.offChannels * sizeof(uint32_t));
    size_t offS = out.putArray(seqs);
    out.align(4);
    size_t offK = out.putArray(keys);
    out.align(4);
    size_t offA = out.putArray(attach);
    out.align(4);
    auto* hp = out.at<W3dCreatureHeader>(0);
    hp->offTex = (uint32_t)offTex; hp->offSubmeshes = (uint32_t)offSub; hp->offVertices = (uint32_t)offV;
    hp->offIndices = (uint32_t)offI; hp->offPalettes = (uint32_t)offP; hp->offBones = (uint32_t)offB;
    hp->offSeqs = (uint32_t)offS; hp->offKeys = (uint32_t)offK;
    hp->attachCount = (uint32_t)attach.size(); hp->offAttach = (uint32_t)offA;

    info.tris = outIdx.size() / 3;
    info.verts = outVerts.size();
    info.palettes = palettes.size();
    info.bones = boneCount;
    info.seqs = seqs.size();
    info.keyBytes = keys.size() * 2;
    return out.data;
}

}  // namespace w3d
