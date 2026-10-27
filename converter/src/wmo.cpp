// WMO (buildings) -> W3D models + placements.
//
// Stage 5.0 scope: every group becomes its own static model (MODL), with
// vertices relative to the group's centre so the 3DS gets a tight bounding
// sphere per group (culling and draw distance work per room/section). No
// portals yet: all groups of a building are drawn when visible. The WMO's
// doodads (doodad set 0 + the placement's set) become ordinary doodad
// placements. Placement maths follows WoWee terrain_manager.cpp:
//   wmoMatrix = T(pos) * Rz * Ry * Rx (placementEuler), doodad = wmoMatrix * T * R(quat) * S.
#include "conv.h"

#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wmo_loader.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace w3d {

namespace {

struct WmoAsset {
    bool ok = false;
    struct Group { uint32_t modelId; glm::vec3 center; };
    std::vector<Group> groups;
    std::vector<glm::vec3> coll;               // collidable triangles (WMO space), 3 vertices each
    struct Doodad { std::string path; glm::mat4 local; };
    std::vector<std::vector<Doodad>> sets;     // per doodad set
};

std::map<std::string, WmoAsset> g_wmoCache;    // normalized path -> converted asset

uint8_t mapWmoBlend(uint32_t b) {
    switch (b) {
        case 0: return W3D_BLEND_OPAQUE;
        case 1: return W3D_BLEND_ALPHAKEY;
        case 2: return W3D_BLEND_ALPHA;
        case 3: return W3D_BLEND_ADD;
        default: return W3D_BLEND_ALPHA;
    }
}

}  // namespace

// Rotation part of m (uniform scale removed) as W3D Euler angles:
// R = Rz(out[2]) * Ry(out[1]) * Rx(out[0]). Returns the scale.
float matrixToPlacement(const float m4[16], float outRot[3]) {
    // column-major like glm: m4[col*4 + row]
    auto M = [&](int r, int c) { return m4[c * 4 + r]; };
    float s = std::sqrt(M(0, 0) * M(0, 0) + M(1, 0) * M(1, 0) + M(2, 0) * M(2, 0));
    if (s < 1e-8f) s = 1;
    float r00 = M(0, 0) / s, r10 = M(1, 0) / s, r20 = M(2, 0) / s;
    float r01 = M(0, 1) / s, r11 = M(1, 1) / s;
    float r21 = M(2, 1) / s, r22 = M(2, 2) / s;
    // b from atan2 (stable near +-90 deg); a and c from the remaining entries unless cos(b) ~ 0.
    float cb = std::sqrt(r00 * r00 + r10 * r10);
    float b = std::atan2(-r20, cb);
    float a, c;
    if (cb > 1e-6f) {
        a = std::atan2(r21, r22);
        c = std::atan2(r10, r00);
    } else {   // gimbal lock: only a -+ c is defined; put it all into c
        a = 0;
        c = std::atan2(-r01, r11);
    }
    outRot[0] = a;
    outRot[1] = b;
    outRot[2] = c;
    return s;
}

void resetWmoCache() { g_wmoCache.clear(); }

static const WmoAsset& loadWmo(PackWriter& pack, const DataDir& data, const std::string& path, const Options& o,
                               ConvStats& st) {
    std::string key = DataDir::normalize(path);
    auto it = g_wmoCache.find(key);
    if (it != g_wmoCache.end()) return it->second;
    WmoAsset& a = g_wmoCache[key];
    {
        size_t k = path.find_last_of("\\/");
        progress(true, "tile %d_%d: building %s (%zu so far)", o.tileX, o.tileY,
                 path.substr(k == std::string::npos ? 0 : k + 1).c_str(), st.wmoOk);
    }

    std::vector<uint8_t> bytes;
    if (!data.read(path, bytes)) {
        if (o.verbose) std::fprintf(stderr, "  WMO missing: %s\n", path.c_str());
        ++st.wmoFailed;
        return a;
    }
    wowee::pipeline::WMOModel w = wowee::pipeline::WMOLoader::load(bytes);
    if (w.nGroups == 0) { ++st.wmoFailed; return a; }
    for (uint32_t gi = 0; gi < w.nGroups; ++gi)
        for (const std::string& gp : wowee::pipeline::wmoGroupCandidates(path, gi)) {
            std::vector<uint8_t> gb;
            if (!data.read(gp, gb)) continue;
            wowee::pipeline::WMOLoader::loadGroup(gb, w, gi);
            break;
        }

    size_t untextured = 0, untexLogged = 0;
    // Textures used by this WMO: MOTX offset -> TEX id (per blend capability).
    std::map<std::pair<uint32_t, bool>, uint32_t> texCache;
    auto texFor = [&](uint32_t motxOffset, bool alpha) -> uint32_t {
        auto key2 = std::make_pair(motxOffset, alpha);
        auto f = texCache.find(key2);
        if (f != texCache.end()) return f->second;
        uint32_t id = 0;
        auto ti = w.textureOffsetToIndex.find(motxOffset);
        if (ti != w.textureOffsetToIndex.end() && ti->second < w.textures.size()) {
            id = addTextureFromGame(pack, data, w.textures[ti->second], o.wmoTex, o.texFormat, alpha, o.verbose);
            if (id) ++st.texturesOk; else ++st.texturesFailed;
        }
        texCache[key2] = id;
        return id;
    };

    // Collision faces (MOPY, wowdev): collidable = F_COLLISION (0x08) or a render face
    // (F_RENDER 0x20 without F_DETAIL 0x04). Collision-only faces are in MOVI but in no batch.
    for (const auto& g : w.groups) {
        size_t nTri = std::min(g.triFlags.size(), g.indices.size() / 3);
        for (size_t t = 0; t < nTri; ++t) {
            uint8_t f = g.triFlags[t];
            bool collidable = (f & 0x08) || ((f & 0x20) && !(f & 0x04));
            if (!collidable) continue;
            uint16_t i0 = g.indices[t * 3], i1 = g.indices[t * 3 + 1], i2 = g.indices[t * 3 + 2];
            if (i0 >= g.vertices.size() || i1 >= g.vertices.size() || i2 >= g.vertices.size()) continue;
            a.coll.push_back(g.vertices[i0].position);
            a.coll.push_back(g.vertices[i1].position);
            a.coll.push_back(g.vertices[i2].position);
        }
    }

    size_t groupsOk = 0;
    for (uint32_t gi = 0; gi < w.groups.size(); ++gi) {
        const auto& g = w.groups[gi];
        if (g.vertices.empty() || g.indices.empty()) continue;
        if (o.wmoExteriorOnly && (g.flags & 0x2000) && !(g.flags & 0x8)) { ++st.wmoGroupsSkipped; continue; }
        // Group centre = centre of the vertices actually used by render batches.
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const auto& b : g.batches) {
            if (b.materialId == 0xFF) continue;
            for (uint32_t k = b.startIndex; k < b.startIndex + b.indexCount && k < g.indices.size(); ++k) {
                uint16_t vi = g.indices[k];
                if (vi >= g.vertices.size()) continue;
                lo = glm::min(lo, g.vertices[vi].position);
                hi = glm::max(hi, g.vertices[vi].position);
            }
        }
        if (lo.x > hi.x) continue;
        glm::vec3 center = 0.5f * (lo + hi);

        ModelSource src;
        src.verts.resize(g.vertices.size());
        for (size_t i = 0; i < g.vertices.size(); ++i) {
            const auto& v = g.vertices[i];
            auto& d = src.verts[i];
            d.p[0] = v.position.x - center.x; d.p[1] = v.position.y - center.y; d.p[2] = v.position.z - center.z;
            d.n[0] = v.normal.x; d.n[1] = v.normal.y; d.n[2] = v.normal.z;
            d.uv[0] = v.texCoord.x; d.uv[1] = v.texCoord.y;
        }
        std::map<uint32_t, uint32_t> slot;   // TEX id -> slot in this model's table
        for (const auto& b : g.batches) {
            if (b.materialId == 0xFF || b.indexCount < 3) continue;
            if ((size_t)b.startIndex + b.indexCount > g.indices.size()) continue;
            ModelSource::Sub s;
            s.texId = UINT32_MAX;
            s.blend = W3D_BLEND_OPAQUE;
            s.flags = W3D_SUBF_BUILDING | (((g.flags & 0x2000) && !(g.flags & 0x8)) ? W3D_SUBF_INTERIOR : 0);
            if (b.materialId < w.materials.size()) {
                const auto& m = w.materials[b.materialId];
                s.blend = mapWmoBlend(m.blendMode);
                s.flags |= (uint8_t)(((m.flags & 0x4) ? W3D_SUBF_TWO_SIDED : 0) | ((m.flags & 0x1) ? W3D_SUBF_UNLIT : 0));
                uint32_t tid = texFor(m.texture1, s.blend != W3D_BLEND_OPAQUE);
                if (!tid) {   // the batch is drawn untextured (flat grey): say why
                    ++untextured;
                    if (o.verbose && untexLogged++ < 12) {
                        auto ti = w.textureOffsetToIndex.find(m.texture1);
                        std::printf("    UNTEXTURED batch in group %u: material %u texture1 offset %u (%s), flags 0x%x, shader %u, blend %u\n", gi,
                                    (unsigned)b.materialId, (unsigned)m.texture1,
                                    ti == w.textureOffsetToIndex.end() ? "offset not in the texture table" :
                                    ti->second >= w.textures.size() ? "index out of range" : w.textures[ti->second].c_str(),
                                    (unsigned)m.flags, (unsigned)m.shader, (unsigned)m.blendMode);
                    }
                }
                if (tid) {
                    auto [si, fresh] = slot.emplace(tid, (uint32_t)src.texIds.size());
                    if (fresh) src.texIds.push_back(tid);
                    s.texId = si->second;
                }
            }
            s.idx.assign(g.indices.begin() + b.startIndex, g.indices.begin() + b.startIndex + b.indexCount);
            for (uint32_t& i : s.idx) if (i >= src.verts.size()) i = 0;
            src.subs.push_back(std::move(s));
        }
        if (src.subs.empty()) continue;
        size_t tris = 0;
        int flipped = 0;
        auto blob = buildModelBlob(src, o.wmoGroupTris, &tris, &flipped);
        st.wmoSubsFlipped += flipped;
        if (blob.empty()) { ++st.wmoGroupsFailed; continue; }
        char idKey[600];
        std::snprintf(idKey, sizeof idKey, "%s#%u", key.c_str(), gi);
        uint32_t id = w3d_fnv1a(idKey);
        pack.add(W3D_ENTRY_MODEL, id, std::move(blob));
        a.groups.push_back({id, center});
        st.wmoTris += tris;
        ++st.wmoGroups;
        ++groupsOk;
        if (o.verbose)
            std::printf("    group %2u %-28.28s %5zu tris %s\n", gi, g.name.c_str(), tris,
                        (g.flags & 0x2000) ? "interior" : "exterior");
    }

    if (untextured && o.verbose) std::printf("  WMO %s: %zu batches drawn untextured\n", path.c_str(), untextured);
    st.wmoBatchesUntextured += untextured;

    // Doodad sets: local transforms (WMO space).
    a.sets.resize(w.doodadSets.size());
    for (size_t si = 0; si < w.doodadSets.size(); ++si) {
        const auto& set = w.doodadSets[si];
        for (uint32_t k = 0; k < set.count; ++k) {
            uint32_t di = set.startIndex + k;
            if (di >= w.doodads.size()) break;
            const auto& d = w.doodads[di];
            auto nm = w.doodadNames.find(d.nameIndex);
            if (nm == w.doodadNames.end() || nm->second.empty()) continue;
            glm::quat q(d.rotation.w, d.rotation.x, d.rotation.y, d.rotation.z);
            glm::mat4 local(1.0f);
            local = glm::translate(local, d.position);
            local *= glm::mat4_cast(q);
            local = glm::scale(local, glm::vec3(d.scale));
            a.sets[si].push_back({wowee::pipeline::modelPathToM2(nm->second), local});
        }
    }
    a.ok = groupsOk > 0;
    if (a.ok) ++st.wmoOk; else ++st.wmoFailed;
    if (o.verbose)
        std::printf("  WMO %-56s %zu/%u groups, %zu doodad sets\n", path.c_str(), groupsOk, w.nGroups, a.sets.size());
    return a;
}

// Sutherland-Hodgman clip of a triangle (x, y, z) to the rectangle [x0,x1] x [y0,y1];
// z is interpolated linearly (exact for planar faces). Returns the polygon.
static std::vector<glm::vec3> clipToRect(const glm::vec3* tri, float x0, float y0, float x1, float y1) {
    std::vector<glm::vec3> poly(tri, tri + 3), out;
    for (int edge = 0; edge < 4; ++edge) {
        out.clear();
        auto inside = [&](const glm::vec3& p) {
            switch (edge) {
                case 0: return p.x >= x0;
                case 1: return p.x <= x1;
                case 2: return p.y >= y0;
                default: return p.y <= y1;
            }
        };
        auto cut = [&](const glm::vec3& a, const glm::vec3& b) {
            float t;
            switch (edge) {
                case 0: t = (x0 - a.x) / (b.x - a.x); break;
                case 1: t = (x1 - a.x) / (b.x - a.x); break;
                case 2: t = (y0 - a.y) / (b.y - a.y); break;
                default: t = (y1 - a.y) / (b.y - a.y); break;
            }
            return a + (b - a) * t;
        };
        for (size_t i = 0; i < poly.size(); ++i) {
            const glm::vec3& cur = poly[i];
            const glm::vec3& prev = poly[(i + poly.size() - 1) % poly.size()];
            bool ci = inside(cur), pi = inside(prev);
            if (ci) {
                if (!pi) out.push_back(cut(prev, cur));
                out.push_back(cur);
            } else if (pi) {
                out.push_back(cut(prev, cur));
            }
        }
        poly.swap(out);
        if (poly.empty()) break;
    }
    return poly;
}

// Classifies collision triangles (local space, 3 vertices each) placed by W as floors or
// walls and clips them into every tile of the region they touch.
static void addCollisionTris(const Options& o, ConvStats& st, const glm::vec3* src, size_t vertCount,
                             const glm::mat4& W, bool fromM2) {
    const float TS = 533.33333f;
    for (size_t i = 0; i + 2 < vertCount; i += 3) {
        glm::vec3 v[3];
        for (int k = 0; k < 3; ++k) v[k] = glm::vec3(W * glm::vec4(src[i + k], 1.0f));
        (fromM2 ? st.m2CollSource : st.collSource)++;
        glm::vec3 n = glm::cross(v[1] - v[0], v[2] - v[0]);
        float len = glm::length(n);
        if (!(len > 1e-6f)) continue;                     // degenerate (or NaN from a broken model)
        bool wall = std::fabs(n.z) / len < 0.5f;          // steeper than 60 degrees
        if (wall && o.noWalls) continue;
        if (wall) (fromM2 ? st.m2CollWalls : st.collWalls)++;
        else (fromM2 ? st.m2CollWalkable : st.collWalkable)++;
        uint16_t flags = (uint16_t)((wall ? W3D_COLL_WALL : 0u) | (fromM2 ? W3D_COLL_M2 : 0u));
        float minX = std::min({v[0].x, v[1].x, v[2].x}), maxX = std::max({v[0].x, v[1].x, v[2].x});
        float minY = std::min({v[0].y, v[1].y, v[2].y}), maxY = std::max({v[0].y, v[1].y, v[2].y});
        // tiles overlapped: tileY from x, tileX from y (tile min corner = (32 - t - 1) * TS)
        int ty0 = (int)std::floor(32.0f - maxX / TS), ty1 = (int)std::floor(32.0f - minX / TS);
        int tx0 = (int)std::floor(32.0f - maxY / TS), tx1 = (int)std::floor(32.0f - minY / TS);
        for (int tx = tx0; tx <= tx1; ++tx)
            for (int ty = ty0; ty <= ty1; ++ty) {
                if (tx < o.tileX0 || tx > o.tileX1 || ty < o.tileY0 || ty > o.tileY1) continue;
                float rx0 = (32.0f - ty - 1.0f) * TS, ry0 = (32.0f - tx - 1.0f) * TS;
                std::vector<glm::vec3> poly = clipToRect(v, rx0, ry0, rx0 + TS, ry0 + TS);
                if (poly.size() < 3) continue;
                auto& dst = st.collTris[w3d_tile_id(tx, ty)];
                for (size_t k = 1; k + 1 < poly.size(); ++k) {
                    ConvStats::CollTri t = {{poly[0].x, poly[0].y, poly[0].z, poly[k].x, poly[k].y, poly[k].z,
                                             poly[k + 1].x, poly[k + 1].y, poly[k + 1].z}, flags};
                    dst.push_back(t);
                    ++st.collPieces;
                }
            }
    }
}

void addPlacementCollision(const Options& o, ConvStats& st, const W3dPlacement& p) {
    if (o.noFloors || o.noDoodadColl) return;
    auto it = st.m2Coll.find(p.modelId);
    if (it == st.m2Coll.end() || it->second.empty()) return;
    glm::mat4 M(1.0f);   // same order as the 3DS renderer: T(pos) * Rz * Ry * Rx * S(scale)
    M = glm::translate(M, glm::vec3(p.pos[0], p.pos[1], p.pos[2]));
    M = glm::rotate(M, p.rot[2], glm::vec3(0, 0, 1));
    M = glm::rotate(M, p.rot[1], glm::vec3(0, 1, 0));
    M = glm::rotate(M, p.rot[0], glm::vec3(1, 0, 0));
    M = glm::scale(M, glm::vec3(p.scale));
    static_assert(sizeof(glm::vec3) == 3 * sizeof(float), "packed vec3");
    addCollisionTris(o, st, reinterpret_cast<const glm::vec3*>(it->second.data()), it->second.size() / 3, M, true);
    ++st.m2CollPlaced;
}

void emitCollision(const Options& o, PackWriter& pack, ConvStats& st) {
    const float TS = 533.33333f;
    for (auto& [tid, tris] : st.collTris) {
        if (tris.empty()) continue;
        // Finer grid for dense tiles (cities, forests with trunk collision): walls are tested
        // every step, so fewer triangles per cell matter more than the index size.
        const uint32_t N = tris.size() > 6000 ? 64 : 32;
        uint32_t tx = tid / 64, ty = tid % 64;
        float ox = (32.0f - ty - 1.0f) * TS, oy = (32.0f - tx - 1.0f) * TS;
        float zmin = 1e30f, zmax = -1e30f;
        for (auto& t : tris) for (int k = 0; k < 3; ++k) { zmin = std::min(zmin, t.v[k * 3 + 2]); zmax = std::max(zmax, t.v[k * 3 + 2]); }
        W3dCollHeader h{};
        h.tileX = tx; h.tileY = ty;
        h.origin[0] = ox; h.origin[1] = oy; h.origin[2] = 0.5f * (zmin + zmax);
        h.zScale = std::max((zmax - zmin) / 65000.0f, 1.0f / 256.0f);
        h.gridN = N;
        h.triCount = (uint32_t)tris.size();
        std::vector<W3dCollTri> q(tris.size());
        std::vector<std::vector<uint32_t>> cells(N * N);
        auto qxy = [&](float v) { return (uint16_t)std::clamp((int)std::lround(v / W3D_COLL_XY_SCALE), 0, 65535); };
        for (size_t i = 0; i < tris.size(); ++i) {
            const auto& t = tris[i];
            float lx[3], ly[3];
            for (int k = 0; k < 3; ++k) {
                lx[k] = t.v[k * 3] - ox; ly[k] = t.v[k * 3 + 1] - oy;
                q[i].x[k] = qxy(lx[k]);
                q[i].y[k] = qxy(ly[k]);
                q[i].z[k] = (int16_t)std::clamp((int)std::lround((t.v[k * 3 + 2] - h.origin[2]) / h.zScale), -32767, 32767);
            }
            q[i].flags = t.flags;
            float cs = TS / N;
            int cx0 = std::clamp((int)std::floor(std::min({lx[0], lx[1], lx[2]}) / cs), 0, (int)N - 1);
            int cx1 = std::clamp((int)std::floor(std::max({lx[0], lx[1], lx[2]}) / cs), 0, (int)N - 1);
            int cy0 = std::clamp((int)std::floor(std::min({ly[0], ly[1], ly[2]}) / cs), 0, (int)N - 1);
            int cy1 = std::clamp((int)std::floor(std::max({ly[0], ly[1], ly[2]}) / cs), 0, (int)N - 1);
            for (int cy = cy0; cy <= cy1; ++cy)
                for (int cx = cx0; cx <= cx1; ++cx) cells[cy * N + cx].push_back((uint32_t)i);
        }
        std::vector<uint32_t> start(N * N + 1), index;
        for (uint32_t c = 0; c < N * N; ++c) {
            start[c] = (uint32_t)index.size();
            index.insert(index.end(), cells[c].begin(), cells[c].end());
        }
        start[N * N] = (uint32_t)index.size();
        h.indexCount = (uint32_t)index.size();
        Blob b;
        b.put(h);
        b.align(4);
        size_t offC = b.putArray(start);
        b.align(4);
        size_t offI = b.putArray(index);
        b.align(4);
        size_t offT = b.putArray(q);
        b.align(4);
        auto* hp = b.at<W3dCollHeader>(0);
        hp->offCells = (uint32_t)offC; hp->offIndex = (uint32_t)offI; hp->offTris = (uint32_t)offT;
        st.collBytes += b.data.size();
        st.collMaxTileBytes = std::max(st.collMaxTileBytes, b.data.size());
        size_t walls = 0;
        for (auto& t : tris) walls += (t.flags & W3D_COLL_WALL) != 0;
        if (o.verbose)
            std::printf("  collision tile %u_%u: %zu triangles (%zu walls), grid %u, %.1f KB\n", tx, ty, tris.size(), walls, N,
                        b.data.size() / 1024.0);
        pack.add(W3D_ENTRY_COLLISION, tid, std::move(b.data));
    }
}

void placeWmo(PackWriter& pack, const DataDir& data, const std::string& path, const float pos[3],
              const float eulerRad[3], uint16_t doodadSet, const Options& o, ConvStats& st,
              const std::function<uint32_t(const std::string&)>& modelFor, std::vector<W3dPlacement>& out) {
    const WmoAsset& a = loadWmo(pack, data, path, o, st);
    if (!a.ok) return;
    glm::mat4 W(1.0f);
    W = glm::translate(W, glm::vec3(pos[0], pos[1], pos[2]));
    W = glm::rotate(W, eulerRad[2], glm::vec3(0, 0, 1));
    W = glm::rotate(W, eulerRad[1], glm::vec3(0, 1, 0));
    W = glm::rotate(W, eulerRad[0], glm::vec3(1, 0, 0));
    for (const auto& g : a.groups) {
        glm::vec4 c = W * glm::vec4(g.center, 1.0f);
        W3dPlacement p{};
        p.modelId = g.modelId;
        p.pos[0] = c.x; p.pos[1] = c.y; p.pos[2] = c.z;
        p.rot[0] = eulerRad[0]; p.rot[1] = eulerRad[1]; p.rot[2] = eulerRad[2];
        p.scale = 1.0f;
        out.push_back(p);
        ++st.wmoPlacedGroups;
        if (st.dump)
            std::fprintf(st.dump, "group %s %08x pos %.4f %.4f %.4f rot %.6f %.6f %.6f\n", path.c_str(), g.modelId,
                         p.pos[0], p.pos[1], p.pos[2], p.rot[0], p.rot[1], p.rot[2]);
    }
    ++st.wmoPlacements;
    if (!o.noFloors) addCollisionTris(o, st, a.coll.data(), a.coll.size(), W, false);
    if (o.noWmoDoodads) return;
    std::vector<size_t> sets = {0};
    if (doodadSet > 0 && doodadSet < a.sets.size()) sets.push_back(doodadSet);
    for (size_t si : sets) {
        if (si >= a.sets.size()) continue;
        for (const auto& d : a.sets[si]) {
            uint32_t mid = modelFor(d.path);
            glm::mat4 M = W * d.local;
            W3dPlacement p{};
            p.modelId = mid;
            p.pos[0] = M[3][0]; p.pos[1] = M[3][1]; p.pos[2] = M[3][2];
            p.scale = matrixToPlacement(&M[0][0], p.rot);
            if (st.dump)
                std::fprintf(st.dump, "doodad %s %s pos %.4f %.4f %.4f rot %.6f %.6f %.6f scale %.5f\n", d.path.c_str(),
                             mid ? "ok" : "missing", p.pos[0], p.pos[1], p.pos[2], p.rot[0], p.rot[1], p.rot[2], p.scale);
            if (!mid) continue;
            out.push_back(p);
            addPlacementCollision(o, st, p);
            ++st.wmoDoodads;
        }
    }
}

}  // namespace w3d
