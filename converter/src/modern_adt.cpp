#include "modern_adt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace w3d {
namespace {

struct Ck { std::string id; size_t off, size; };   // id in reading order ("MCNK")

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4); return v; }

std::vector<Ck> chunksOf(const std::vector<uint8_t>& d, size_t from, size_t to) {
    std::vector<Ck> out;
    if (to > d.size()) to = d.size();
    for (size_t at = from; at + 8 <= to;) {
        Ck c;
        c.id = {(char)d[at + 3], (char)d[at + 2], (char)d[at + 1], (char)d[at]};
        c.size = rd32(d, at + 4);
        c.off = at + 8;
        if (c.off + c.size > to) break;
        out.push_back(c);
        at = c.off + c.size;
    }
    return out;
}
void put32(std::vector<uint8_t>& o, uint32_t v) { o.insert(o.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
void putChunk(std::vector<uint8_t>& o, const char* id, const uint8_t* p, size_t n) {
    o.push_back((uint8_t)id[3]); o.push_back((uint8_t)id[2]); o.push_back((uint8_t)id[1]); o.push_back((uint8_t)id[0]);
    put32(o, (uint32_t)n);
    if (n) o.insert(o.end(), p, p + n);
}
void patch32(std::vector<uint8_t>& o, size_t at, uint32_t v) { memcpy(o.data() + at, &v, 4); }

}  // namespace

std::string fdidPseudoPath(uint32_t id, const char* ext) { return "fdid\\" + std::to_string(id) + ext; }
uint32_t fdidFromPseudoPath(const std::string& p) {
    std::string s;
    for (char c : p) s += (char)(c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? c + 32 : c));
    if (s.compare(0, 5, "fdid\\") != 0) return 0;
    return (uint32_t)strtoul(s.c_str() + 5, nullptr, 10);
}

bool modernAdtToClassic(const std::vector<uint8_t>& root, const std::vector<uint8_t>& tex0, const std::vector<uint8_t>& obj0,
                        std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    const std::vector<Ck> rc = chunksOf(root, 0, root.size());
    std::vector<Ck> rootMcnk, texMcnk, objMcnk;
    const Ck* mh2o = nullptr;
    for (const Ck& c : rc) { if (c.id == "MCNK") rootMcnk.push_back(c); else if (c.id == "MH2O") mh2o = &c; }
    if (rootMcnk.size() != 256) { err = "root ADT: " + std::to_string(rootMcnk.size()) + " MCNK chunks (need 256)"; return false; }
    std::vector<uint32_t> texIds;
    for (const Ck& c : chunksOf(tex0, 0, tex0.size())) {
        if (c.id == "MCNK") texMcnk.push_back(c);
        else if (c.id == "MDID") for (size_t i = 0; i + 4 <= c.size; i += 4) texIds.push_back(rd32(tex0, c.off + i));
    }
    std::vector<uint8_t> mddf, modf;
    for (const Ck& c : chunksOf(obj0, 0, obj0.size())) {
        if (c.id == "MCNK") objMcnk.push_back(c);
        else if (c.id == "MDDF") mddf.assign(obj0.begin() + (long)c.off, obj0.begin() + (long)(c.off + c.size));
        else if (c.id == "MODF") modf.assign(obj0.begin() + (long)c.off, obj0.begin() + (long)(c.off + c.size));
    }
    if (!texMcnk.empty() && texMcnk.size() != 256) { err = "tex0 ADT: " + std::to_string(texMcnk.size()) + " MCNK chunks"; return false; }

    // MTEX: one pseudo path per MDID entry (layers index into it)
    std::vector<uint8_t> mtex;
    for (uint32_t id : texIds) { const std::string p = fdidPseudoPath(id, ".blp"); mtex.insert(mtex.end(), p.begin(), p.end()); mtex.push_back(0); }

    // doodads / buildings: nameId is a FileDataID (MDDF flag 0x40 / MODF flag 0x8): list each id once, placements index the list
    std::vector<uint8_t> mmdx, mmid, mwmo, mwid;
    auto remap = [&](std::vector<uint8_t>& pl, size_t stride, uint16_t idFlag, size_t flagAt, std::vector<uint8_t>& names, std::vector<uint8_t>& ofs, const char* ext) {
        std::map<uint32_t, uint32_t> index;
        for (size_t i = 0; i + stride <= pl.size(); i += stride) {
            uint32_t id = rd32(pl, i);
            uint16_t fl = (uint16_t)(pl[i + flagAt] | (pl[i + flagAt + 1] << 8));
            if (!(fl & idFlag)) continue;   // an old-style name index: leave it
            auto it = index.find(id);
            if (it == index.end()) {
                it = index.emplace(id, (uint32_t)index.size()).first;
                put32(ofs, (uint32_t)names.size());
                const std::string p = fdidPseudoPath(id, ext);
                names.insert(names.end(), p.begin(), p.end()); names.push_back(0);
            }
            memcpy(pl.data() + i, &it->second, 4);
        }
    };
    remap(mddf, 36, 0x40, 34, mmdx, mmid, ".m2");
    remap(modf, 64, 0x8, 56, mwmo, mwid, ".wmo");

    put32(out, 0);   // placeholder to keep the vector non-empty while building; replaced below
    out.clear();
    const uint32_t ver = 18;
    putChunk(out, "MVER", (const uint8_t*)&ver, 4);
    uint8_t mhdr[64] = {};
    putChunk(out, "MHDR", mhdr, 64);
    putChunk(out, "MTEX", mtex.data(), mtex.size());
    putChunk(out, "MMDX", mmdx.data(), mmdx.size());
    putChunk(out, "MMID", mmid.data(), mmid.size());
    putChunk(out, "MWMO", mwmo.data(), mwmo.size());
    putChunk(out, "MWID", mwid.data(), mwid.size());
    putChunk(out, "MDDF", mddf.data(), mddf.size());
    putChunk(out, "MODF", modf.data(), modf.size());
    if (mh2o) putChunk(out, "MH2O", root.data() + mh2o->off, mh2o->size);

    for (size_t i = 0; i < 256; ++i) {
        const Ck& rm = rootMcnk[i];
        if (rm.size < 128) { err = "root MCNK " + std::to_string(i) + " too small"; return false; }
        std::vector<uint8_t> hdr(root.begin() + (long)rm.off, root.begin() + (long)(rm.off + 128));
        const uint32_t flags = rd32(hdr, 0);
        // high resolution holes (flag 0x10000): 8x8 bits at 0x14 -> the 4x4 mask the pipeline reads at 0x3C
        if (flags & 0x10000) {
            uint64_t hi; memcpy(&hi, hdr.data() + 0x14, 8);
            uint16_t lo = 0;
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
                if ((hi >> (y * 8 + x)) & 1) lo |= (uint16_t)(1u << ((y / 2) * 4 + x / 2));
            memcpy(hdr.data() + 0x3C, &lo, 2);
            memset(hdr.data() + 0x14, 0, 8);
        }
        std::vector<uint8_t> body;
        const size_t base = 128;
        uint32_t ofsH = 0, ofsN = 0, ofsL = 0, ofsA = 0, sizeA = 0, nLayers = 0;
        for (const Ck& c : chunksOf(root, rm.off + 128, rm.off + rm.size)) {
            if (c.id == "MCVT") { ofsH = (uint32_t)(base + body.size()); putChunk(body, "MCVT", root.data() + c.off, c.size); }
            else if (c.id == "MCNR") { ofsN = (uint32_t)(base + body.size()); putChunk(body, "MCNR", root.data() + c.off, c.size); }
        }
        if (!texMcnk.empty()) {
            for (const Ck& c : chunksOf(tex0, texMcnk[i].off, texMcnk[i].off + texMcnk[i].size)) {
                if (c.id == "MCLY") { ofsL = (uint32_t)(base + body.size()); nLayers = (uint32_t)(c.size / 16); putChunk(body, "MCLY", tex0.data() + c.off, c.size); }
                else if (c.id == "MCAL") { ofsA = (uint32_t)(base + body.size()); sizeA = (uint32_t)(c.size + 8); putChunk(body, "MCAL", tex0.data() + c.off, c.size); }
            }
        }
        memcpy(hdr.data() + 12, &nLayers, 4);
        memcpy(hdr.data() + 20, &ofsH, 4); memcpy(hdr.data() + 24, &ofsN, 4); memcpy(hdr.data() + 28, &ofsL, 4);
        memcpy(hdr.data() + 36, &ofsA, 4); memcpy(hdr.data() + 40, &sizeA, 4);
        std::vector<uint8_t> mcnk = hdr;
        mcnk.insert(mcnk.end(), body.begin(), body.end());
        putChunk(out, "MCNK", mcnk.data(), mcnk.size());
    }
    (void)patch32; (void)objMcnk;
    return true;
}

bool modernM2ToClassic(const std::vector<uint8_t>& f, std::vector<uint8_t>& out, uint32_t& skinId, std::string& err) {
    out.clear(); skinId = 0;
    if (f.size() >= 4 && !memcmp(f.data(), "MD20", 4)) { out = f; return true; }   // an old style file: as it is
    std::vector<uint32_t> txid;
    bool have = false;
    for (const Ck& c : chunksOf(f, 0, f.size())) {
        // M2 chunk ids are in reading order: chunksOf() reversed them
        const std::string id(c.id.rbegin(), c.id.rend());
        if (id == "MD21") { out.assign(f.begin() + (long)c.off, f.begin() + (long)(c.off + c.size)); have = true; }
        else if (id == "SFID" && c.size >= 4) skinId = rd32(f, c.off);
        else if (id == "TXID") for (size_t i = 0; i + 4 <= c.size; i += 4) txid.push_back(rd32(f, c.off + i));
    }
    if (!have || out.size() < 0x58 || memcmp(out.data(), "MD20", 4) != 0) { err = "no MD21 chunk"; return false; }
    const uint32_t nTex = rd32(out, 80), ofsTex = rd32(out, 84);
    if ((size_t)ofsTex + (size_t)nTex * 16 > out.size()) { err = "texture table past the end"; return false; }
    for (uint32_t t = 0; t < nTex && t < txid.size(); ++t) {
        if (!txid[t]) continue;
        const std::string p = fdidPseudoPath(txid[t], ".blp");
        const uint32_t at = (uint32_t)out.size();
        out.insert(out.end(), p.begin(), p.end()); out.push_back(0);
        const uint32_t len = (uint32_t)p.size() + 1;
        memcpy(out.data() + ofsTex + t * 16 + 8, &len, 4);
        memcpy(out.data() + ofsTex + t * 16 + 12, &at, 4);
        const uint32_t zero = 0;   // type 0 = a file texture
        memcpy(out.data() + ofsTex + t * 16, &zero, 4);
    }
    return true;
}

bool modernWmoRootToClassic(const std::vector<uint8_t>& f, std::vector<uint8_t>& out, std::vector<uint32_t>& gfid, std::string& err) {
    out.clear(); gfid.clear();
    std::vector<Ck> cs = chunksOf(f, 0, f.size());
    const Ck *momt = nullptr, *modi = nullptr, *modd = nullptr;
    bool hasMotx = false, hasModn = false;
    for (const Ck& c : cs) {
        if (c.id == "MOMT") momt = &c; else if (c.id == "MODI") modi = &c; else if (c.id == "MODD") modd = &c;
        else if (c.id == "MOTX") hasMotx = true; else if (c.id == "MODN") hasModn = true;
        else if (c.id == "GFID") for (size_t i = 0; i + 4 <= c.size; i += 4) gfid.push_back(rd32(f, c.off + i));
    }
    if (!momt) { err = "no MOMT"; return false; }
    if (hasMotx && hasModn) { out = f; return true; }   // an old style root
    // texture names: every distinct FileDataID in MOMT (offsets 12, 24, 40 of each 64 byte material)
    std::vector<uint8_t> motx, momtNew(f.begin() + (long)momt->off, f.begin() + (long)(momt->off + momt->size));
    std::map<uint32_t, uint32_t> ofs;
    motx.push_back(0);   // offset 0 = "" (an unused slot)
    for (size_t m = 0; m + 64 <= momtNew.size(); m += 64)
        for (size_t at : {m + 12, m + 24, m + 40}) {
            const uint32_t id = rd32(momtNew, at);
            if (!id) continue;
            auto it = ofs.find(id);
            if (it == ofs.end()) {
                it = ofs.emplace(id, (uint32_t)motx.size()).first;
                const std::string p = fdidPseudoPath(id, ".blp");
                motx.insert(motx.end(), p.begin(), p.end()); motx.push_back(0);
            }
            memcpy(momtNew.data() + at, &it->second, 4);
        }
    // doodad model names: MODI[i] -> MODN offset; MODD's name index (low 24 bits of the first word) is an index into MODI
    std::vector<uint8_t> modn, moddNew;
    std::vector<uint32_t> nameOfs;
    if (modi) for (size_t i = 0; i + 4 <= modi->size; i += 4) {
        nameOfs.push_back((uint32_t)modn.size());
        const std::string p = fdidPseudoPath(rd32(f, modi->off + i), ".m2");
        modn.insert(modn.end(), p.begin(), p.end()); modn.push_back(0);
    }
    if (modd) {
        moddNew.assign(f.begin() + (long)modd->off, f.begin() + (long)(modd->off + modd->size));
        for (size_t e = 0; e + 40 <= moddNew.size(); e += 40) {
            const uint32_t w = rd32(moddNew, e), idx = w & 0xFFFFFF;
            const uint32_t nw = (w & 0xFF000000u) | (idx < nameOfs.size() ? nameOfs[idx] : 0);
            memcpy(moddNew.data() + e, &nw, 4);
        }
    }
    // MOTX goes before MOMT (the loader reads in file order), MODN before MODD
    for (const Ck& c : cs) {
        const size_t headAt = c.off - 8;
        if (c.id == "MOMT") { putChunk(out, "MOTX", motx.data(), motx.size()); putChunk(out, "MOMT", momtNew.data(), momtNew.size()); }
        else if (c.id == "MODD") { putChunk(out, "MODN", modn.data(), modn.size()); putChunk(out, "MODD", moddNew.data(), moddNew.size()); }
        else if (c.id == "MOTX" || c.id == "MODN") continue;
        else out.insert(out.end(), f.begin() + (long)headAt, f.begin() + (long)(c.off + c.size));
    }
    // MOHD: nTextures (offset 4... "nMaterials" is at 0) stays; the doodad name table size is in MODN's own size
    return true;
}

bool modernWmoGroupToClassic(const std::vector<uint8_t>& f, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    const std::vector<Ck> top = chunksOf(f, 0, f.size());
    const Ck* mogp = nullptr;
    for (const Ck& c : top) if (c.id == "MOGP") mogp = &c;
    if (!mogp || mogp->size < 68) { err = "no MOGP"; return false; }
    std::vector<uint8_t> body(f.begin() + (long)mogp->off, f.begin() + (long)(mogp->off + 68));
    bool mopy = false;
    for (const Ck& c : chunksOf(f, mogp->off + 68, mogp->off + mogp->size)) {
        if (c.id == "MPY2") {
            std::vector<uint8_t> m;
            for (size_t i = 0; i + 4 <= c.size; i += 4) { m.push_back(f[c.off + i]); const uint16_t mat = (uint16_t)(f[c.off + i + 2] | (f[c.off + i + 3] << 8)); m.push_back(mat > 255 ? 255 : (uint8_t)mat); }
            putChunk(body, "MOPY", m.data(), m.size());
            mopy = true;
        } else if (c.id == "MOPY") { putChunk(body, "MOPY", f.data() + c.off, c.size); mopy = true; }
        else if (c.id == "MOGX" || c.id == "MOBS") continue;   // modern only
        else putChunk(body, c.id.c_str(), f.data() + c.off, c.size);
    }
    (void)mopy;
    for (const Ck& c : top) if (c.id == "MVER") putChunk(out, "MVER", f.data() + c.off, c.size);
    putChunk(out, "MOGP", body.data(), body.size());
    return true;
}

}  // namespace w3d
