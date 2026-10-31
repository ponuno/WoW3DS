#include "wmo_portals.hpp"

#include <math.h>

#include <algorithm>

namespace wmoportal {

void rotate(const float r[3], const float v[3], float o[3]) {
    const float cx = cosf(r[0]), sx = sinf(r[0]), cy = cosf(r[1]), sy = sinf(r[1]), cz = cosf(r[2]), sz = sinf(r[2]);
    const float y1 = cx * v[1] - sx * v[2], z1 = sx * v[1] + cx * v[2];   // Rx
    const float x2 = cy * v[0] + sy * z1, z2 = -sy * v[0] + cy * z1;       // Ry
    o[0] = cz * x2 - sz * y1;                                               // Rz
    o[1] = sz * x2 + cz * y1;
    o[2] = z2;
}

void unrotate(const float r[3], const float v[3], float o[3]) {
    const float cx = cosf(r[0]), sx = sinf(r[0]), cy = cosf(r[1]), sy = sinf(r[1]), cz = cosf(r[2]), sz = sinf(r[2]);
    const float x1 = cz * v[0] + sz * v[1], y1 = -sz * v[0] + cz * v[1];   // Rz^-1
    const float x2 = cy * x1 - sy * v[2], z2 = sy * x1 + cy * v[2];        // Ry^-1
    o[0] = x2;
    o[1] = cx * y1 + sx * z2;                                               // Rx^-1
    o[2] = -sx * y1 + cx * z2;
}

bool inside(const Node& n, const float p[3], float margin) {
    float d[3] = {p[0] - n.pos[0], p[1] - n.pos[1], p[2] - n.pos[2]}, l[3];
    unrotate(n.rot, d, l);
    for (int k = 0; k < 3; ++k)
        if (l[k] < n.bmin[k] - margin || l[k] > n.bmax[k] + margin) return false;
    return true;
}

static float volume(const Node& n) {
    return (n.bmax[0] - n.bmin[0]) * (n.bmax[1] - n.bmin[1]) * (n.bmax[2] - n.bmin[2]);
}

void buildGraph(std::vector<Node>& nodes) {
    std::vector<uint32_t> byModel(nodes.size());
    for (uint32_t i = 0; i < byModel.size(); ++i) byModel[i] = i;
    std::sort(byModel.begin(), byModel.end(), [&](uint32_t a, uint32_t b) {
        return nodes[a].modelId != nodes[b].modelId ? nodes[a].modelId < nodes[b].modelId : a < b;
    });
    std::vector<uint32_t> parent(nodes.size());
    for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    auto root = [&](uint32_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        Node& n = nodes[i];
        n.next.assign(n.nPortals, kNone);
        for (uint32_t k = 0; k < n.nPortals; ++k) {
            const W3dPortal& p = n.portals[k];
            float del[3], want[3];
            rotate(n.rot, p.delta, del);
            want[0] = n.pos[0] + del[0]; want[1] = n.pos[1] + del[1]; want[2] = n.pos[2] + del[2];
            auto lo = std::lower_bound(byModel.begin(), byModel.end(), p.toModel,
                                       [&](uint32_t a, uint32_t m) { return nodes[a].modelId < m; });
            float bestD = 4.0f;   // (yards, squared: within 2)
            for (auto it = lo; it != byModel.end() && nodes[*it].modelId == p.toModel; ++it) {
                const Node& o = nodes[*it];
                const float ex = o.pos[0] - want[0], ey = o.pos[1] - want[1], ez = o.pos[2] - want[2];
                const float e2 = ex * ex + ey * ey + ez * ez;
                if (e2 < bestD) { bestD = e2; n.next[k] = *it; }
            }
            if (n.next[k] != kNone) parent[root(i)] = root(n.next[k]);
        }
    }
    for (uint32_t i = 0; i < nodes.size(); ++i) nodes[i].comp = root(i);
}

// The smallest box holding p (among the groups with a portal table), or kNone.
static uint32_t containing(const std::vector<Node>& nodes, const float p[3]) {
    uint32_t best = kNone;
    float bestV = 0;
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        if (!n.hasTable || !inside(n, p, 0.0f)) continue;
        const float v = volume(n);
        if (best == kNone || v < bestV) { best = i; bestV = v; }
    }
    return best;
}

uint32_t reach(std::vector<Node>& nodes, const float cam[3], const float* viewer, SphereFn inView, void* ctx, uint32_t frame) {
    // 1. surely indoors? (WoWee: else no portal culling at all)
    const uint32_t c = containing(nodes, cam);
    if (c == kNone) return kNone;                                  // outside every group
    const Node& cn = nodes[c];
    if (!cn.interior || !cn.nPortals) return kNone;                // an outdoor group / a dead end
    const uint32_t comp = cn.comp;
    for (const Node& n : nodes)                                    // a doorway: an outdoor group's box holds the camera too
        if (n.comp == comp && n.hasTable && !n.interior && inside(n, cam, 0.0f)) return kNone;
    // 2. the walk: seeds are the camera's group, the character's and every outdoor group of the building
    std::vector<uint32_t> queue;
    auto mark = [&](uint32_t i) {
        if (i == kNone || nodes[i].stamp == frame || nodes[i].comp != comp) return;
        nodes[i].stamp = frame;
        queue.push_back(i);
    };
    mark(c);
    if (viewer) mark(containing(nodes, viewer));
    for (uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].comp == comp && !nodes[i].interior) mark(i);
    for (size_t q = 0; q < queue.size(); ++q) {
        const Node& n = nodes[queue[q]];
        for (uint32_t k = 0; k < n.nPortals && k < n.next.size(); ++k) {
            const uint32_t to = n.next[k];
            if (to == kNone || nodes[to].stamp == frame) continue;
            const W3dPortal& p = n.portals[k];
            float cc[3];
            rotate(n.rot, p.center, cc);
            cc[0] += n.pos[0]; cc[1] += n.pos[1]; cc[2] += n.pos[2];
            if (!inView(ctx, cc, p.radius + 0.5f)) continue;
            mark(to);
        }
    }
    return comp;
}

}  // namespace wmoportal
