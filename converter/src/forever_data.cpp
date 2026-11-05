// DataDir for the modern client: FileDataID paths and the map files assembled from split ADTs.
#include <cstdlib>
#include <cstring>

#include "conv.h"
#include "modern_adt.h"

namespace w3d {

namespace {
constexpr size_t kGfidStride = 1;
std::string lowerStr(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); for (char& c : s) if (c == '/') c = '\\'; return s; }
uint32_t u32(const std::vector<uint8_t>& d, size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4); return v; }

// MAID: 64x64 entries of 8 FileDataIDs: root, obj0, obj1, tex0, lod, map texture, map normal, minimap
bool maidOf(const std::vector<uint8_t>& wdt, int x, int y, uint32_t ids[8]) {
    for (size_t at = 0; at + 8 <= wdt.size();) {
        const size_t sz = u32(wdt, at + 4);
        if (at + 8 + sz > wdt.size()) break;
        if (!memcmp(&wdt[at], "DIAM", 4)) {
            const size_t o = at + 8 + (size_t)(y * 64 + x) * 32;
            if (o + 32 > wdt.size()) return false;
            for (int i = 0; i < 8; ++i) ids[i] = u32(wdt, o + (size_t)i * 4);
            return ids[0] != 0;
        }
        at += 8 + sz;
    }
    return false;
}
}  // namespace

bool DataDir::readForever(const std::string& gamePath, std::vector<uint8_t>& out, bool& handled) const {
    handled = false;
    const std::string p = lowerStr(gamePath);
    if (const uint32_t id = fdidFromPseudoPath(p)) {
        handled = true;
        const std::string ext = p.size() > 4 ? p.substr(p.size() - 4) : "";
        if (ext == ".m2" || (p.size() > 3 && p.compare(p.size() - 3, 3, ".m2") == 0)) {   // a chunked model -> plain MD20 + texture names
            std::vector<uint8_t> raw; uint32_t skin = 0; std::string err;
            return fdid_(id, raw) && modernM2ToClassic(raw, out, skin, err);
        }
        if (p.size() > 7 && p.compare(p.size() - 7, 7, "00.skin") == 0) {   // "fdid\\<model id>00.skin": LOD 0 skin of that model (its SFID[0])
            std::vector<uint8_t> raw, plain; uint32_t skin = 0; std::string err;
            const uint32_t model = id / 100;
            return fdid_(model, raw) && modernM2ToClassic(raw, plain, skin, err) && skin && fdid_(skin, out);
        }
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".wmo") == 0) {
            std::vector<uint8_t> raw, plain; std::vector<uint32_t> gfid; std::string err;
            const size_t us = p.find('_', 5);
            if (us == std::string::npos) return fdid_(id, raw) && modernWmoRootToClassic(raw, out, gfid, err);   // the root
            // "fdid\\<root id>_<group>.wmo": group g is GFID[g * kGfidStride] (consecutive: confirmed by the probe, ids 7964530, 31, 32, 35 ...)
            const size_t g = (size_t)strtoul(p.c_str() + us + 1, nullptr, 10);
            return fdid_(id, raw) && modernWmoRootToClassic(raw, plain, gfid, err) && g * kGfidStride < gfid.size() && fdid_(gfid[g * kGfidStride], raw) && modernWmoGroupToClassic(raw, out, err);
        }
        return fdid_(id, out);
    }
    if (p.compare(0, 11, "world\\maps\\") != 0) return false;
    const size_t slash = p.find('\\', 11);
    if (slash == std::string::npos) return false;
    const std::string dir = p.substr(11, slash - 11), file = p.substr(slash + 1);
    auto w = wdtIds_.find(dir);
    if (w == wdtIds_.end()) return false;
    if (file == dir + ".wdt") { handled = true; return fdid_(w->second, out); }
    // <dir>_X_Y.adt
    int x = -1, y = -1;
    if (file.size() > dir.size() + 5 && file.compare(0, dir.size(), dir) == 0 && file.compare(file.size() - 4, 4, ".adt") == 0 &&
        sscanf(file.c_str() + dir.size(), "_%d_%d.adt", &x, &y) == 2 && x >= 0 && x < 64 && y >= 0 && y < 64) {
        handled = true;
        std::vector<uint8_t> wdt;
        uint32_t ids[8];
        if (!fdid_(w->second, wdt) || !maidOf(wdt, x, y, ids)) return false;
        std::vector<uint8_t> root, tex, obj, classic;
        std::string err;
        if (!fdid_(ids[0], root)) return false;
        if (ids[3]) fdid_(ids[3], tex);
        if (ids[1]) fdid_(ids[1], obj);
        if (!modernAdtToClassic(root, tex, obj, classic, err)) { std::fprintf(stderr, "%s: %s\n", gamePath.c_str(), err.c_str()); return false; }
        out.swap(classic);
        return true;
    }
    return false;
}

}  // namespace w3d
