// worldobj_test — rotation matrices of tilted objects and the door blockers (common/w3d_worldobj.h).
#include <cstdio>
#include <cmath>
#include "../../common/w3d_worldobj.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
#define NEAR(a, b, e) do { if (!(std::fabs((a) - (b)) <= (e))) { printf("FAIL line %d: %s = %g, want %g\n", __LINE__, #a, (double)(a), (double)(b)); ++fails; } } while (0)
int main() {
    {   // quaternion -> matrix: 90 degrees about Z turns +x into +y; about X turns +y into +z
        const float qz[4] = {0, 0, std::sin(0.7853982f), std::cos(0.7853982f)};
        float r[9];
        w3d_obj_rot_from_quat(qz, r);
        NEAR(r[0], 0, 1e-6f); NEAR(r[3], 1, 1e-6f); NEAR(r[1], -1, 1e-6f);
        const float qx[4] = {std::sin(0.7853982f), 0, 0, std::cos(0.7853982f)};
        w3d_obj_rot_from_quat(qx, r);
        NEAR(r[7], 1, 1e-6f); NEAR(r[5], -1, 1e-6f); NEAR(r[8], 0, 1e-6f);
        float ry[9];
        w3d_obj_rot_from_yaw(1.1f, ry);
        const float qy[4] = {0, 0, std::sin(0.55f), std::cos(0.55f)};
        w3d_obj_rot_from_quat(qy, r);
        for (int i = 0; i < 9; ++i) NEAR(r[i], ry[i], 1e-6f);
    }
    // a gate: model bounds x -0.3..0.3 (thin), y -3..3, z 0..4, standing at (100, 200, 10) turned 90 degrees (so it spans world x)
    const float bmin[3] = {-0.3f, -3.0f, 0}, bmax[3] = {0.3f, 3.0f, 4.0f}, pos[3] = {100, 200, 10};
    const W3dBlockBox gate = w3d_block_from_bounds(bmin, bmax, pos, 1.5707963f, 1.0f, 0.25f);
    NEAR(gate.cx, 100, 1e-4f); NEAR(gate.cy, 200, 1e-4f); NEAR(gate.z0, 10, 1e-5f); NEAR(gate.z1, 14, 1e-5f);
    NEAR(gate.hx, 0.3f, 1e-6f); NEAR(gate.hy, 3.0f, 1e-6f);
    const float r = 0.5f;
    {   // walking north (+y) into the gate from the south: stopped in front of it, x kept
        float tx = 101, ty = 199.9f;
        CHECK(w3d_block_boxes_move(&gate, 1, 101, 198.0f, 10, &tx, &ty, r) == 1);
        NEAR(tx, 101, 1e-4f);
        NEAR(ty, 200 - (0.3f + r), 1e-3f);
    }
    {   // diagonal into it: slides along the leaf (x moves on, y held)
        float tx = 102.0f, ty = 199.6f;
        CHECK(w3d_block_boxes_move(&gate, 1, 101.0f, 199.0f, 10, &tx, &ty, r) == 1);
        NEAR(tx, 102.0f, 1e-3f);
        NEAR(ty, 200 - (0.3f + r), 1e-3f);
    }
    {   // beside the gate (past its end) and far away: free
        float tx = 104.5f, ty = 200.0f;
        CHECK(w3d_block_boxes_move(&gate, 1, 104.5f, 198.0f, 10, &tx, &ty, r) == 0);
        tx = 100; ty = 190;
        CHECK(w3d_block_boxes_move(&gate, 1, 100, 189, 10, &tx, &ty, r) == 0);
    }
    {   // from the other side
        float tx = 100, ty = 200.2f;
        CHECK(w3d_block_boxes_move(&gate, 1, 100, 202, 10, &tx, &ty, r) == 1);
        NEAR(ty, 200 + 0.3f + r, 1e-3f);
    }
    {   // a rider far above (a bridge over the gate) or below it is not blocked
        float tx = 100, ty = 200.1f;
        CHECK(w3d_block_boxes_move(&gate, 1, 100, 199.0f, 20, &tx, &ty, r) == 0);
        tx = 100; ty = 200.1f;
        CHECK(w3d_block_boxes_move(&gate, 1, 100, 199.0f, 2, &tx, &ty, r) == 0);
    }
    {   // the door closed on the walker (it stands inside): held there or let out a short way, never thrown through
        float tx = 100.05f, ty = 200.4f;
        CHECK(w3d_block_boxes_move(&gate, 1, 100, 200.35f, 10, &tx, &ty, r) == 1);
        CHECK(std::fabs(ty - 200.35f) < 1.0f);
    }
    {   // an unrotated thin box widened to the minimum
        const float a[3] = {-0.01f, -2, 0}, b[3] = {0.01f, 2, 3};
        const float p0[3] = {0, 0, 0};
        W3dBlockBox t = w3d_block_from_bounds(a, b, p0, 0, 2.0f, 0.25f);
        NEAR(t.hx, 0.25f, 1e-6f); NEAR(t.hy, 4.0f, 1e-5f);
    }
    printf("WORLDOBJ: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
