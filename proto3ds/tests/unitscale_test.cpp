// 6.78: w3d_unit_draw_scale against the cases of the 6.74 perf log (vMaNGOS puts the display's scale into OBJECT_FIELD_SCALE_X).
#include <cmath>
#include <cstdio>
#include "w3d_format.h"
static int fails = 0;
static void check(bool c, const char* w) { std::printf("%s %s\n", c ? "ok  " : "FAIL", w); fails += !c; }
static W3dDisplay disp(float scale, uint16_t milli) { W3dDisplay d{}; d.scale = scale; d.modelScaleMilli = milli; return d; }
static bool near(float a, float b) { return std::fabs(a - b) < 0.005f; }
int main() {
    W3dDisplay a = disp(0.85f, 1000);
    check(near(w3d_unit_draw_scale(0.85f, &a), 0.85f), "display 10973: server 0.85, display 0.85 -> 0.85 (was 0.72)");
    W3dDisplay b = disp(0.10f, 250);
    check(near(w3d_unit_draw_scale(0.40f, &b), 0.10f), "bat 4732: server 0.40, display 0.40 x model 0.25 -> 0.10 (was 0.04)");
    W3dDisplay c = disp(0.75f, 1000);
    check(near(w3d_unit_draw_scale(1.0f, &c), 0.75f), "server sends 1 (display left to the client) -> 0.75");
    W3dDisplay e = disp(0.85f, 0);
    check(near(w3d_unit_draw_scale(0.85f, &e), 0.85f), "older pack, the same number twice -> once");
    check(near(w3d_unit_draw_scale(0.70f, &b), 0.70f * 0.25f), "template scale differs from the display: server x model");
    check(near(w3d_unit_draw_scale(1.0f, nullptr), 1.0f), "no display -> 1");
    std::printf("UNIT SCALE: %d failed -> %s\n", fails, fails ? "FAIL" : "PASS");
    return fails != 0;
}
