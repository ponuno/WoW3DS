// w3dfdata — WoW Forever data probe on the PC (step 1 of the Forever port, see docs/FOREVER_RU.md): opens the client's CASC
// storage with CascLib, reads DB2 tables (Map, ...), the WDT of a map with its FileDataIDs, one ADT tile (root, _obj0, _tex0)
// and one model of it, and writes what it found - the formats the converter will have to read - into a report. Files can be
// dumped for a closer look.
//   w3dfdata <WoW folder> [--product wow_classic] [--map Azeroth] [--tile 32 48] [--out report.txt] [--dump folder] [--listfile forever_wdt_names.csv]
// (the maps' WDT files have no names in the client's storage: forever_wdt_names.csv, 'FileDataID;path' lines of the community listfile,
// gives their FileDataIDs; the file next to the program or in the current folder is used by default)
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "CascLib.h"
#include "fdata.hpp"

using namespace wf;

static FILE* g_out = nullptr;
static void line(const std::string& s) {
    printf("%s\n", s.c_str());
    fflush(stdout);
    if (g_out) { fprintf(g_out, "%s\n", s.c_str()); fflush(g_out); }
}

static HANDLE g_st = nullptr;
static std::string g_dump;

static bool readFile(const void* nameOrId, DWORD how, Bytes& out, std::string& err) {
    HANDLE f = nullptr;
    if (!CascOpenFile(g_st, nameOrId, CASC_LOCALE_ALL, how | CASC_OVERCOME_ENCRYPTED, &f)) {
        err = "not found (CascLib error " + std::to_string(GetCascError()) + ")";
        return false;
    }
    ULONGLONG size = 0;
    CascGetFileSize64(f, &size);
    if (size > (512ull << 20)) { CascCloseFile(f); err = "too big"; return false; }
    out.resize((size_t)size);
    DWORD got = 0;
    const bool ok = size == 0 || CascReadFile(f, out.data(), (DWORD)size, &got);
    CascCloseFile(f);
    if (!ok || got != size) { err = "read failed (" + std::to_string(got) + " of " + std::to_string(size) + " bytes, error " + std::to_string(GetCascError()) + ")"; return false; }
    return true;
}
static bool byName(const std::string& name, Bytes& out, std::string& err) { return readFile(name.c_str(), CASC_OPEN_BY_NAME, out, err); }
static bool byId(uint32_t id, Bytes& out, std::string& err) { return readFile(CASC_FILE_DATA_ID(id), CASC_OPEN_BY_FILEID, out, err); }
static void dump(const std::string& name, const Bytes& b) {
    if (g_dump.empty()) return;
    std::string p = g_dump + "/" + name;
    if (FILE* f = fopen(p.c_str(), "wb")) { fwrite(b.data(), 1, b.size(), f); fclose(f); }
}

// The DB2 tables the client needs first, by name (DB2 names have hashes in the root) and the FileDataID they had in Classic.
struct Table { const char* name; uint32_t fdid; };
static const Table kTables[] = {
    {"Map", 1349477}, {"AreaTable", 1353545}, {"ChrRaces", 1305311}, {"ChrClasses", 1361031}, {"CreatureDisplayInfo", 1108759},
    {"CreatureModelData", 1365368}, {"ItemDisplayInfo", 1266429}, {"Spell", 1140089}, {"SpellName", 1990283}, {"Light", 1375579},
    {"WorldMapArea", 0}, {"UiMap", 1957206}, {"AnimationData", 1375432}, {"SoundKitEntry", 1237434},
};

static std::vector<std::string> listProducts(const std::string& dir) {
    std::vector<std::string> found;
    FILE* f = fopen((dir + "/.build.info").c_str(), "rb");
    if (!f) { line("  (no .build.info in " + dir + ")"); return found; }
    char buf[4096];
    std::vector<std::string> lines;
    while (fgets(buf, sizeof buf, f)) lines.push_back(buf);
    fclose(f);
    if (lines.empty()) return found;
    // header "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|...|Product!STRING:0|Version!STRING:0"
    auto split = [](const std::string& s) { std::vector<std::string> v; std::string c; for (char ch : s) { if (ch == '|' || ch == '\n' || ch == '\r') { v.push_back(c); c.clear(); } else c += ch; } if (!c.empty()) v.push_back(c); return v; };
    std::vector<std::string> head = split(lines[0]);
    int prodCol = -1, verCol = -1, activeCol = -1;
    for (size_t i = 0; i < head.size(); ++i) {
        if (head[i].rfind("Product!", 0) == 0) prodCol = (int)i;
        if (head[i].rfind("Version!", 0) == 0) verCol = (int)i;
        if (head[i].rfind("Active!", 0) == 0) activeCol = (int)i;
    }
    for (size_t l = 1; l < lines.size(); ++l) {
        std::vector<std::string> v = split(lines[l]);
        auto col = [&](int c) { return c >= 0 && c < (int)v.size() ? v[(size_t)c] : std::string("?"); };
        line("  product in .build.info: " + col(prodCol) + "  version " + col(verCol) + (col(activeCol) == "1" ? "  (active)" : ""));
        if (col(prodCol) != "?") found.push_back(col(prodCol));
    }
    return found;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: w3dfdata <WoW folder> [--product wow_classic] [--map Azeroth] [--tile X Y] [--out report.txt] [--dump folder] [--listfile forever_wdt_names.csv]\n"
                        "  <WoW folder> = the folder with .build.info (e.g. \"C:\\Program Files (x86)\\World of Warcraft\")\n");
        return 2;
    }
    std::string dir = argv[1], product, mapDir, outPath = "forever_data.txt", listPath, idList;
    int tileX = -1, tileY = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--product") product = next();
        else if (a == "--map") mapDir = next();
        else if (a == "--tile") { tileX = atoi(next()); tileY = atoi(next()); }
        else if (a == "--out") outPath = next();
        else if (a == "--dump") g_dump = next();
        else if (a == "--listfile") listPath = next();
        else if (a == "--ids") idList = next();
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    g_out = fopen(outPath.c_str(), "w");
    line("w3dfdata: WoW Forever data probe, " + dir);
    const std::vector<std::string> products = listProducts(dir);
    // ---- storage
    std::vector<std::string> tries;
    if (!product.empty()) tries.push_back(dir + ":" + product);
    for (const std::string& p : products) if (product.empty()) tries.push_back(dir + ":" + p);   // the installed ones first
    if (product.empty()) for (const char* p : {"wow_classic", "wow_classic_beta", "wow_classic_ptr", "wow_forever", "wow_classic_era", "wow"}) tries.push_back(dir + ":" + p);
    tries.push_back(dir);
    std::string used;
    for (const std::string& t : tries) {
        if (CascOpenStorage(t.c_str(), 0, &g_st)) { used = t; break; }
        g_st = nullptr;
    }
    if (!g_st) { line("FAILED: CascLib cannot open the storage in " + dir + " (last error " + std::to_string(GetCascError()) + "); try --product <name from .build.info>"); return 1; }
    CASC_STORAGE_PRODUCT prod{};
    CascGetStorageInfo(g_st, CascStorageProduct, &prod, sizeof prod, nullptr);
    DWORD files = 0, features = 0;
    CascGetStorageInfo(g_st, CascStorageTotalFileCount, &files, sizeof files, nullptr);
    CascGetStorageInfo(g_st, CascStorageFeatures, &features, sizeof features, nullptr);
    char b[512];
    snprintf(b, sizeof b, "storage: %s -> product '%s' build %u, %u files, features 0x%x%s", used.c_str(), prod.szCodeName, (unsigned)prod.BuildNumber,
             (unsigned)files, (unsigned)features, (features & CASC_FEATURE_FILE_NAMES) ? " (names)" : "");
    line(b);
    std::string err;
    if (!idList.empty()) {   // --ids a,b,c: how each FileDataID opens (plain / with missing-key blocks zero-filled), size, how much of it is real data
        line("[ids]");
        size_t at = 0;
        while (at < idList.size()) {
            size_t e = idList.find(',', at); if (e == std::string::npos) e = idList.size();
            const uint32_t id = (uint32_t)strtoul(idList.substr(at, e - at).c_str(), nullptr, 10);
            at = e + 1;
            if (!id) continue;
            std::string r = "  " + std::to_string(id) + ": ";
            HANDLE f = nullptr;
            if (!CascOpenFile(g_st, CASC_FILE_DATA_ID(id), CASC_LOCALE_ALL, CASC_OPEN_BY_FILEID, &f)) r += "plain open failed (error " + std::to_string(GetCascError()) + "); ";
            else {
                ULONGLONG sz = 0; CascGetFileSize64(f, &sz);
                Bytes buf((size_t)(sz < (64u << 20) ? sz : 0)); DWORD got = 0;
                const bool ok = sz == 0 || CascReadFile(f, buf.data(), (DWORD)buf.size(), &got);
                r += "plain open ok, size " + std::to_string(sz) + ", read " + (ok ? "ok" : "failed (error " + std::to_string(GetCascError()) + ")") + "; ";
                CascCloseFile(f);
            }
            f = nullptr;
            if (!CascOpenFile(g_st, CASC_FILE_DATA_ID(id), CASC_LOCALE_ALL, CASC_OPEN_BY_FILEID | CASC_OVERCOME_ENCRYPTED, &f)) r += "tolerant open failed (error " + std::to_string(GetCascError()) + ")";
            else {
                ULONGLONG sz = 0; CascGetFileSize64(f, &sz);
                Bytes buf((size_t)(sz < (64u << 20) ? sz : 0)); DWORD got = 0;
                const bool ok = sz == 0 || CascReadFile(f, buf.data(), (DWORD)buf.size(), &got);
                size_t nz = 0; for (uint8_t c : buf) nz += c != 0;
                r += "tolerant: size " + std::to_string(sz) + ", read " + (ok ? "ok" : "failed") + ", " + std::to_string(nz) + " non-zero bytes, magic " + std::string((const char*)buf.data(), buf.size() >= 4 ? 4 : 0);
                CascCloseFile(f);
            }
            line(r);
        }
        line("DONE.");
        if (g_out) fclose(g_out);
        return 0;
    }
    // ---- DB2 tables
    line("[DB2]");
    Db2 mapDb;
    for (const Table& t : kTables) {
        Bytes data;
        const std::string name = std::string("DBFilesClient\\") + t.name + ".db2";
        bool ok = byName(name, data, err);
        std::string how = "by name";
        if (!ok && t.fdid) { ok = byId(t.fdid, data, err); how = "by FileDataID " + std::to_string(t.fdid); }
        if (!ok) { line("  " + std::string(t.name) + ": " + err); continue; }
        dump(std::string(t.name) + ".db2", data);
        Db2 db;
        if (!db.load(data, err)) { line("  " + std::string(t.name) + " (" + how + ", " + std::to_string(data.size()) + " bytes): " + err); continue; }
        line("  " + std::string(t.name) + " (" + how + "): " + db.describe() + (db.skipped.empty() ? "" : "; " + db.skipped));
        if (std::string(t.name) == "Map") mapDb = std::move(db);
    }
    // the maps: id and the strings of each record (directory, name ...)
    std::string firstDir;
    if (!mapDb.records.empty()) {
        line("[maps] id: strings of the record");
        int shown = 0;
        for (const Db2::Record& r : mapDb.records) {
            std::string s;
            for (size_t f = 0; f < r.values.size(); ++f) {
                std::string str = mapDb.stringAt(r, f);
                if (!str.empty()) s += " | " + str;
            }
            if (s.empty()) continue;
            if (firstDir.empty()) firstDir = mapDb.stringAt(r, 0);
            if (shown++ < 80) line("  " + std::to_string(r.id) + s);
        }
        line("  (" + std::to_string(mapDb.records.size()) + " maps)");
    }
    if (mapDir.empty()) mapDir = "Azeroth";
    // ---- the names of the WDT files (FileDataIDs from the listfile subset)
    auto lower = [](std::string x) { for (char& c : x) c = (char)tolower((unsigned char)c); return x; };
    std::map<std::string, uint32_t> wdtIds;   // lowercase directory -> FileDataID
    {
        std::vector<std::string> cands;
        if (!listPath.empty()) cands.push_back(listPath);
        cands.push_back("forever_wdt_names.csv");
        cands.push_back(std::string(argv[0]).substr(0, std::string(argv[0]).find_last_of("/\\") == std::string::npos ? 0 : std::string(argv[0]).find_last_of("/\\") + 1) + "forever_wdt_names.csv");
        for (const std::string& c : cands) {
            std::ifstream f(c);
            if (!f) continue;
            std::string ln;
            while (std::getline(f, ln)) {
                const size_t sc = ln.find(';');
                if (sc == std::string::npos) continue;
                std::string path = lower(ln.substr(sc + 1));
                while (!path.empty() && (path.back() == '\r' || path.back() == '\n')) path.pop_back();
                const std::string pre = "world/maps/";
                if (path.compare(0, pre.size(), pre) != 0 || path.size() < 5 || path.compare(path.size() - 4, 4, ".wdt") != 0) continue;
                const size_t sl = path.find('/', pre.size());
                if (sl == std::string::npos) continue;
                wdtIds[path.substr(pre.size(), sl - pre.size())] = (uint32_t)strtoul(ln.c_str(), nullptr, 10);
            }
            line("[names] " + std::to_string(wdtIds.size()) + " map WDT names from " + c);
            break;
        }
        if (wdtIds.empty()) line("[names] no forever_wdt_names.csv: the maps' WDT files cannot be found by name (they have none in the storage)");
    }
    if (!wdtIds.empty() && !mapDb.records.empty()) {
        line("[maps in this client] id directory: WDT FileDataID, present?");
        int have = 0, total = 0;
        for (const Db2::Record& r : mapDb.records) {
            const std::string d = mapDb.stringAt(r, 0);
            auto it = wdtIds.find(lower(d));
            if (d.empty() || it == wdtIds.end()) continue;
            ++total;
            Bytes w2; std::string e2;
            const bool ok = byId(it->second, w2, e2);
            have += ok;
            snprintf(b, sizeof b, "  %u %s: %u %s", r.id, d.c_str(), it->second, ok ? ("present, " + std::to_string(w2.size()) + " bytes").c_str() : ("absent (" + e2 + ")").c_str());
            line(b);
        }
        line("  " + std::to_string(have) + " of " + std::to_string(total) + " maps have their WDT in this client");
    }
    // ---- WDT of the map
    line("[WDT] World\\Maps\\" + mapDir + "\\" + mapDir + ".wdt");
    Bytes wdt;
    bool gotWdt = byName("World\\Maps\\" + mapDir + "\\" + mapDir + ".wdt", wdt, err);
    if (!gotWdt) {
        auto it = wdtIds.find(lower(mapDir));
        if (it != wdtIds.end()) { gotWdt = byId(it->second, wdt, err); line("  by FileDataID " + std::to_string(it->second) + (gotWdt ? ": found" : ": " + err)); }
    }
    if (!gotWdt) {
        line("  " + err + (firstDir.empty() ? "" : " (the first map's directory in Map.db2 is '" + firstDir + "': try --map " + firstDir + ")"));
    } else {
        dump(mapDir + ".wdt", wdt);
        line("  chunks: " + chunkSummary(chunks(wdt)));
        std::vector<WdtTile> tiles;
        uint32_t mphd = 0;
        if (!readWdt(wdt, tiles, mphd, err)) line("  " + err);
        else {
            snprintf(b, sizeof b, "  MPHD flags 0x%x, %zu tiles with FileDataIDs", mphd, tiles.size());
            line(b);
            const WdtTile* pick = nullptr;
            for (const WdtTile& t : tiles) if (t.x == tileX && t.y == tileY) pick = &t;
            if (!pick && !tiles.empty()) pick = &tiles[tiles.size() / 2];
            if (pick) {
                snprintf(b, sizeof b, "  tile %d_%d: root %u, obj0 %u, obj1 %u, tex0 %u, lod %u, map tex %u, map normal %u, minimap %u", pick->x, pick->y,
                         pick->fdid[0], pick->fdid[1], pick->fdid[2], pick->fdid[3], pick->fdid[4], pick->fdid[5], pick->fdid[6], pick->fdid[7]);
                line(b);
                const char* kind[4] = {"root", "obj0", "obj1", "tex0"};
                const int idx[4] = {0, 1, 2, 3};
                uint32_t firstModel = 0, firstWmo = 0;
                for (int k = 0; k < 4; ++k) {
                    Bytes adt;
                    if (!pick->fdid[idx[k]]) continue;
                    if (!byId(pick->fdid[idx[k]], adt, err)) { line(std::string("  ADT ") + kind[k] + ": " + err); continue; }
                    dump(mapDir + "_" + std::to_string(pick->x) + "_" + std::to_string(pick->y) + "_" + kind[k] + ".adt", adt);
                    std::vector<Chunk> cs = chunks(adt);
                    line(std::string("  ADT ") + kind[k] + " (" + std::to_string(adt.size()) + " bytes): " + chunkSummary(cs));
                    for (const Chunk& c : cs) {
                        if (c.id == "MDDF" && c.size >= 36 && !firstModel) {   // nameId, uniqueId, pos, rot, scale, flags (0x40: nameId is a FileDataID)
                            uint32_t nameId, fl;
                            memcpy(&nameId, adt.data() + c.offset, 4);
                            uint16_t f16;
                            memcpy(&f16, adt.data() + c.offset + 34, 2);
                            fl = f16;
                            line("    first doodad: nameId " + std::to_string(nameId) + ((fl & 0x40) ? " (a FileDataID)" : " (an index into MMID / MMDX)") +
                                 ", " + std::to_string(c.size / 36) + " doodads");
                            if (fl & 0x40) firstModel = nameId;
                        }
                        if (c.id == "MODF" && c.size >= 64 && !firstWmo) {
                            uint32_t nameId;
                            uint16_t f16;
                            memcpy(&nameId, adt.data() + c.offset, 4);
                            memcpy(&f16, adt.data() + c.offset + 56, 2);
                            line("    first building: nameId " + std::to_string(nameId) + ((f16 & 0x8) ? " (a FileDataID)" : " (an index into MWID / MWMO)") +
                                 ", " + std::to_string(c.size / 64) + " buildings");
                            if (f16 & 0x8) firstWmo = nameId;
                        }
                        if (c.id == "MDID" || c.id == "MHID") line("    " + c.id + ": " + std::to_string(c.size / 4) + " texture FileDataIDs");
                    }
                }
                if (firstModel) {
                    Bytes m2;
                    if (!byId(firstModel, m2, err)) line("  model " + std::to_string(firstModel) + ": " + err);
                    else {
                        dump("model_" + std::to_string(firstModel) + ".m2", m2);
                        M2Info mi;
                        if (!readM2(m2, mi, err)) line("  model " + std::to_string(firstModel) + ": " + err + "; chunks " + mi.chunks);
                        else {
                            snprintf(b, sizeof b, "  model %u '%s': version %u, %u vertices, %u bones, %u sequences, %u textures, %u views; %zu skin, %zu texture, %zu anim file ids",
                                     firstModel, mi.name.c_str(), mi.version, mi.vertices, mi.bones, mi.sequences, mi.textures, mi.views, mi.skinFileIds.size(),
                                     mi.textureFileIds.size(), mi.animFileIds.size());
                            line(b);
                            line("    chunks: " + mi.chunks);
                            if (!mi.skinFileIds.empty()) {
                                Bytes skin;
                                if (byId(mi.skinFileIds[0], skin, err)) { dump("model_" + std::to_string(firstModel) + "_00.skin", skin); line("    skin " + std::to_string(mi.skinFileIds[0]) + ": " + std::to_string(skin.size()) + " bytes, magic " + std::string((const char*)skin.data(), skin.size() >= 4 ? 4 : 0)); }
                                else line("    skin " + std::to_string(mi.skinFileIds[0]) + ": " + err);
                            }
                            if (!mi.textureFileIds.empty() && mi.textureFileIds[0]) {
                                Bytes tex;
                                if (byId(mi.textureFileIds[0], tex, err)) line("    texture " + std::to_string(mi.textureFileIds[0]) + ": " + std::to_string(tex.size()) + " bytes, magic " + std::string((const char*)tex.data(), tex.size() >= 4 ? 4 : 0));
                                else line("    texture " + std::to_string(mi.textureFileIds[0]) + ": " + err);
                            }
                        }
                    }
                }
                if (firstWmo) {
                    Bytes wmo;
                    if (!byId(firstWmo, wmo, err)) line("  building " + std::to_string(firstWmo) + ": " + err);
                    else { dump("building_" + std::to_string(firstWmo) + ".wmo", wmo); {
                        line("  building " + std::to_string(firstWmo) + " root: " + chunkSummary(chunks(wmo)));
                        std::vector<uint32_t> gf;
                        for (const Chunk& c : chunks(wmo)) if (c.id == "GFID") for (size_t i = 0; i + 4 <= c.size; i += 4) { uint32_t v = 0; memcpy(&v, wmo.data() + c.offset + i, 4); gf.push_back(v); }
                        line("  GFID: " + std::to_string(gf.size()) + " ids, first " + (gf.empty() ? std::string("-") : std::to_string(gf[0]) + " " + std::to_string(gf.size() > 1 ? gf[1] : 0) + " " + std::to_string(gf.size() > 2 ? gf[2] : 0) + " " + std::to_string(gf.size() > 3 ? gf[3] : 0) + " " + std::to_string(gf.size() > 4 ? gf[4] : 0)));
                        for (size_t k = 0; k < 5 && k < gf.size(); ++k) {
                            Bytes g; std::string e2;
                            if (!byId(gf[k], g, e2)) { line("  group file GFID[" + std::to_string(k) + "] " + std::to_string(gf[k]) + ": " + e2); continue; }
                            dump("building_" + std::to_string(firstWmo) + "_g" + std::to_string(k) + ".wmo", g);
                            line("  group file GFID[" + std::to_string(k) + "] " + std::to_string(gf[k]) + ": " + std::to_string(g.size()) + " bytes: " + chunkSummary(chunks(g)));
                        }
                        // the first material's texture
                        for (const Chunk& c : chunks(wmo)) if (c.id == "MOMT" && c.size >= 64) {
                            uint32_t tid = 0; memcpy(&tid, wmo.data() + c.offset + 12, 4);
                            Bytes t; std::string e3;
                            if (byId(tid, t, e3)) { dump("building_tex_" + std::to_string(tid) + ".blp", t); line("  material texture " + std::to_string(tid) + ": " + std::to_string(t.size()) + " bytes, magic " + std::string((const char*)t.data(), t.size() >= 4 ? 4 : 0)); }
                            else line("  material texture " + std::to_string(tid) + ": " + e3);
                            break;
                        }
                    } }
                }
            }
        }
    }
    CascCloseStorage(g_st);
    line("DONE. Report " + outPath + (g_dump.empty() ? "" : ", files in " + g_dump) + ".");
    if (g_out) fclose(g_out);
    return 0;
}
