// Replays the floor-height and wall expectations of gen_wmo_data.py against the COLL entries
// of a converted pack, using the same w3d_walk_height() / w3d_collide_move() as the 3DS client.
//   coll_test <pack.w3dp> <expect.json> [trajectories.txt]   (terrain of the synthetic map is flat at 80)
// trajectories.txt: every position of the random walks, checked independently by check_walls.py.
#include "w3d_collision.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = (const W3dEntry*)(b.data() + h.indexOffset);
    std::map<uint32_t, const uint8_t*> coll;
    for (uint32_t i = 0; i < h.entryCount; ++i)
        if (e[i].type == W3D_ENTRY_COLLISION) coll[e[i].id] = b.data() + e[i].offset;
    auto collAt = [&](float x, float y) -> const uint8_t* {
        int tx = (int)std::floor(32.0f - y / 533.33333f), ty = (int)std::floor(32.0f - x / 533.33333f);
        auto it = coll.find(w3d_tile_id(tx, ty));
        return it == coll.end() ? nullptr : it->second;
    };
    std::ifstream ej(argv[2]);
    std::string js((std::istreambuf_iterator<char>(ej)), {});
    // tiny extraction of {"x": .., "y": .., ..., "z": ..} objects from the "walk" and "spots" arrays
    auto objects = [&](const std::string& key) {
        std::vector<std::map<std::string, double>> out;
        size_t at = js.find("\"" + key + "\":");
        size_t open = js.find('[', at), close = js.find(']', open);
        std::string arr = js.substr(open, close - open);
        std::regex obj("\\{[^}]*\\}"), kv("\"(\\w+)\": (-?[0-9.eE+-]+)");
        for (auto it = std::sregex_iterator(arr.begin(), arr.end(), obj); it != std::sregex_iterator(); ++it) {
            std::map<std::string, double> m;
            std::string o = it->str();
            for (auto k = std::sregex_iterator(o.begin(), o.end(), kv); k != std::sregex_iterator(); ++k) m[(*k)[1]] = std::stod((*k)[2]);
            out.push_back(m);
        }
        return out;
    };
    int bad = 0, n = 0;
    float z = 80.0f;
    for (auto& q : objects("walk")) {
        int onFloor = 0;
        z = w3d_walk_height(collAt(q["x"], q["y"]), q["x"], q["y"], z, 1.5f, 1, 80.0f, &onFloor);
        bool ok = std::fabs(z - q["z"]) < 0.03;
        if (!ok) std::printf("FAIL walk (%.2f, %.2f): %.3f want %.3f\n", q["x"], q["y"], z, q["z"]);
        bad += !ok; ++n;
    }
    std::printf("walk over the stairs: %d points, top %.2f\n", n, 84.5);
    for (auto& q : objects("spots")) {
        float r = w3d_walk_height(collAt(q["x"], q["y"]), q["x"], q["y"], q["zfeet"], 1.5f, 1, 80.0f, nullptr);
        bool ok = std::fabs(r - q["z"]) < 0.03;
        std::printf("%s spot (%.1f, %.1f) feet %.1f -> %.3f want %.3f\n", ok ? "ok  " : "FAIL", q["x"], q["y"], q["zfeet"], r, q["z"]);
        bad += !ok; ++n;
    }
    std::printf("FLOORS: %s (%d checks, %d failed)\n", bad ? "FAIL" : "PASS", n, bad);

    // ---- walls: same tile gathering as streamerCollideMove (every tile under the move's box)
    const float R = W3D_WALL_RADIUS;
    auto collideMove = [&](float fx, float fy, float zf, float& tx, float& ty) {
        float x0 = std::min(fx, tx) - R, x1 = std::max(fx, tx) + R, y0 = std::min(fy, ty) - R, y1 = std::max(fy, ty) + R;
        const uint8_t* cs[4];
        int nc = 0;
        for (float px : {x0, x1})
            for (float py : {y0, y1}) {
                const uint8_t* c = collAt(px, py);
                if (!c) continue;
                bool dup = false;
                for (int i = 0; i < nc; ++i) dup |= cs[i] == c;
                if (!dup) cs[nc++] = c;
            }
        return w3d_collide_move(cs, nc, fx, fy, zf, &tx, &ty, R);
    };
    int wbad = 0, wn = 0;
    {   // named cases: name is the first string field of each object
        size_t at = js.find("\"wall_cases\":");
        size_t open = js.find('[', at), close = js.find(']', open);
        std::string arr = js.substr(open, close - open);
        std::regex obj("\\{[^}]*\\}"), kv("\"(\\w+)\": (-?[0-9.eE+-]+)"), nm("\"name\": \"([^\"]*)\"");
        for (auto it = std::sregex_iterator(arr.begin(), arr.end(), obj); it != std::sregex_iterator(); ++it) {
            std::map<std::string, double> m;
            std::string o = it->str();
            for (auto k = std::sregex_iterator(o.begin(), o.end(), kv); k != std::sregex_iterator(); ++k) m[(*k)[1]] = std::stod((*k)[2]);
            std::smatch sm;
            std::regex_search(o, sm, nm);
            float x = (float)m["x"], y = (float)m["y"];
            for (int s = 0; s < (int)m["steps"]; ++s) {
                float tx = x + (float)m["dx"], ty = y + (float)m["dy"];
                collideMove(x, y, (float)m["z"], tx, ty);
                x = tx; y = ty;
            }
            double err = std::hypot(x - m["want_x"], y - m["want_y"]);
            bool ok = err < 0.03;
            std::printf("%s wall case %-36s end off by %.4f yd", ok ? "ok  " : "FAIL", sm[1].str().c_str(), err);
            if (!ok) std::printf(" (at %.3f %.3f, want %.3f %.3f)", x, y, m["want_x"], m["want_y"]);
            std::printf("\n");
            wbad += !ok; ++wn;
        }
    }
    {   // random walks -> trajectory file
        FILE* tf = argc > 3 ? std::fopen(argv[3], "w") : nullptr;
        size_t at = js.find("\"random_walks\":");
        size_t pos = js.find('[', at) + 1;
        int walks = 0;
        long moves = 0;
        auto num = [&](size_t& p) { char* e; double v = std::strtod(js.c_str() + p, &e); p = e - js.c_str(); return v; };
        auto field = [&](size_t from, const char* key) { size_t p = js.find(std::string("\"") + key + "\": ", from); p += std::strlen(key) + 4; return num(p); };
        while (true) {
            size_t ob = js.find('{', pos);
            size_t endArr = js.find("\"walker_radius\"", pos);
            if (ob == std::string::npos || ob > endArr) break;
            float x = (float)field(ob, "x"), y = (float)field(ob, "y"), z = (float)field(ob, "z");
            size_t mp = js.find("\"moves\": [", ob) + 10;
            if (tf) std::fprintf(tf, "walk %d %.4f\n%.5f %.5f\n", walks, z, x, y);
            while (true) {
                size_t lb = js.find('[', mp), rb = js.find(']', mp);
                if (rb < lb) { pos = rb + 1; break; }   // end of the moves array
                size_t p = lb + 1;
                double dx = num(p);
                p = js.find(',', p) + 1;
                double dy = num(p);
                mp = js.find(']', p) + 1;
                float tx = x + (float)dx, ty = y + (float)dy;
                collideMove(x, y, z, tx, ty);
                x = tx; y = ty;
                ++moves;
                if (tf) std::fprintf(tf, "%.5f %.5f\n", x, y);
            }
            ++walks;
        }
        if (tf) std::fclose(tf);
        std::printf("random walks: %d walks, %ld moves%s\n", walks, moves, tf ? " written" : "");
        if (walks == 0) ++wbad;
    }
    std::printf("WALLS: %s (%d cases, %d failed)\n", wbad ? "FAIL" : "PASS", wn, wbad);
    return (bad || wbad) ? 1 : 0;
}
