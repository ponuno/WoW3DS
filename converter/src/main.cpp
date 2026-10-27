// w3dconv — offline converter: WoW 1.12 loose files -> W3DP pack for the 3DS client.
#include "conv.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace w3d;

static void usage() {
    std::puts(
        "usage:\n"
        "  w3dconv --data <WoW Data dir with .MPQ, or extracted dir> [--map Azeroth] [--tile 32 48] [-o out.w3dp] [options]\n"
        "  w3dconv --synthetic [--tiles 31 47 33 49] [-o synth.w3dp] [options]\n"
        "  w3dconv --info <pack.w3dp>\n"
        "  w3dconv --data <dir> --ui [-o ui.w3dp] [ui options]   interface pack: icons, skin, fonts, maps (+ text files)\n"
        "  w3dconv --data <dir> --sound [-o sound] [--sound-events F] [--no-music] [--no-ambience] [--no-areas]\n"
        "                        [--no-world-sounds] [--sound-variants N]\n"
        "                        sounds: interface + events (sfx.w3ds), zone music, ambience, zone grid\n"
        "options:\n"
        "  --tiles X0 Y0 X1 Y1   convert a rectangle of tiles into one pack (streaming test)\n"
        "  --grid 9|5|3          terrain vertices per chunk edge (default 9)\n"
        "  --terrain-tex N       max terrain texture size (default 64)\n"
        "  --model-tex N         max model texture size (default 64)\n"
        "  --format etc1|rgb565  texture format (default etc1; alpha -> etc1a4/rgba4)\n"
        "  --max-tris N          triangle budget per model (default 400)\n"
        "  --max-doodads N       limit doodad placements (default all; synthetic 600)\n"
        "  --no-wmo              skip buildings (WMO)\n"
        "  --no-wmo-doodads      buildings without the models placed inside them\n"
        "  --wmo-exterior-only   drop interior-only WMO groups\n"
        "  --wmo-tex N           max WMO texture size (default 128)\n"
        "  --wmo-tris N          triangle budget per WMO group (default 3000, 0 = all)\n"
        "  --dump-placements F   write every building group / doodad placement to text file F\n"
        "  --tex-quality Q       ETC1 encoder: low (default, fast), medium (~14x slower, +0.3 dB), high\n"
        "  --no-floors           no COLL entries at all (walking ignores building floors and walls)\n"
        "  --no-walls            COLL with floors only (walking passes through walls)\n"
        "  --no-doodad-coll      no collision of doodads (fences, tree trunks, model bridges)\n"
        "  --doodad-coll-tris N  collision triangle budget per doodad model (96; 0 = all)\n"
        "  --world               every tile of the map (0..63; missing ones are skipped), big packs go through a spill file\n"
        "  --creatures           add creature displays + skinned animated models (all of CreatureDisplayInfo)\n"
        "  --creature-displays L only these display ids (comma list), implies --creatures\n"
        "  --players             player race models + body texture layers (CharSections) for other players\n"
        "  --dungeons            every instance map of Map.dbc that is only a WDT with one building: map<ID>.w3dp into the folder of -o\n"
        "  --gameobjects [L]     models of ordinary game objects (pumpkins, banners, chests ...); L = path keywords, empty: all\n"
        "  --transports          ship and zeppelin models (GameObjectDisplayInfo) + transports.txt (paths, decks)\n"
        "  --transport-filter L  model path keywords of transports (default transport,zeppelin,ship,boat)\n"
        "  --no-items            players without worn items (armour on the body, helmets, shoulders, weapons)\n"
        "  --item-tris N         triangle budget per item model (default 300)\n"
        "  --item-tex N          max texture size of item models (default 128)\n"
        "  --creature-tex N      max creature texture size (default 128)\n"
        "  --creature-tris N     triangle budget per creature model (default 800)\n"
        "  --creature-anim-kb N  animation budget per creature model, KB (default 320; 0 = every animation)\n"
        "  --player-anim-kb N    animation budget per race model, KB (default 900)\n"
        "  --texts-only          only write spells.txt and factions.txt next to -o (seconds, no tiles)\n"
        "  -v                    verbose\n"
        "ui options:\n"
        "  --ui-skin F           sprite list (default: built in, same as ui_skin.txt)\n"
        "  --ui-font F           TTF for the normal / large text (default: the client's Fonts\\FRIZQT__.TTF)\n"
        "  --ui-font-fallback F  TTF for glyphs the font lacks, e.g. Cyrillic (repeatable; system Arial tried last)\n"
        "  --ui-font-small F     TTF for the small text (default: the 3DS built-in font)\n"
        "  --ui-font-sizes S N L pixel sizes (default 9 12 16)\n"
        "  --ui-icon N           icon size in the atlas, 16..64 (default 40)\n"
        "  --ui-map-width N      map picture width (default 384)\n"
        "  --ui-no-maps / --ui-no-icons");
}

static bool pow2ok(int v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; }

int main(int argc, char** argv) {
    Options o;
    std::string info;
    bool textsOnly = false, uiMode = false, soundMode = false;
    SoundOptions so;
    UiOptions uo;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); std::exit(1); }
            return argv[++i];
        };
        if (a == "--data") o.dataDir = next();
        else if (a == "--map") o.map = next();
        else if (a == "--tile") {
            o.tileX0 = o.tileX1 = std::atoi(next());
            o.tileY0 = o.tileY1 = std::atoi(next());
        } else if (a == "--tiles") {
            o.tileX0 = std::atoi(next()); o.tileY0 = std::atoi(next());
            o.tileX1 = std::atoi(next()); o.tileY1 = std::atoi(next());
            if (o.tileX0 > o.tileX1) std::swap(o.tileX0, o.tileX1);
            if (o.tileY0 > o.tileY1) std::swap(o.tileY0, o.tileY1);
        }
        else if (a == "--world") { o.tileX0 = o.tileY0 = 0; o.tileX1 = o.tileY1 = 63; }
        else if (a == "-o") o.out = next();
        else if (a == "--grid") o.grid = std::atoi(next());
        else if (a == "--terrain-tex") o.terrainTex = std::atoi(next());
        else if (a == "--model-tex") o.modelTex = std::atoi(next());
        else if (a == "--tex-quality") {
            std::string q = next();
            setEtcQuality(q == "high" ? 2 : q == "medium" ? 1 : 0);
        }
        else if (a == "--format") {
            std::string f = next();
            if (f == "etc1") o.texFormat = W3D_TEX_ETC1;
            else if (f == "rgb565") o.texFormat = W3D_TEX_RGB565;
            else { usage(); return 1; }
        }
        else if (a == "--max-tris") o.maxModelTris = std::atoi(next());
        else if (a == "--max-doodads") o.maxDoodads = std::atoi(next());
        else if (a == "--no-wmo") o.noWmo = true;
        else if (a == "--no-wmo-doodads") o.noWmoDoodads = true;
        else if (a == "--wmo-exterior-only") o.wmoExteriorOnly = true;
        else if (a == "--wmo-tex") o.wmoTex = std::atoi(next());
        else if (a == "--wmo-tris") o.wmoGroupTris = std::atoi(next());
        else if (a == "--dump-placements") o.dumpPlacements = next();
        else if (a == "--no-floors") o.noFloors = true;
        else if (a == "--no-walls") o.noWalls = true;
        else if (a == "--no-doodad-coll") o.noDoodadColl = true;
        else if (a == "--doodad-coll-tris") o.doodadCollTris = std::atoi(next());
        else if (a == "--creatures") o.creatures = true;
        else if (a == "--players") o.players = true;
        else if (a == "--no-items") o.noItems = true;
        else if (a == "--transports") o.transports = true;
        else if (a == "--dungeons") o.dungeons = true;
        else if (a == "--gameobjects") {
            o.gameobjects = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.goFilter = argv[++i];
        }
        else if (a == "--transport-filter") { o.transports = true; o.transportFilter = next(); }
        else if (a == "--item-tris") o.itemModelTris = std::atoi(next());
        else if (a == "--item-tex") o.itemTex = std::atoi(next());
        else if (a == "--creature-displays") { o.creatures = true; o.creatureDisplays = next(); }
        else if (a == "--creature-tex") o.creatureTex = std::atoi(next());
        else if (a == "--creature-tris") o.creatureTris = std::atoi(next());
        else if (a == "--creature-anim-kb") o.creatureAnimKb = std::atoi(next());
        else if (a == "--player-anim-kb") o.playerAnimKb = std::atoi(next());
        else if (a == "--synthetic") o.synthetic = true;
        else if (a == "--info") info = next();
        else if (a == "--texts-only") textsOnly = true;
        else if (a == "--ui") uiMode = true;
        else if (a == "--sound") soundMode = true;
        else if (a == "--sound-events") so.eventsFile = next();
        else if (a == "--no-music") so.music = false;
        else if (a == "--no-ambience") so.ambience = false;
        else if (a == "--no-areas") so.areas = false;
        else if (a == "--no-world-sounds") so.world = false;
        else if (a == "--sound-variants") so.worldVariants = std::max(1, std::min(10, std::atoi(next())));
        else if (a == "--ui-skin") uo.skinConfig = next();
        else if (a == "--ui-font") uo.font = next();
        else if (a == "--ui-font-fallback") uo.fallbackFonts.push_back(next());
        else if (a == "--ui-font-small") uo.fontSmall = next();
        else if (a == "--ui-font-sizes") { uo.smallPx = std::atoi(next()); uo.normalPx = std::atoi(next()); uo.largePx = std::atoi(next()); }
        else if (a == "--ui-icon") uo.iconSize = std::atoi(next());
        else if (a == "--ui-map-width") uo.mapWidth = std::atoi(next());
        else if (a == "--ui-no-maps") uo.maps = false;
        else if (a == "--ui-no-icons") uo.icons = false;
        else if (a == "-v") o.verbose = true;
        else { usage(); return 1; }
    }
    if (!info.empty()) return printPackInfo(info);
    if (o.grid != 9 && o.grid != 5 && o.grid != 3) { std::fputs("--grid must be 9, 5 or 3\n", stderr); return 1; }
    if (!pow2ok(o.terrainTex) || !pow2ok(o.modelTex)) { std::fputs("texture sizes must be pow2 in 8..1024\n", stderr); return 1; }

    auto t0 = std::chrono::steady_clock::now();
    PackWriter pack;
    ConvStats st;
    if (!o.dumpPlacements.empty()) st.dump = std::fopen(o.dumpPlacements.c_str(), "w");
    DataDir data;
    if (o.synthetic) {
        if (o.out == "test.w3dp") o.out = "synth.w3dp";
    } else {
        if (o.dataDir.empty()) { usage(); return 1; }
        progress(true, "opening the game data in %s ...", o.dataDir.c_str());
        if (!data.open(o.dataDir)) { std::fprintf(stderr, "no files under %s\n", o.dataDir.c_str()); return 1; }
    }
    auto textDir = [&]() {
        size_t cut = o.out.find_last_of("\\/");
        return cut == std::string::npos ? std::string() : o.out.substr(0, cut + 1);
    };
    if (soundMode) {
        if (o.synthetic) { std::fputs("--sound needs --data\n", stderr); return 1; }
        so.outDir = o.out == "test.w3dp" ? std::string("sound") : o.out;
        bool ok = convertSound(so, data, o.verbose);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("done in %.0f s\n", secs);
        return ok ? 0 : 4;
    }
    if (uiMode) {   // UI stage 7: the interface pack (and the text files beside it)
        if (o.synthetic) { std::fputs("--ui needs --data\n", stderr); return 1; }
        uo.out = o.out == "test.w3dp" ? std::string("ui.w3dp") : o.out;
        o.out = uo.out;
        bool ok = convertUi(uo, data, o.verbose);
        exportSpellNames(data, textDir() + "spells.txt", o.verbose);
        exportFactions(data, textDir() + "factions.txt", o.verbose);
        exportTalents(data, textDir() + "talents.txt", o.verbose);
        exportTaxi(data, textDir() + "taxi.txt", o.verbose);
        exportLocks(data, textDir() + "locks.txt", o.verbose);
        exportEmotes(data, textDir() + "emotes.txt", o.verbose);
        exportTransports(data, textDir() + "transports.txt", o.verbose);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("done in %.0f s\n", secs);
        return ok ? 0 : 4;
    }
    if (textsOnly) {   // stage 5.12: the text files of the 3DS client without converting anything else
        if (o.synthetic) { std::fputs("--texts-only needs --data\n", stderr); return 1; }
        bool spells = exportSpellNames(data, textDir() + "spells.txt", o.verbose);
        bool facts = exportFactions(data, textDir() + "factions.txt", o.verbose);
        exportTalents(data, textDir() + "talents.txt", o.verbose);
        exportTaxi(data, textDir() + "taxi.txt", o.verbose);
        exportLocks(data, textDir() + "locks.txt", o.verbose);
        exportEmotes(data, textDir() + "emotes.txt", o.verbose);
        exportTransports(data, textDir() + "transports.txt", o.verbose);
        return spells || facts ? 0 : 4;
    }
    if (o.dungeons) {   // stage 5.24: one pack per instance map, same texture/creature options as the world run
        if (o.synthetic) { std::fputs("--dungeons needs --data\n", stderr); return 1; }
        std::string folder = o.out == "test.w3dp" ? std::string(".") : o.out;
        if (!folder.empty() && folder.back() != '/' && folder.back() != '\\') folder += '/';
        std::vector<DungeonMap> list = listDungeonMaps(data);
        if (list.empty()) { std::fputs("no instance maps (WDT with a global building) found in Map.dbc\n", stderr); return 2; }
        int okCount = 0;
        for (const DungeonMap& d : list) {
            Options m = o;
            m.dungeons = false;
            m.map = d.dir;
            m.out = folder + "map" + std::to_string(d.id) + ".w3dp";
            std::printf("dungeon map %u (%s) -> %s\n", d.id, d.dir.c_str(), m.out.c_str());
            resetWmoCache();
            PackWriter dp;
            ConvStats ds;
            if (!dp.setSpillPath(m.out + ".spill")) std::fprintf(stderr, "note: cannot create %s.spill\n", m.out.c_str());
            if (!convertDungeon(m, data, dp, ds)) continue;
            emitCollision(m, dp, ds);
            if (m.creatures || m.players || m.gameobjects) convertCreatures(m, data, dp, ds);
            if (!dp.write(m.out)) { std::fprintf(stderr, "cannot write %s\n", m.out.c_str()); continue; }
            std::printf("  wrote %s: %zu buildings, %zu models, %zu textures, floors %zu triangles\n", m.out.c_str(), ds.wmoOk,
                        ds.modelsOk, ds.texturesOk, ds.collSource);
            ++okCount;
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("done: %d of %zu dungeon packs in %.0f s\n", okCount, list.size(), secs);
        return okCount ? 0 : 4;
    }
    if (!o.synthetic && !pack.setSpillPath(o.out + ".spill"))
        std::fprintf(stderr, "note: cannot create %s.spill, the pack is built in memory\n", o.out.c_str());
    int failed = 0;
    if (!o.synthetic && isDungeonMap(data, o.map)) {   // an instance map named with --map: the same as one --dungeons pack
        std::printf("%s is an instance map (a single building)\n", o.map.c_str());
        if (!convertDungeon(o, data, pack, st)) return 2;
        o.tileX0 = o.tileY0 = o.tileX1 = o.tileY1 = 32;
    } else {
    const int tileTotal = (o.tileX1 - o.tileX0 + 1) * (o.tileY1 - o.tileY0 + 1);
    int tileNo = 0;
    for (int tx = o.tileX0; tx <= o.tileX1; ++tx)
        for (int ty = o.tileY0; ty <= o.tileY1; ++ty) {
            o.tileX = tx;
            o.tileY = ty;
            ++tileNo;
            progress(true, "tile %d/%d (%d_%d): %zu models, %zu buildings, %zu textures so far", tileNo, tileTotal, tx, ty,
                     st.modelsOk, st.wmoOk, st.texturesOk);
            std::printf("tile %d_%d...\n", tx, ty);
            bool ok = o.synthetic ? buildSynthetic(o, pack, st) : convertTile(o, data, pack, st);
            if (!ok) ++failed;   // missing tiles (ocean, map edge) are skipped
        }
    }
    progressDone("tiles: %zu converted, %d skipped, %zu models, %zu buildings", st.tiles, failed, st.modelsOk, st.wmoOk);
    if (st.tiles == 0) { std::fputs("no tiles converted\n", stderr); return 2; }
    if (st.dump) std::fclose(st.dump);
    emitCollision(o, pack, st);
    if ((o.creatures || o.players || o.transports || o.gameobjects) && !o.synthetic) {
        std::printf(o.creatures ? "creatures...\n" : "players...\n");
        convertCreatures(o, data, pack, st);
    }
    if (o.creatures && !o.synthetic) {   // the 3DS action bar reads spells.txt next to the pack, the reactions factions.txt
        exportSpellNames(data, textDir() + "spells.txt", o.verbose);
        exportFactions(data, textDir() + "factions.txt", o.verbose);
        exportTalents(data, textDir() + "talents.txt", o.verbose);
        exportTaxi(data, textDir() + "taxi.txt", o.verbose);
        exportLocks(data, textDir() + "locks.txt", o.verbose);
        exportEmotes(data, textDir() + "emotes.txt", o.verbose);
    }
    if ((o.creatures || o.transports) && !o.synthetic) exportTransports(data, textDir() + "transports.txt", o.verbose);
    progress(true, "writing %s ...", o.out.c_str());
    if (!pack.write(o.out)) { std::fprintf(stderr, "cannot write %s\n", o.out.c_str()); return 3; }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    {
        FILE* f = std::fopen(o.out.c_str(), "rb");
        long sz = 0;
        if (f) { std::fseek(f, 0, SEEK_END); sz = std::ftell(f); std::fclose(f); }
        progressDone("wrote %s (%.1f MB) in %d min %02d s", o.out.c_str(), sz / 1048576.0, (int)secs / 60, (int)secs % 60);
    }
    std::printf("wrote %s in %.1fs\n", o.out.c_str(), secs);
    std::printf("  tiles: %zu converted, %d skipped\n", st.tiles, failed);
    std::printf("  terrain: %zu verts, %zu tris (grid %d); water: %zu tris\n", st.terrainVerts, st.terrainTris, o.grid,
                st.waterTris);
    if (st.wmoSkipped) std::printf("  note: %zu WMO placements skipped (--no-wmo)\n", st.wmoSkipped);
    if (st.wmoPlacements || st.wmoFailed)
        std::printf("  buildings: %zu placed (%zu distinct WMO ok, %zu failed), %zu groups converted (%zu failed, %zu interior skipped),\n"
                    "             %zu group instances, %zu WMO tris, %zu doodads inside buildings\n",
                    st.wmoPlacements, st.wmoOk, st.wmoFailed, st.wmoGroups, st.wmoGroupsFailed, st.wmoGroupsSkipped,
                    st.wmoPlacedGroups, st.wmoTris, st.wmoDoodads);
    if (st.collSource || st.m2CollSource)
        std::printf("  floors: %zu collision triangles in buildings, %zu walkable, %zu walls; doodads: %zu models with\n"
                    "          collision (%zu simplified), %zu placements, %zu triangles (%zu walkable, %zu walls); %zu pieces after clipping\n"
                    "          to tiles, COLL %.1f MB (largest tile %.0f KB)\n",
                    st.collSource, st.collWalkable, st.collWalls, st.m2CollModels, st.m2CollSimplified, st.m2CollPlaced, st.m2CollSource,
                    st.m2CollWalkable, st.m2CollWalls, st.collPieces, st.collBytes / 1048576.0, st.collMaxTileBytes / 1024.0);
    if (o.creatures)
        std::printf("  creatures: %zu displays (%zu failed), %zu models (%zu failed), %zu tris, %zu bones, %zu palettes,\n"
                    "             %zu animations, %.1f MB of animation keys; %zu of %zu vertices lose >10%% weight (2 bones kept)\n",
                    st.displaysOk, st.displaysFailed, st.creatureModelsOk, st.creatureModelsFailed, st.creatureTris,
                    st.creatureBones, st.creaturePalettes, st.creatureSeqs, st.creatureKeyBytes / 1048576.0,
                    st.creatureLossyVerts, st.creatureSrcVerts);
    if (o.creatures) std::printf("  npc equipment: %zu humanoid displays with geometry from their items (robes, boots, gloves...)\n",
                                 st.npcEquipGeosets);
    if (o.players)
        std::printf("  players: %zu race/sex models (%zu failed), %zu CharSections rows, %zu layers (%zu failed), %.1f MB\n",
                    st.charRaces, st.charRacesFailed, st.charSections, st.charImages, st.charImagesFailed,
                    st.charImageBytes / 1048576.0);
    if (o.players && !o.noItems)
        std::printf("  items: %zu displays, %zu body layers (%.1f MB), %zu models (%zu failed, %zu tris), %zu helmets, %zu cloaks\n",
                    st.itemDisplays, st.itemLayers, st.itemLayerBytes / 1048576.0, st.itemModels, st.itemModelsFailed,
                    st.itemModelTris, st.itemHelms, st.itemCapes);
    std::printf("  winding: %zu M2 and %zu WMO submeshes flipped to match their normals\n", st.m2SubsFlipped, st.wmoSubsFlipped);
    if (st.duplicatesSkipped) std::printf("  %zu duplicate placements skipped (same uniqueId listed by several tiles)\n", st.duplicatesSkipped);
    std::printf("  models: %zu ok, %zu failed, %zu tris total (unique)\n", st.modelsOk, st.modelsFailed, st.modelTrisTotal);
    std::printf("  placements: %zu\n", st.placements);
    {
        const TexTiming& tt = textureTiming();
        std::printf("  timing: %.0f s total; game textures %zu: read+decode %.1f s, encode %.1f s; creature models %.1f s\n",
                    secs, tt.count, tt.readDecodeSec, tt.encodeSec, st.creatureSec);
    }
    std::printf("  WMO batches drawn untextured (material texture did not resolve): %zu\n", st.wmoBatchesUntextured);
    std::printf("  textures: %zu ok, %zu failed; %.1f KB in pack\n", st.texturesOk, st.texturesFailed,
                pack.totalBytes(W3D_ENTRY_TEXTURE) / 1024.0);
    return printPackInfo(o.out);
}
