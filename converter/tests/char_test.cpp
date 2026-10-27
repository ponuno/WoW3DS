// Players (stage 5.5): resolves the looks of gen_creature_data.py against the CHRS/CIMG
// entries of a converted pack with the same w3d_char_resolve / w3d_humanoid_geosets /
// w3d_char_composite as the 3DS client, and compares hair texture, geosets and the
// composited body texture (every pixel, after undoing the PICA tile order) with the
// expectations computed independently in Python.
//   char_test <pack.w3dp> <creature expect.json> [out dir for PNM previews]
#include "w3d_character.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

static std::vector<long> numbers(const std::string& js, size_t from, size_t to) {
    std::vector<long> v;
    const char* p = js.c_str() + from;
    const char* e = js.c_str() + to;
    while (p < e) {
        if ((*p >= '0' && *p <= '9') || *p == '-') { char* q; v.push_back(std::strtol(p, &q, 10)); p = q; }
        else ++p;
    }
    return v;
}
static long field(const std::string& obj, const char* key) {
    size_t k = obj.find(std::string("\"") + key + "\":");
    return k == std::string::npos ? -1 : std::strtol(obj.c_str() + k + std::strlen(key) + 3, nullptr, 10);
}
static std::vector<long> array(const std::string& obj, const char* key) {
    size_t k = obj.find(std::string("\"") + key + "\":");
    size_t a = obj.find('[', k), b = obj.find(']', a);
    return numbers(obj, a + 1, b);
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    const W3dEntry* e = (const W3dEntry*)(b.data() + h.indexOffset);
    std::map<std::pair<uint32_t, uint32_t>, const uint8_t*> ent;
    int charEntries = 0;
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        ent[{e[i].type, e[i].id}] = b.data() + e[i].offset;
        charEntries += e[i].type == W3D_ENTRY_CHARACTER;
    }
    std::ifstream ej(argv[2]);
    std::string js((std::istreambuf_iterator<char>(ej)), {});
    size_t pl = js.find("\"players\":");
    std::string P = js.substr(pl);
    int bad = 0;
    uint32_t race = (uint32_t)field(P, "race"), sex = (uint32_t)field(P, "sex");
    auto it = ent.find({W3D_ENTRY_CHARACTER, w3d_char_id(race, sex)});
    if (it == ent.end()) { std::printf("FAIL no CHRS for race %u sex %u\n", race, sex); return 1; }
    const uint8_t* chrs = it->second;
    const W3dCharHeader* ch = w3d_char(chrs);
    if (charEntries != field(P, "races_ok")) { std::printf("FAIL %d CHRS entries\n", charEntries); ++bad; }
    if (ch->modelId != (uint32_t)field(P, "model")) { std::printf("FAIL model %08x\n", ch->modelId); ++bad; }
    size_t sk = P.find("\"scale\":");
    double scale = std::strtod(P.c_str() + sk + 8, nullptr);
    if (std::abs(ch->scale - scale) > 1e-5) { std::printf("FAIL scale %.3f want %.3f\n", ch->scale, scale); ++bad; }
    // model geosets from the CMDL submeshes
    auto mit = ent.find({W3D_ENTRY_CREATURE, ch->modelId});
    if (mit == ent.end()) { std::printf("FAIL race model not packed\n"); return 1; }
    const W3dCreatureHeader* cm = (const W3dCreatureHeader*)mit->second;
    const W3dSkinSubmesh* sm = (const W3dSkinSubmesh*)(mit->second + cm->offSubmeshes);
    std::vector<uint16_t> all;
    for (uint32_t i = 0; i < cm->submeshCount; ++i) all.push_back(sm[i].geoset);

    static uint8_t rgb[W3D_CHAR_ATLAS * W3D_CHAR_ATLAS * 3];
    static uint16_t tex[W3D_CHAR_ATLAS * W3D_CHAR_ATLAS];
    size_t at = P.find("\"looks\":");
    int n = 0;
    while (true) {
        size_t ob = P.find("{\"skin\"", at);
        if (ob == std::string::npos) break;
        size_t oe = P.find('}', ob);
        std::string o = P.substr(ob, oe - ob + 1);
        at = oe;
        W3dLook L{(uint8_t)race, (uint8_t)sex, (uint8_t)field(o, "skin"), (uint8_t)field(o, "face"), (uint8_t)field(o, "hs"),
                  (uint8_t)field(o, "hc"), (uint8_t)field(o, "fh")};
        W3dCharResolved r;
        w3d_char_resolve(chrs, &L, &r);
        std::vector<std::string> errs;
        if (r.hairTex != (uint32_t)field(o, "hair")) errs.push_back("hair texture");
        if (r.hairTex && !ent.count({W3D_ENTRY_TEXTURE, r.hairTex})) errs.push_back("hair texture not packed");
        uint16_t g[W3D_DISPLAY_GEOSETS];
        int gn = w3d_humanoid_geosets(all.data(), (int)all.size(), race, r.scalp, r.f100, r.f200, r.f300, nullptr, g, W3D_DISPLAY_GEOSETS);
        std::vector<long> wantG = array(o, "geosets");
        if (std::vector<long>(g, g + gn) != wantG) errs.push_back("geosets");
        const uint8_t* layers[7];
        int nl = 0;
        for (int k = 0; k < 7; ++k) {
            layers[k] = nullptr;
            if (!r.layers[k]) continue;
            auto li = ent.find({W3D_ENTRY_CHARIMG, r.layers[k]});
            if (li == ent.end()) { errs.push_back("layer not packed"); continue; }
            layers[k] = li->second;
            ++nl;
        }
        if (nl != field(o, "layers")) errs.push_back("layer count " + std::to_string(nl));
        w3d_char_composite(layers, 7, rgb, tex);
        std::vector<long> want = array(o, "image");
        int diff = 0;
        for (int y = 0; y < W3D_CHAR_ATLAS; ++y)
            for (int x = 0; x < W3D_CHAR_ATLAS; ++x) {
                uint32_t tile = (uint32_t)(y >> 3) * (W3D_CHAR_ATLAS >> 3) + (uint32_t)(x >> 3);
                uint16_t v = tex[tile * 64 + w3d_morton8((uint32_t)x & 7, (uint32_t)y & 7)];
                diff += v != (uint16_t)want[(size_t)y * W3D_CHAR_ATLAS + x];
            }
        if (diff) errs.push_back(std::to_string(diff) + " texels differ");
        if (argc > 3) {   // PPM preview of the composite
            char name[512];
            std::snprintf(name, sizeof name, "%s/player_look%d.ppm", argv[3], n);
            FILE* pf = std::fopen(name, "wb");
            if (pf) {
                std::fprintf(pf, "P6 %d %d 255\n", W3D_CHAR_ATLAS, W3D_CHAR_ATLAS);
                std::fwrite(rgb, 1, sizeof rgb, pf);
                std::fclose(pf);
            }
        }
        std::printf("%s look %d (skin %u face %u hair %u/%u beard %u): %d layers, %d geosets, scalp %u%s",
                    errs.empty() ? "ok  " : "FAIL", n, L.skin, L.face, L.hairStyle, L.hairColor, L.facialHair, nl, gn,
                    r.scalp, r.exactFace ? ", exact face" : "");
        for (auto& s : errs) std::printf("; %s", s.c_str());
        std::printf("\n");
        bad += !errs.empty();
        ++n;
    }
    if (n == 0) ++bad;
    // ---- worn armour: ITEM entries -> w3d_equip_resolve -> geosets + composite, like the client
    size_t eqAt = js.find("\"equips\":");
    int ne = 0;
    while (eqAt != std::string::npos) {
        size_t ob = js.find("{\"look\"", eqAt);
        if (ob == std::string::npos) break;
        size_t oe = js.find("\"image\"", ob);
        oe = js.find(']', oe);
        std::string o = js.substr(ob, oe - ob + 1);
        eqAt = oe;
        std::vector<long> slots = array(o, "slots"), ids = array(o, "ids");
        // the look of the case: index into the looks above
        size_t lk = P.find("{\"skin\"");
        std::string lo = P.substr(lk, P.find('}', lk) - lk + 1);
        W3dLook L{(uint8_t)race, (uint8_t)sex, (uint8_t)field(lo, "skin"), (uint8_t)field(lo, "face"), (uint8_t)field(lo, "hs"),
                  (uint8_t)field(lo, "hc"), (uint8_t)field(lo, "fh")};
        W3dCharResolved r;
        w3d_char_resolve(chrs, &L, &r);
        const W3dItemDisplay* worn[W3D_EQ_SLOTS] = {};
        std::vector<std::string> errs;
        for (size_t k = 0; k < slots.size(); ++k) {
            auto ii = ent.find({W3D_ENTRY_ITEM, (uint32_t)ids[k]});
            if (ii == ent.end()) { errs.push_back("ITEM " + std::to_string(ids[k]) + " not packed"); continue; }
            worn[slots[k]] = (const W3dItemDisplay*)ii->second;
        }
        W3dEquipResolved er;
        w3d_equip_resolve(worn, L.sex, &er);
        uint16_t scalp = er.hideHair ? 1 : r.scalp;
        uint16_t g[W3D_DISPLAY_GEOSETS];
        int gn = w3d_humanoid_geosets(all.data(), (int)all.size(), race, scalp, r.f100, r.f200, r.f300, &er.geo, g, W3D_DISPLAY_GEOSETS);
        if (std::vector<long>(g, g + gn) != array(o, "geosets")) errs.push_back("geosets");
        if ((long)er.capeTex != field(o, "capetex")) errs.push_back("cape texture " + std::to_string(er.capeTex));
        if (er.capeTex && !ent.count({W3D_ENTRY_TEXTURE, er.capeTex})) errs.push_back("cape texture not packed");
        const uint8_t* layers[7 + W3D_MAX_EQ_LAYERS] = {};
        int nl = 0;
        for (int k = 0; k < 7; ++k, ++nl) {
            auto li = r.layers[k] ? ent.find({W3D_ENTRY_CHARIMG, r.layers[k]}) : ent.end();
            layers[nl] = li == ent.end() ? nullptr : li->second;
        }
        int counted = 0;
        for (int k = 0; k < 7; ++k) counted += layers[k] != nullptr;
        nl = 7;
        for (int k = 0; k < er.layerCount; ++k) {
            auto li = ent.find({W3D_ENTRY_CHARIMG, er.layers[k]});
            if (li == ent.end()) { errs.push_back("item layer not packed"); continue; }
            layers[nl++] = li->second;
            ++counted;
        }
        if (counted != field(o, "layers")) errs.push_back("layer count " + std::to_string(counted));
        w3d_char_composite(layers, nl, rgb, tex);
        std::vector<long> want = array(o, "image");
        int diff = 0;
        for (int y = 0; y < W3D_CHAR_ATLAS; ++y)
            for (int x = 0; x < W3D_CHAR_ATLAS; ++x) {
                uint32_t tile = (uint32_t)(y >> 3) * (W3D_CHAR_ATLAS >> 3) + (uint32_t)(x >> 3);
                diff += tex[tile * 64 + w3d_morton8((uint32_t)x & 7, (uint32_t)y & 7)] != (uint16_t)want[(size_t)y * W3D_CHAR_ATLAS + x];
            }
        if (diff) errs.push_back(std::to_string(diff) + " texels differ");
        // the display key tells different armour apart
        uint32_t eqA[W3D_EQ_SLOTS] = {}, eqB[W3D_EQ_SLOTS] = {};
        for (size_t k = 0; k < slots.size(); ++k) eqA[slots[k]] = (uint32_t)ids[k];
        if (w3d_equip_key(eqA) == 0 || w3d_equip_key(eqA) == w3d_equip_key(eqB)) errs.push_back("equip key");
        std::printf("%s equip case %d (%zu items): %d layers, %d geosets%s", errs.empty() ? "ok  " : "FAIL", ne, slots.size(), counted, gn,
                    er.hideHair ? ", hair hidden" : "");
        for (auto& s2 : errs) std::printf("; %s", s2.c_str());
        std::printf("\n");
        bad += !errs.empty();
        ++ne;
    }
    if (ne != 4) { std::printf("FAIL %d equipment cases\n", ne); ++bad; }
    std::printf("PLAYERS: %s (%d looks)\n", bad ? "FAIL" : "PASS", n);
    return bad ? 1 : 0;
}
