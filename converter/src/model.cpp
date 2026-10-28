// M2 -> static low-poly W3D model.
//
// Prototype scope: bind pose only (no skinning), LOD 0 skin, one texture per
// submesh, simplification with meshoptimizer down to a triangle budget.
#include "conv.h"

#include "pipeline/m2_loader.hpp"
#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <tuple>

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
    std::map<std::tuple<uint32_t, uint8_t, uint8_t>, ModelSource::Sub> merged;
    for (auto& s : m.subs) {
        auto& d = merged[{s.texId, s.blend, s.flags}];
        d.texId = s.texId; d.blend = s.blend; d.flags = s.flags;
        d.idx.insert(d.idx.end(), s.idx.begin(), s.idx.end());
    }
    std::vector<ModelSource::Sub> subs;
    for (auto& [k, s] : merged) if (!s.idx.empty()) subs.push_back(std::move(s));
    if (subs.empty()) return {};

    // ---- 2. simplify each submesh proportionally to hit the budget
    size_t total = 0;
    for (auto& s : subs) total += s.idx.size() / 3;
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
                                                      &m.verts[0].uv[0], stride, attrW, 2, nullptr, target, 0.02f, 0, &err);
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
    if (mdl.vertices.empty()) {
        ++st.modelsFailed;
        return 0;
    }
    if (mdl.batches.empty() || mdl.indices.empty()) {
        // Post-vanilla layout keeps LOD 0 in an external .skin.
        std::vector<uint8_t> skin;
        if (data.read(wowee::pipeline::skinPathForM2(m2Path), skin)) wowee::pipeline::M2Loader::loadSkin(skin, mdl);
    }
    if (!mdl.isValid() || mdl.batches.empty()) {
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
    for (const auto& b : mdl.batches) {
        if (b.submeshLevel != 0) continue;
        if (b.indexStart + b.indexCount > mdl.indices.size() || b.indexCount < 3) continue;
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
        src.subs.push_back(std::move(s));
    }
    if (src.subs.empty()) { ++st.modelsFailed; return 0; }

    size_t tris = 0;
    int flipped = 0;
    auto blob = buildModelBlob(src, o.maxModelTris, &tris, &flipped);
    st.m2SubsFlipped += flipped;
    if (blob.empty()) { ++st.modelsFailed; return 0; }
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
