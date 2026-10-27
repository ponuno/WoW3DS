// Transport simulation check (stage 5.21): TransportPath on a synthetic ferry path (stops at both ends, curved legs).
#include "transport.hpp"
#include <cmath>
#include <cstdio>
using namespace w3n;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static TaxiPathNode N(float x, float y, float z, uint32_t flags = 0, uint32_t delay = 0, uint32_t map = 0) {
    TaxiPathNode n; n.map = map; n.x = x; n.y = y; n.z = z; n.flags = flags; n.delay = delay; return n;
}
int main() {
    // pad, A (stop 10 s), curve out, B (stop 10 s), curve back, last point next to A, pad
    std::vector<TaxiPathNode> nodes;
    nodes.push_back(N(-30, 0, 0));
    nodes.push_back(N(0, 0, 0, 2, 10));
    for (int x = 50; x < 700; x += 50) nodes.push_back(N((float)x, 40.0f * std::sin(x / 700.0f * 3.14159f), 0));
    nodes.push_back(N(700, 0, 0, 2, 10));
    for (int x = 650; x > 0; x -= 50) nodes.push_back(N((float)x, -40.0f * std::sin(x / 700.0f * 3.14159f), 0));
    nodes.push_back(N(-5, 0, 0));
    TransportPath p;
    CHECK(p.build(nodes, 14.0f, 1.0f));
    CHECK(p.frames() == nodes.size() - 2);
    CHECK(p.period() > 60000 && p.period() < 200000);
    TransportPose a = p.at(0);
    CHECK(a.valid && !a.moving && std::fabs(a.x) < 0.01f);
    TransportPose b = p.at(5000);
    CHECK(!b.moving && std::fabs(b.x) < 0.01f);   // still waiting at A
    // walk the whole loop at 100 ms steps: bounded speed, B reached and held, smooth heading
    float maxSpeed = 0, maxX = -1e9f;
    int stopsAtB = 0;
    TransportPose prev = p.at(0);
    for (uint32_t t = 100; t < p.period(); t += 100) {
        TransportPose c = p.at(t);
        float v = std::hypot(c.x - prev.x, c.y - prev.y) / 0.1f;
        if (t + 100 < p.period()) maxSpeed = std::fmax(maxSpeed, v);
        maxX = std::fmax(maxX, c.x);
        if (!c.moving && std::fabs(c.x - 700) < 0.01f) ++stopsAtB;
        CHECK(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.o));
        prev = c;
    }
    printf("period %u ms, max speed %.2f, max x %.1f, B stop samples %d\n", p.period(), maxSpeed, maxX, stopsAtB);
    CHECK(maxSpeed <= 14.5f);
    CHECK(maxSpeed >= 13.0f);        // reaches cruising speed
    CHECK(maxX > 699.0f);
    CHECK(stopsAtB >= 90 && stopsAtB <= 110);   // ~10 s at 100 ms
    // no teleports mid-loop: successive samples never jump
    // modulo behaviour
    CHECK(std::fabs(p.at(1234).x - p.at(1234 + p.period()).x) < 1e-3f);
    // coordinate round trip
    TransportPose s = p.at(p.period() / 4);
    float w[3], l[3], o1, o2;
    transportToWorld(s, 3.0f, -2.0f, 1.5f, 0.5f, w, &o1);
    worldToTransport(s, w[0], w[1], w[2], o1, l, &o2);
    CHECK(std::fabs(l[0] - 3.0f) < 1e-3f && std::fabs(l[1] + 2.0f) < 1e-3f && std::fabs(l[2] - 1.5f) < 1e-3f && std::fabs(o2 - 0.5f) < 1e-3f);
    // table parse
    TransportTable tab;
    std::string txt = "# x\n";
    for (size_t i = 0; i < nodes.size(); ++i) {
        char b2[128];
        std::snprintf(b2, sizeof b2, "T\t7\t%zu\t%u\t%.1f\t%.1f\t%.1f\t%u\t%u\n", i, nodes[i].map, nodes[i].x, nodes[i].y, nodes[i].z, nodes[i].flags, nodes[i].delay);
        txt += b2;
    }
    CHECK(tab.parse(txt));
    const TransportPath* tp = tab.path(7, 14.0f, 1.0f);
    CHECK(tp && tp->period() == p.period());
    CHECK(tab.path(8, 14.0f, 1.0f) == nullptr);
    CHECK(tab.path(7, 0.0f, 1.0f) == nullptr);
    printf("TRANSPORT: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
