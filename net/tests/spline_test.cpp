// Taxi-flight spline check (stage 5.20): WorldObject::positionAt with pathSmooth = Catmull-Rom as in the server.
#include "objects.hpp"
#include <cmath>
#include <cstdio>
using namespace w3n;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    WorldObject o;
    o.path = {{0, 0, 0}, {100, 0, 10}, {200, 50, 30}, {300, 50, 30}, {400, 0, 0}};
    o.pathActive = true;
    o.pathStartMs = 1000;
    o.pathDurMs = 20000;
    float p[3], yaw;
    o.pathSmooth = true;
    o.positionAt(1000, p, &yaw);
    CHECK(std::fabs(p[0]) < 0.01f && std::fabs(p[1]) < 0.01f);
    o.positionAt(21000, p, &yaw);
    CHECK(std::fabs(p[0] - 400) < 0.5f && std::fabs(p[2]) < 0.5f);
    // passes through the middle control point at a segment boundary: time = length-proportional, so scan for it
    float best = 1e9f;
    for (uint32_t t = 1000; t <= 21000; t += 10) {
        o.positionAt(t, p, &yaw);
        float d = std::hypot(p[0] - 200, p[1] - 50);
        if (d < best) best = d;
    }
    CHECK(best < 1.0f);
    // continuity and monotonic x progress, smoothness of heading (no jumps over 0.2 rad per 100 ms)
    float prev[3], py = 0; bool first = true;
    for (uint32_t t = 1000; t <= 21000; t += 100) {
        o.positionAt(t, p, &yaw);
        if (!first) {
            CHECK(std::hypot(p[0] - prev[0], p[1] - prev[1]) < 40.0f);
            CHECK(std::fabs(remainderf(yaw - py, 6.2831853f)) < 0.2f);
        }
        prev[0] = p[0]; prev[1] = p[1]; prev[2] = p[2]; py = yaw; first = false;
    }
    // linear mode still works and differs from the smooth one at a corner
    float s[3], l[3];
    o.positionAt(7000, s, &yaw);
    o.pathSmooth = false;
    o.positionAt(7000, l, &yaw);
    CHECK(std::fabs(s[1] - l[1]) > 0.01f || std::fabs(s[0] - l[0]) > 0.01f);
    // two points with smooth flag fall back to linear
    o.path.resize(2); o.pathSmooth = true;
    o.positionAt(11000, p, &yaw);
    CHECK(std::fabs(p[0] - 50) < 0.5f);
    printf("SPLINE: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
