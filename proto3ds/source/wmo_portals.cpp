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

bool cameraInside(const Node& n, const float cam[3], float margin) {
    float d[3] = {cam[0] - n.pos[0], cam[1] - n.pos[1], cam[2] - n.pos[2]}, l[3];
    unrotate(n.rot, d, l);
    for (int k = 0; k < 3; ++k)
        if (l[k] < n.bmin[k] - margin || l[k] > n.bmax[k] + margin) return false;
    return true;
}

std::vector<uint32_t> buildIndex(const std::vector<Node>& nodes) {
    std::vector<uint32_t> idx(nodes.size());
    for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        return nodes[a].modelId != nodes[b].modelId ? nodes[a].modelId < nodes[b].modelId : a < b;
    });
    return idx;
}

uint32_t reach(std::vector<Node>& nodes, const std::vector<uint32_t>& byModel, const float cam[3], SphereFn inView, void* ctx,
               float maxDist, uint32_t frame) {
    uint32_t reached = 0;
    std::vector<uint32_t> stack;
    std::vector<uint8_t> seed(nodes.size(), 0);
    auto mark = [&](uint32_t i, bool isSeed) {
        if (nodes[i].stamp == frame) return;
        nodes[i].stamp = frame;
        seed[i] = isSeed;
        stack.push_back(i);
        ++reached;
    };
    // seeds: the groups the camera is in
    for (uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].hasTable && cameraInside(nodes[i], cam, 1.0f)) mark(i, true);
    if (stack.empty()) {   // outside: the outer groups near and in view, so that the portals of a facade show what lies behind
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            const Node& n = nodes[i];
            if (!n.hasTable || n.interior || !n.nPortals) continue;
            const float dx = n.pos[0] - cam[0], dy = n.pos[1] - cam[1], dz = n.pos[2] - cam[2];
            const float lim = maxDist + n.radius;
            if (dx * dx + dy * dy + dz * dz > lim * lim) continue;
            if (!inView(ctx, n.pos, n.radius)) continue;
            mark(i, true);
        }
    }
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        const Node& n = nodes[i];
        float camLocal[3];
        if (seed[i]) {
            const float d[3] = {cam[0] - n.pos[0], cam[1] - n.pos[1], cam[2] - n.pos[2]};
            unrotate(n.rot, d, camLocal);
        }
        for (uint32_t k = 0; k < n.nPortals; ++k) {
            const W3dPortal& p = n.portals[k];
            if (seed[i] && !(p.flags & W3D_PORTAL_NOFACE)) {   // the camera must be on this group's side of the portal
                const float s = p.plane[0] * camLocal[0] + p.plane[1] * camLocal[1] + p.plane[2] * camLocal[2] - p.plane[3];
                if (s > 0.5f) continue;
            }
            float c[3];
            rotate(n.rot, p.center, c);
            c[0] += n.pos[0]; c[1] += n.pos[1]; c[2] += n.pos[2];
            const float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
            const float lim = maxDist + p.radius;
            if (dx * dx + dy * dy + dz * dz > lim * lim) continue;
            if (!inView(ctx, c, p.radius + 0.5f)) continue;
            // the group behind: the one with that model nearest to where the portal says it is
            float del[3], want[3];
            rotate(n.rot, p.delta, del);
            want[0] = n.pos[0] + del[0]; want[1] = n.pos[1] + del[1]; want[2] = n.pos[2] + del[2];
            auto lo = std::lower_bound(byModel.begin(), byModel.end(), p.toModel,
                                       [&](uint32_t a, uint32_t m) { return nodes[a].modelId < m; });
            int best = -1;
            float bestD = 4.0f;   // (yards, squared: within 2)
            for (auto it = lo; it != byModel.end() && nodes[*it].modelId == p.toModel; ++it) {
                const Node& o = nodes[*it];
                const float ex = o.pos[0] - want[0], ey = o.pos[1] - want[1], ez = o.pos[2] - want[2];
                const float e2 = ex * ex + ey * ey + ez * ez;
                if (e2 < bestD) { bestD = e2; best = (int)*it; }
            }
            if (best >= 0) mark((uint32_t)best, false);
        }
    }
    return reached;
}

}  // namespace wmoportal
