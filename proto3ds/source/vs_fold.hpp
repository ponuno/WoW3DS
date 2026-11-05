// vs_fold.hpp — 6.70: the model vertex shader made shorter by doing per-draw work once on the CPU (no citro3d here, so the PC
// test checks it). The old shader computed per vertex: p' = posScale * p + posOffset; view = modelView * p'; clip = projection * view;
// uv' = (uvRowA, uvRowB) . (uvScale * uv, 1); light from lightDir . (normal / 127). The new one gets
//   modelView' = modelView * T(posOffset) * S(posScale)   (view = modelView' * p; only rows 0..2 are used: the fog distance)
//   mvp        = projection * modelView'                   (clip = mvp * p)
//   uvRowA/B'  = rows with their u, v terms times uvScale
//   lightDir'  = lightDir / 127
// and the same numbers come out (vsfold_test compares both shaders run in C++ on random input).
#pragma once

namespace vsfold {

struct M4 { float m[4][4]; };   // row-major: m[row][col], a column vector on the right

inline M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// modelView * T(off) * S(scale)
inline M4 foldScaleOffset(const M4& mv, const float scale[3], const float off[3]) {
    M4 r = mv;
    for (int i = 0; i < 4; ++i) {
        float t = mv.m[i][3];
        for (int j = 0; j < 3; ++j) {
            t += mv.m[i][j] * off[j];
            r.m[i][j] = mv.m[i][j] * scale[j];
        }
        r.m[i][3] = t;
    }
    return r;
}

// a texture transform row (u, v, constant) with the vertex's uv scale taken in
inline void foldUvRow(const float row[3], float uvScale, float out[3]) {
    out[0] = row[0] * uvScale;
    out[1] = row[1] * uvScale;
    out[2] = row[2];
}

}  // namespace vsfold
