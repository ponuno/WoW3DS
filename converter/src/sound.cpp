// w3dconv --sound: the sounds of the 3DS client, taken from the user's own client (common/w3d_sound.h).
//   sfx.w3ds         interface and event sounds (SoundEntries.dbc by name -> WAV / MP3 -> PCM16 mono <= 22050 Hz)
//   music.txt, music/ zone music (AreaTable -> ZoneMusic -> SoundEntries, MP3s copied as they are) and ambience
//                    (SoundAmbience, WAV loops -> IMA ADPCM)
//   areas_<map>.bin  the area of every terrain chunk (ADT MCNK headers), so the 3DS knows the zone it is in
// 1.12 layouts: SoundEntries ID, type, Name 2, File[10] 3-12, Freq[10] 13-22, DirectoryBase 23, Volume 24 (float);
// AreaTable ID, map 1, parent 2, ..., ambience 7, ZoneMusic 8; ZoneMusic ID, name, silMin[2] 2-3, silMax[2] 4-5,
// sound[2] 6-7 (day, night); SoundAmbience ID, day, night; Map ID, Directory 1.
#include "conv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <array>
#include <tuple>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
#include "w3d_sound.h"

namespace w3d {

namespace {

// event -> SoundEntries names tried in order (the first that exists wins); --sound-events overrides / adds
const std::vector<std::pair<std::string, std::vector<std::string>>> kDefaultEvents = {
    {"click", {"igMainMenuOptionCheckBoxOn", "igMainMenuOption", "@Sound\\Interface\\iUiInterfaceButtonA.wav"}},
    {"tab", {"igCharacterInfoTab"}},
    {"open", {"igCharacterInfoOpen", "igMainMenuOpen"}},
    {"close", {"igCharacterInfoClose", "igMainMenuClose"}},
    {"bagsopen", {"igBackPackOpen"}},
    {"bagsclose", {"igBackPackClose"}},
    {"spellsopen", {"igSpellBookOpen"}},
    {"spellsclose", {"igSpellBookClose"}},
    {"questsopen", {"igQuestLogOpen"}},
    {"questsclose", {"igQuestLogClose"}},
    {"npcopen", {"igQuestListOpen", "igCharacterInfoOpen"}},
    {"npcclose", {"igQuestListClose", "igCharacterInfoClose"}},
    {"questaccept", {"igQuestListSelect", "igQuestListOpen"}},
    {"questcomplete", {"igQuestListComplete", "@Sound\\Interface\\iQuestComplete.wav"}},
    {"questabandon", {"igQuestLogAbandonQuest"}},
    {"coin", {"LOOTWINDOWCOINSOUND", "LootWindowCoinSound", "*coinsound", "@Sound\\Interface\\LootCoinSmall.wav"}},
    // 6.39: an item landing in the bags / being put on (the names are guesses; the built-in blip fills in when this client has none)
    {"lootitem", {"PickUpCloth_Leather01", "PickUpMetalSmall", "PickUpGems", "@Sound\\Interface\\igLootCreature.wav", "@Sound\\Interface\\PickUp\\PickUpBag.wav"}},
    {"equip", {"PickUpMetalLarge", "PickUpCloth_Leather01", "@Sound\\Interface\\PickUp\\PickUpBag.wav"}},
    {"error", {"@Sound\\Interface\\Error.wav"}},
    {"questupdate", {"@Sound\\Interface\\iQuestUpdate.wav"}},
    // our own spell going off by its school of magic, for spells whose data gave no sound (the files WoWee plays)
    {"spellcast_fire", {"@Sound\\Spells\\Cast\\FireCast.wav"}},
    {"spellcast_frost", {"@Sound\\Spells\\Cast\\IceCast.wav"}},
    {"spellcast_holy", {"@Sound\\Spells\\Cast\\HolyCast.wav"}},
    {"spellcast_nature", {"@Sound\\Spells\\Cast\\NatureCast.wav"}},
    {"spellcast_shadow", {"@Sound\\Spells\\Cast\\ShadowCast.wav"}},
    {"spellcast_arcane", {"@Sound\\Spells\\Arcane_Form_Precast.wav"}},
    {"levelup", {"LevelUp", "LEVELUPSOUND", "@Sound\\Interface\\LevelUp.wav"}},
    // an area explored for the first time (stage 5.45): the exact names are a guess; "*word" takes the first SoundEntries name
    // containing the word (alphabetical), so an odd spelling in this client is still found (the log says which one it took)
    {"discover", {"DiscoverArea", "AreaDiscovered", "ZoneDiscovered", "UI_DiscoverArea", "Discovery", "*discover", "*explor"}},
    {"whisper", {"TellMessage"}},
    {"invite", {"igPlayerInvite"}},
    {"auctionopen", {"AuctionWindowOpen"}},
    {"auctionclose", {"AuctionWindowClose"}},
    {"targethostile", {"igCreatureAggroSelect"}},
    {"targetfriend", {"igCharacterNPCSelect"}},
    {"targetneutral", {"igCreatureNeutralSelect"}},
    {"targetlost", {"INTERFACESOUND_LOSTTARGETUNIT", "igCreatureAggroDeselect"}},
    {"mail", {"igMainMenuOptionCheckBoxOn"}},
};

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

struct Pcm { std::vector<int16_t> s; uint32_t rate = 0; };   // mono

// --- WAV: PCM 8/16, MS ADPCM (2), IMA ADPCM (0x11); any channel count, mixed down to mono
bool decodeWav(const std::vector<uint8_t>& f, Pcm& out) {
    if (f.size() < 12 || std::memcmp(f.data(), "RIFF", 4) || std::memcmp(&f[8], "WAVE", 4)) return false;
    uint16_t fmt = 0, ch = 0, align = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    std::vector<int16_t> msCoef;
    for (size_t p = 12; p + 8 <= f.size();) {
        uint32_t len = rd32(&f[p + 4]);
        size_t body = p + 8, avail = std::min<size_t>(len, f.size() - body);
        if (!std::memcmp(&f[p], "fmt ", 4) && avail >= 16) {
            fmt = rd16(&f[body]); ch = rd16(&f[body + 2]); rate = rd32(&f[body + 4]);
            align = rd16(&f[body + 12]); bits = rd16(&f[body + 14]);
            if (fmt == 2 && avail >= 22) {
                uint16_t n = rd16(&f[body + 20]);
                for (uint16_t k = 0; k < n && body + 22 + k * 4 + 4 <= body + avail; ++k) {
                    msCoef.push_back((int16_t)rd16(&f[body + 22 + k * 4]));
                    msCoef.push_back((int16_t)rd16(&f[body + 24 + k * 4]));
                }
            }
        } else if (!std::memcmp(&f[p], "data", 4)) {
            data = &f[body];
            dataLen = avail;
        }
        p = body + len + (len & 1);
    }
    if (!data || !ch || !rate) return false;
    std::vector<int32_t> mix;   // summed channels
    auto put = [&](size_t i, int v) { if (i >= mix.size()) mix.resize(i + 1, 0); mix[i] += v; };
    if (fmt == 1 || fmt == 0xFFFE) {
        const int bps = bits / 8;
        if (bps != 1 && bps != 2) return false;
        size_t frames = dataLen / (bps * ch);
        mix.assign(frames, 0);
        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < ch; ++c) {
                const uint8_t* s = data + (i * ch + c) * bps;
                mix[i] += bps == 1 ? ((int)s[0] - 128) << 8 : (int16_t)rd16(s);
            }
    } else if (fmt == 0x11 && align) {   // IMA ADPCM
        size_t outBase = 0;
        for (size_t b = 0; b + align <= dataLen; b += align) {
            const uint8_t* blk = data + b;
            int pred[8], idx[8];
            if (ch > 8) return false;
            for (int c = 0; c < ch; ++c) {
                pred[c] = (int16_t)rd16(blk + c * 4);
                idx[c] = std::min(88, (int)blk[c * 4 + 2]);
                put(outBase, pred[c]);
            }
            const uint8_t* q = blk + 4 * ch;
            size_t n = (align - 4 * ch) / (4 * ch);   // groups of 8 samples per channel
            for (size_t g = 0; g < n; ++g)
                for (int c = 0; c < ch; ++c)
                    for (int k = 0; k < 4; ++k) {
                        uint8_t v = q[(g * ch + c) * 4 + k];
                        put(outBase + 1 + g * 8 + k * 2, w3dImaStep(v & 15, &pred[c], &idx[c]));
                        put(outBase + 2 + g * 8 + k * 2, w3dImaStep(v >> 4, &pred[c], &idx[c]));
                    }
            outBase += 1 + n * 8;
        }
    } else if (fmt == 2 && align && msCoef.size() >= 14) {   // MS ADPCM
        static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
        size_t outBase = 0;
        for (size_t b = 0; b + align <= dataLen; b += align) {
            const uint8_t* blk = data + b;
            if (ch > 2 || (size_t)7 * ch > align) return false;
            int c1[2], c2[2], delta[2], s1[2], s2[2];
            for (int c = 0; c < ch; ++c) {
                int pi = std::min<int>(blk[c], (int)msCoef.size() / 2 - 1);
                c1[c] = msCoef[pi * 2]; c2[c] = msCoef[pi * 2 + 1];
                delta[c] = (int16_t)rd16(blk + ch + c * 2);
                s1[c] = (int16_t)rd16(blk + ch * 3 + c * 2);
                s2[c] = (int16_t)rd16(blk + ch * 5 + c * 2);
                put(outBase, s2[c]);
                put(outBase + 1, s1[c]);
            }
            size_t o = outBase + 2;
            int c = 0;
            for (size_t q = 7 * ch; q < align; ++q)
                for (int h = 0; h < 2; ++h) {
                    int nib = h == 0 ? blk[q] >> 4 : blk[q] & 15;
                    int sn = nib >= 8 ? nib - 16 : nib;
                    int pred = (s1[c] * c1[c] + s2[c] * c2[c]) / 256 + sn * delta[c];
                    pred = std::clamp(pred, -32768, 32767);
                    s2[c] = s1[c];
                    s1[c] = pred;
                    delta[c] = std::max(16, adapt[nib] * delta[c] / 256);
                    put(o, pred);
                    if (++c == ch) { c = 0; ++o; }
                }
            outBase = o;
        }
    } else {
        return false;
    }
    out.rate = rate;
    out.s.resize(mix.size());
    for (size_t i = 0; i < mix.size(); ++i) out.s[i] = (int16_t)std::clamp(mix[i] / (int)ch, -32768, 32767);
    return !out.s.empty();
}

bool decodeMp3(const std::vector<uint8_t>& f, Pcm& out) {
    mp3dec_t dec;
    mp3dec_init(&dec);
    std::vector<mp3d_sample_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    size_t pos = 0;
    while (pos < f.size()) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&dec, f.data() + pos, (int)(f.size() - pos), pcm.data(), &info);
        if (!info.frame_bytes) break;
        pos += info.frame_bytes;
        if (!n) continue;
        out.rate = (uint32_t)info.hz;
        for (int i = 0; i < n; ++i) {
            int v = 0;
            for (int c = 0; c < info.channels; ++c) v += pcm[i * info.channels + c];
            out.s.push_back((int16_t)(v / info.channels));
        }
    }
    return !out.s.empty() && out.rate;
}

bool decodeSound(const std::vector<uint8_t>& f, Pcm& out) {
    if (decodeWav(f, out)) return true;
    out = {};
    return decodeMp3(f, out);
}

void resample(Pcm& p, uint32_t maxRate) {
    if (p.rate <= maxRate || p.s.empty()) return;
    const double step = (double)p.rate / maxRate;
    std::vector<int16_t> o((size_t)(p.s.size() / step));
    for (size_t i = 0; i < o.size(); ++i) {
        // a box filter over the step (cheap anti-aliasing for 44.1 -> 22.05 kHz) then linear interpolation
        double x = i * step;
        size_t a = (size_t)x;
        double acc = 0;
        int n = 0;
        for (size_t k = a; k < std::min(p.s.size(), a + (size_t)std::ceil(step)); ++k, ++n) acc += p.s[k];
        o[i] = (int16_t)(n ? acc / n : 0);
    }
    p.s.swap(o);
    p.rate = maxRate;
}

void imaEncode(const Pcm& p, std::vector<uint8_t>& out) {
    W3dImaHeader h{W3D_IMA_MAGIC, p.rate, (uint32_t)p.s.size(), (uint32_t)((p.s.size() + kW3dImaBlock - 1) / kW3dImaBlock)};
    out.resize(sizeof h);
    std::memcpy(out.data(), &h, sizeof h);
    int pred = 0, index = 0;
    for (uint32_t b = 0; b < h.blocks; ++b) {
        uint8_t blk[kW3dImaBlockBytes] = {};
        blk[0] = (uint8_t)(pred & 0xFF); blk[1] = (uint8_t)((pred >> 8) & 0xFF); blk[2] = (uint8_t)index;
        for (int k = 0; k < kW3dImaBlock; ++k) {
            size_t i = (size_t)b * kW3dImaBlock + k;
            int target = i < p.s.size() ? p.s[i] : 0;
            int step = kW3dImaStep[index], diff = target - pred;
            uint8_t nib = 0;
            if (diff < 0) { nib = 8; diff = -diff; }
            if (diff >= step) { nib |= 4; diff -= step; }
            if (diff >= step >> 1) { nib |= 2; diff -= step >> 1; }
            if (diff >= step >> 2) nib |= 1;
            w3dImaStep(nib, &pred, &index);   // track the decoder exactly
            blk[4 + k / 2] |= (uint8_t)(k & 1 ? nib << 4 : nib);
        }
        out.insert(out.end(), blk, blk + sizeof blk);
    }
}

bool writeFile(const std::string& path, const void* d, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    std::fwrite(d, 1, n, f);
    std::fclose(f);
    return true;
}

struct SoundRow { uint32_t row = 0; std::vector<std::string> files; float volume = 1; };

SoundRow soundRow(const DbcTable& se, uint32_t r) {
    SoundRow s;
    s.row = r;
    std::string dir = se.fields > 23 ? se.str(r, 23) : std::string();
    for (uint32_t c = 3; c < 13 && c < se.fields; ++c) {
        std::string f = se.str(r, c);
        if (f.empty()) continue;
        s.files.push_back(dir.empty() ? f : dir + "\\" + f);
    }
    if (se.fields > 24) {
        float v = se.f32(r, 24);
        if (v > 0.01f && v <= 1.0f) s.volume = v;
    }
    return s;
}

// --- areas_<map>.bin from the ADT MCNK headers
bool exportAreas(const DataDir& data, uint32_t mapId, const std::string& dir, const std::string& outPath, size_t* tilesOut) {
    std::vector<uint8_t> wdt;
    if (!data.read("World\\Maps\\" + dir + "\\" + dir + ".wdt", wdt)) return false;
    const uint8_t* main = nullptr;
    for (size_t p = 0; p + 8 <= wdt.size();) {
        uint32_t len = rd32(&wdt[p + 4]);
        if (!std::memcmp(&wdt[p], "NIAM", 4) && p + 8 + 64 * 64 * 8 <= wdt.size()) main = &wdt[p + 8];
        p += 8 + len;
    }
    if (!main) return false;
    std::vector<uint16_t> slots(64 * 64, 0xFFFF), areas;
    size_t n = 0;
    for (int ty = 0; ty < 64; ++ty)
        for (int tx = 0; tx < 64; ++tx) {
            if (!(rd32(main + (ty * 64 + tx) * 8) & 1)) continue;
            std::vector<uint8_t> adt;
            char path[256];
            std::snprintf(path, sizeof path, "World\\Maps\\%s\\%s_%d_%d.adt", dir.c_str(), dir.c_str(), tx, ty);
            progress(false, "areas %s: tile %d_%d (%zu)", dir.c_str(), tx, ty, n);
            if (!data.read(path, adt)) continue;
            std::vector<uint16_t> grid(256, 0);
            bool any = false;
            for (size_t p = 0; p + 8 <= adt.size();) {
                uint32_t len = rd32(&adt[p + 4]);
                if (!std::memcmp(&adt[p], "KNCM", 4) && p + 8 + 0x38 <= adt.size()) {
                    const uint8_t* h = &adt[p + 8];
                    uint32_t cx = rd32(h + 4), cy = rd32(h + 8), area = rd32(h + 0x34);
                    if (cx < 16 && cy < 16) { grid[cy * 16 + cx] = (uint16_t)std::min<uint32_t>(area, 0xFFFE); any = true; }
                }
                p += 8 + len;
            }
            if (!any) continue;
            slots[tx * 64 + ty] = (uint16_t)n++;   // [tileX][tileY] as the 3DS computes them from the position
            areas.insert(areas.end(), grid.begin(), grid.end());
        }
    if (!n) return false;
    W3dAreaHeader h{W3D_AREA_MAGIC, mapId, (uint32_t)n, 0};
    std::vector<uint8_t> out(sizeof h + slots.size() * 2 + areas.size() * 2);
    std::memcpy(out.data(), &h, sizeof h);
    std::memcpy(out.data() + sizeof h, slots.data(), slots.size() * 2);
    std::memcpy(out.data() + sizeof h + slots.size() * 2, areas.data(), areas.size() * 2);
    *tilesOut = n;
    return writeFile(outPath, out.data(), out.size());
}

// ---- sounds of the world: creatures (CreatureSoundData), weapon impacts, spells (SpellVisual -> SpellVisualKit),
// footsteps. Column layouts of these tables changed between builds and are not all documented for 1.12, so the
// columns are found from the data: by the names of the SoundEntries rows they point at ("...Aggro", "...WoundCrit").
struct WorldCtx {
    const DbcTable& se;
    const std::unordered_map<uint32_t, uint32_t>& byId;
    std::set<uint32_t> used;
    std::string txt;
    // UI stage 8: voices that SoundEntries does not list (found by their file names in the archives) get ids of their own
    // from kSyntheticSound up; their files are here. Voices keep more variants than the other world sounds.
    std::map<uint32_t, std::vector<std::string>> extra;
    std::map<uint32_t, int> variants;
    std::string nameOf(uint32_t id) const {
        auto it = byId.find(id);
        return it == byId.end() ? std::string() : lower(se.str(it->second, 2));
    }
    bool valid(uint32_t id) const { return id && (byId.count(id) || extra.count(id)); }
};
constexpr uint32_t kSyntheticSound = 0x7F000000u;

bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

// The column of 't' (from 1) whose values point at the most SoundEntries rows 'pred' accepts; -1 when none does.
int bestColumn(const WorldCtx& w, const DbcTable& t, const std::function<bool(const std::string&)>& pred, const std::set<int>& taken,
               uint32_t minHits = 2) {
    int best = -1;
    uint32_t bestN = 0;
    for (uint32_t c = 1; c < t.fields; ++c) {
        if (taken.count((int)c)) continue;
        uint32_t n = 0;
        for (uint32_t r = 0; r < t.records; ++r) {
            uint32_t v = t.u32(r, c);
            if (w.valid(v) && pred(w.nameOf(v))) ++n;
        }
        if (n > bestN) { bestN = n; best = (int)c; }
    }
    return bestN >= minHits ? best : -1;
}

void exportWorldTables(const DataDir& data, WorldCtx& w, bool verbose) {
    // creatures
    DbcTable csd, cdi, ftl, tt;
    std::set<uint32_t> csdIds;
    if (csd.load(data, "CreatureSoundData.dbc")) {
        for (uint32_t r = 0; r < csd.records; ++r) csdIds.insert(csd.u32(r, 0));
        std::set<int> taken;
        auto pick = [&](const char* what, const std::function<bool(const std::string&)>& p) {
            int c = bestColumn(w, csd, p, taken);
            if (c >= 0) taken.insert(c);
            if (verbose) std::printf("  CreatureSoundData %-11s column %d\n", what, c);
            return c;
        };
        const int cWoundCrit = pick("woundCrit", [](const std::string& n) { return has(n, "wound") && has(n, "crit"); });
        const int cWound = pick("wound", [](const std::string& n) { return has(n, "wound") && !has(n, "crit"); });
        const int cAttackCrit = pick("attackCrit", [](const std::string& n) { return has(n, "attack") && has(n, "crit"); });
        const int cAttack = pick("attack", [](const std::string& n) { return has(n, "attack") && !has(n, "crit"); });
        const int cDeath = pick("death", [](const std::string& n) { return has(n, "death"); });
        const int cAggro = pick("aggro", [](const std::string& n) { return has(n, "aggro"); });
        // footsteps: the column whose values are CreatureFootstepIDs of FootstepTerrainLookup
        int cFoot = -1;
        std::set<uint32_t> footIds;
        const bool haveFtl = ftl.load(data, "FootstepTerrainLookup.dbc") && ftl.fields >= 4;
        if (haveFtl) {
            for (uint32_t r = 0; r < ftl.records; ++r) footIds.insert(ftl.u32(r, 1));
            uint32_t bestN = 0;
            for (uint32_t c = 1; c < csd.fields; ++c) {
                if (taken.count((int)c)) continue;
                uint32_t n = 0, nz = 0;
                for (uint32_t r = 0; r < csd.records; ++r) {
                    uint32_t v = csd.u32(r, c);
                    if (!v) continue;
                    ++nz;
                    if (footIds.count(v)) ++n;   // (small ids may also be sound ids: the 1.12 footstep ids 7, 8 are)
                }
                if (n > bestN && n * 2 >= nz) { bestN = n; cFoot = (int)c; }
            }
            if (verbose) std::printf("  CreatureSoundData footstep   column %d\n", cFoot);
        }
        auto col = [&](uint32_t r, int c) { return c >= 0 ? csd.u32(r, (uint32_t)c) : 0u; };
        for (uint32_t r = 0; r < csd.records; ++r) {
            uint32_t v[6] = {col(r, cAggro), col(r, cDeath), col(r, cWound), col(r, cWoundCrit), col(r, cAttack), col(r, cAttackCrit)};
            bool any = false;
            for (uint32_t& x : v) {
                if (!w.valid(x)) x = 0;
                if (x) { any = true; w.used.insert(x); }
            }
            const uint32_t foot = col(r, cFoot);
            if (!any && !foot) continue;
            w.txt += "K\t" + std::to_string(csd.u32(r, 0));
            for (uint32_t x : v) w.txt += "\t" + std::to_string(x);
            w.txt += "\t" + std::to_string(foot) + "\n";
        }
        // footstep sounds on dirt (TerrainType "Dirt"; else the lowest terrain of each footstep id)
        if (haveFtl) {
            uint32_t dirt = ~0u;
            if (tt.load(data, "TerrainType.dbc"))
                for (uint32_t r = 0; r < tt.records; ++r)
                    if (has(lower(tt.str(r, 1)), "dirt")) { dirt = tt.u32(r, 0); break; }
            std::map<uint32_t, std::pair<uint32_t, uint32_t>> per;   // footstep id -> (terrain, sound)
            for (uint32_t r = 0; r < ftl.records; ++r) {
                uint32_t f = ftl.u32(r, 1), terr = ftl.u32(r, 2), snd = ftl.u32(r, 3);
                if (!w.valid(snd)) continue;
                auto it = per.find(f);
                const bool better = it == per.end() || (terr == dirt && it->second.first != dirt) ||
                                    (it->second.first != dirt && terr < it->second.first);
                if (better) per[f] = {terr, snd};
            }
            // id 0 is what a creature without a footstep id (and the player) falls back to: a person's step on dirt, not
            // the first row of the table (which is a giant's)
            for (const char* want : {"charactermediumlargedirt", "charactermediumdirt", "charactersmalldirt"}) {
                bool done = false;
                for (const auto& [f, ts] : per)
                    if (lower(w.nameOf(ts.second)) == want) { per[0] = ts; done = true; break; }
                if (done) break;
            }
            for (const auto& [f, ts] : per) {
                w.txt += "# footstep " + std::to_string(f) + " terrain " + std::to_string(ts.first) + ": " + w.nameOf(ts.second) + "\n";
                w.txt += "F\t" + std::to_string(f) + "\t" + std::to_string(ts.second) + "\n";
                w.used.insert(ts.second);
            }
        }
    }
    if (!csdIds.empty() && cdi.load(data, "CreatureDisplayInfo.dbc") && cdi.fields > 2) {
        // the sound column: 2 in 1.12; checked against the CreatureSoundData ids
        uint32_t c = 2, ok = 0, nz = 0;
        for (uint32_t r = 0; r < cdi.records; ++r)
            if (uint32_t v = cdi.u32(r, c)) { ++nz; ok += csdIds.count(v) ? 1 : 0; }
        if (nz && ok * 2 >= nz)
            for (uint32_t r = 0; r < cdi.records; ++r)
                if (uint32_t v = cdi.u32(r, c); csdIds.count(v))
                    w.txt += "D\t" + std::to_string(cdi.u32(r, 0)) + "\t" + std::to_string(v) + "\n";
    }
    // weapon impacts: ID, WeaponSubClassID, ParrySoundType, ImpactSoundID[10] 3-12, CritImpactSoundID[10] 13-22
    DbcTable wis;
    if (wis.load(data, "WeaponImpactSounds.dbc") && wis.fields >= 23) {
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> per;
        std::map<uint32_t, bool> flesh;
        for (uint32_t r = 0; r < wis.records; ++r) {
            uint32_t sub = wis.u32(r, 1), imp = 0, crit = 0;
            if (sub > 20) continue;
            for (uint32_t c = 3; c < 13 && !imp; ++c) if (w.valid(wis.u32(r, c))) imp = wis.u32(r, c);
            for (uint32_t c = 13; c < 23 && !crit; ++c) if (w.valid(wis.u32(r, c))) crit = wis.u32(r, c);
            if (!imp) continue;
            const bool fl = has(w.nameOf(imp), "flesh");
            if (!per.count(sub) || (fl && !flesh[sub])) { per[sub] = {imp, crit}; flesh[sub] = fl; }
        }
        for (const auto& [sub, ic] : per) {
            w.txt += "W\t" + std::to_string(sub) + "\t" + std::to_string(ic.first) + "\t" + std::to_string(ic.second) + "\n";
            w.used.insert(ic.first);
            if (ic.second) w.used.insert(ic.second);
        }
    }
    // spells: Spell.SpellVisual (5 columns before the name) -> SpellVisual kits 1-6 / missile sound -> kit sounds
    DbcTable spell, sv, svk;
    if (spell.load(data, "Spell.dbc") && sv.load(data, "SpellVisual.dbc") && svk.load(data, "SpellVisualKit.dbc")) {
        std::map<uint32_t, uint32_t> kitSound;
        const int cKit = bestColumn(w, svk, [](const std::string& n) {
            for (const char* k : {"cast", "impact", "spell", "missile", "buff", "heal", "fire", "frost", "holy", "shadow",
                                  "nature", "arcane", "lightning", "aura", "shield", "summon"})
                if (has(n, k)) return true;
            return false;
        }, {});
        if (cKit >= 0)
            for (uint32_t r = 0; r < svk.records; ++r)
                if (uint32_t s = svk.u32(r, (uint32_t)cKit); w.valid(s)) kitSound[svk.u32(r, 0)] = s;
        int cMissile = -1;   // MissileSound: 11 in 1.12, if (nearly) every value there is a sound
        if (sv.fields > 11) {
            uint32_t nz = 0, ok = 0;
            for (uint32_t r = 0; r < sv.records; ++r)
                if (uint32_t v = sv.u32(r, 11)) { ++nz; ok += w.valid(v) ? 1 : 0; }
            if (nz && ok * 10 >= nz * 9) cMissile = 11;
        }
        std::set<uint32_t> visuals;
        for (uint32_t r = 0; r < sv.records; ++r) {
            auto kit = [&](uint32_t c) {
                auto it = c < sv.fields ? kitSound.find(sv.u32(r, c)) : kitSound.end();
                return it == kitSound.end() ? 0u : it->second;
            };
            uint32_t pre = kit(1), cast = kit(2), imp = kit(3), chan = kit(6);
            uint32_t mis = cMissile >= 0 && w.valid(sv.u32(r, 11)) ? sv.u32(r, 11) : 0;
            if (!pre && !cast && !imp && !mis && !chan) continue;
            const uint32_t id = sv.u32(r, 0);
            visuals.insert(id);
            w.txt += "Q\t" + std::to_string(id) + "\t" + std::to_string(pre) + "\t" + std::to_string(cast) + "\t" + std::to_string(imp) +
                     "\t" + std::to_string(mis) + "\t" + std::to_string(chan) + "\n";
            for (uint32_t s : {pre, cast, imp, mis, chan}) if (s) w.used.insert(s);
        }
        int nameCol = spellNameColumn(spell);
        if (nameCol < 0) nameCol = 120;
        const uint32_t visCol = (uint32_t)nameCol - 5;
        size_t np = 0;
        for (uint32_t r = 0; r < spell.records; ++r)
            if (uint32_t v = spell.u32(r, visCol); visuals.count(v)) {
                w.txt += "P\t" + std::to_string(spell.u32(r, 0)) + "\t" + std::to_string(v) + "\n";
                ++np;
            }
        if (verbose) std::printf("  spells: kit sound column %d, %zu visuals with sounds, %zu spells\n", cKit, visuals.size(), np);
    }
}

// ---- voices (UI stage 8): what NPCs say when selected / talked to / left (NPCSounds.dbc: greeting, farewell, pissed - via
// CreatureDisplayInfo; humanoids without a set of their own use the "<Race><Gender>StandardNPC" voices of their race), what
// our character says when something cannot be done (the "<Race><Gender>_err_<token>NN.wav" files, VocalUISounds) and the
// voices of the text emotes (EmotesTextSound.dbc). sounds.txt lines:
//   "N display greeting farewell pissed"   "V race gender token sound"   "T textEmote race gender sound"
struct VoiceRace { uint32_t id; const char* tok[2]; const char* folder; const char* errDir; const char* errFile; const char* npc[2]; };
const VoiceRace kVoiceRaces[] = {
    {1, {"human", nullptr}, "Human", "", "Human", {"Human", nullptr}},
    {2, {"orc", nullptr}, "Orc", "Orc", "Orc", {"Orc", nullptr}},
    {3, {"dwarf", nullptr}, "Dwarf", "Dwarf", "Dwarf", {"Dwarf", nullptr}},
    {4, {"nightelf", nullptr}, "NightElf", "NightElf", "NightElf", {"NightElf", nullptr}},
    {5, {"undead", "scourge"}, "Scourge", "Scourge", "Undead", {"Undead", "Scourge"}},
    {6, {"tauren", nullptr}, "Tauren", "Tauren", "Tauren", {"Tauren", nullptr}},
    {7, {"gnome", nullptr}, "Gnome", "Gnome", "Gnome", {"Gnome", nullptr}},
    {8, {"troll", nullptr}, "Troll", "Troll", "Troll", {"Troll", nullptr}},
    {9, {"goblin", nullptr}, "Goblin", nullptr, "Goblin", {"Goblin", nullptr}},
};
const char* const kErrTokens[] = {
    "nomana", "norage", "noenergy", "outofrange", "genericnotarget", "invalidattacktarget", "spellcooldown",
    "abilitycooldown", "itemcooldown", "noammo", "chestinuse", "inventoryfull", "bagfull", "itemmaxcount", "notabag",
    "cantuseitem", "itemlocked", "notequippable", "cantequiplevel", "cantequipskill", "cantequipever",
    "cantdropsoulbounditem", "notenoughmoney", "cantlearnspell", "loottoofar", "lootdidntkill", "cantloot",
};

std::string baseName(const std::string& path) {
    size_t k = path.find_last_of("\\/");
    return lower(k == std::string::npos ? path : path.substr(k + 1));
}

// "humanfemale..." -> race 1, gender 1, the rest; false when the name does not start with a race and a gender.
bool raceGenderPrefix(const std::string& n, uint32_t* race, uint32_t* gender, std::string* rest) {
    for (const VoiceRace& r : kVoiceRaces)
        for (const char* t : r.tok) {
            if (!t || n.compare(0, strlen(t), t) != 0) continue;
            std::string m = n.substr(strlen(t));
            for (int g = 1; g >= 0; --g) {   // "female" first: "male" is its suffix
                const char* gw = g ? "female" : "male";
                if (m.compare(0, strlen(gw), gw) == 0) { *race = r.id; *gender = (uint32_t)g; *rest = m.substr(strlen(gw)); return true; }
            }
        }
    return false;
}

// Files that exist among base + "01" .. base + "<maxN>" + ".wav" (variant numbers may be sparse).
std::vector<std::string> probeFiles(const DataDir& data, const std::string& base, int maxN) {
    std::vector<std::string> out;
    std::vector<uint8_t> tmp;
    for (int n = 1; n <= maxN; ++n) {
        char num[8];
        std::snprintf(num, sizeof num, "%02d", n);
        if (data.read(base + num + ".wav", tmp)) out.push_back(base + num + ".wav");
    }
    return out;
}

void exportVoices(const DataDir& data, WorldCtx& w, bool verbose) {
    uint32_t nextId = kSyntheticSound;
    auto synthetic = [&](std::vector<std::string> files, int variants) {
        const uint32_t id = nextId++;
        w.extra[id] = std::move(files);
        w.variants[id] = variants;
        return id;
    };
    // what every SoundEntries row's first file is called
    std::vector<std::pair<uint32_t, std::string>> firstFile;   // sound id, lower-case base name
    for (const auto& [id, r] : w.byId) {
        SoundRow sr = soundRow(w.se, r);
        if (!sr.files.empty()) firstFile.push_back({id, baseName(sr.files[0])});
    }
    std::sort(firstFile.begin(), firstFile.end());   // the same picks on every run

    // ---- our character's error speech
    std::map<std::tuple<uint32_t, uint32_t, std::string>, uint32_t> err;
    for (const auto& [id, n] : firstFile) {
        const size_t k = n.find("_err_");
        if (k == std::string::npos) continue;
        uint32_t race, gender;
        std::string rest;
        if (!raceGenderPrefix(n.substr(0, k), &race, &gender, &rest) || !rest.empty()) continue;
        std::string tok = n.substr(k + 5);
        if (tok.size() > 4 && tok.compare(tok.size() - 4, 4, ".wav") == 0) tok.resize(tok.size() - 4);
        while (!tok.empty() && isdigit((unsigned char)tok.back())) tok.pop_back();
        if (std::find_if(std::begin(kErrTokens), std::end(kErrTokens), [&](const char* t) { return tok == t; }) == std::end(kErrTokens)) continue;
        err.emplace(std::make_tuple(race, gender, tok), id);
    }
    size_t errListed = err.size(), errProbed = 0;
    for (const VoiceRace& r : kVoiceRaces) {   // a race / gender SoundEntries has none of: the files by their usual names
        if (!r.errDir) continue;
        for (uint32_t g = 0; g < 2; ++g) {
            bool any = false;
            for (const auto& [key, id] : err) any |= std::get<0>(key) == r.id && std::get<1>(key) == g;
            if (any) continue;
            const std::string gw = g ? "Female" : "Male";
            const std::string dir = std::string("Sound\\Character\\") + r.folder + "\\" + r.errDir + gw + "ErrorMessages\\";
            for (const char* t : kErrTokens) {
                std::vector<std::string> files = probeFiles(data, dir + r.errFile + gw + "_err_" + t, 8);
                if (files.empty()) continue;
                err[std::make_tuple(r.id, g, std::string(t))] = synthetic(std::move(files), 2);
                ++errProbed;
            }
        }
    }
    for (const auto& [key, id] : err) {
        w.txt += "V\t" + std::to_string(std::get<0>(key)) + "\t" + std::to_string(std::get<1>(key)) + "\t" + std::get<2>(key) +
                 "\t" + std::to_string(id) + "\n";
        w.used.insert(id);
        if (!w.variants.count(id)) w.variants[id] = 2;
    }

    // ---- NPC voices: NPCSounds (greeting / farewell / pissed columns found by the names of the rows they point at)
    DbcTable npc, cdi, cdx;
    std::map<uint32_t, std::array<uint32_t, 3>> sets;   // NPCSounds id -> greeting, farewell, pissed
    if (npc.load(data, "NPCSounds.dbc") && npc.fields >= 2) {
        std::set<int> taken;
        const int cG = bestColumn(w, npc, [](const std::string& n) { return has(n, "greet") || has(n, "hello"); }, taken, 1);
        if (cG >= 0) taken.insert(cG);
        const int cF = bestColumn(w, npc, [](const std::string& n) { return has(n, "farewell") || has(n, "bye"); }, taken, 1);
        if (cF >= 0) taken.insert(cF);
        const int cP = bestColumn(w, npc, [](const std::string& n) { return has(n, "pissed") || has(n, "annoy"); }, taken, 1);
        // without telling names: the documented order (ID, greeting, farewell, pissed, unused)
        const int g = cG >= 0 ? cG : 1, f = cF >= 0 ? cF : (npc.fields > 2 ? 2 : -1), pz = cP >= 0 ? cP : (npc.fields > 3 ? 3 : -1);
        for (uint32_t r = 0; r < npc.records; ++r) {
            auto v = [&](int c) { uint32_t x = c >= 0 ? npc.u32(r, (uint32_t)c) : 0; return w.valid(x) ? x : 0u; };
            std::array<uint32_t, 3> a{v(g), v(f), v(pz)};
            if (a[0] || a[1] || a[2]) sets[npc.u32(r, 0)] = a;
        }
        if (verbose) std::printf("  NPCSounds: %zu sets (columns greeting %d farewell %d pissed %d)\n", sets.size(), g, f, pz);
    }
    // the generic voices of each race: "<Race><Gender>StandardNPC<Greeting(s)|Farewell|Pissed>NN.wav"
    std::map<std::pair<uint32_t, uint32_t>, std::array<uint32_t, 3>> generic;
    for (const auto& [id, n] : firstFile) {
        uint32_t race, gender;
        std::string rest;
        if (!raceGenderPrefix(n, &race, &gender, &rest) || rest.compare(0, 11, "standardnpc") != 0) continue;
        const int kind = has(rest, "greet") ? 0 : has(rest, "farewell") ? 1 : has(rest, "pissed") ? 2 : -1;
        if (kind < 0) continue;
        uint32_t& slot = generic[{race, gender}][kind];
        if (!slot) slot = id;
    }
    for (const VoiceRace& r : kVoiceRaces)
        for (uint32_t g = 0; g < 2; ++g) {
            if (generic.count({r.id, g})) continue;
            std::array<uint32_t, 3> a{0, 0, 0};
            static const char* kKinds[3][2] = {{"Greetings", "Greeting"}, {"Farewell", "Farewells"}, {"Pissed", nullptr}};
            for (const char* rn : r.npc) {
                if (!rn) continue;
                const std::string who = std::string(rn) + (g ? "Female" : "Male") + "StandardNPC";
                for (int k = 0; k < 3; ++k)
                    for (const char* kw : kKinds[k]) {
                        if (a[k] || !kw) continue;
                        std::vector<std::string> files = probeFiles(data, "Sound\\Creature\\" + who + "\\" + who + kw, 12);
                        if (!files.empty()) a[k] = synthetic(std::move(files), 4);
                    }
            }
            if (a[0] || a[1] || a[2]) generic[{r.id, g}] = a;
        }
    size_t displays = 0, displaysGeneric = 0;
    if (cdi.load(data, "CreatureDisplayInfo.dbc") && cdi.fields > 3) {
        // the NPCSounds column: (nearly) every non-zero value is an NPCSounds id, and the most different ones
        int col = -1;
        size_t bestDistinct = 0;
        for (uint32_t c = 3; c < cdi.fields && !sets.empty(); ++c) {
            uint32_t nz = 0, ok = 0;
            std::set<uint32_t> distinct;
            for (uint32_t r = 0; r < cdi.records; ++r)
                if (uint32_t v = cdi.u32(r, c)) { ++nz; if (sets.count(v)) { ++ok; distinct.insert(v); } }
            if (nz && ok * 10 >= nz * 9 && distinct.size() >= 3 && distinct.size() > bestDistinct) { bestDistinct = distinct.size(); col = (int)c; }
        }
        // humanoids: CreatureDisplayInfoExtra race (1) and gender (2) for the generic voices
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> extraRace;
        if (cdx.load(data, "CreatureDisplayInfoExtra.dbc") && cdx.fields > 3)
            for (uint32_t r = 0; r < cdx.records; ++r) extraRace[cdx.u32(r, 0)] = {cdx.u32(r, 1), cdx.u32(r, 2)};
        for (uint32_t r = 0; r < cdi.records; ++r) {
            std::array<uint32_t, 3> a{0, 0, 0};
            bool own = false;
            if (col >= 0) {
                auto it = sets.find(cdi.u32(r, (uint32_t)col));
                if (it != sets.end()) { a = it->second; own = true; }
            }
            if (!own) {
                auto ex = extraRace.find(cdi.u32(r, 3));
                if (ex == extraRace.end()) continue;
                auto ge = generic.find(ex->second);
                if (ge == generic.end()) continue;
                a = ge->second;
                ++displaysGeneric;
            }
            if (!a[0] && !a[1] && !a[2]) continue;
            w.txt += "N\t" + std::to_string(cdi.u32(r, 0)) + "\t" + std::to_string(a[0]) + "\t" + std::to_string(a[1]) + "\t" +
                     std::to_string(a[2]) + "\n";
            for (uint32_t x : a)
                if (x) { w.used.insert(x); if (!w.variants.count(x)) w.variants[x] = 4; }
            ++displays;
        }
        if (verbose) std::printf("  CreatureDisplayInfo NPC sound column %d\n", col);
    }

    // ---- text emotes: EmotesTextSound ID, EmotesText, race, gender, sound
    DbcTable ets;
    size_t emotes = 0;
    if (ets.load(data, "EmotesTextSound.dbc") && ets.fields >= 5)
        for (uint32_t r = 0; r < ets.records; ++r) {
            const uint32_t te = ets.u32(r, 1), race = ets.u32(r, 2), gender = ets.u32(r, 3), snd = ets.u32(r, 4);
            if (!te || !race || gender > 1 || !w.valid(snd)) continue;
            w.txt += "T\t" + std::to_string(te) + "\t" + std::to_string(race) + "\t" + std::to_string(gender) + "\t" + std::to_string(snd) + "\n";
            w.used.insert(snd);
            if (!w.variants.count(snd)) w.variants[snd] = 2;
            ++emotes;
        }
    std::printf("  voices: %zu error lines (%zu listed, %zu by file name), %zu NPC sets, %zu displays with a voice (%zu by race), "
                "%zu emote voices\n", err.size(), errListed, errProbed, sets.size(), displays, displaysGeneric, emotes);
}

bool writeWorldBank(const DataDir& data, const SoundOptions& so, WorldCtx& w, const std::string& path) {
    std::vector<W3dWorldEntry> entries;
    std::vector<uint8_t> blocks;
    size_t done = 0;
    for (uint32_t id : w.used) {
        ++done;
        progress(false, "world sounds: %zu/%zu, %.1f MB", done, w.used.size(), blocks.size() / 1048576.0);
        SoundRow s;
        float minD = 8, cut = 40;
        if (auto ex = w.extra.find(id); ex != w.extra.end()) {
            s.files = ex->second;
        } else {
            s = soundRow(w.se, w.byId.at(id));
            if (w.se.fields > 27) {
                float a = w.se.f32(s.row, 26), b = w.se.f32(s.row, 27);
                if (a > 0.5f && a < 100 && b > a && b < 500) { minD = a; cut = b; }
            }
        }
        const auto vv = w.variants.find(id);
        const int keep = vv != w.variants.end() ? std::max(so.worldVariants, vv->second) : so.worldVariants;
        int kept = 0;
        for (const std::string& f : s.files) {
            if (kept >= keep) break;
            std::vector<uint8_t> bytes;
            Pcm p;
            if (!data.read(f, bytes) || !decodeSound(bytes, p)) continue;
            resample(p, so.streamRate);
            if (p.s.size() > (size_t)p.rate * (size_t)so.worldMaxSec) p.s.resize((size_t)p.rate * (size_t)so.worldMaxSec);   // 12 s at most by default (--sound-max-sec)
            std::vector<uint8_t> ima;
            imaEncode(p, ima);
            W3dWorldEntry e{id, (uint32_t)blocks.size(), (uint32_t)p.s.size(), (uint16_t)p.rate,
                            (uint16_t)std::lround(s.volume * 1000), (uint16_t)std::lround(minD * 10), (uint16_t)std::lround(cut * 10)};
            blocks.insert(blocks.end(), ima.begin() + sizeof(W3dImaHeader), ima.end());
            entries.push_back(e);
            ++kept;
        }
    }
    W3dSfxHeader h{W3D_WORLD_MAGIC, 1, (uint32_t)entries.size(), 0};
    const uint32_t dataStart = (uint32_t)(sizeof h + entries.size() * sizeof(W3dWorldEntry));
    for (W3dWorldEntry& e : entries) e.offset += dataStart;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    std::fwrite(&h, sizeof h, 1, f);
    if (!entries.empty()) std::fwrite(entries.data(), sizeof(W3dWorldEntry), entries.size(), f);
    std::fwrite(blocks.data(), 1, blocks.size(), f);
    std::fclose(f);
    progressDone("world sounds: %zu sounds, %zu files, %.1f MB", w.used.size(), entries.size(), (dataStart + blocks.size()) / 1048576.0);
    return true;
}

}  // namespace

bool convertSound(const SoundOptions& so, const DataDir& data, bool verbose) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(so.outDir, ec);
    const std::string base = so.outDir.empty() ? std::string() : so.outDir + "/";
    DbcTable se;
    if (!se.load(data, "SoundEntries.dbc") || se.fields < 13) {
        std::fprintf(stderr, "sound: SoundEntries.dbc not found\n");
        return false;
    }
    std::unordered_map<std::string, uint32_t> byName;
    std::unordered_map<uint32_t, uint32_t> byId;
    for (uint32_t r = 0; r < se.records; ++r) {
        byName.emplace(lower(se.str(r, 2)), r);
        byId[se.u32(r, 0)] = r;
    }

    // ---- interface / event sounds
    auto events = kDefaultEvents;
    if (!so.eventsFile.empty()) {   // "event name1 name2 ..." lines replace or add events
        FILE* f = std::fopen(so.eventsFile.c_str(), "r");
        if (!f) { std::fprintf(stderr, "cannot read %s\n", so.eventsFile.c_str()); return false; }
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::vector<std::string> w;
            for (char* t = std::strtok(line, " \t\r\n"); t; t = std::strtok(nullptr, " \t\r\n")) w.push_back(t);
            if (w.size() < 2 || w[0][0] == '#') continue;
            std::vector<std::string> names(w.begin() + 1, w.end());
            auto it = std::find_if(events.begin(), events.end(), [&](auto& e) { return e.first == lower(w[0]); });
            if (it != events.end()) it->second = names;
            else events.push_back({lower(w[0]), names});
        }
        std::fclose(f);
    }
    std::vector<W3dSfxEntry> entries;
    std::vector<uint8_t> pcmData;
    size_t found = 0;
    std::string missing;
    for (const auto& [ev, names] : events) {
        bool got = false;
        for (const std::string& nm : names) {
            auto it = byName.find(lower(nm));
            std::string used = nm;
            if (!nm.empty() && nm[0] == '*') {   // "*word": the first name containing it
                const std::string w = lower(nm.substr(1));
                std::string best;
                for (const auto& [k, r] : byName)
                    if (k.find(w) != std::string::npos && (best.empty() || k < best)) best = k;
                it = best.empty() ? byName.end() : byName.find(best);
                used = best;
            }
            SoundRow s;
            if (!nm.empty() && nm[0] == '@') {   // 6.40: "@Sound\\Interface\\Error.wav": a file of the client itself, not a SoundEntries name
                s.files.push_back(nm.substr(1));
                s.volume = 1.0f;
                used = nm;
            } else {
                if (it == byName.end()) continue;
                s = soundRow(se, it->second);
            }
            for (size_t k = 0; k < s.files.size() && k < 3; ++k) {
                std::vector<uint8_t> f;
                Pcm p;
                if (!data.read(s.files[k], f) || !decodeSound(f, p)) {
                    if (verbose && nm[0] != '@') std::fprintf(stderr, "  sound %s: cannot decode %s\n", ev.c_str(), s.files[k].c_str());
                    continue;
                }
                resample(p, so.sfxRate);
                if (p.s.size() > (size_t)p.rate * 6) p.s.resize((size_t)p.rate * 6);   // event sounds: 6 s at most
                W3dSfxEntry e{w3dSndHash(ev.c_str()), 0, (uint32_t)p.s.size(), (uint16_t)p.rate,
                              (uint16_t)std::lround(s.volume * 1000)};
                e.offset = (uint32_t)pcmData.size();   // fixed up below
                pcmData.insert(pcmData.end(), (const uint8_t*)p.s.data(), (const uint8_t*)(p.s.data() + p.s.size()));
                while (pcmData.size() & 3) pcmData.push_back(0);
                entries.push_back(e);
                got = true;
            }
            if (got) {
                if (verbose) std::printf("  sound %-14s <- %s\n", ev.c_str(), used.c_str());
                break;
            }
        }
        if (got) ++found;
        else missing += " " + ev;
    }
    {
        W3dSfxHeader h{W3D_SFX_MAGIC, 1, (uint32_t)entries.size(), 0};
        const uint32_t dataStart = (uint32_t)(sizeof h + entries.size() * sizeof(W3dSfxEntry));
        for (W3dSfxEntry& e : entries) e.offset += dataStart;
        std::vector<uint8_t> out(dataStart);
        std::memcpy(out.data(), &h, sizeof h);
        if (!entries.empty()) std::memcpy(out.data() + sizeof h, entries.data(), entries.size() * sizeof(W3dSfxEntry));
        out.insert(out.end(), pcmData.begin(), pcmData.end());
        if (!writeFile(base + "sfx.w3ds", out.data(), out.size())) return false;
        std::printf("  sfx: %zu of %zu events, %zu sounds, %.1f MB%s%s\n", found, events.size(), entries.size(),
                    out.size() / 1048576.0, missing.empty() ? "" : "; not in this client:", missing.c_str());
    }

    // ---- the world: creatures, weapons, spells, footsteps (sounds.txt + world.w3ds)
    if (so.world) {
        WorldCtx w{se, byId, {}, "# wow3ds world sounds: D display csd | K csd aggro death wound woundCrit attack attackCrit footstep | "
                                 "W sub impact crit | F footstep sound | Q visual precast cast impact missile channel | P spell visual | "
                                 "L liquid-sound-kind sound | N display greeting farewell pissed | V race gender token sound | T textEmote race gender sound\n"};
        exportWorldTables(data, w, verbose);
        exportVoices(data, w, verbose);
        // footsteps of any creature when FootstepTerrainLookup gave nothing: a SoundEntries name "*footstep*dirt*"
        if (w.txt.find("\nF\t") == std::string::npos)
            for (const auto& [nm, r] : byName)
                if (has(nm, "footstep") && (has(nm, "dirt") || has(nm, "grass"))) {
                    const uint32_t id = se.u32(r, 0);
                    w.txt += "F\t0\t" + std::to_string(id) + "\n";
                    w.used.insert(id);
                    break;
                }
        // liquids (stage 5.26): "L kind sound" - splash (falling / wading into deep water), swim (a stroke), wade (a step in
        // shallow water), fishbite (the server plays SoundEntries 3355 at the bobber when a fish bites). The names of these
        // rows are not documented for 1.12: the first SoundEntries row whose name or first file has the keyword wins.
        {
            struct Want { const char* kind; std::vector<const char*> keys; uint32_t fixedId; };
            const Want wants[] = {
                {"splash", {"enterwatermedium", "enterwater", "watersplash", "splashlarge", "splash"}, 0},
                {"swim", {"swimslow", "swimming", "swim"}, 0},
                {"wade", {"footstepswater", "footstepwater", "wade", "watersplashsmall"}, 0},
                {"fishbite", {"fishbite", "fishingbobber", "bobbersplash", "bobber"}, 3355},
            };
            std::string found;
            for (const Want& wt : wants) {
                uint32_t id = wt.fixedId && byId.count(wt.fixedId) ? wt.fixedId : 0;
                for (const char* k : wt.keys) {
                    if (id) break;
                    for (uint32_t r = 0; r < se.records && !id; ++r)   // in table order: the same pick every run
                        if (has(lower(se.str(r, 2)), k) || has(lower(se.str(r, 3)), k)) id = se.u32(r, 0);
                }
                if (!id) continue;
                w.txt += std::string("L\t") + wt.kind + "\t" + std::to_string(id) + "\n";
                w.used.insert(id);
                found += std::string(" ") + wt.kind + "=" + w.nameOf(id);
            }
            std::printf("  liquid sounds:%s\n", found.empty() ? " none found" : found.c_str());
        }
        FILE* tf = std::fopen((base + "sounds.txt").c_str(), "wb");
        if (tf) { std::fwrite(w.txt.data(), 1, w.txt.size(), tf); std::fclose(tf); }
        writeWorldBank(data, so, w, base + "world.w3ds");
    }

    // ---- zone music and ambience
    DbcTable area, zm, amb;
    const bool haveArea = area.load(data, "AreaTable.dbc") && area.fields > 8;
    const bool haveZm = zm.load(data, "ZoneMusic.dbc") && zm.fields >= 8;
    const bool haveAmb = amb.load(data, "SoundAmbience.dbc") && amb.fields >= 3;
    if (!haveArea) {
        std::fprintf(stderr, "sound: AreaTable.dbc not found (no music)\n");
        return true;
    }
    std::string txt = "# wow3ds music: A area parent zoneMusic ambience | Z zm silMin silMax day night | Y amb day night | "
                      "S/B sound vol*100 files | M map area\n";
    std::set<uint32_t> musicSounds, ambSounds, zmUsed, ambUsed;
    std::map<uint32_t, uint32_t> zmRow, ambRow;
    if (haveZm) for (uint32_t r = 0; r < zm.records; ++r) zmRow[zm.u32(r, 0)] = r;
    if (haveAmb) for (uint32_t r = 0; r < amb.records; ++r) ambRow[amb.u32(r, 0)] = r;
    std::map<uint32_t, uint32_t> mapFirstArea;
    for (uint32_t r = 0; r < area.records; ++r) {
        uint32_t id = area.u32(r, 0), map = area.u32(r, 1), parent = area.u32(r, 2);
        uint32_t a = so.ambience ? area.u32(r, 7) : 0, z = so.music ? area.u32(r, 8) : 0;
        if (!zmRow.count(z)) z = 0;
        if (!ambRow.count(a)) a = 0;
        if (!parent && !mapFirstArea.count(map)) mapFirstArea[map] = id;
        txt += "A\t" + std::to_string(id) + "\t" + std::to_string(parent) + "\t" + std::to_string(z) + "\t" + std::to_string(a) + "\n";
        if (z) zmUsed.insert(z);
        if (a) ambUsed.insert(a);
    }
    for (uint32_t z : zmUsed) {
        uint32_t r = zmRow[z];
        uint32_t day = zm.u32(r, 6), night = zm.u32(r, 7);
        txt += "Z\t" + std::to_string(z) + "\t" + std::to_string(zm.u32(r, 2)) + "\t" + std::to_string(zm.u32(r, 4)) + "\t" +
               std::to_string(day) + "\t" + std::to_string(night) + "\n";
        if (day) musicSounds.insert(day);
        if (night) musicSounds.insert(night);
    }
    for (uint32_t a : ambUsed) {
        uint32_t r = ambRow[a];
        uint32_t day = amb.u32(r, 1), night = amb.u32(r, 2);
        txt += "Y\t" + std::to_string(a) + "\t" + std::to_string(day) + "\t" + std::to_string(night) + "\n";
        if (day) ambSounds.insert(day);
        if (night) ambSounds.insert(night);
    }
    fs::create_directories(base + "music", ec);
    size_t files = 0, bytes = 0, done = 0;
    const size_t total = musicSounds.size() + ambSounds.size();
    auto emit = [&](uint32_t id, bool isAmb) {
        ++done;
        auto it = byId.find(id);
        if (it == byId.end()) return;
        SoundRow s = soundRow(se, it->second);
        std::string line = std::string(isAmb ? "B" : "S") + "\t" + std::to_string(id) + "\t" + std::to_string(std::lround(s.volume * 100));
        int k = 0, kept = 0;
        for (const std::string& path : s.files) {
            progress(false, "music: %zu/%zu sounds, %zu files, %.0f MB", done, total, files, bytes / 1048576.0);
            std::vector<uint8_t> f;
            if (!data.read(path, f)) { ++k; continue; }
            std::string name = (isAmb ? "a" : "m") + std::to_string(id) + "_" + std::to_string(k++);
            const bool mp3 = lower(path).size() > 4 && lower(path).substr(lower(path).size() - 4) == ".mp3";
            if (mp3 && !isAmb) {   // music: the MP3 as it is
                name += ".mp3";
                if (!writeFile(base + "music/" + name, f.data(), f.size())) continue;
                bytes += f.size();
            } else {   // WAV (or MP3 ambience): IMA ADPCM mono at <= 22050 Hz
                Pcm p;
                if (!decodeSound(f, p)) continue;
                resample(p, so.streamRate);
                std::vector<uint8_t> ima;
                imaEncode(p, ima);
                name += ".ima";
                if (!writeFile(base + "music/" + name, ima.data(), ima.size())) continue;
                bytes += ima.size();
            }
            line += "\t" + name;
            ++files;
            if (isAmb && ++kept >= 2) break;   // ambience: two variants are plenty
        }
        txt += line + "\n";
    };
    for (uint32_t id : musicSounds) emit(id, false);
    for (uint32_t id : ambSounds) emit(id, true);
    // the login / character screens' music (stage 5.31): the SoundEntries row playing a GlueScreenMusic file (the original
    // GlueXML plays it by name; "login" / "main theme" rows first), else the file itself. "G vol*100 files" in music.txt.
    if (so.music) {
        int best = -1, bestScore = -1;
        for (uint32_t r = 0; r < se.records; ++r) {
            SoundRow s = soundRow(se, r);
            bool glue = false;
            for (const std::string& f : s.files) glue |= lower(f).find("gluescreenmusic") != std::string::npos;
            if (!glue) continue;
            const std::string nm = lower(se.str(r, 2));
            int score = 1 + (nm.find("login") != std::string::npos ? 4 : 0) + (nm.find("main") != std::string::npos ? 2 : 0);
            for (const std::string& f : s.files) score += lower(f).find("main_theme") != std::string::npos ? 2 : 0;
            if (score > bestScore) { bestScore = score; best = (int)r; }
        }
        SoundRow g;
        if (best >= 0) g = soundRow(se, (uint32_t)best);
        else g.files = {"Sound\\Music\\GlueScreenMusic\\wow_main_theme.mp3"};
        std::string line = "G\t" + std::to_string(std::lround(g.volume * 100));
        int k = 0;
        for (const std::string& path : g.files) {
            std::vector<uint8_t> f;
            if (!data.read(path, f) || f.size() < 1024) continue;
            const std::string name = "glue_" + std::to_string(k++) + ".mp3";
            if (!writeFile(base + "music/" + name, f.data(), f.size())) continue;
            bytes += f.size();
            line += "\t" + name;
        }
        if (k) txt += line + "\n";
        std::printf("  login screen music: %s\n", k ? (best >= 0 ? se.str((uint32_t)best, 2).c_str() : "wow_main_theme.mp3") : "not found");
    }
    progressDone("music: %zu music + %zu ambience sounds, %zu files, %.0f MB", musicSounds.size(), ambSounds.size(), files,
                 bytes / 1048576.0);

    // ---- which area each terrain chunk belongs to
    DbcTable maps;
    if (maps.load(data, "Map.dbc")) {
        for (uint32_t r = 0; r < maps.records; ++r) {
            uint32_t id = maps.u32(r, 0);
            std::string dir = maps.str(r, 1);
            if (dir.empty()) continue;
            size_t tiles = 0;
            if (so.areas && exportAreas(data, id, dir, base + "areas_" + std::to_string(id) + ".bin", &tiles))
                progressDone("areas: %s (map %u) %zu tiles", dir.c_str(), id, tiles);
            if (mapFirstArea.count(id))   // fallback where the grid has nothing (dungeons, inside buildings)
                txt += "M\t" + std::to_string(id) + "\t" + std::to_string(mapFirstArea[id]) + "\n";
        }
    }
    FILE* f = std::fopen((base + "music.txt").c_str(), "wb");
    if (!f) return false;
    std::fwrite(txt.data(), 1, txt.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace w3d
