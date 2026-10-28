// all.cpp — w3dconv --all (stage 5.27): the interface, the sound and every map of Map.dbc in one run, with one shared
// asset pool. Map packs (map<ID>.w3dp) hold their tiles only; textures, models, buildings, creatures, players and
// items are written once into common.w3dp (+ common1.w3dp ... when the pool passes the FAT32 limit). The files share
// a buildId (PINF entries): the 3DS refuses a map pack whose pool is from another conversion.
#include "conv.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <random>

namespace w3d {

namespace {

// ADT tiles of a map: the WDT's MAIN table (flag 1 = the tile exists), checked against the files themselves.
std::vector<std::pair<int, int>> mapTiles(const DataDir& data, const std::string& dir) {
    std::vector<std::pair<int, int>> out;
    std::vector<uint8_t> wdt;
    std::vector<bool> cand(64 * 64, false);
    bool haveMain = false;
    if (data.read("World\\Maps\\" + dir + "\\" + dir + ".wdt", wdt)) {
        for (size_t p = 0; p + 8 <= wdt.size();) {
            uint32_t len;
            std::memcpy(&len, &wdt[p + 4], 4);
            if (len > wdt.size() - p - 8) break;
            if (!std::memcmp(&wdt[p], "NIAM", 4) && len >= 64 * 64 * 8) {
                haveMain = true;
                for (int i = 0; i < 64 * 64; ++i) {
                    uint32_t f;
                    std::memcpy(&f, &wdt[p + 8 + (size_t)i * 8], 4);
                    if (f & 1) { cand[i] = true; cand[(i % 64) * 64 + i / 64] = true; }   // either axis order: the files decide
                }
            }
            p += 8 + len;
        }
    }
    std::vector<uint8_t> tmp;
    for (int tx = 0; tx < 64; ++tx)
        for (int ty = 0; ty < 64; ++ty) {
            if (haveMain && !cand[ty * 64 + tx]) continue;
            char path[256];
            std::snprintf(path, sizeof path, "World\\Maps\\%s\\%s_%d_%d.adt", dir.c_str(), dir.c_str(), tx, ty);
            if (data.read(path, tmp)) out.push_back({tx, ty});
        }
    return out;
}

void resetMapState(ConvStats& st) {
    st.seenDoodadUid.clear();
    st.seenWmoUid.clear();
    st.collTris.clear();
    st.liquids.clear();
}

}  // namespace

int convertAll(const Options& oIn, const DataDir& data, const AllOptions& ao, UiOptions uo, SoundOptions so) {
    auto t0 = std::chrono::steady_clock::now();
    std::string folder = ao.folder.empty() ? std::string(".") : ao.folder;
    if (folder.back() != '/' && folder.back() != '\\') folder += '/';
    Options o = oIn;
    o.creatures = o.players = o.transports = true;
    o.dungeons = false;
    int problems = 0;

    // ---- 1. interface pack and the text files
    if (ao.ui) {
        std::printf("== interface (ui.w3dp) and texts\n");
        uo.out = folder + "ui.w3dp";
        if (!convertUi(uo, data, o.verbose)) ++problems;
    }
    {
        exportSpellNames(data, folder + "spells.txt", o.verbose);
        exportFactions(data, folder + "factions.txt", o.verbose);
        exportTalents(data, folder + "talents.txt", o.verbose);
        exportTaxi(data, folder + "taxi.txt", o.verbose);
        exportLocks(data, folder + "locks.txt", o.verbose);
        exportAreaTriggers(data, folder + "areatriggers.txt", o.verbose);
        exportEmotes(data, folder + "emotes.txt", o.verbose);
        exportGlueTexts(data, folder + "glue.txt", o.verbose);
    }
    // ---- 2. sound
    if (ao.sound) {
        std::printf("== sound\n");
        so.outDir = folder + "sound";
        if (!convertSound(so, data, o.verbose)) ++problems;
    }

    // ---- 3. the maps, their assets into the pool
    PackWriter pool;
    if (!pool.setSpillPath(folder + "common.spill")) {
        std::fprintf(stderr, "cannot create %scommon.spill\n", folder.c_str());
        return 3;
    }
    std::random_device rd;
    const uint32_t buildId = ((uint32_t)rd() ^ (uint32_t)std::chrono::system_clock::now().time_since_epoch().count()) | 1u;
    W3dPackInfo pinfo{};
    pinfo.buildId = buildId;
    pinfo.kind = W3D_PACK_POOL;
    pool.setInfo(pinfo);
    ConvStats st;
    struct Done { uint32_t id; std::string dir, file; size_t tiles; uint64_t bytes; };
    std::vector<Done> done;
    DbcTable maps;
    if (!maps.load(data, "Map.dbc") || maps.fields < 2) {
        std::fputs("--all: Map.dbc not found\n", stderr);
        return 2;
    }
    std::vector<std::pair<uint32_t, std::string>> list;
    for (uint32_t r = 0; r < maps.records; ++r) {
        const uint32_t id = maps.u32(r, 0);
        std::string dir = maps.str(r, 1);
        if (dir.empty()) continue;
        if (!ao.maps.empty() && !ao.maps.count(id)) continue;
        if (ao.maps.empty() && ao.skip.count(id)) {
            std::printf("  map %u (%s): skipped (test / development map; --maps lists the maps to take)\n", id, dir.c_str());
            continue;
        }
        list.push_back({id, dir});
    }
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {   // the continents first
        auto rank = [](uint32_t id) { return id == 0 ? 0 : id == 1 ? 1 : 2; };
        return rank(a.first) != rank(b.first) ? rank(a.first) < rank(b.first) : a.first < b.first;
    });
    for (const auto& [id, dir] : list) {
        trace("map %u (%s)", id, dir.c_str());
        Options m = o;
        m.map = dir;
        m.tileX0 = m.tileY0 = 0;
        m.tileX1 = m.tileY1 = 63;
        resetMapState(st);
        PackWriter mp;
        mp.setPool(&pool);
        const std::string file = folder + "map" + std::to_string(id) + ".w3dp";
        if (!mp.setSpillPath(file + ".spill")) std::fprintf(stderr, "note: cannot create %s.spill\n", file.c_str());
        W3dPackInfo mi{};
        mi.buildId = buildId;
        mi.kind = W3D_PACK_MAP;
        mi.mapId = id;
        mp.setInfo(mi);
        const size_t tilesBefore = st.tiles;
        const auto mapStart = std::chrono::steady_clock::now();
        if (isDungeonMap(data, dir)) {
            std::printf("== map %u (%s): an instance (one building)\n", id, dir.c_str());
            if (!convertDungeon(m, data, mp, st)) continue;
        } else {
            std::vector<std::pair<int, int>> tiles = mapTiles(data, dir);
            if (tiles.empty()) {
                std::printf("  map %u (%s): no tiles in this client, skipped\n", id, dir.c_str());
                continue;
            }
            std::printf("== map %u (%s): %zu tiles\n", id, dir.c_str(), tiles.size());
            size_t k = 0;
            for (auto [tx, ty] : tiles) {
                m.tileX = tx;
                m.tileY = ty;
                ++k;
                progress(true, "map %u %s: tile %zu/%zu (%d_%d), %zu models, %zu buildings, %zu textures so far", id, dir.c_str(), k,
                         tiles.size(), tx, ty, st.modelsOk, st.wmoOk, st.texturesOk);
                if (!convertTile(m, data, mp, st)) ++problems;
            }
        }
        trace("map %u (%s): collision", id, dir.c_str());
        emitCollision(m, mp, st);
        trace("map %u (%s): liquids", id, dir.c_str());
        emitLiquids(m, &data, mp, st);
        trace("map %u (%s): writing", id, dir.c_str());
        if (st.tiles == tilesBefore) {
            std::printf("  map %u (%s): nothing converted\n", id, dir.c_str());
            continue;
        }
        if (!mp.write(file)) {
            std::fprintf(stderr, "cannot write %s\n", file.c_str());
            ++problems;
            continue;
        }
        FILE* f = std::fopen(file.c_str(), "rb");
        uint64_t sz = 0;
        if (f) { std::fseek(f, 0, SEEK_END); sz = (uint64_t)std::ftell(f); std::fclose(f); }
        done.push_back({id, dir, file, st.tiles - tilesBefore, sz});
        std::printf("  map %u (%s) took %.0f s\n", id, dir.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - mapStart).count());
        progressDone("map %u (%s): %zu tiles, %.1f MB; pool so far %.1f MB", id, dir.c_str(), st.tiles - tilesBefore,
                     sz / 1048576.0, pool.storedBytes() / 1048576.0);
    }
    if (done.empty()) {
        std::fputs("--all: no map converted\n", stderr);
        return 2;
    }

    // ---- 4. creatures, players and their items, ships, game objects: once, into the pool
    std::printf("== creatures, players, items, ships, game objects\n");
    trace("creatures, players, items, ships, game objects");
    resetMapState(st);
    convertCreatures(o, data, pool, st);
    convertGlueScenes(o, data, pool, st);   // stage 5.29: login / character screen scenes
    if (o.spellFx) convertSpellEffects(o, data, pool, st);   // stage 5.33: what spells look like
    exportTransports(data, folder + "transports.txt", o.verbose);

    // ---- 5. the pool
    trace("writing the pool");
    progress(true, "writing the pool (%.1f MB) ...", pool.storedBytes() / 1048576.0);
    const int parts = pool.writeParts(folder + "common.w3dp", ao.partBytes);
    if (!parts) {
        std::fprintf(stderr, "cannot write the pool %scommon.w3dp\n", folder.c_str());
        return 3;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint64_t mapBytes = 0;
    for (const Done& d : done) mapBytes += d.bytes;
    progressDone("all: %zu maps, pool %.1f MB in %d part(s), maps %.1f MB, in %d min %02d s", done.size(), pool.storedBytes() / 1048576.0,
                 parts, mapBytes / 1048576.0, (int)secs / 60, (int)secs % 60);
    std::printf("all: build %08x, %zu maps:\n", buildId, done.size());
    {   // 5.35h: where the time went (texture encoding runs on a pool of threads: its CPU seconds overlap the rest)
        const TexTiming& tt = textureTiming();
        std::printf("  timing: %.0f s total; game textures %zu: read+decode %.0f s, encode %.0f s of CPU (on %u threads); creature models %.0f s\n",
                    secs, tt.count, tt.readDecodeSec, tt.encodeSec, std::thread::hardware_concurrency(), st.creatureSec);
    }
    for (const Done& d : done)
        std::printf("  map%-4u %-24s %5zu tiles %9.1f MB\n", d.id, d.dir.c_str(), d.tiles, d.bytes / 1048576.0);
    std::printf("  pool: %zu textures, %zu models, %zu creature models, %zu displays; %.1f MB; %zu name collisions resolved\n",
                pool.count(W3D_ENTRY_TEXTURE), pool.count(W3D_ENTRY_MODEL), pool.count(W3D_ENTRY_CREATURE),
                pool.count(W3D_ENTRY_DISPLAY), pool.storedBytes() / 1048576.0, pool.keyCollisions());
    std::printf("  copy everything in %s to sdmc:/3ds/w3dproto/ (map0.w3dp is the Eastern Kingdoms, map1.w3dp Kalimdor)\n",
                folder.c_str());
    std::printf("== checking the packs\n");
    for (const Done& d : done) {
        progress(true, "checking %s ...", d.file.c_str());
        problems += printPackInfo(d.file) != 0;
    }   // checks their references into the pool too
    return problems ? 4 : 0;
}

}  // namespace w3d
