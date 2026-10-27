// Stage 5.27: PackWriter::keyId - two names with the same 32-bit FNV-1a hash get different ids, the same name always the
// same id, and a map writer hands its assets (not its tiles) to the pool.   pool_test
#include "conv.h"

#include <cstdio>
#include <string>
#include <unordered_map>

using namespace w3d;

int main() {
    // find a colliding pair by brute force (birthday search over short paths)
    std::unordered_map<uint32_t, std::string> seen;
    std::string a, b;
    for (uint32_t i = 0; i < 3000000 && a.empty(); ++i) {
        std::string s = "world/generic/test" + std::to_string(i) + ".blp@64/1";
        uint32_t h = w3d_fnv1a(s.c_str());
        auto [it, fresh] = seen.emplace(h, s);
        if (!fresh) { a = it->second; b = s; }
    }
    int bad = 0;
    if (a.empty()) { std::puts("FAIL no collision found"); return 1; }
    std::printf("colliding names: %s / %s -> %08x\n", a.c_str(), b.c_str(), w3d_fnv1a(a.c_str()));
    PackWriter pool, map;
    map.setPool(&pool);
    const uint32_t ia = map.keyId(W3D_ENTRY_TEXTURE, a), ib = pool.keyId(W3D_ENTRY_TEXTURE, b);
    if (ia == ib) { std::puts("FAIL the two names share an id"); ++bad; }
    if (map.keyId(W3D_ENTRY_TEXTURE, a) != ia || pool.keyId(W3D_ENTRY_TEXTURE, b) != ib) { std::puts("FAIL ids not stable"); ++bad; }
    if (pool.keyCollisions() != 1 || map.keyCollisions() != 1) { std::puts("FAIL collision count"); ++bad; }
    if (pool.keyId(W3D_ENTRY_MODEL, b) != w3d_fnv1a(b.c_str())) { std::puts("FAIL other types are separate names"); ++bad; }
    map.add(W3D_ENTRY_TEXTURE, ia, {1, 2, 3});
    map.add(W3D_ENTRY_TERRAIN, 7, {4});
    if (!pool.has(W3D_ENTRY_TEXTURE, ia) || pool.count(W3D_ENTRY_TEXTURE) != 1 || map.count(W3D_ENTRY_TEXTURE) != 0) { std::puts("FAIL asset not in the pool"); ++bad; }
    if (pool.has(W3D_ENTRY_TERRAIN, 7) || !map.has(W3D_ENTRY_TERRAIN, 7) || !map.has(W3D_ENTRY_TEXTURE, ia)) { std::puts("FAIL tile routing"); ++bad; }
    if (map.add(W3D_ENTRY_TEXTURE, ia, {9})) { std::puts("FAIL duplicate accepted"); ++bad; }
    std::printf("POOL IDS: %s\n", bad ? "FAIL" : "PASS");
    return bad;
}
