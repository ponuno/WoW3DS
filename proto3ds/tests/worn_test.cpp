// 6.92: the matrix of a two-hander / bow / gun on the back, against WoWee's glm sequence
// T(-0.03,-0.10,0) * Rx(33) * Ry(90) * Rx(90), written out here with plain axis rotations (each applied on the right).
#include <cstdio>
#include <cmath>
#include "w3d_format.h"
typedef struct { double m[4][4]; } M4;
static M4 ident() { M4 r{}; for (int i = 0; i < 4; ++i) r.m[i][i] = 1; return r; }
static M4 mul(const M4& a, const M4& b) { M4 r{}; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j]; return r; }
static M4 rot(double deg, int ax) {   // glm::rotate: right-handed, counter-clockwise about the axis
    const double a = deg * M_PI / 180, c = cos(a), s = sin(a);
    M4 r = ident();
    if (ax == 0) { r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; }
    if (ax == 1) { r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c; }
    return r;
}
int main() {
    M4 w = ident();
    w.m[0][3] = -0.03; w.m[1][3] = -0.10;
    w = mul(w, rot(33, 0)); w = mul(w, rot(90, 1)); w = mul(w, rot(90, 0));
    float f[12];
    w3d_back_weapon_xform(f);
    int bad = 0;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) if (fabs(f[r * 4 + c] - w.m[r][c]) > 1e-5) { ++bad; printf("mismatch [%d][%d]: %f vs %f\n", r, c, f[r * 4 + c], w.m[r][c]); }
    // the blade (local +X) must stand up: after the turn it points mostly along the character's Z
    const double bx = f[0], by = f[4], bz = f[8];
    printf("blade axis -> (%.3f %.3f %.3f)\n", bx, by, bz);
    if (fabs(bz) < 0.8) { ++bad; puts("blade not upright"); }
    // a proper rotation: det = 1, orthonormal rows
    double det = f[0] * (f[5] * f[10] - f[6] * f[9]) - f[1] * (f[4] * f[10] - f[6] * f[8]) + f[2] * (f[4] * f[9] - f[5] * f[8]);
    if (fabs(det - 1) > 1e-5) { ++bad; printf("det %f\n", det); }
    printf("worn_test: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
