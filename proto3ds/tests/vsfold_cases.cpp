// vsfold_cases — 6.70: random draws folded by vs_fold.hpp, printed for vsfold_check.py (which runs the old and the new model
// shader on them with a small PICA200 interpreter and compares the outputs).
#include <cstdio>
#include <cstdlib>
#include "vs_fold.hpp"

static float rnd(float a, float b) { return a + (b - a) * (float)rand() / (float)RAND_MAX; }

int main() {
    srand(1234);
    for (int c = 0; c < 200; ++c) {
        vsfold::M4 proj{}, mv{};
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { proj.m[i][j] = rnd(-2, 2); mv.m[i][j] = rnd(-3, 3); }
        mv.m[3][0] = mv.m[3][1] = mv.m[3][2] = 0; mv.m[3][3] = 1;   // an affine model-view, as drawn
        float scale[4] = {rnd(0.001f, 0.05f), rnd(0.001f, 0.05f), rnd(0.001f, 0.05f), rnd(0.0001f, 0.01f)};
        float off[3] = {rnd(-50, 50), rnd(-50, 50), rnd(-50, 50)};
        float uvA[3] = {rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)}, uvB[3] = {rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)};
        float light[3] = {rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)};
        float lp[4] = {rnd(0, 1), rnd(0, 1), 0, rnd(0, 1)}, fog[4] = {rnd(0.001f, 0.01f), rnd(1, 3), 0, 0};
        float pos[3] = {rnd(-30000, 30000), rnd(-30000, 30000), rnd(-30000, 30000)}, nrm[3] = {rnd(-127, 127), rnd(-127, 127), rnd(-127, 127)};
        float uv[2] = {rnd(-30000, 30000), rnd(-30000, 30000)};
        const vsfold::M4 mvF = vsfold::foldScaleOffset(mv, scale, off), mvp = vsfold::mul(proj, mvF);
        float a2[3], b2[3];
        vsfold::foldUvRow(uvA, scale[3], a2);
        vsfold::foldUvRow(uvB, scale[3], b2);
        auto m4 = [](const char* n, const vsfold::M4& m) { for (int i = 0; i < 4; ++i) printf("%s%d %.9g %.9g %.9g %.9g\n", n, i, m.m[i][0], m.m[i][1], m.m[i][2], m.m[i][3]); };
        printf("case %d\n", c);
        m4("projection", proj); m4("modelView", mv); m4("mvp", mvp); m4("modelViewF", mvF);
        printf("posScale %.9g %.9g %.9g %.9g\nposOffset %.9g %.9g %.9g 0\n", scale[0], scale[1], scale[2], scale[3], off[0], off[1], off[2]);
        printf("uvRowA %.9g %.9g %.9g 0\nuvRowB %.9g %.9g %.9g 0\nuvRowAF %.9g %.9g %.9g 0\nuvRowBF %.9g %.9g %.9g 0\n", uvA[0], uvA[1], uvA[2], uvB[0], uvB[1], uvB[2], a2[0], a2[1], a2[2], b2[0], b2[1], b2[2]);
        printf("lightDir %.9g %.9g %.9g 0\nlightDirF %.9g %.9g %.9g 0\n", light[0], light[1], light[2], light[0] / 127.0f, light[1] / 127.0f, light[2] / 127.0f);
        printf("lightParams %.9g %.9g %.9g %.9g\nfogParams %.9g %.9g 0 0\n", lp[0], lp[1], lp[2], lp[3], fog[0], fog[1]);
        printf("v0 %.9g %.9g %.9g 77\nv1 %.9g %.9g %.9g 5\nv2 %.9g %.9g 0 1\n", pos[0], pos[1], pos[2], nrm[0], nrm[1], nrm[2], uv[0], uv[1]);
    }
}
