// Creatures: CreatureDisplayInfo -> CDSP entries, their M2 models -> CMDL entries.
//
// DBC columns are the vanilla 1.12 ones from WoWee Data/expansions/classic/dbc_layouts.json:
//   CreatureDisplayInfo: ID 0, ModelID 1, ExtraDisplayId 3, Scale 4, Skin1..3 6..8
//   CreatureModelData:   ID 0, ModelPath 2
//   CreatureDisplayInfoExtra: ID 0, Race 1, Sex 2, Skin 3, Face 4, HairStyle 5, HairColor 6, FacialHair 7,
//                        BakeName 18 (<= 19 fields) or 20
//   CharHairGeosets: Race 1, Sex 2, Variation 3, Geoset 4, Showscalp 5
//   CharacterFacialHairStyles: Race 0, Sex 1, Variation 2, then Geoset100/300/200 at 3/4/5, or at 6/7/8 when the
//                        file has 9+ fields (WoWee detectFacialHairFields)
//   CharSections: Race 1, Sex 2, BaseSection 3, Variation 4, Color 5, Texture1 6
// Humanoid geosets (NPCs here, players on the 3DS): w3d_humanoid_geosets in common/w3d_character.h.
// Players (--players): one CHRS entry per race/sex + CIMG body texture layers, see characters below.
#include "conv.h"
#include "creature_model.h"
#include "w3d_character.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace w3d {

namespace {

// Minimal WDBC reader.
struct Dbc {
    uint32_t records = 0, fields = 0, recSize = 0, strSize = 0;
    std::vector<uint8_t> b;
    bool load(const DataDir& data, const char* name) {
        std::string path = std::string("DBFilesClient\\") + name;
        if (!data.read(path, b) || b.size() < 20 || std::memcmp(b.data(), "WDBC", 4) != 0) return false;
        std::memcpy(&records, &b[4], 4); std::memcpy(&fields, &b[8], 4);
        std::memcpy(&recSize, &b[12], 4); std::memcpy(&strSize, &b[16], 4);
        return 20 + (size_t)records * recSize + strSize <= b.size();
    }
    uint32_t u32(uint32_t r, uint32_t f) const {
        if (r >= records || f >= fields || (f + 1) * 4 > recSize) return 0;
        uint32_t v; std::memcpy(&v, &b[20 + (size_t)r * recSize + f * 4], 4); return v;
    }
    float f32(uint32_t r, uint32_t f) const { uint32_t v = u32(r, f); float x; std::memcpy(&x, &v, 4); return x; }
    std::string str(uint32_t r, uint32_t f) const {
        uint32_t off = u32(r, f);
        size_t base = 20 + (size_t)records * recSize;
        if (off >= strSize) return {};
        const char* s = (const char*)&b[base + off];
        return std::string(s, strnlen(s, strSize - off));
    }
};

std::string dirOf(const std::string& p) {
    size_t k = p.find_last_of("\\/");
    return k == std::string::npos ? std::string() : p.substr(0, k + 1);
}

uint32_t key3(uint32_t a, uint32_t b, uint32_t c) { return (a << 16) | (b << 8) | c; }

struct ModelInfo {
    bool ok = false;
    uint32_t id = 0;
    CreatureBuildInfo info;
    bool deckOk = false;      // stage 5.21: a walkable deck found in the collision mesh (transports)
    TransportDeck deck;
};

// The deck of a ship: the horizontal level with the most flat area (the main deck), and the extent of the triangles on
// that level (model space = transport-local space of the passengers).
// 6.14: from the model's collision mesh (what the game itself walks on); a model without one (or with too little flat area
// in it) falls back to the drawn mesh, where a level of upward-facing triangles is the floor. Static platforms are taken
// from the collision mesh only (a flat drawn thing without collision - a ground glow, a rug in the air - is not solid).
struct FlatTri { float z, area, x0, y0, x1, y1; bool up; };

static bool deckFromTris(const std::vector<FlatTri>& tris, double minArea, TransportDeck& out) {
    // up-facing pieces first (a floor), else either side of a flat piece (winding is not trusted)
    for (int pass = 0; pass < 2; ++pass) {
        std::map<int, double> bins;   // z in 0.25 steps -> area
        for (const FlatTri& t : tris)
            if (pass == 1 || t.up) bins[(int)std::lround(t.z * 4.0f)] += t.area;
        int best = 0;
        double bestArea = 0;
        for (auto& kv : bins) {
            double a = kv.second + (bins.count(kv.first - 1) ? bins[kv.first - 1] : 0) + (bins.count(kv.first + 1) ? bins[kv.first + 1] : 0);
            if (a > bestArea) { bestArea = a; best = kv.first; }
        }
        if (bestArea < minArea) continue;
        double zsum = 0, asum = 0;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const FlatTri& t : tris) {
            if (pass == 0 && !t.up) continue;
            if (std::fabs(t.z - best * 0.25f) > 0.5f) continue;
            zsum += (double)t.z * t.area;
            asum += t.area;
            x0 = std::min(x0, t.x0); y0 = std::min(y0, t.y0); x1 = std::max(x1, t.x1); y1 = std::max(y1, t.y1);
        }
        if (asum <= 0) continue;
        out.z = (float)(zsum / asum);
        out.x0 = x0; out.y0 = y0; out.x1 = x1; out.y1 = y1;
        out.area = (float)asum;
        const double box = (double)(x1 - x0) * (y1 - y0);
        out.fill = box > 1e-6 ? (float)std::min(1.0, asum / box) : 0.0f;
        return true;
    }
    return false;
}

template <class P>
static void flatTri(std::vector<FlatTri>& tris, const P& a, const P& b, const P& c) {
    float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(len > 1e-5f) || std::fabs(nz) / len < 0.9f) return;
    FlatTri t;
    t.z = (a.z + b.z + c.z) / 3.0f;
    t.area = len * 0.5f;
    t.up = nz > 0;
    t.x0 = std::min({a.x, b.x, c.x}); t.x1 = std::max({a.x, b.x, c.x});
    t.y0 = std::min({a.y, b.y, c.y}); t.y1 = std::max({a.y, b.y, c.y});
    tris.push_back(t);
}

bool findDeck(const wowee::pipeline::M2Model& m, TransportDeck& out) {
    out = TransportDeck();
    {
        const auto& V = m.collisionVertices;
        const auto& I = m.collisionIndices;
        std::vector<FlatTri> tris;
        for (size_t k = 0; k + 2 < I.size(); k += 3)
            if (I[k] < V.size() && I[k + 1] < V.size() && I[k + 2] < V.size()) flatTri(tris, V[I[k]], V[I[k + 1]], V[I[k + 2]]);
        if (deckFromTris(tris, 20.0, out)) return true;
    }
    // the drawn mesh (a tram car whose collision is a box or missing)
    {
        const auto& V = m.vertices;
        const auto& I = m.indices;
        std::vector<FlatTri> tris;
        for (size_t k = 0; k + 2 < I.size(); k += 3)
            if (I[k] < V.size() && I[k + 1] < V.size() && I[k + 2] < V.size())
                flatTri(tris, V[I[k]].position, V[I[k + 1]].position, V[I[k + 2]].position);
        if (deckFromTris(tris, 12.0, out)) {
            out.fromMesh = true;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ players (stage 5.5)
// Region of a CharSections layer on the 256x256 character atlas (WoWee character_renderer regionFor).
struct Region { int x, y, w, h; };
const Region kBase = {0, 0, 256, 256}, kFaceUpper = {0, 160, 128, 32}, kFaceLower = {0, 192, 128, 64},
             kPelvis = {128, 96, 128, 64}, kTorso = {128, 0, 128, 64};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Underwear art is placed by name like the game does ("...Pelvis...", "...Torso..."); else by column.
Region underwearRegion(const std::string& path, int column) {
    std::string l = lower(path);
    if (l.find("torso") != std::string::npos) return kTorso;
    if (l.find("pelvis") != std::string::npos) return kPelvis;
    return column == 0 ? kPelvis : kTorso;
}

const char* raceFolder(uint32_t race) {
    static const char* k[] = {"", "Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren", "Gnome", "Troll", "Goblin", "BloodElf"};
    return race >= 1 && race <= 10 ? k[race] : "";
}
// 6.95: the Turtle WoW races (9 Goblin, 10 High Elf): the folder of the model; a High Elf wears the Blood Elf model or, if the
// client has a folder of its own, "HighElf". The first folder whose model exists is used.
const char* const* raceFolderAlternatives(uint32_t race) {
    static const char* const g[] = {"Goblin", nullptr}, * const h[] = {"BloodElf", "HighElf", nullptr};
    static const char* const none[] = {nullptr};
    static const char* const* single[9];
    static const char* one[9][2];
    if (race == 9) return g;
    if (race == 10) return h;
    if (race >= 1 && race <= 8) {
        one[race][0] = raceFolder(race);
        one[race][1] = nullptr;
        single[race] = one[race];
        return single[race];
    }
    return none;
}

}  // namespace

// One CHRS entry per playable race/sex: CharSections rows -> CIMG layers (RGBA4 at half the
// atlas size, placed at their region) and hair TEX entries; CharHairGeosets/FacialHairStyles
// -> style table. The 3DS composites the body texture per player (w3d_char_composite).
// 6.98: WoWee's magenta key (isMagentaKeyCandidate / bleedAndStripMagentaKey)
static void stripMagentaKey(Image& im) {
    const size_t n = (size_t)im.w * im.h;
    if (im.rgba.size() < n * 4) return;
    std::vector<uint8_t> mask(n, 0);
    size_t count = 0;
    for (size_t p = 0; p < n; ++p) {
        const uint8_t* c = &im.rgba[p * 4];
        const int r = c[0], g = c[1], b = c[2], rb = r > b ? r - b : b - r;
        if (r >= 170 && b >= 170 && g <= 120 && r >= g + 70 && b >= g + 70 && rb <= 96) { mask[p] = 1; ++count; }
    }
    if (!count) return;
    const std::vector<uint8_t> src = im.rgba;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const size_t p = (size_t)y * im.w + x;
            if (!mask[p]) continue;
            uint32_t sum[3] = {0, 0, 0}, k = 0;
            for (int rad = 1; rad <= 8 && !k; ++rad)
                for (int dy = -rad; dy <= rad; ++dy)
                    for (int dx = -rad; dx <= rad; ++dx) {
                        if (std::abs(dx) != rad && std::abs(dy) != rad) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= im.w || ny >= im.h) continue;
                        const size_t q = (size_t)ny * im.w + nx;
                        if (mask[q] || src[q * 4 + 3] == 0) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += src[q * 4 + c];
                        ++k;
                    }
            uint8_t* o = &im.rgba[p * 4];
            for (int c = 0; c < 3; ++c) o[c] = k ? (uint8_t)(sum[c] / k) : 0;
            o[3] = 0;
        }
}

template <class ModelFor>
static void convertCharactersImpl(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st,
                                  const Dbc* sections, const Dbc* hair, const Dbc* facial, const Dbc& cdi,
                                  const std::map<uint32_t, std::string>& modelPath, ModelFor& modelFor) {
    const int A = W3D_CHAR_ATLAS;
    // CIMG id: the layer's path hash mixed with its region, so one file in two places stays two entries.
    auto addLayer = [&](const std::string& path, const Region& rg) -> uint32_t {
        if (path.empty()) return 0;
        uint32_t id = pack.keyId(W3D_ENTRY_CHARIMG, DataDir::normalize(path) + "@" + std::to_string(rg.x) + "," + std::to_string(rg.y) + "," +
                                                     std::to_string(rg.w) + "," + std::to_string(rg.h));
        if (pack.has(W3D_ENTRY_CHARIMG, id)) return id;
        std::vector<uint8_t> bytes;
        Image im;
        if (!data.read(path, bytes) || !decodeBlp(bytes, im)) {
            ++st.charImagesFailed;
            if (o.verbose) std::fprintf(stderr, "  character layer missing: %s\n", path.c_str());
            return 0;
        }
        // 6.98 (as WoWee's character renderer): magenta marks "no paint" in some characters' art (custom servers' layers):
        // made transparent, its colour bled in from the neighbours so the filtering shows no pink fringe
        stripMagentaKey(im);
        // a square layer for a region that is not square is a whole-atlas layer (a later client's underwear / face drawn
        // at full size with the rest transparent): placed over the whole atlas, not squeezed into the region
        Region at = rg;
        if (im.w == im.h && rg.w != rg.h) at = kBase;
        int w = at.w * A / 256, h = at.h * A / 256;
        if (im.w != w || im.h != h) im = resizeImage(im, w, h);
        W3dCharImage ci{(uint16_t)w, (uint16_t)h, (uint16_t)(at.x * A / 256), (uint16_t)(at.y * A / 256)};
        Blob b;
        b.put(ci);
        std::vector<uint16_t> px((size_t)w * h);
        for (size_t i = 0; i < px.size(); ++i) {
            const uint8_t* p = &im.rgba[i * 4];
            auto q = [](uint8_t v) { return (uint16_t)((v * 15 + 127) / 255); };
            px[i] = (uint16_t)((q(p[0]) << 12) | (q(p[1]) << 8) | (q(p[2]) << 4) | q(p[3]));
        }
        b.putArray(px);
        st.charImageBytes += b.data.size();
        pack.add(W3D_ENTRY_CHARIMG, id, std::move(b.data));
        ++st.charImages;
        return id;
    };
    for (uint32_t race = 1; race <= 10; ++race)
        for (uint32_t sex = 0; sex <= 1; ++sex) {
            const char* folder = raceFolder(race);
            const char* sx = sex ? "Female" : "Male";
            progress(true, "players: race %u %s (%u/20), %zu layers so far", race, sx, (race - 1) * 2 + sex + 1, st.charImages);
            std::string m2;
            const ModelInfo* miP = nullptr;
            for (const char* const* f = raceFolderAlternatives(race); *f; ++f) {
                std::string cand = std::string("Character\\") + *f + "\\" + sx + "\\" + *f + sx + ".m2";
                const ModelInfo& tried = modelFor(cand);
                if (tried.ok) { folder = *f; m2 = cand; miP = &tried; break; }
            }
            if (!miP) {   // races 9 / 10 exist only in a Turtle WoW client: their absence is not a failure of a vanilla one
                if (race <= 8) ++st.charRacesFailed;
                else if (o.verbose) std::printf("  player race %u %s: no model in this client (not a Turtle WoW client?)\n", race, sx);
                continue;
            }
            const ModelInfo& mi = *miP;
            W3dCharHeader hdr{};
            hdr.race = race;
            hdr.sex = sex;
            hdr.modelId = mi.id;
            hdr.scale = 1.0f;
            // Scale of the race's own display: a CreatureDisplayInfo row on this model without extra info.
            std::string want = DataDir::normalize(m2);
            for (uint32_t r = 0; r < cdi.records; ++r) {
                auto mp = modelPath.find(cdi.u32(r, 1));
                if (mp == modelPath.end() || cdi.u32(r, 3) != 0) continue;
                if (DataDir::normalize(wowee::pipeline::modelPathToM2(mp->second)) != want) continue;
                if (cdi.f32(r, 4) > 0) { hdr.scale = cdi.f32(r, 4); break; }
            }
            std::vector<W3dCharSection> secs;
            if (sections)
                for (uint32_t r = 0; r < sections->records; ++r) {
                    if (sections->u32(r, 1) != race || sections->u32(r, 2) != sex) continue;
                    uint32_t kind = sections->u32(r, 3);
                    if (kind > W3D_CSEC_UNDERWEAR) continue;
                    W3dCharSection c{};
                    c.kind = (uint8_t)kind;
                    c.variation = (uint8_t)sections->u32(r, 4);
                    c.color = (uint8_t)sections->u32(r, 5);
                    std::string t1 = sections->str(r, 6), t2 = sections->str(r, 7);
                    switch (kind) {
                        case W3D_CSEC_SKIN: c.img[0] = addLayer(t1, kBase); break;
                        case W3D_CSEC_FACE:
                        case W3D_CSEC_FACIAL_HAIR:
                            c.img[0] = addLayer(t1, kFaceLower);
                            c.img[1] = addLayer(t2, kFaceUpper);
                            break;
                        case W3D_CSEC_UNDERWEAR:
                            c.img[0] = addLayer(t1, underwearRegion(t1, 0));
                            c.img[1] = addLayer(t2, underwearRegion(t2, 1));
                            break;
                        case W3D_CSEC_HAIR:
                            if (!t1.empty()) {
                                c.img[0] = addTextureFromGame(pack, data, t1, o.creatureTex, o.texFormat,
                                                              mi.info.needsAlpha[W3D_CTEX_HAIR], o.verbose);
                                if (c.img[0]) ++st.texturesOk; else ++st.texturesFailed;
                            }
                            break;
                    }
                    if (!c.img[0] && !c.img[1]) continue;   // empty row (e.g. "no facial hair" or missing art)
                    secs.push_back(c);
                }
            std::vector<W3dCharStyle> styles;
            if (hair)
                for (uint32_t r = 0; r < hair->records; ++r) {
                    if (hair->u32(r, 1) != race || hair->u32(r, 2) != sex) continue;
                    bool scalp = hair->fields > 5 && hair->u32(r, 5) != 0;
                    styles.push_back({0, (uint8_t)hair->u32(r, 3), {(uint16_t)(scalp ? 1 : hair->u32(r, 4)), 0, 0}});
                }
            if (facial) {
                const uint32_t g100 = facial->fields >= 9 ? 6 : 3;
                for (uint32_t r = 0; r < facial->records; ++r) {
                    if (facial->u32(r, 0) != race || facial->u32(r, 1) != sex) continue;
                    styles.push_back({1, (uint8_t)facial->u32(r, 2),
                                      {(uint16_t)facial->u32(r, g100), (uint16_t)facial->u32(r, g100 + 2),
                                       (uint16_t)facial->u32(r, g100 + 1)}});
                }
            }
            hdr.sectionCount = (uint32_t)secs.size();
            hdr.styleCount = (uint32_t)styles.size();
            Blob b;
            b.put(hdr);
            b.align(4);
            size_t offS = b.putArray(secs);
            b.align(4);
            size_t offT = b.putArray(styles);
            auto* hp = b.at<W3dCharHeader>(0);
            hp->offSections = (uint32_t)offS;
            hp->offStyles = (uint32_t)offT;
            pack.add(W3D_ENTRY_CHARACTER, w3d_char_id(race, sex), std::move(b.data));
            ++st.charRaces;
            st.charSections += secs.size();
            if (o.verbose)
                std::printf("  player %s %s: %zu sections, %zu styles, scale %.2f\n", folder, sx, secs.size(), styles.size(),
                            hdr.scale);
        }
}

template <class F>
static void convertCharactersDone(ConvStats& st) {
    progressDone("players: %zu race models, %zu layers (%.1f MB)", st.charRaces, st.charImages, st.charImageBytes / 1048576.0);
}

template <class F>
static void convertCharacters(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st,
                              const Dbc* sections, const Dbc* hair, const Dbc* facial, const Dbc& cdi,
                              const std::map<uint32_t, std::string>& modelPath, F& modelFor) {
    convertCharactersImpl(o, data, pack, st, sections, hair, facial, cdi, modelPath, modelFor);
    convertCharactersDone<F>(st);
}

std::map<uint32_t, TransportDeck>& transportDecks() {
    static std::map<uint32_t, TransportDeck> d;
    return d;
}
std::map<uint32_t, TransportDeck>& staticDecks() {
    static std::map<uint32_t, TransportDeck> d;
    return d;
}
std::vector<std::string>& transportsWithoutDeck() {
    static std::vector<std::string> v;
    return v;
}

// Stage 5.21: GameObjectDisplayInfo rows whose model looks like a ship or zeppelin -> CMDL + a CDSP entry under
// 0x80000000 | display id (the GAMEOBJECT_DISPLAYID of the transport), and the walkable deck for boarding.
// GameObjectDisplayInfo (1.12): ID 0, ModelName 1.
template <class F>
static void convertTransports(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st, F& modelFor) {
    Dbc gdi;
    if (!gdi.load(data, "GameObjectDisplayInfo.dbc")) {
        std::fprintf(stderr, "transports: GameObjectDisplayInfo.dbc not found\n");
        return;
    }
    std::vector<std::string> words;
    {
        std::stringstream ss(o.transportFilter);
        std::string tok;
        while (std::getline(ss, tok, ',')) if (!tok.empty()) words.push_back(lower(tok));
    }
    std::vector<std::string> goWords;
    {
        std::stringstream ss(o.goFilter);
        std::string tok;
        while (std::getline(ss, tok, ',')) if (!tok.empty()) goWords.push_back(lower(tok));
    }
    auto stored = [&]() -> uint64_t { return pack.storedBytes() + (pack.pool() ? pack.pool()->storedBytes() : 0); };
    std::vector<std::string> defWords;   // stage 5.41: the compact default set (only with the creatures)
    if (o.creatures && o.defaultObjects) {
        std::stringstream ss(o.defaultObjectWords);
        std::string tok;
        while (std::getline(ss, tok, ',')) if (!tok.empty()) defWords.push_back(lower(tok));
    }
    for (uint32_t r = 0; r < gdi.records; ++r) {
        std::string mdx = gdi.str(r, 1);
        if (mdx.empty()) continue;
        std::string lm = lower(mdx);
        bool isT = false, isG = o.gameobjects && goWords.empty(), isDef = false;
        // the fishing bobber (stage 5.26) always comes with the creatures: it is small and every angler needs it
        if (o.creatures && lm.find("fishingbobber") != std::string::npos) isG = true;
        if (o.transports) for (auto& w : words) isT = isT || lm.find(w) != std::string::npos;
        if (o.gameobjects) for (auto& w : goWords) isG = isG || lm.find(w) != std::string::npos;
        for (auto& w : defWords) isDef = isDef || lm.find(w) != std::string::npos;
        if (o.creatures && o.defaultObjects && defWords.empty()) isDef = true;   // stage 6.12: every object model by default
        const bool hit = isT || isG || isDef;
        if (!hit) continue;
        const uint64_t bytesBefore = stored();
        progress(false, "transports: display %u/%u, %zu ok", r + 1, gdi.records, st.transportDisplays);
        if (lm.size() > 4 && lm.compare(lm.size() - 4, 4, ".wmo") == 0) {
            // 6.59: a building as a game object (Turtle WoW's new houses, towers, ruins): its groups + furniture as parts and its
            // collision, in an OWMO entry; no CDSP (the creature path cannot draw a WMO)
            std::vector<W3dPlacement> parts;
            std::vector<float> coll;
            auto doodad = [&](const std::string& m2) { return addModelFromGame(pack, data, m2, o, st); };
            bool tooBig = false;
            if (o.noWmo || !wmoObjectParts(pack, data, mdx, o, st, doodad, parts, coll, &tooBig)) {
                ++st.objWmoFailed;
                if (tooBig) ++st.objWmoTooBig;
                if (o.verbose)
                    std::fprintf(stderr, tooBig ? "  object WMO too big for an object (a city / dungeon, skipped): %s\n"
                                                : "  object WMO failed: %s\n", mdx.c_str());
                continue;
            }
            W3dObjWmoHeader h{};
            h.partCount = (uint32_t)parts.size();
            h.collTriCount = (uint32_t)(coll.size() / 9);
            float r2 = 0;
            for (int k = 0; k < 3; ++k) { h.bmin[k] = 1e9f; h.bmax[k] = -1e9f; }
            for (size_t i = 0; i + 2 < coll.size(); i += 3) {
                for (int k = 0; k < 3; ++k) { h.bmin[k] = std::min(h.bmin[k], coll[i + k]); h.bmax[k] = std::max(h.bmax[k], coll[i + k]); }
                r2 = std::max(r2, coll[i] * coll[i] + coll[i + 1] * coll[i + 1] + coll[i + 2] * coll[i + 2]);
            }
            if (coll.empty()) for (int k = 0; k < 3; ++k) h.bmin[k] = h.bmax[k] = 0;
            float rad = std::sqrt(r2);
            for (const W3dPlacement& p : parts)   // (a part's own size is not known here: its centre plus a margin)
                rad = std::max(rad, std::sqrt(p.pos[0] * p.pos[0] + p.pos[1] * p.pos[1] + p.pos[2] * p.pos[2]) + 10.0f);
            h.radius = rad;
            Blob b;
            b.put(h);
            for (const W3dPlacement& p : parts) b.put(p);
            for (float f : coll) b.put(f);
            if (!pack.add(W3D_ENTRY_OBJWMO, 0x80000000u | gdi.u32(r, 0), std::move(b.data))) continue;
            ++st.objWmoDisplays;
            st.objWmoParts += parts.size();
            st.objWmoCollTris += coll.size() / 9;
            if (isDef) { ++st.defaultObjectDisplays; st.defaultObjectBytes += (size_t)(stored() - bytesBefore); }
            if (o.verbose) std::printf("  object WMO display %u: %s, %zu parts, %zu collision triangles, radius %.0f yd\n", gdi.u32(r, 0),
                                       mdx.c_str(), parts.size(), coll.size() / 9, rad);
            continue;
        }
        const ModelInfo& mi = modelFor(mdx);
        if (!mi.ok) { ++st.transportDisplaysFailed; continue; }
        W3dDisplay d{};
        d.modelId = mi.id;
        d.scale = 1.0f;
        Blob b;
        b.put(d);
        const uint32_t id = 0x80000000u | gdi.u32(r, 0);
        if (!pack.add(W3D_ENTRY_DISPLAY, id, std::move(b.data))) continue;
        ++st.transportDisplays;
        if (isDef && !isT) {   // what the default set costs (6.12: also when --gameobjects asked for the same model): the display, its model and the textures it was the first to need
            ++st.defaultObjectDisplays;
            st.defaultObjectBytes += (size_t)(stored() - bytesBefore);
        }
        if (mi.deckOk && isT) {
            transportDecks()[gdi.u32(r, 0)] = mi.deck;
            ++st.transportDecks;
            if (mi.deck.fromMesh) ++st.transportDecksFromMesh;
        } else if (isT) {
            transportsWithoutDeck().push_back(mdx);
        } else if (mi.deckOk && !mi.deck.fromMesh && mi.deck.area >= 12.0f && mi.deck.fill >= 0.5f) {
            // 6.14: a big flat floor in the model's own collision mesh (a bridge, a dock, a raft, a stage): the client lets the
            // objects the server places carry a walker. A transport whose name the filter does not know is covered the same way.
            staticDecks()[gdi.u32(r, 0)] = mi.deck;
            ++st.staticDecks;
        }
        if (o.verbose)
            std::printf("  transport display %u: %s%s\n", gdi.u32(r, 0), mdx.c_str(), mi.deckOk ? " (deck)" : " (no deck)");
    }
    progressDone("transports: %zu displays (%zu failed), %zu with a deck (%zu from the drawn mesh), %zu platforms among the objects",
                 st.transportDisplays, st.transportDisplaysFailed, st.transportDecks, st.transportDecksFromMesh, st.staticDecks);
    if (st.objWmoDisplays || st.objWmoFailed)
        std::printf("  object WMOs (6.59): %zu game object displays are buildings (%zu parts, %zu collision triangles), %zu failed "
                    "(%zu too big: cities / dungeons)\n",
                    st.objWmoDisplays, st.objWmoParts, st.objWmoCollTris, st.objWmoFailed, st.objWmoTooBig);
    for (const std::string& n : transportsWithoutDeck())
        std::printf("  transport model WITHOUT a deck (nobody can stand on it): %s\n", n.c_str());
    if (o.creatures && o.defaultObjects)
        std::printf("  default game objects: %zu displays added (%.1f MB; %s)\n", st.defaultObjectDisplays,
                    st.defaultObjectBytes / 1048576.0, defWords.empty() ? "all of them (6.12)" : o.defaultObjectWords.c_str());
}

bool convertCreatures(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st) {
    Dbc cdi, cmd, cdie, hair, facial, sections;
    if (!cdi.load(data, "CreatureDisplayInfo.dbc") || !cmd.load(data, "CreatureModelData.dbc")) {
        std::fprintf(stderr, "creatures: CreatureDisplayInfo.dbc / CreatureModelData.dbc not found\n");
        return false;
    }
    bool haveExtra = cdie.load(data, "CreatureDisplayInfoExtra.dbc");
    bool haveHair = hair.load(data, "CharHairGeosets.dbc");
    bool haveFacial = facial.load(data, "CharacterFacialHairStyles.dbc");
    bool haveSections = sections.load(data, "CharSections.dbc");
    Dbc items;   // ItemDisplayInfo (vanilla): ID 0, GeosetGroup1..3 at 6..8 (WoWee classic dbc_layouts)
    bool haveItems = items.load(data, "ItemDisplayInfo.dbc") && items.fields > 8;
    const uint32_t itemGG1 = 6;
    std::map<uint32_t, uint32_t> itemRow;
    if (haveItems) for (uint32_t r = 0; r < items.records; ++r) itemRow[items.u32(r, 0)] = r;

    std::map<uint32_t, std::string> modelPath;   // CreatureModelData id -> path
    for (uint32_t r = 0; r < cmd.records; ++r) modelPath[cmd.u32(r, 0)] = cmd.str(r, 2);
    // 6.60: the model's own scale (CreatureModelData.ModelScale, column 4): the game draws a creature at the model's scale x the
    // display's (CreatureDisplayInfo.Scale) x the unit's (OBJECT_FIELD_SCALE_X). Only the last two were used, so every creature
    // whose model asks for more than 1 came out small (and those under 1 too big). WoWee's EntitySpawner multiplies all three.
    std::map<uint32_t, float> modelScale;
    if (cmd.fields > 4)
        for (uint32_t r = 0; r < cmd.records; ++r) {
            const float ms = cmd.f32(r, 4);
            if (ms > 0.01f && ms < 50.0f && std::fabs(ms - 1.0f) > 1e-3f) modelScale[cmd.u32(r, 0)] = ms;
        }
    std::map<uint32_t, uint32_t> extraRow;        // CreatureDisplayInfoExtra id -> row
    uint32_t bakeField = cdie.fields <= 19 ? 18 : 20;
    if (haveExtra) for (uint32_t r = 0; r < cdie.records; ++r) extraRow[cdie.u32(r, 0)] = r;
    std::map<uint32_t, uint16_t> hairGeoset;      // (race, sex, variation) -> scalp geoset
    if (haveHair)
        for (uint32_t r = 0; r < hair.records; ++r) {
            bool scalp = hair.fields > 5 && hair.u32(r, 5) != 0;
            hairGeoset[key3(hair.u32(r, 1), hair.u32(r, 2), hair.u32(r, 3))] = (uint16_t)(scalp ? 1 : hair.u32(r, 4));
        }
    struct Facial { uint16_t g100, g200, g300; };
    std::map<uint32_t, Facial> facialGeosets;
    const uint32_t fg100 = facial.fields >= 9 ? 6 : 3, fg300 = fg100 + 1, fg200 = fg100 + 2;
    if (haveFacial)
        for (uint32_t r = 0; r < facial.records; ++r)
            facialGeosets[key3(facial.u32(r, 0), facial.u32(r, 1), facial.u32(r, 2))] =
                {(uint16_t)facial.u32(r, fg100), (uint16_t)facial.u32(r, fg200), (uint16_t)facial.u32(r, fg300)};
    std::map<std::pair<uint32_t, uint32_t>, std::string> hairTexture;   // (race<<8|sex, style<<8|color) -> texture
    if (haveSections)
        for (uint32_t r = 0; r < sections.records; ++r)
            if (sections.u32(r, 3) == 3)   // hair
                hairTexture[{sections.u32(r, 1) << 8 | sections.u32(r, 2), sections.u32(r, 4) << 8 | sections.u32(r, 5)}] =
                    sections.str(r, 6);

    std::set<uint32_t> only;
    if (!o.creatureDisplays.empty()) {
        std::stringstream ss(o.creatureDisplays);
        std::string tok;
        while (std::getline(ss, tok, ',')) if (!tok.empty()) only.insert((uint32_t)std::stoul(tok));
    }

    std::map<std::string, ModelInfo> models;   // normalized m2 path -> converted
    auto modelFor = [&](const std::string& mdx) -> const ModelInfo& {
        std::string m2 = wowee::pipeline::modelPathToM2(mdx);
        std::string key = DataDir::normalize(m2);
        auto it = models.find(key);
        if (it != models.end()) return it->second;
        ModelInfo& mi = models[key];
        std::vector<uint8_t> bytes;
        if (!data.read(m2, bytes)) { mi.info.error = "missing"; ++st.creatureModelsFailed; return mi; }
        wowee::pipeline::M2Model m = loadM2(bytes);
        if (m.batches.empty() || m.indices.empty()) {
            std::vector<uint8_t> skin;
            if (data.read(wowee::pipeline::skinPathForM2(m2), skin)) wowee::pipeline::M2Loader::loadSkin(skin, m);
        }
        if (o.transports) mi.deckOk = findDeck(m, mi.deck);
        auto fixedTex = [&](const std::string& f, bool alpha) {
            uint32_t id = addTextureFromGame(pack, data, f, o.creatureTex, o.texFormat, alpha, o.verbose);
            if (id) ++st.texturesOk; else ++st.texturesFailed;
            return id;
        };
        auto tb = std::chrono::steady_clock::now();
        double texBefore = textureTiming().readDecodeSec + textureTiming().encodeSec;
        // character (race) models carry every emote players use: a larger animation budget than creatures
        const bool character = key.rfind("character/", 0) == 0;
        auto blob = buildCreatureBlob(m, character ? o.playerTris : o.creatureTris, fixedTex, mi.info,
                                      (size_t)(character ? o.playerAnimKb : o.creatureAnimKb) * 1024);
        st.creatureSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - tb).count() -
                          (textureTiming().readDecodeSec + textureTiming().encodeSec - texBefore);
        if (blob.empty()) {
            ++st.creatureModelsFailed;
            if (o.verbose) std::fprintf(stderr, "  creature model failed: %s (%s)\n", m2.c_str(), mi.info.error.c_str());
            return mi;
        }
        if (!character && !o.noFx) {   // 6.13: particles (a brazier's fire and smoke, a forge's sparks) behind the CMDL blob
            std::vector<W3dFxEmitter> em;
            std::vector<W3dFxGlow> glows;
            std::vector<uint32_t> fxTex;
            if (m2Emitters(m, pack, data, o, st, fireLikePath(key), 6, em, fxTex, nullptr)) {
                appendFxBlock(blob, em, glows, fxTex);
                ++st.fxModels;
                st.fxEmitters += em.size();
                if (o.verbose) std::printf("    fx %s: %zu emitters (creature / object model)\n", m2.c_str(), em.size());
            }
        }
        mi.id = pack.keyId(W3D_ENTRY_CREATURE, key);
        pack.add(W3D_ENTRY_CREATURE, mi.id, std::move(blob));
        mi.ok = true;
        ++st.creatureModelsOk;
        st.creatureTris += mi.info.tris;
        st.creatureSeqs += mi.info.seqs;
        st.creatureKeyBytes += mi.info.keyBytes;
        st.creaturePalettes += mi.info.palettes;
        st.creatureBones += mi.info.bones;
        st.creatureSrcVerts += mi.info.srcVerts;
        st.creatureLossyVerts += mi.info.lossyVerts;
        if (o.verbose)
            std::printf("  creature %-56s %4zu tris %3zu bones %2zu palettes %zu anims (%zu over budget) %5.1f KB keys\n", m2.c_str(),
                        mi.info.tris, mi.info.bones, mi.info.palettes, mi.info.seqs, mi.info.droppedSeqs, mi.info.keyBytes / 1024.0);
        return mi;
    };

    for (uint32_t r = 0; r < (o.creatures ? cdi.records : 0); ++r) {
        progress(false, "creatures: display %u/%u, %zu models, %zu failed", r + 1, cdi.records, st.creatureModelsOk,
                 st.creatureModelsFailed);
        uint32_t displayId = cdi.u32(r, 0);
        if (!only.empty() && !only.count(displayId)) continue;
        auto mp = modelPath.find(cdi.u32(r, 1));
        if (mp == modelPath.end() || mp->second.empty()) { ++st.displaysFailed; continue; }
        const ModelInfo& mi = modelFor(mp->second);
        if (!mi.ok) { ++st.displaysFailed; continue; }
        W3dDisplay d{};
        d.modelId = mi.id;
        d.scale = cdi.f32(r, 4) > 0 ? cdi.f32(r, 4) : 1.0f;
        d.modelScaleMilli = 1000;
        if (auto ms = modelScale.find(cdi.u32(r, 1)); ms != modelScale.end()) {
            d.scale *= ms->second;
            d.modelScaleMilli = (uint16_t)std::clamp((int)std::lround(ms->second * 1000.0f), 1, 65535);
            ++st.displaysModelScaled;
        }
        std::string dir = dirOf(wowee::pipeline::modelPathToM2(mp->second));
        auto tex = [&](const std::string& path, int kind) -> uint32_t {
            if (path.empty()) return 0;
            uint32_t id = addTextureFromGame(pack, data, path, o.creatureTex, o.texFormat, mi.info.needsAlpha[kind], o.verbose);
            if (id) ++st.texturesOk; else ++st.texturesFailed;
            return id;
        };
        for (int k = 0; k < 3; ++k) {
            std::string skin = cdi.str(r, 6 + k);
            if (!skin.empty()) d.tex[k] = tex(dir + skin + ".blp", W3D_CTEX_SKIN1 + k);
        }
        // Humanoid NPC: baked body texture, hair texture, geosets.
        auto ex = extraRow.find(cdi.u32(r, 3));
        if (cdi.u32(r, 3) && ex != extraRow.end()) {
            uint32_t er = ex->second;
            uint32_t race = cdie.u32(er, 1), sex = cdie.u32(er, 2), style = cdie.u32(er, 5), color = cdie.u32(er, 6),
                     beard = cdie.u32(er, 7);
            std::string bake = cdie.str(er, bakeField);
            if (!bake.empty()) d.tex[3] = tex("Textures\\BakedNpcTextures\\" + bake, W3D_CTEX_BODY);
            auto ht = hairTexture.find({race << 8 | sex, style << 8 | color});
            if (ht != hairTexture.end()) d.tex[4] = tex(ht->second, W3D_CTEX_HAIR);
            // geosets (WoWee normalizeHumanoidClothingGeosets without equipment)
            std::vector<uint16_t> all(mi.info.geosets.begin(), mi.info.geosets.end());
            uint16_t scalp = 1;
            auto hg = hairGeoset.find(key3(race, sex, style));
            if (hg != hairGeoset.end() && hg->second > 0) scalp = hg->second;
            uint16_t f100 = 101, f200 = 201, f300 = 301;
            auto fg = facialGeosets.find(key3(race, sex, beard));
            if (fg != facialGeosets.end()) {
                f100 = (uint16_t)(100 + fg->second.g100);
                f200 = (uint16_t)(200 + fg->second.g200);
                f300 = (uint16_t)(300 + fg->second.g300);
            }
            // NPC equipment (CreatureDisplayInfoExtra item display ids -> ItemDisplayInfo GeosetGroup):
            // [2] shirt [3] chest [4] belt [5] legs [6] feet [7] wrist [8] gloves [9] tabard [10] cape
            W3dEquipGeo eq{};
            const uint32_t equipCount = std::min<uint32_t>(11, bakeField > 8 ? bakeField - 8 : 0);
            auto equip = [&](uint32_t slot) { return slot < equipCount ? cdie.u32(er, 8 + slot) : 0u; };
            auto gg = [&](uint32_t displayId, uint32_t col) -> uint8_t {
                if (!displayId || !haveItems) return 0;
                auto it = itemRow.find(displayId);
                return it == itemRow.end() ? 0 : (uint8_t)std::min<uint32_t>(items.u32(it->second, col), 20);
            };
            uint32_t chest = equip(3) ? equip(3) : equip(2);
            eq.sleeves = gg(chest, itemGG1);
            if (!eq.sleeves) eq.sleeves = gg(equip(7), itemGG1);
            eq.robe = gg(equip(3), itemGG1 + 2);
            eq.legs = gg(equip(5), itemGG1);
            eq.boots = gg(equip(6), itemGG1);
            eq.gloves = gg(equip(8), itemGG1);
            eq.belt = gg(equip(4), itemGG1);
            eq.tabard = equip(9) ? 1 : 0;
            eq.cape = 0;   // capes need their own texture (not baked): drawn without one for now
            if (eq.robe || eq.legs || eq.boots || eq.gloves || eq.sleeves) ++st.npcEquipGeosets;
            d.geosetCount = (uint16_t)w3d_humanoid_geosets(all.data(), (int)all.size(), race, scalp, f100, f200, f300,
                                                           &eq, d.geosets, W3D_DISPLAY_GEOSETS);
        }
        Blob b;
        b.put(d);
        pack.add(W3D_ENTRY_DISPLAY, displayId, std::move(b.data));
        ++st.displaysOk;
    }
    if (o.creatures)
        progressDone("creatures: %zu displays, %zu models (%zu failed)", st.displaysOk, st.creatureModelsOk,
                     st.creatureModelsFailed);
    if (o.players) {
        convertCharacters(o, data, pack, st, haveSections ? &sections : nullptr, haveHair ? &hair : nullptr,
                          haveFacial ? &facial : nullptr, cdi, modelPath, modelFor);
        if (!o.noItems) convertItems(o, data, pack, st);   // stage 5.19: armour, helmets, shoulders, weapons for players
    }
    if (o.transports || o.gameobjects || o.creatures) convertTransports(o, data, pack, st, modelFor);
    return true;
}

}  // namespace w3d
