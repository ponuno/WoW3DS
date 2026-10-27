// Worn items (stage 5.19): ItemDisplayInfo -> ITEM entries.
//
// A display is everything the client needs to show one worn item on a character:
//   * body layers - the eight texture regions (Item\TextureComponents\<Region>Texture\<name>_M/_F/_U.blp) as CIMG
//     entries placed on the 128x128 body atlas (the 3DS paints them over the skin in slot order, like WoWee
//     compositeWithRegions);
//   * GeosetGroup1..3 - which sleeve / boot / glove / trouser / kilt / cloak variant the model shows;
//   * hideHair - helmets that take the hair off (HelmetGeosetVisData);
//   * cloak texture (Item\ObjectComponents\Cape or Item\TextureComponents\Cape);
//   * models: weapons and shields (Item\ObjectComponents\Weapon / Shield), shoulders (Shoulder, left + right),
//     helmets (Head, one model per race and sex: <name>_HuM.m2 ...), baked as static MODL with the display's texture.
//
// ItemDisplayInfo (vanilla, 23 fields; WoWee classic dbc_layouts): ID 0, LeftModel 1, RightModel 2, LeftModelTexture 3,
// RightModelTexture 4, Icon 5, GeosetGroup1..3 6..8, Flags 9, SpellVisualID 10, GroupSoundID 11, HelmetGeosetVis 12/13,
// textures ArmUpper..Foot 14..21, ItemVisual 22.
#include "conv.h"
#include "w3d_character.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace w3d {

namespace {

struct Rect { int x, y, w, h; };
// Regions on the game's 256x256 body atlas (WoWee compositeWithRegions).
const Rect kRegion[W3D_ITEM_REGIONS] = {{0, 0, 128, 64},    {0, 64, 128, 64},   {0, 128, 128, 32},  {128, 0, 128, 64},
                                       {128, 64, 128, 32}, {128, 96, 128, 64}, {128, 160, 128, 64}, {128, 224, 128, 32}};
const char* const kRegionDir[W3D_ITEM_REGIONS] = {"ArmUpperTexture", "ArmLowerTexture", "HandTexture",       "TorsoUpperTexture",
                                                  "TorsoLowerTexture", "LegUpperTexture", "LegLowerTexture", "FootTexture"};
const uint32_t kColTex[W3D_ITEM_REGIONS] = {14, 15, 16, 17, 18, 19, 20, 21};

// Suffix of the racial models and textures (Helm_Plate_B_01_HuM.m2 ...), races 1..8.
const char* const kRaceSuffix[9][2] = {{"", ""},        {"_HuM", "_HuF"}, {"_OrM", "_OrF"}, {"_DwM", "_DwF"}, {"_NiM", "_NiF"},
                                      {"_ScM", "_ScF"}, {"_TaM", "_TaF"}, {"_GnM", "_GnF"}, {"_TrM", "_TrF"}};

std::string stripExt(std::string n) {
    size_t d = n.rfind('.');
    if (d != std::string::npos) n.resize(d);
    return n;
}

bool exists(const DataDir& data, const std::string& path) {
    std::vector<uint8_t> b;
    return data.read(path, b);
}

// Columns of HelmetGeosetVis in ItemDisplayInfo: the columns whose non-zero values are all ids of HelmetGeosetVisData
// (WoWee findVisColumns); 12 and 13 in the 23-field vanilla table.
void findVisColumns(const DbcTable& idi, const DbcTable& vis, uint32_t& male, uint32_t& female) {
    male = female = 0xFFFFFFFFu;
    std::set<uint32_t> ids;
    for (uint32_t r = 0; r < vis.records; ++r) ids.insert(vis.u32(r, 0));
    if (ids.empty()) return;
    const uint32_t step = std::max(1u, idi.records / 400u);
    for (uint32_t c = 0; c < idi.fields; ++c) {
        uint32_t nonZero = 0, valid = 0;
        for (uint32_t r = 0; r < idi.records; r += step) {
            uint32_t v = idi.u32(r, c);
            if (!v) continue;
            ++nonZero;
            valid += ids.count(v) ? 1 : 0;
        }
        if (nonZero >= 8 && valid == nonZero) {
            if (male == 0xFFFFFFFFu) male = c;
            else { female = c; break; }
        }
    }
}

}  // namespace

bool convertItems(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st) {
    DbcTable idi, vis;
    if (!idi.load(data, "ItemDisplayInfo.dbc") || idi.fields < 22) {
        std::fprintf(stderr, "items: ItemDisplayInfo.dbc not found (or not the vanilla layout), no worn items\n");
        return false;
    }
    const bool haveVis = vis.load(data, "HelmetGeosetVisData.dbc");
    uint32_t visCol[2] = {12, 13};
    if (haveVis) {
        uint32_t m, f;
        findVisColumns(idi, vis, m, f);
        if (m != 0xFFFFFFFFu) { visCol[0] = m; visCol[1] = f != 0xFFFFFFFFu ? f : m; }
    }
    std::map<uint32_t, bool> visHidesHair;   // HelmetGeosetVisData id -> any mask set (WoWee helmHidesHair)
    for (uint32_t r = 0; haveVis && r < vis.records; ++r) {
        bool any = false;
        for (uint32_t f = 1; f < vis.fields; ++f) any |= vis.u32(r, f) != 0;
        visHidesHair[vis.u32(r, 0)] = any;
    }

    const int A = W3D_CHAR_ATLAS;
    auto addLayer = [&](const std::string& path, int region) -> uint32_t {
        const Rect& rg = kRegion[region];
        uint32_t id = pack.keyId(W3D_ENTRY_CHARIMG, DataDir::normalize(path) + "@" + std::to_string(rg.x) + "," + std::to_string(rg.y) + "," +
                                                     std::to_string(rg.w) + "," + std::to_string(rg.h));
        if (pack.has(W3D_ENTRY_CHARIMG, id)) return id;
        std::vector<uint8_t> bytes;
        Image im;
        if (!data.read(path, bytes) || !decodeBlp(bytes, im)) { ++st.itemLayersMissing; return 0; }
        int w = rg.w * A / 256, h = rg.h * A / 256;
        if (im.w != w || im.h != h) im = resizeImage(im, w, h);
        W3dCharImage ci{(uint16_t)w, (uint16_t)h, (uint16_t)(rg.x * A / 256), (uint16_t)(rg.y * A / 256)};
        Blob b;
        b.put(ci);
        std::vector<uint16_t> px((size_t)w * h);
        bool any = false;
        for (size_t i = 0; i < px.size(); ++i) {
            const uint8_t* p = &im.rgba[i * 4];
            auto q = [](uint8_t v) { return (uint16_t)((v * 15 + 127) / 255); };
            px[i] = (uint16_t)((q(p[0]) << 12) | (q(p[1]) << 8) | (q(p[2]) << 4) | q(p[3]));
            any |= q(p[3]) != 0;
        }
        if (!any) return 0;   // fully transparent art: nothing to paint
        b.putArray(px);
        st.itemLayerBytes += b.data.size();
        pack.add(W3D_ENTRY_CHARIMG, id, std::move(b.data));
        ++st.itemLayers;
        return id;
    };
    // <region dir>\<name>_M|_F, then _U, then the bare name (WoWee resolveItemRegionTexture)
    auto regionLayer = [&](int region, const std::string& name, int sex) -> uint32_t {
        const std::string base = std::string("Item\\TextureComponents\\") + kRegionDir[region] + "\\" + name;
        for (const std::string& p : {base + (sex ? "_F.blp" : "_M.blp"), base + "_U.blp", base + ".blp"})
            if (exists(data, p)) return addLayer(p, region);
        return 0;
    };

    auto capeTexture = [&](const std::string& name, int sex) -> uint32_t {
        if (name.empty()) return 0;
        const std::string baseObj = "Item\\ObjectComponents\\Cape\\" + name, baseTex = "Item\\TextureComponents\\Cape\\" + name;
        const char* sfx = sex ? "_F.blp" : "_M.blp";
        for (const std::string& p : {baseObj + ".blp", baseTex + ".blp", baseObj + sfx, baseObj + "_U.blp", baseTex + sfx,
                                     baseTex + "_U.blp"})
            if (exists(data, p)) {
                uint32_t t = addTextureFromGame(pack, data, p, o.itemTex, o.texFormat, true, o.verbose);
                if (t) ++st.texturesOk; else ++st.texturesFailed;
                return t;
            }
        return 0;
    };

    // A model file of an item: Weapon, Shield, Shoulder (plain name) or Head (racial); returns its game path or "".
    auto findModel = [&](const std::string& name, const char* dir) -> std::string {
        std::string p = std::string("Item\\ObjectComponents\\") + dir + "\\" + name + ".m2";
        return exists(data, p) ? p : std::string();
    };
    auto findTexture = [&](const std::string& name, const char* dir, const std::string& suffix = "") -> std::string {
        if (name.empty()) return {};
        std::string base = std::string("Item\\ObjectComponents\\") + dir + "\\" + name;
        if (!suffix.empty() && exists(data, base + suffix + ".blp")) return base + suffix + ".blp";
        return exists(data, base + ".blp") ? base + ".blp" : std::string();
    };

    for (uint32_t r = 0; r < idi.records; ++r) {
        progress(false, "items: display %u/%u, %zu layers, %zu models", r + 1, idi.records, st.itemLayers, st.itemModels);
        const uint32_t displayId = idi.u32(r, 0);
        if (!displayId) continue;
        W3dItemDisplay d{};
        bool any = false;
        for (int k = 0; k < 3; ++k) {
            d.gg[k] = (uint8_t)std::min<uint32_t>(idi.u32(r, 6 + k), 20);
            any |= d.gg[k] != 0;
        }
        for (int reg = 0; reg < W3D_ITEM_REGIONS; ++reg) {
            std::string tex = idi.str(r, kColTex[reg]);
            if (tex.empty()) continue;
            for (int sex = 0; sex < 2; ++sex) {
                d.img[sex][reg] = regionLayer(reg, tex, sex);
                any |= d.img[sex][reg] != 0;
            }
        }
        const std::string modelL = stripExt(idi.str(r, 1)), modelR = stripExt(idi.str(r, 2));
        const std::string texL = idi.str(r, 3), texR = idi.str(r, 4);
        // cloak: its texture name is in either texture column (a woman's first the right one, as WoWee)
        for (int sex = 0; sex < 2; ++sex) {
            const std::string& first = sex ? texR : texL;
            const std::string& second = sex ? texL : texR;
            d.cape[sex] = capeTexture(first, sex);
            if (!d.cape[sex] && second != first) d.cape[sex] = capeTexture(second, sex);
            any |= d.cape[sex] != 0;
        }
        if (d.cape[0] || d.cape[1]) ++st.itemCapes;
        // models
        if (!modelL.empty() || !modelR.empty()) {
            // weapons, shields: the left model, else the right one
            const std::string weaponName = !modelL.empty() ? modelL : modelR;
            const std::string weaponTex = !modelL.empty() ? texL : texR;
            std::string m2 = findModel(weaponName, "Weapon"), tex;
            if (!m2.empty()) tex = findTexture(weaponTex, "Weapon");
            if (m2.empty()) {
                m2 = findModel(weaponName, "Shield");
                if (!m2.empty()) tex = findTexture(weaponTex, "Shield");
            }
            if (!m2.empty()) {
                d.model[0] = addItemModel(pack, data, m2, tex, o, st);
                any |= d.model[0] != 0;
            } else {
                // shoulders: left and right models, never racial in vanilla (the Human male one is the fallback)
                for (int side = 0; side < 2; ++side) {
                    const std::string& name = side ? modelR : modelL;
                    if (name.empty()) continue;
                    std::string sm = findModel(name, "Shoulder");
                    if (sm.empty()) sm = findModel(name + kRaceSuffix[1][0], "Shoulder");
                    if (sm.empty()) continue;
                    std::string shTex = findTexture(side ? texR : texL, "Shoulder");
                    d.model[side] = addItemModel(pack, data, sm, shTex, o, st);
                    any |= d.model[side] != 0;
                }
                // helmets: Head\<name><_HuM>.m2 per race and sex; the texture <tex><_HuM>.blp, else <tex>.blp
                if (!d.model[0] && !d.model[1] && !modelL.empty()) {
                    bool haveAny = false;
                    for (uint32_t race = 1; race <= 8; ++race)
                        for (uint32_t sex = 0; sex < 2; ++sex) {
                            const std::string sfx = kRaceSuffix[race][sex];
                            std::string hm = findModel(modelL + sfx, "Head");
                            if (hm.empty()) hm = findModel(modelL, "Head");
                            if (hm.empty()) continue;
                            std::string ht = findTexture(texL, "Head", sfx);
                            uint32_t id = addItemModel(pack, data, hm, ht, o, st);
                            d.helm[(race - 1) * 2 + sex] = id;
                            haveAny |= id != 0;
                        }
                    if (haveAny) {
                        any = true;
                        ++st.itemHelms;
                        // a helmet that hides the hair: HelmetGeosetVis[male / female] -> HelmetGeosetVisData
                        for (int sex = 0; sex < 2; ++sex) {
                            bool hide = true;   // no table: WoWee's old behaviour
                            if (haveVis) {
                                uint32_t v = idi.u32(r, visCol[sex]);
                                hide = v != 0 && visHidesHair.count(v) && visHidesHair[v];
                            }
                            if (hide) d.hideHair |= (uint8_t)(1 << sex);
                        }
                    }
                }
            }
        }
        if (!any) continue;
        Blob b;
        b.put(d);
        pack.add(W3D_ENTRY_ITEM, displayId, std::move(b.data));
        ++st.itemDisplays;
    }
    progressDone("items: %zu displays, %zu body layers (%.1f MB, %zu art files missing), %zu models (%zu failed), %zu helmets, %zu cloaks",
                 st.itemDisplays, st.itemLayers, st.itemLayerBytes / 1048576.0, st.itemLayersMissing, st.itemModels,
                 st.itemModelsFailed, st.itemHelms, st.itemCapes);
    return true;
}

}  // namespace w3d
