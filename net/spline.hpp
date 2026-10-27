// Catmull-Rom splines as the vMaNGOS server builds them (Movement/spline/spline.cpp), shared by taxi flights
// (objects.cpp) and transports (transport.cpp).
#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

namespace w3n {

struct Vec3f { float x = 0, y = 0, z = 0; };

inline float dist3(const Vec3f& a, const Vec3f& b) {
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Catmull-Rom as the server's MoveSpline (vMaNGOS Movement/spline/spline.cpp): uniform, the first control point is
// mirrored in front (2*c0 - c1), the last one repeated; segment lengths from 3 steps; the time within a segment is
// linear in the parameter and segments take time in proportion to their length.
inline void crWeights(float t, float w[4]) {
    static const float M[4][4] = {{-0.5f, 1.5f, -1.5f, 0.5f}, {1.f, -2.5f, 2.f, -0.5f}, {-0.5f, 0.f, 0.5f, 0.f}, {0.f, 1.f, 0.f, 0.f}};
    const float tv[4] = {t * t * t, t * t, t, 1.0f};
    for (int j = 0; j < 4; ++j) w[j] = tv[0] * M[0][j] + tv[1] * M[1][j] + tv[2] * M[2][j] + tv[3] * M[3][j];
}
inline void crDerivWeights(float t, float w[4]) {
    static const float M[4][4] = {{-0.5f, 1.5f, -1.5f, 0.5f}, {1.f, -2.5f, 2.f, -0.5f}, {-0.5f, 0.f, 0.5f, 0.f}, {0.f, 1.f, 0.f, 0.f}};
    const float tv[4] = {3 * t * t, 2 * t, 1.0f, 0.0f};
    for (int j = 0; j < 4; ++j) w[j] = tv[0] * M[0][j] + tv[1] * M[1][j] + tv[2] * M[2][j] + tv[3] * M[3][j];
}
// the 4 control points of segment s (between path[s] and path[s+1])
inline void crSegment(const std::vector<Vec3f>& path, size_t s, Vec3f out[4]) {
    const size_t n = path.size();
    out[0] = s == 0 ? Vec3f{2 * path[0].x - path[1].x, 2 * path[0].y - path[1].y, 2 * path[0].z - path[1].z} : path[s - 1];
    out[1] = path[s];
    out[2] = path[s + 1];
    out[3] = s + 2 < n ? path[s + 2] : path[n - 1];
}
inline Vec3f crEval(const Vec3f p[4], const float w[4]) {
    return {p[0].x * w[0] + p[1].x * w[1] + p[2].x * w[2] + p[3].x * w[3], p[0].y * w[0] + p[1].y * w[1] + p[2].y * w[2] + p[3].y * w[3],
            p[0].z * w[0] + p[1].z * w[1] + p[2].z * w[2] + p[3].z * w[3]};
}
inline float crSegLength(const std::vector<Vec3f>& path, size_t s) {
    Vec3f p[4];
    crSegment(path, s, p);
    Vec3f cur = p[1];
    float len = 0;
    for (int i = 1; i <= 3; ++i) {
        float w[4];
        crWeights(i / 3.0f, w);
        Vec3f nx = crEval(p, w);
        len += dist3(cur, nx);
        cur = nx;
    }
    return len;
}


}  // namespace w3n
