// Internal converter interfaces.
#pragma once
#include "w3d_format.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <vector>

namespace wowee::pipeline { struct ADTTerrain; }

namespace w3d {

// ------------------------------------------------------------ progress (console window, not the log)
// One updating line: throttled to 4 per second unless force. progressDone ends it with a newline.
void progress(bool force, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void progressDone(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ------------------------------------------------------------ crash guard (crashguard.cpp)
// installCrashGuard(): call first thing in main. If the process dies (access violation, abort, uncaught exception),
// the last trace() line is printed to stderr and stdout is flushed, so the log ends where the converter really died
// (stdout to a pipe/file is block-buffered: without this the log lost its last few KB and stopped mid-line).
void installCrashGuard();
// trace(): remember what is being converted right now ("WMO <path> group 3") and flush stdout. Cheap enough per asset.
void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ------------------------------------------------------------ settings
struct Options {
    std::string dataDir;        // loose files extracted from MPQ (WoWee extract_assets)
    std::string map = "Azeroth";
    int tileX = 32, tileY = 48;                 // current tile (set per tile by the region loop)
    int tileX0 = 32, tileY0 = 48, tileX1 = 32, tileY1 = 48;  // inclusive region
    std::string out = "test.w3dp";
    int grid = 9;               // terrain vertices per chunk edge: 9, 5, 3
    int terrainTex = 64;        // max terrain texture size
    int modelTex = 64;          // max model texture size
    uint8_t texFormat = W3D_TEX_ETC1;
    int maxModelTris = 400;     // per model after simplification
    int maxDoodads = -1;        // -1 = all placements on the tile
    int syntheticVariety = 0;   // --synthetic-variety N: N distinct models + textures per synthetic tile (a city's load)
    // WMO buildings (stage 5.0)
    bool noWmo = false;         // skip buildings entirely
    bool noWmoDoodads = false;  // buildings without their furniture/lamps
    bool wmoExteriorOnly = false;   // drop interior-only groups (no portals yet, they are drawn anyway)
    int wmoTex = 128;           // max WMO texture size
    int wmoGroupTris = 3000;    // triangle budget per WMO group (0 = keep all)
    std::string dumpPlacements; // debug: text file with every building group/doodad placement
    bool noFloors = false;      // skip COLL entirely (no floors, no walls)
    bool noWalls = false;       // COLL with floors only (walking passes through walls)
    bool noFx = false;          // stage 5.32: no particle emitters / glow sprites on doodads (--no-fx)
    bool fxSpell = false;       // stage 5.33: the model being converted is a spell effect (internal)
    bool spellFx = true;        // stage 5.33: spell visuals with the creatures (--no-spell-fx)
    bool noDoodadColl = false;  // no M2 doodad collision (fences, trunks, bridges) in COLL
    int doodadCollTris = 96;    // collision triangle budget per doodad model (0 = keep all)
    // Creatures (stage 5.3): every CreatureDisplayInfo row -> CDSP + CMDL (skinned, animated)
    bool creatures = false;
    int creatureTex = 128;      // max creature texture size
    int creatureTris = 800;     // triangle budget per creature model
    int creatureAnimKb = 320;   // animation keys per creature model (the essential animations always fit)
    int playerAnimKb = 900;     // per character (race) model: emotes, sitting, riding ...
    std::string creatureDisplays;   // optional comma list of display ids (default: all)
    bool players = false;       // stage 5.5: race models + CharSections layers (CHRS/CIMG) for players
    bool noItems = false;       // stage 5.19: no worn items (ITEM entries: armour layers, helmets, shoulders, weapons)
    int itemModelTris = 300;    // triangle budget per item model (weapon, shield, helmet, shoulder)
    int itemTex = 128;          // max texture size of item models
    bool gameobjects = false;   // stage 5.23: models of ordinary game objects (event decorations, chests ...) as CDSP 0x80000000|id
    std::string goFilter;       // model path keywords limiting --gameobjects (empty: every display)
    bool transports = false;    // stage 5.21: ship / zeppelin models (GameObjectDisplayInfo) as CDSP 0x80000000|id + deck data
    std::string transportFilter = "transport,zeppelin,ship,boat";   // model path keywords that make a GO display a transport
    bool dungeons = false;      // stage 5.24: one map<ID>.w3dp per instance map (WDT-only maps of Map.dbc) into the folder given with -o
    // Water (stage 5.26): the game's animated liquid textures, Light.dbc colours
    int waterTex = 128;         // max size of a liquid texture frame
    int waterFrames = 30;       // animation frames kept of the 30 (1, 2, 3, 5, 6, 10, 15 or 30)
    float waterTint = -1;       // 0..1: how much the Light.dbc colour tints the texture (-1: by the texture's own colour)
    bool noWmoWater = false;    // leave out the liquids of buildings (canals, fountains, dungeon pools)
    bool synthetic = false;
    bool verbose = false;
};

// ------------------------------------------------------------ pack writer
class PackWriter {
public:
    PackWriter();
    ~PackWriter();
    PackWriter(const PackWriter&) = delete;
    PackWriter& operator=(const PackWriter&) = delete;
    // World-sized packs do not fit in memory: with a spill file every blob goes to disk when it is added (only the
    // index stays in memory) and write() copies them in pack order. Without one (tests, the small UI pack) it is memory.
    bool setSpillPath(const std::string& path);
    // Returns false if (type,id) already present.
    bool add(uint32_t type, uint32_t id, std::vector<uint8_t> blob);
    bool has(uint32_t type, uint32_t id) const {
        if (pool_ && !w3d_is_tile_entry(type)) return pool_->has(type, id);
        return keys_.count(((uint64_t)type << 32) | id) != 0;
    }
    // The id of an asset named by 'key' (its game path, plus whatever makes two conversions of it differ): FNV-1a of
    // the key, unless that id already belongs to another key of the same type - then a salted hash, so two names never
    // share an entry (a world has hundreds of thousands of names: 32-bit collisions do happen). Same key, same id.
    uint32_t keyId(uint32_t type, const std::string& key);
    size_t keyCollisions() const { return pool_ ? pool_->keyCollisions() : collisions_; }
    // Stage 5.27: a map pack hands every non-tile entry (and the names) to the shared pool.
    void setPool(PackWriter* pool) { pool_ = pool; }
    PackWriter* pool() const { return pool_; }
    // What the file is (PINF entry, written by write()); without it nothing is written (a standalone pack).
    void setInfo(const W3dPackInfo& info) { info_ = info; haveInfo_ = true; }
    // False when it cannot write the file or the pack would pass 4 GiB (32-bit offsets, also the FAT32 file limit).
    bool write(const std::string& path) const;
    // The pool: split into parts of at most partBytes, base.w3dp, base1.w3dp, ... (part numbers in their PINF).
    // Returns the number of parts written, 0 on failure.
    int writeParts(const std::string& basePath, uint64_t partBytes) const;
    size_t totalBytes(uint32_t type) const;
    size_t count(uint32_t type) const;
    uint64_t storedBytes() const { return pos_; }

private:
    struct Item { uint32_t type, id; uint64_t off; uint32_t size; };
    bool readAt(uint64_t off, void* dst, size_t n) const;
    bool writeItems(const std::string& path, const std::vector<const Item*>& order, const W3dPackInfo* info) const;
    std::vector<const Item*> fileOrder() const;
    PackWriter* pool_ = nullptr;
    std::unordered_map<uint64_t, std::string> names_;   // (type, id) -> key (keyId)
    size_t collisions_ = 0;
    W3dPackInfo info_{};
    bool haveInfo_ = false;
    std::vector<Item> items_;
    std::unordered_set<uint64_t> keys_;
    std::vector<uint8_t> mem_;        // storage without a spill file
    mutable FILE* spill_ = nullptr;
    std::string spillPath_;
    uint64_t pos_ = 0;
};

// Little blob builder: append PODs, patch offsets later.
class Blob {
public:
    template <class T> size_t put(const T& v) { return putBytes(&v, sizeof(T)); }
    template <class T> size_t putArray(const std::vector<T>& v) {
        return v.empty() ? align(4) : putBytes(v.data(), v.size() * sizeof(T));
    }
    size_t putBytes(const void* p, size_t n) {
        size_t at = data.size();
        data.resize(at + n);
        if (n) std::memcpy(data.data() + at, p, n);
        return at;
    }
    size_t align(size_t a) {
        while (data.size() % a) data.push_back(0);
        return data.size();
    }
    template <class T> T* at(size_t off) { return reinterpret_cast<T*>(data.data() + off); }
    std::vector<uint8_t> data;
};

// ------------------------------------------------------------ data access
// Case-insensitive view of an extracted Data/ directory.
// Two backends: a directory of loose files (output of an MPQ extractor), or
// the client's Data/ folder read directly through StormLib (no extraction).
class DataDir {
public:
    ~DataDir();
    // Picks the backend: a folder that contains *.MPQ files is opened as archives.
    bool open(const std::string& root);
    // Game path like "World\\Maps\\Azeroth\\Azeroth_32_48.adt".
    bool read(const std::string& gamePath, std::vector<uint8_t>& out) const;
    static std::string normalize(const std::string& gamePath);
    size_t archiveCount() const { return archives_.size(); }
    size_t looseCount() const { return files_.size(); }

private:
    bool openMpq(const std::string& dir);
    bool readMpq(const std::string& gamePath, std::vector<uint8_t>& out) const;
    std::unordered_map<std::string, std::string> files_;  // normalized -> fs path
    std::vector<void*> archives_;                          // StormLib HANDLEs, highest priority first
};

// ------------------------------------------------------------ textures
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // 8:8:8:8, row 0 = top
    bool hasAlpha() const;
};

// Resize to power-of-two <= maxSize, build mips, encode to fmt in PICA layout.
// Returns a full TEX blob (header + data).
// ETC1 encoder quality 0 low (default), 1 medium, 2 high; time spent on game textures.
void setEtcQuality(int q);
struct TexTiming { double readDecodeSec = 0, encodeSec = 0; size_t count = 0; };
TexTiming& textureTiming();
// 5.35h: textures are encoded on a thread pool; this waits for all of them and adds them to their packs (PackWriter::write
// and writeParts call it, and so does a PackWriter going away).
void textureFlush();
bool textureInPack(PackWriter& pack, uint32_t id);   // in it, or on its way (being encoded)
std::vector<uint8_t> encodeTexture(const Image& src, int maxSize, uint8_t fmt, bool wantAlpha);
// Box-filter downscale by integer factors / pad up: result is w x h.
Image resizeImage(const Image& src, int w, int h);
// Decode a BLP via WoWee's loader.
bool decodeBlp(const std::vector<uint8_t>& bytes, Image& out);

// Adds TEX entry for a game path (if missing). Returns entry id or 0 on failure.
uint32_t addTextureFromGame(PackWriter& pack, const DataDir& data, const std::string& gamePath,
                            int maxSize, uint8_t fmt, bool alphaCapable, bool verbose);

// ------------------------------------------------------------ geometry
struct ModelSource {               // engine-neutral mesh before packing
    struct V { float p[3]; float n[3]; float uv[2]; };
    struct Sub { uint32_t texId; uint8_t blend, flags; std::vector<uint32_t> idx; uint16_t tag = 0; };   // tag: stage 5.35, kept apart in the merge
    std::vector<V> verts;
    std::vector<Sub> subs;
    std::vector<uint32_t> texIds;   // TEX entry ids referenced by Sub::texId (index)
    std::vector<uint16_t> outTags;   // out (stage 5.35): the Sub::tag of every submesh of the blob, in order
    size_t dropped = 0;             // out: indices/triangles buildModelBlob had to throw away (partial triangle, index out of range)
};
// Flips idx (triangle list over m.verts) to be CCW seen from the vertex normals; true if flipped.
bool orientByNormals(const ModelSource& m, std::vector<uint32_t>& idx);
// Also normalises winding: CCW seen from the vertex normals' side (flippedOut = submeshes flipped).
std::vector<uint8_t> buildModelBlob(ModelSource& m, int maxTris, size_t* trisOut, int* flippedOut = nullptr);

// Whole file into memory in one read (the byte-by-byte istreambuf_iterator took minutes on a multi-GB map pack).
bool readFileFast(const std::string& path, std::vector<uint8_t>& out);

struct ConvStats {
    size_t collOrphanTiles = 0;   // collision tiles dropped because the map has no terrain tile there
    size_t tiles = 0, waterTris = 0, wmoSkipped = 0;
    size_t wmoOk = 0, wmoFailed = 0, wmoGroups = 0, wmoGroupsFailed = 0, wmoGroupsSkipped = 0, wmoTris = 0;
    size_t wmoPlacements = 0, wmoPlacedGroups = 0, wmoDoodads = 0, duplicatesSkipped = 0;
    size_t m2SubsFlipped = 0, wmoSubsFlipped = 0;
    size_t fxModels = 0, fxEmitters = 0, fxGlows = 0;   // stage 5.32
    size_t fxAnimated = 0, fxEmittersDropped = 0, fxParticleOnly = 0;   // 5.35f: emitters without a rate or a life                                // stage 5.35: effect models with baked bone animation
    size_t creatureModelsOk = 0, creatureModelsFailed = 0, displaysOk = 0, displaysFailed = 0, creatureTris = 0;
    size_t creatureSeqs = 0, creatureKeyBytes = 0, creaturePalettes = 0, creatureBones = 0;
    size_t creatureSrcVerts = 0, creatureLossyVerts = 0;
    double creatureSec = 0;
    size_t npcEquipGeosets = 0;   // humanoid NPC displays whose equipment changes geometry   // building CMDL blobs (parse, simplify, bake), textures excluded
    size_t itemDisplays = 0, itemLayers = 0, itemLayerBytes = 0, itemLayersMissing = 0, itemModels = 0, itemModelsFailed = 0,
           itemModelTris = 0, itemHelms = 0, itemCapes = 0;
    size_t wmoBatchesUntextured = 0;   // WMO batches whose material texture did not resolve (drawn flat)
    size_t transportDisplays = 0, transportDisplaysFailed = 0, transportDecks = 0;   // stage 5.21
    size_t charRaces = 0, charRacesFailed = 0, charSections = 0, charImages = 0, charImagesFailed = 0, charImageBytes = 0;   // submeshes whose winding disagreed with their normals
    // Placements repeat in every ADT they overlap (same uniqueId): emit each once.
    std::unordered_set<uint32_t> seenDoodadUid, seenWmoUid;
    FILE* dump = nullptr;       // --dump-placements
    // Collision triangles (render space) per tile id, clipped to the tile: 9 coords + flags.
    struct CollTri { float v[9]; uint16_t flags; };
    std::unordered_map<uint32_t, std::vector<CollTri>> collTris;
    size_t collSource = 0, collWalkable = 0, collWalls = 0, collPieces = 0, collBytes = 0, collMaxTileBytes = 0;
    // M2 bounding (collision) triangles in model space, 3 vertices x 3 floats each, by model id.
    std::unordered_map<uint32_t, std::vector<float>> m2Coll;
    size_t m2CollModels = 0, m2CollSimplified = 0, m2CollPlaced = 0, m2CollSource = 0, m2CollWalkable = 0, m2CollWalls = 0;
    size_t terrainTris = 0, terrainVerts = 0, modelTrisTotal = 0, placements = 0;
    // Liquids (stage 5.26), collected per tile while the tiles and buildings are converted, written by emitLiquids().
    struct LiquidChunk {            // one MCNK's surface (the topmost of its MCLQ layers per cell)
        uint8_t cell[64];           // W3D_LIQ_* (kind | deep), W3D_LIQ_DRY
        float h[64][4];             // corner heights of each cell: (r,k) (r,k+1) (r+1,k) (r+1,k+1)
        float depth[64][4];         // the same corners: surface minus terrain (yards; < 0 = above ground)
    };
    struct LiquidQuad { float p[4][3]; uint8_t kind; };   // a building's MLIQ cell, render space, corners in order
    struct TileLiquids { std::map<int, LiquidChunk> chunks; std::vector<LiquidQuad> quads; };
    std::map<uint32_t, TileLiquids> liquids;              // by tile id
    size_t liquidLayers = 0, liquidCells = 0, liquidDeepCells = 0, liquidQuads = 0, liquidWmoGroups = 0, liquidTiles = 0;
    size_t liquidFramesOk = 0, liquidFramesMissing = 0, liquidBytes = 0, liquidMclqBad = 0;
    size_t modelsOk = 0, modelsFailed = 0, texturesOk = 0, texturesFailed = 0;
};

uint32_t addModelFromGame(PackWriter& pack, const DataDir& data, const std::string& m2Path,
                          const Options& o, ConvStats& st);
// WMO: converts the building once (groups -> MODL entries) and appends its group and
// doodad placements for one MODF placement. pos = render space, eulerRad = placementEuler().
void placeWmo(PackWriter& pack, const DataDir& data, const std::string& path, const float pos[3],
              const float eulerRad[3], uint16_t doodadSet, const Options& o, ConvStats& st,
              const std::function<uint32_t(const std::string&)>& modelFor, std::vector<W3dPlacement>& out);
// A WDBC table (DBFilesClient\\<name>), fields read as u32 / f32 / string offsets.
struct DbcTable {
    uint32_t records = 0, fields = 0, recSize = 0, strSize = 0;
    std::vector<uint8_t> b;
    bool load(const DataDir& data, const char* name);
    uint32_t u32(uint32_t r, uint32_t f) const;
    float f32(uint32_t r, uint32_t f) const;
    std::string str(uint32_t r, uint32_t f) const;
};

// Spell names for the action bar of the 3DS client: spells.txt ("id<TAB>name<TAB>rank" lines, stage 5.8).
bool exportSpellNames(const DataDir& data, const std::string& path, bool verbose);
bool exportFactions(const DataDir& data, const std::string& path, bool verbose);
// Talent trees (talents.txt) and flight masters (taxi.txt) for the 3DS client (UI stages 4/6).
bool exportTalents(const DataDir& data, const std::string& path, bool verbose);
bool exportTaxi(const DataDir& data, const std::string& path, bool verbose);
bool exportLocks(const DataDir& data, const std::string& path, bool verbose);
// areatriggers.txt (AreaTrigger.dbc, stage 5.28): the zones the client reports with CMSG_AREATRIGGER (dungeon doors, the tram).
bool exportAreaTriggers(const DataDir& data, const std::string& path, bool verbose);
bool exportEmotes(const DataDir& data, const std::string& path, bool verbose);
// glue.txt (stage 5.29): GlueStrings.lua texts, CharStartOutfit.dbc clothes, AreaTable.dbc zone names.
bool exportGlueTexts(const DataDir& data, const std::string& path, bool verbose);
// Transports (stage 5.21): transports.txt = "T path node map x y z flags delay" per TaxiPathNode.dbc row and
// "M display deckZ x0 y0 x1 y1" per transport model that has a walkable deck (found by convertTransports in creatures.cpp).
struct TransportDeck { float z = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
std::map<uint32_t, TransportDeck>& transportDecks();
bool exportTransports(const DataDir& data, const std::string& path, bool verbose);
// The column of the English spell name in Spell.dbc (calibrated on known spells; 120 in 1.12).
int spellNameColumn(const DbcTable& spell);

// Interface pack (UI stage 7): ui.w3dp from Interface\\* of the client + the skin config (w3d_ui.h).
// w3dconv --sound (sound/ for the 3DS: sfx.w3ds, music.txt, music/*, areas_<map>.bin; common/w3d_sound.h)
struct SoundOptions {
    std::string outDir = "sound";
    std::string eventsFile;          // "event SoundEntriesName ..." lines (override the built-in event list)
    uint32_t sfxRate = 22050, streamRate = 22050;
    bool music = true, ambience = true, areas = true, world = true;
    int worldVariants = 2;           // files kept per world sound (SoundEntries has up to 10)
};
bool convertSound(const SoundOptions& so, const DataDir& data, bool verbose);

struct UiOptions {
    std::string out = "ui.w3dp";
    std::string skinConfig;          // file with the sprite list (default: the built-in list = converter/ui_skin.txt)
    std::string font;                // TTF for the normal / large sizes (default: Fonts\\FRIZQT__.TTF of the client)
    std::string fontSmall;           // TTF for the small size (default: none -> the 3DS built-in font)
    std::vector<std::string> fallbackFonts;   // for glyphs the font lacks (Cyrillic); system fonts tried after these
    int iconSize = 40;               // icon picture size in the atlas (cells are 2 px larger)
    int normalPx = 12, largePx = 16, smallPx = 9;
    bool maps = true, icons = true;
    int mapWidth = 384;              // map pictures: mapWidth x mapWidth*2/3
};
bool convertUi(const UiOptions& uo, const DataDir& data, bool verbose);

// Glyph atlas baking with stb_truetype (ui_font.cpp). fonts[0] is the main font, the rest fill in missing glyphs.
struct BakedGlyph { uint32_t cp; int x, y, w, h, xoff, yoff, advance; };
struct BakedFont { int pixelSize = 0, lineHeight = 0, ascent = 0; std::vector<BakedGlyph> glyphs; };
// Bakes every size into one 8-bit alpha atlas atlasW wide (height grows to a power of two). False when the main
// font cannot be read.
bool bakeFonts(const std::vector<std::vector<uint8_t>>& ttfs, const std::vector<int>& sizes, const std::vector<uint32_t>& cps,
               int atlasW, std::vector<uint8_t>& atlas, int* atlasH, std::vector<BakedFont>& out, size_t* missing);
std::vector<uint32_t> uiCodepoints();   // Latin, Cyrillic, the signs the UI draws

// Item models (stage 5.19): a static MODL of an M2 from Item\\ObjectComponents, baked in the bind pose with texture
// 'texPath' (a game path, may be empty) in place of its replaceable textures (types 2, 3, 4; else texture 0).
// Returns the MODL id (hash of model + texture) or 0.
uint32_t addItemModel(PackWriter& pack, const DataDir& data, const std::string& m2Path, const std::string& texPath,
                      const Options& o, ConvStats& st);
// ITEM entries for every ItemDisplayInfo row: body layers per sex, geoset groups, cloak, models (items.cpp).
bool convertItems(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st);

// Creatures: CMDL/CDSP entries for CreatureDisplayInfo (o.creatures).
bool convertCreatures(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st);
// Stage 5.29: the login / character screen scenes (MODL + GLUE entries), with the player models.
bool convertGlueScenes(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st);
// Spell visuals (stage 5.33, spellfx.cpp): effect models + the SVFX table, into the pack (the pool with --all).
bool convertSpellEffects(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st);
int explainSpells(const DataDir& data, const std::vector<uint32_t>& ids);   // --explain-spell (5.35f)
// Collision of one doodad placement (st.m2Coll of its model, matrix T(pos)*Rz*Ry*Rx*S like the renderer).
void addPlacementCollision(const Options& o, ConvStats& st, const W3dPlacement& p);
// Writes the COLL entries collected by placeWmo / addPlacementCollision (one per tile).
void emitCollision(const Options& o, PackWriter& pack, ConvStats& st);
// Rotation of a column-major 4x4 as W3D Euler angles (R = Rz*Ry*Rx); returns uniform scale.
float matrixToPlacement(const float m4[16], float outRot[3]);
// Which tile owns a render-space position; true if that tile is inside the converted region.
bool placementOwner(const Options& o, float x, float y, int& tx, int& ty);
// Grey 8x8 stand-in so every texture reference resolves.
uint32_t addPlaceholderTexture(PackWriter& pack, uint8_t fmt);

// Builds TERR + PLCM from a parsed ADT. texIds = tile texture table (slot 0 =
// placeholder), mtexToSlot maps MTEX indices to slots, modelFor() adds a model
// for a doodad name and returns its id (0 = skip placement).
// wmoPlace (optional) appends building placements to the tile's PLCM.
bool emitTile(const Options& o, const wowee::pipeline::ADTTerrain& adt, const std::vector<uint32_t>& texIds,
              const std::vector<uint16_t>& mtexToSlot, const std::function<uint32_t(const std::string&)>& modelFor,
              PackWriter& pack, ConvStats& st,
              const std::function<void(std::vector<W3dPlacement>&)>& wmoPlace = nullptr,
              const std::vector<uint8_t>* adtBytes = nullptr);
// Liquids (stage 5.26). collectAdtLiquids: the MCLQ layers of a tile (from the raw ADT when given - all layers, deep
// water - else from WoWee's parse), with their depth over the terrain. emitLiquids: WATR (mesh, animated game
// textures, Light.dbc colours) + LIQD (query grid) for every tile with a TERR that got liquids (data may be null).
void collectAdtLiquids(const Options& o, const std::vector<uint8_t>* adtBytes, const wowee::pipeline::ADTTerrain& adt,
                       ConvStats& st);
void emitLiquids(const Options& o, const DataDir* data, PackWriter& pack, ConvStats& st);
// Kind (W3dLiquid) of a WMO group's MLIQ the way the server's extractor resolves it (vMaNGOS vmapextract), -1 = none.
int wmoLiquidKind(uint32_t rootFlags, uint32_t rootId, uint32_t groupFlags, uint32_t groupLiquid, const uint8_t* tiles,
                  size_t tileCount);
bool convertTile(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st);
// Dungeons (stage 5.24): maps whose WDT holds one global WMO and no ADTs. convertDungeon() puts the building (split by
// tile) plus a hidden terrain quad per tile into the pack; listDungeonMaps() = Map.dbc rows (id, directory) of such maps.
void resetWmoCache();   // forget the converted buildings (they live in the pack that was current when they were loaded)
struct DungeonMap { uint32_t id; std::string dir; };
bool isDungeonMap(const DataDir& data, const std::string& dir);
bool convertDungeon(const Options& o, const DataDir& data, PackWriter& pack, ConvStats& st);
std::vector<DungeonMap> listDungeonMaps(const DataDir& data);
bool buildSynthetic(const Options& o, PackWriter& pack, ConvStats& st);
// Stage 5.27: w3dconv --all - interface, sound, every map of Map.dbc with one shared asset pool (all.cpp).
struct AllOptions {
    std::string folder;                       // where everything goes (copy it to sdmc:/3ds/w3dproto/)
    std::set<uint32_t> maps;                  // only these Map.dbc ids (empty: all but 'skip')
    std::set<uint32_t> skip = {13, 25, 29, 42, 169, 451};   // test and development maps of 1.12
    bool ui = true, sound = true;
    uint64_t partBytes = 3900ull << 20;       // pool parts below the FAT32 file limit of 4 GiB
};
int convertAll(const Options& o, const DataDir& data, const AllOptions& ao, UiOptions uo, SoundOptions so);
int  printPackInfo(const std::string& path);   // validator, returns 0 if pack is sound

}  // namespace w3d
