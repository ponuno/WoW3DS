#include <cmath>
#include "conv.h"
#include "w3d_liquid.h"
#include "w3d_ground.h"
#include "w3d_fx.h"
#include <unordered_set>
#include "w3d_ui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace w3d {

// ------------------------------------------------------------ PackWriter
PackWriter::PackWriter() = default;

PackWriter::~PackWriter() {
    textureFlush();   // 5.35h: no texture of the pool may still be on its way to this pack
    if (spill_) {
        std::fclose(spill_);
        std::remove(spillPath_.c_str());
    }
}

bool PackWriter::setSpillPath(const std::string& path) {
    if (spill_ || !items_.empty()) return false;
    spill_ = std::fopen(path.c_str(), "w+b");
    if (!spill_) return false;
    std::setvbuf(spill_, nullptr, _IOFBF, 1 << 20);
    spillPath_ = path;
    return true;
}

uint32_t PackWriter::keyId(uint32_t type, const std::string& key) {
    if (pool_) return pool_->keyId(type, key);
    uint32_t id = w3d_fnv1a(key.c_str());
    for (uint32_t salt = 1;; ++salt) {
        if (id == 0) id = 1;   // 0 means "none" in many tables
        auto [it, fresh] = names_.emplace(((uint64_t)type << 32) | id, key);
        if (fresh && salt > 1) ++collisions_;   // a new name that had to move off another's hash
        if (fresh || it->second == key) return id;
        const std::string salted = key + "#" + std::to_string(salt);
        id = w3d_fnv1a(salted.c_str());
    }
}

bool PackWriter::add(uint32_t type, uint32_t id, std::vector<uint8_t> blob) {
    if (pool_ && !w3d_is_tile_entry(type)) return pool_->add(type, id, std::move(blob));
    const uint64_t key = ((uint64_t)type << 32) | id;
    if (!keys_.insert(key).second) return false;
    Item it{type, id, pos_, (uint32_t)blob.size()};
    if (spill_) {
        // reads and writes of one stream must be separated by a seek
        if (std::fseek(spill_, 0, SEEK_END) != 0 || (!blob.empty() && std::fwrite(blob.data(), 1, blob.size(), spill_) != blob.size())) {
            std::fprintf(stderr, "cannot write the spill file %s (disk full?)\n", spillPath_.c_str());
            std::exit(5);
        }
    } else {
        mem_.insert(mem_.end(), blob.begin(), blob.end());
    }
    pos_ += blob.size();
    items_.push_back(it);
    return true;
}

bool PackWriter::readAt(uint64_t off, void* dst, size_t n) const {
    if (spill_) {
        if (std::fflush(spill_) != 0) return false;
#ifdef _WIN32
        if (_fseeki64(spill_, (long long)off, SEEK_SET) != 0) return false;
#else
        if (fseeko(spill_, (off_t)off, SEEK_SET) != 0) return false;
#endif
        return n == 0 || std::fread(dst, 1, n, spill_) == n;
    }
    if (off + n > mem_.size()) return false;
    if (n) std::memcpy(dst, mem_.data() + off, n);
    return true;
}

size_t PackWriter::totalBytes(uint32_t type) const {
    size_t n = 0;
    for (const auto& it : items_)
        if (it.type == type) n += it.size;
    return n;
}

size_t PackWriter::count(uint32_t type) const {
    size_t n = 0;
    for (const auto& it : items_)
        if (it.type == type) ++n;
    return n;
}

std::vector<const PackWriter::Item*> PackWriter::fileOrder() const {
    // Order: terrain, placements, models, textures — the client reads in file
    // order, so the small structural entries come first.
    std::vector<const Item*> order;
    const uint32_t known[] = {W3D_ENTRY_PACKINFO, W3D_ENTRY_TERRAIN, W3D_ENTRY_PLACEMENT, W3D_ENTRY_WATER, W3D_ENTRY_LIQUID, W3D_ENTRY_GROUND, W3D_ENTRY_COLLISION, W3D_ENTRY_MODEL,
                              W3D_ENTRY_DISPLAY, W3D_ENTRY_CHARACTER, W3D_ENTRY_ITEM, W3D_ENTRY_CREATURE, W3D_ENTRY_TEXTURE,
                              W3D_ENTRY_CHARIMG};
    for (uint32_t t : known)
        for (const auto& it : items_)
            if (it.type == t) order.push_back(&it);
    for (const auto& it : items_)   // anything newer than this list still gets written
        if (std::find(std::begin(known), std::end(known), it.type) == std::end(known)) order.push_back(&it);
    return order;
}

bool PackWriter::writeItems(const std::string& path, const std::vector<const Item*>& orderIn, const W3dPackInfo* info) const {
    static const uint8_t pad0[W3D_BLOB_ALIGN] = {};
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    bool ok = true;
    uint64_t at = 0;
    auto put = [&](const void* p, size_t n) {
        if (n && std::fwrite(p, 1, n, f) != n) ok = false;
        at += n;
    };
    std::vector<W3dEntry> index;
    const std::vector<const Item*>& order = orderIn;
    index.reserve(order.size() + 1);
    W3dPackHeader zero{};
    put(&zero, sizeof zero);
    if (info) {   // PINF first: the client reads it before anything else
        while (at % W3D_BLOB_ALIGN) put(pad0, 1);
        index.push_back(W3dEntry{W3D_ENTRY_PACKINFO, 0, (uint32_t)at, (uint32_t)sizeof(W3dPackInfo)});
        put(info, sizeof *info);
    }
    std::vector<uint8_t> buf(1 << 20);
    const uint8_t pad[W3D_BLOB_ALIGN] = {};
    uint64_t total = 0;
    for (const Item* it : order) {
        if (!ok) break;
        while (at % W3D_BLOB_ALIGN) put(pad, 1);
        if (at + it->size > 0xFFFFFFFFull) {
            std::fprintf(stderr, "the pack passes 4 GB (32-bit offsets, and FAT32 cannot hold more): convert a smaller region "
                                 "or lower --terrain-tex / --model-tex / --creature-tex\n");
            ok = false;
            break;
        }
        index.push_back(W3dEntry{it->type, it->id, (uint32_t)at, it->size});
        for (uint64_t done = 0; done < it->size && ok;) {
            size_t n = (size_t)std::min<uint64_t>(buf.size(), it->size - done);
            if (!readAt(it->off + done, buf.data(), n)) { ok = false; break; }
            put(buf.data(), n);
            done += n;
        }
        total += it->size;
    }
    if (ok) {
        while (at % W3D_BLOB_ALIGN) put(pad, 1);
        const uint64_t indexAt = at;
        if (indexAt + index.size() * sizeof(W3dEntry) > 0xFFFFFFFFull) {
            std::fprintf(stderr, "the pack passes 4 GB\n");
            ok = false;
        } else {
            put(index.data(), index.size() * sizeof(W3dEntry));
            W3dPackHeader h{W3DP_MAGIC, W3DP_VERSION, (uint32_t)index.size(), (uint32_t)indexAt};
            if (ok && std::fseek(f, 0, SEEK_SET) == 0) {
                if (std::fwrite(&h, sizeof h, 1, f) != 1) ok = false;
            } else {
                ok = false;
            }
        }
    }
    if (std::fclose(f) != 0) ok = false;
    (void)total;
    if (ok) {   // read the pack back: header, index and every entry we added must be in it
        FILE* r = std::fopen(path.c_str(), "rb");
        W3dPackHeader h{};
        bool good = r && std::fread(&h, sizeof h, 1, r) == 1 && h.magic == W3DP_MAGIC && h.entryCount == index.size();
        if (!good && r) std::fprintf(stderr, "  VERIFY: header of %s does not match\n", path.c_str());
        size_t missing = 0;
        if (good) {
            std::vector<W3dEntry> back(h.entryCount);
            good = std::fseek(r, (long)h.indexOffset, SEEK_SET) == 0 &&
                   std::fread(back.data(), sizeof(W3dEntry), back.size(), r) == back.size();
            if (good) {
                std::unordered_set<uint64_t> have;
                for (const W3dEntry& e : back) have.insert(((uint64_t)e.type << 32) | e.id);
                for (const Item* it : order) {
                    const uint64_t k = ((uint64_t)it->type << 32) | it->id;
                    if (!have.count(k)) {
                        if (missing++ < 10) std::fprintf(stderr, "  VERIFY: entry type %u id %08x is not in the written pack\n", (unsigned)(k >> 32), (unsigned)k);
                    }
                }
            }
        }
        if (r) std::fclose(r);
        if (!good || missing) std::fprintf(stderr, "pack verify FAILED (%zu entries missing)\n", missing);
        else std::printf("pack verify: %zu entries, every added entry is in the index\n", index.size());
    }
    return ok;
}

bool PackWriter::write(const std::string& path) const {
    textureFlush();
    if (pool_) {   // a map pack: its tiles only, and the PINF naming the pool
        std::vector<const Item*> order = fileOrder();
        return writeItems(path, order, haveInfo_ ? &info_ : nullptr);
    }
    return writeItems(path, fileOrder(), haveInfo_ ? &info_ : nullptr);
}

int PackWriter::writeParts(const std::string& basePath, uint64_t partBytes) const {
    textureFlush();
    const std::vector<const Item*> order = fileOrder();
    std::vector<std::vector<const Item*>> parts(1);
    uint64_t used = 0;
    const uint64_t limit = std::max<uint64_t>(partBytes, 1 << 14);
    for (const Item* it : order) {
        const uint64_t need = it->size + W3D_BLOB_ALIGN + sizeof(W3dEntry);
        if (used && used + need > limit) { parts.emplace_back(); used = 0; }
        parts.back().push_back(it);
        used += need;
    }
    std::string stem = basePath;
    if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".w3dp") == 0) stem.resize(stem.size() - 5);
    for (size_t k = 0; k < parts.size(); ++k) {
        W3dPackInfo info = info_;
        info.kind = W3D_PACK_POOL;
        info.part = (uint16_t)k;
        info.partCount = (uint16_t)parts.size();
        const std::string p = k ? stem + std::to_string(k) + ".w3dp" : stem + ".w3dp";
        if (!writeItems(p, parts[k], &info)) return 0;
        std::printf("  pool part %zu: %s, %zu entries\n", k, p.c_str(), parts[k].size());
    }
    return (int)parts.size();
}

// ------------------------------------------------------------ DataDir
std::string DataDir::normalize(const std::string& gamePath) {
    std::string s = gamePath;
    for (char& c : s) {
        if (c == '\\') c = '/';
        c = (char)std::tolower((unsigned char)c);
    }
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    return s;
}

bool DataDir::open(const std::string& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;
    for (auto& e : fs::directory_iterator(root, ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".mpq") return openMpq(root);
    }
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = fs::relative(it->path(), root, ec).generic_string();
        files_[normalize(rel)] = it->path().string();
    }
    return !files_.empty();
}

bool DataDir::read(const std::string& gamePath, std::vector<uint8_t>& out) const {
    if (!archives_.empty()) return readMpq(gamePath, out);
    std::string key = normalize(gamePath);
    auto it = files_.find(key);
    if (it == files_.end()) {
        // Some extractors keep a leading "data/" component.
        it = files_.find("data/" + key);
        if (it == files_.end()) return false;
    }
    return readFileFast(it->second, out);
}

// ------------------------------------------------------------ validator
namespace {
const char* typeName(uint32_t t) {
    switch (t) {
        case W3D_ENTRY_TERRAIN: return "TERR";
        case W3D_ENTRY_TEXTURE: return "TEX ";
        case W3D_ENTRY_MODEL: return "MODL";
        case W3D_ENTRY_PLACEMENT: return "PLCM";
        case W3D_ENTRY_WATER: return "WATR";
        case W3D_ENTRY_LIQUID: return "LIQD";
        case W3D_ENTRY_PACKINFO: return "PINF";
        case W3D_ENTRY_COLLISION: return "COLL";
        case W3D_ENTRY_GROUND: return "GRND";
        case W3D_ENTRY_CREATURE: return "CMDL";
        case W3D_ENTRY_DISPLAY: return "CDSP";
        case W3D_ENTRY_CHARACTER: return "CHRS";
        case W3D_ENTRY_CHARIMG: return "CIMG";
        case W3D_ENTRY_ITEM: return "ITEM";
        case W3D_ENTRY_GLUE: return "GLUE";
        case W3D_ENTRY_SPELLFX: return "SVFX";
        case W3D_ENTRY_UI_DIR: return "UIDR";
        case W3D_ENTRY_UI_TEX: return "UITX";
    }
    return "????";
}
const char* fmtName(uint8_t f) {
    static const char* n[] = {"RGB565", "ETC1", "ETC1A4", "RGBA4", "RGBA5551", "A8"};
    return f < 6 ? n[f] : "?";
}
}  // namespace

bool readFileFast(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0);
    out.resize((size_t)size);
    return size == 0 || (bool)f.read(reinterpret_cast<char*>(out.data()), size);
}

int printPackInfo(const std::string& path) {
    std::vector<uint8_t> b;
    if (!readFileFast(path, b)) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    auto fail = [](const char* m) { std::fprintf(stderr, "INVALID: %s\n", m); return 2; };
    if (b.size() < sizeof(W3dPackHeader)) return fail("too small");
    W3dPackHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    if (h.magic != W3DP_MAGIC || h.version != W3DP_VERSION) return fail("bad magic/version");
    if ((uint64_t)h.indexOffset + (uint64_t)h.entryCount * sizeof(W3dEntry) > b.size()) return fail("index out of range");
    const W3dEntry* e = reinterpret_cast<const W3dEntry*>(b.data() + h.indexOffset);

    std::map<uint32_t, size_t> bytesBy, countBy;
    std::map<std::pair<uint32_t, uint32_t>, const W3dEntry*> byKey;
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        if ((uint64_t)e[i].offset + e[i].size > h.indexOffset) return fail("entry out of range");
        if (e[i].offset % W3D_BLOB_ALIGN) return fail("entry misaligned");
        bytesBy[e[i].type] += e[i].size;
        countBy[e[i].type]++;
        byKey[{e[i].type, e[i].id}] = &e[i];
    }
    // stage 5.27: a map pack or a pool part refers to entries of the other pool parts next to it
    std::vector<std::vector<W3dEntry>> companions;
    if (const auto* pe = byKey.count({W3D_ENTRY_PACKINFO, 0}) ? byKey[{W3D_ENTRY_PACKINFO, 0}] : nullptr) {
        W3dPackInfo pi{};
        std::memcpy(&pi, b.data() + pe->offset, std::min<size_t>(sizeof pi, pe->size));
        static const char* kKind[3] = {"standalone", "pool part", "map"};
        std::printf("  PINF: %s, build %08x, part %u of %u, map %u\n", pi.kind < 3 ? kKind[pi.kind] : "?", pi.buildId, pi.part,
                    pi.partCount, pi.mapId);
        if (pi.kind == W3D_PACK_MAP || pi.kind == W3D_PACK_POOL) {
            const fs::path dir = fs::path(path).parent_path();
            auto readIndex = [&](const fs::path& p, std::vector<W3dEntry>& out, W3dPackInfo* info) -> bool {
                std::ifstream g(p, std::ios::binary);
                W3dPackHeader gh;
                if (!g.read(reinterpret_cast<char*>(&gh), sizeof gh) || gh.magic != W3DP_MAGIC) return false;
                out.resize(gh.entryCount);
                g.seekg(gh.indexOffset);
                if (!g.read(reinterpret_cast<char*>(out.data()), (std::streamsize)(out.size() * sizeof(W3dEntry)))) return false;
                for (const W3dEntry& x : out)
                    if (x.type == W3D_ENTRY_PACKINFO) {
                        g.seekg(x.offset);
                        g.read(reinterpret_cast<char*>(info), std::min<size_t>(sizeof *info, x.size));
                    }
                return true;
            };
            int parts = pi.kind == W3D_PACK_POOL ? pi.partCount : 1;
            for (int k = 0; k < parts; ++k) {
                if (pi.kind == W3D_PACK_POOL && k == pi.part) continue;
                const fs::path p = dir / (k ? "common" + std::to_string(k) + ".w3dp" : std::string("common.w3dp"));
                std::vector<W3dEntry> idx;
                W3dPackInfo ci{};
                if (!readIndex(p, idx, &ci)) return fail(("the pool part " + p.string() + " is missing").c_str());
                if (ci.buildId != pi.buildId) return fail(("the pool part " + p.string() + " is from another conversion").c_str());
                if (k == 0 && pi.kind == W3D_PACK_MAP) parts = std::max<int>(1, ci.partCount);
                {   // stage 6.12: do the pool's models carry the LOD table? (a sample: reading them all would take minutes)
                    std::ifstream g(p, std::ios::binary);
                    size_t seenM = 0, lodM = 0, seenC = 0, lodC = 0;
                    std::vector<uint8_t> blob;
                    for (const W3dEntry& x : idx) {
                        const bool model = x.type == W3D_ENTRY_MODEL, cre = x.type == W3D_ENTRY_CREATURE;
                        if ((!model && !cre) || (model ? seenM : seenC) >= 400 || x.size > (8u << 20)) continue;
                        blob.resize(x.size);
                        g.seekg(x.offset);
                        if (!g.read(reinterpret_cast<char*>(blob.data()), x.size)) break;
                        if (model) { ++seenM; lodM += w3d_model_lods(blob.data(), x.size) != nullptr; }
                        else { ++seenC; lodC += w3d_creature_lods(blob.data(), x.size) != nullptr; }
                    }
                    std::printf("  %s: LOD table (6.12) in %zu of %zu sampled models, %zu of %zu creature models\n",
                                p.filename().string().c_str(), lodM, seenM, lodC, seenC);
                }
                companions.push_back(std::move(idx));
            }
            size_t n = 0;
            for (auto& c : companions)
                for (const W3dEntry& x : c)
                    if (x.type != W3D_ENTRY_PACKINFO) { byKey.emplace(std::make_pair(x.type, x.id), &x); ++n; }
            std::printf("  pool: %zu other file(s) of this conversion read, %zu entries\n", companions.size(), n);
        }
    }
    auto texOk = [&](uint32_t id) { return byKey.count({W3D_ENTRY_TEXTURE, id}) != 0; };

    std::printf("%s: %zu bytes, %u entries\n", path.c_str(), b.size(), h.entryCount);
    for (auto& [t, n] : countBy)
        std::printf("  %s x%-4zu %8.1f KB\n", typeName(t), n, bytesBy[t] / 1024.0);

    size_t texGpu = 0, missingTex = 0, totalPlacements = 0, fxModels = 0, fxEmitters = 0, fxGlows = 0, fxAnimated = 0, lodModels = 0, lodCreatures = 0;
    std::map<uint8_t, size_t> fmtCount;
    for (uint32_t i = 0; i < h.entryCount; ++i) {
        const uint8_t* p = b.data() + e[i].offset;
        if (e[i].type == W3D_ENTRY_TEXTURE || e[i].type == W3D_ENTRY_UI_TEX) {
            W3dTexHeader t; std::memcpy(&t, p, sizeof t);
            uint32_t need = 0, w = t.width, hh = t.height;
            for (int l = 0; l < t.mipCount; ++l) { need += w3d_tex_level_size(w, hh, t.format); w >>= 1; hh >>= 1; }
            if (need != t.dataSize || W3D_TEX_DATA_OFFSET + t.dataSize > e[i].size) return fail("texture size mismatch");
            if ((t.width & (t.width - 1)) || (t.height & (t.height - 1)) || t.width < 8 || t.height < 8) return fail("texture not pow2>=8");
            texGpu += t.dataSize;
            fmtCount[t.format]++;
        } else if (e[i].type == W3D_ENTRY_TERRAIN) {
            W3dTerrainHeader t; std::memcpy(&t, p, sizeof t);
            if (t.offIndices + t.indexCount * 2 > e[i].size) return fail("terrain index range");
            const uint16_t* idx = reinterpret_cast<const uint16_t*>(p + t.offIndices);
            for (uint32_t k = 0; k < t.indexCount; ++k) if (idx[k] >= t.vertexCount) return fail("terrain index >= vertexCount");
            const W3dTerrainBatch* bt = reinterpret_cast<const W3dTerrainBatch*>(p + t.offBatches);
            uint32_t sum = 0, lod0 = 0, lod1 = 0, lod2 = 0;
            for (uint32_t k = 0; k < t.batchCount; ++k) {
                for (int l = 0; l < W3D_TERRAIN_LODS; ++l) {
                    if (bt[k].firstIndex[l] + bt[k].indexCount[l] > t.indexCount) return fail("batch range");
                    if (bt[k].indexCount[l] % 3) return fail("batch not a triangle list");
                    sum += bt[k].indexCount[l];
                }
                if (bt[k].tex0 >= t.texCount || bt[k].tex1 >= t.texCount) return fail("batch tex index");
                if (const uint16_t* t2 = w3d_terrain_tex2(&t, p)) if (t2[k] >= t.texCount) return fail("batch tex2 index");
                lod0 += bt[k].indexCount[0]; lod1 += bt[k].indexCount[1]; lod2 += bt[k].indexCount[2];
            }
            const uint32_t* ids = reinterpret_cast<const uint32_t*>(p + t.offTexIds);
            for (uint32_t k = 0; k < t.texCount; ++k) if (!texOk(ids[k])) ++missingTex;
            std::printf("  tile %u_%u grid %u: %u verts, tris LOD0/1/2 %u/%u/%u, %u batches, %u textures\n",
                        t.tileX, t.tileY, t.gridSize, t.vertexCount, lod0 / 3, lod1 / 3, lod2 / 3, t.batchCount, t.texCount);
            if (sum != t.indexCount) return fail("batches don't cover index buffer");
        } else if (e[i].type == W3D_ENTRY_WATER) {
            W3dWaterHeader t; std::memcpy(&t, p, sizeof t);
            if (t.offIndices + t.indexCount * 2 > e[i].size) return fail("water index range");
            const uint16_t* idx = reinterpret_cast<const uint16_t*>(p + t.offIndices);
            for (uint32_t k = 0; k < t.indexCount; ++k) if (idx[k] >= t.vertexCount) return fail("water index >= vertexCount");
            const W3dWaterBatch* bt = reinterpret_cast<const W3dWaterBatch*>(p + t.offBatches);
            uint32_t frames = 0;
            for (uint32_t k = 0; k < t.batchCount; ++k) {
                if (bt[k].firstIndex + bt[k].indexCount > t.indexCount) return fail("water batch range");
                if (bt[k].tex + std::max<uint32_t>(1, bt[k].frames) > t.texCount) return fail("water batch tex");
                if (bt[k].liquid > 3) return fail("water batch liquid");
                frames = std::max<uint32_t>(frames, bt[k].frames);
            }
            std::printf("  water %u_%u: %u tris, %u batches, %u textures (%u frames)\n", t.tileX, t.tileY, t.indexCount / 3,
                        t.batchCount, t.texCount, frames);
            const uint32_t* ids = reinterpret_cast<const uint32_t*>(p + t.offTexIds);
            for (uint32_t k = 0; k < t.texCount; ++k) if (!texOk(ids[k])) ++missingTex;
            if (!byKey.count({W3D_ENTRY_TERRAIN, e[i].id})) return fail("water without terrain tile");
        } else if (e[i].type == W3D_ENTRY_LIQUID) {
            if (!w3d_liquid_valid(p, e[i].size)) return fail("liquid grid");
            if (!byKey.count({W3D_ENTRY_TERRAIN, e[i].id})) return fail("liquid without terrain tile");
            W3dLiquidHeader lh; std::memcpy(&lh, p, sizeof lh);
            const W3dLiquidChunk* lc = reinterpret_cast<const W3dLiquidChunk*>(p + lh.offChunks);
            size_t cells[4] = {0, 0, 0, 0}, deep = 0, wmo = 0;
            for (uint32_t k = 0; k < lh.chunkCount; ++k)
                for (uint8_t f : lc[k].cell) {
                    if (f == W3D_LIQ_DRY) continue;
                    ++cells[f & W3D_LIQ_KIND];
                    deep += (f & W3D_LIQ_DEEP) != 0;
                    wmo += (f & W3D_LIQ_WMO) != 0;
                }
            std::printf("  liquid %u_%u: %u chunks, cells water %zu ocean %zu magma %zu slime %zu (deep %zu, buildings %zu), "
                        "fog %u %u %u / %.0f yd\n", lh.tileX, lh.tileY, lh.chunkCount, cells[0], cells[1], cells[2], cells[3], deep,
                        wmo, lh.look[0].fog[0], lh.look[0].fog[1], lh.look[0].fog[2], lh.look[0].fogEnd);
        } else if (e[i].type == W3D_ENTRY_MODEL) {
            W3dModelHeader m; std::memcpy(&m, p, sizeof m);
            if (m.offIndices + m.indexCount * 2 > e[i].size) return fail("model index range");
            const uint16_t* idx = reinterpret_cast<const uint16_t*>(p + m.offIndices);
            for (uint32_t k = 0; k < m.indexCount; ++k) if (idx[k] >= m.vertexCount) return fail("model index >= vertexCount");
            const W3dSubmesh* s = reinterpret_cast<const W3dSubmesh*>(p + m.offSubmeshes);
            for (uint32_t k = 0; k < m.submeshCount; ++k) {
                if (s[k].firstIndex + s[k].indexCount > m.indexCount) return fail("submesh range");
                if (s[k].tex != W3D_NO_TEXTURE && s[k].tex >= m.texCount) return fail("submesh tex index");
            }
            const uint32_t* ids = reinterpret_cast<const uint32_t*>(p + m.offTexIds);
            for (uint32_t k = 0; k < m.texCount; ++k) if (!texOk(ids[k])) ++missingTex;
            if (m.offTexIds >= W3D_MODEL_HDR_LOD_END && m.lodCount) {   // stage 6.12
                if (!w3d_model_lods(p, e[i].size)) return fail("model LOD table");
                ++lodModels;
            }
            if (const W3dFxHeader* fx = w3d_model_fx(p, e[i].size)) {   // stage 5.32: effects behind the footer
                ++fxModels;
                fxEmitters += fx->emitterCount;
                fxGlows += fx->glowCount;
                const uint32_t* fid = reinterpret_cast<const uint32_t*>(p + fx->offTexIds);
                for (uint32_t k = 0; k < fx->texCount; ++k) if (!texOk(fid[k])) ++missingTex;
                const W3dFxEmitter* em = reinterpret_cast<const W3dFxEmitter*>(p + fx->offEmitters);
                for (uint32_t k = 0; k < fx->emitterCount; ++k)
                    if ((em[k].tex != 0xFFFF && em[k].tex >= fx->texCount) || !em[k].rows || !em[k].cols || !(em[k].life > 0))
                        return fail("fx emitter");
                if (fx->animOff4) {   // stage 5.35: the bone animation must be a valid block that fits the mesh and the fx
                    ++fxAnimated;
                    const W3dFxAnimView av = w3d_fx_anim(p, e[i].size, fx);
                    if (!av.a || av.a->subCount != m.submeshCount || av.a->emitterCount != fx->emitterCount ||
                        av.a->glowCount != fx->glowCount) return fail("fx animation block");
                    for (uint32_t k = 0; k < av.a->subCount; ++k) if (av.subBone[k] >= av.a->boneCount) return fail("fx animation bone");
                    for (uint32_t k = 0; k < av.a->emitterCount; ++k) if (av.emitterBone[k] >= av.a->boneCount) return fail("fx animation bone");
                    for (uint32_t k = 0; k < av.a->glowCount; ++k) if (av.glowBone[k] >= av.a->boneCount) return fail("fx animation bone");
                    for (uint32_t k = 0; k < (uint32_t)av.a->frames * av.a->boneCount * 12u; ++k)
                        if (!std::isfinite(av.mat[k])) return fail("fx animation matrix");
                }
            }
        } else if (e[i].type == W3D_ENTRY_SPELLFX) {   // stage 5.33
            W3dSpellFxHeader h; std::memcpy(&h, p, sizeof h);
            if (h.magic != W3D_SVFX_MAGIC || h.offEffects + (uint64_t)h.effectCount * sizeof(W3dSpellEffect) > e[i].size ||
                h.offVisuals + (uint64_t)h.visualCount * sizeof(W3dSpellVisual) > e[i].size ||
                h.offSpells + (uint64_t)h.spellCount * sizeof(W3dSpellRef) > e[i].size) return fail("spell fx range");
            const W3dSpellEffect* ef = reinterpret_cast<const W3dSpellEffect*>(p + h.offEffects);
            const W3dSpellVisual* vi = reinterpret_cast<const W3dSpellVisual*>(p + h.offVisuals);
            const W3dSpellRef* sr = reinterpret_cast<const W3dSpellRef*>(p + h.offSpells);
            for (uint32_t k = 0; k < h.effectCount; ++k) if (!byKey.count({W3D_ENTRY_MODEL, ef[k].model})) return fail("spell effect model not packed");
            size_t missiles = 0;
            for (uint32_t k = 0; k < h.visualCount; ++k) {
                if (k && vi[k].id <= vi[k - 1].id) return fail("spell visuals not sorted");
                for (int s = 0; s < W3D_SFX_STAGES; ++s)
                    for (int j = 0; j < 2; ++j)
                        if (vi[k].effect[s][j] != 0xFFFF && vi[k].effect[s][j] >= h.effectCount) return fail("spell visual effect index");
                missiles += vi[k].effect[W3D_SFX_MISSILE][0] != 0xFFFF;
            }
            size_t states = 0, channels = 0;
            for (uint32_t k = 0; k < h.visualCount; ++k) {
                states += vi[k].effect[W3D_SFX_STATE][0] != W3D_SFX_NONE;
                channels += vi[k].effect[W3D_SFX_CHANNEL][0] != W3D_SFX_NONE;
            }
            if ((h.levelUp != W3D_SFX_NONE && h.levelUp >= h.effectCount) || (h.loot != W3D_SFX_NONE && h.loot >= h.effectCount))
                return fail("spell fx special effect index");
            size_t withSpeed = 0;
            for (uint32_t k = 0; k < h.spellCount; ++k) {
                if (k && sr[k].spell <= sr[k - 1].spell) return fail("spell refs not sorted");
                if (sr[k].visual >= h.visualCount) return fail("spell ref visual index");
                withSpeed += sr[k].speed > 0;
            }
            std::printf("  spell fx: %u spells (%zu with a missile speed), %u visuals (%zu with a missile), %u effect models\n",
                        h.spellCount, withSpeed, h.visualCount, missiles, h.effectCount);
            {   // 5.35f: a few well-known spells (Fireball, Frostbolt, Smite, Lesser Heal, Wrath, Immolate, Shadow Bolt, Lightning Bolt)
                const W3dSpellRef* sr = reinterpret_cast<const W3dSpellRef*>(p + h.offSpells);
                std::printf("  spell fx known:");
                for (uint32_t id : {133u, 116u, 585u, 2050u, 5176u, 348u, 686u, 403u}) {
                    bool found = false;
                    for (uint32_t k = 0; k < h.spellCount && !found; ++k) found = sr[k].spell == id;
                    std::printf(" %u:%s", id, found ? "yes" : "NO");
                }
                std::printf("\n");
            }
            std::printf("  spell fx: %zu auras, %zu channels, level-up %s, loot sparkle %s\n", states, channels,
                        h.levelUp != W3D_SFX_NONE ? "yes" : "no", h.loot != W3D_SFX_NONE ? "yes" : "no");
        } else if (e[i].type == W3D_ENTRY_GROUND) {
            if (!w3d_ground_valid(p, e[i].size)) return fail("ground range");
            if (!byKey.count({W3D_ENTRY_TERRAIN, e[i].id})) return fail("ground without terrain tile");
            const W3dGroundHeader* gh = reinterpret_cast<const W3dGroundHeader*>(p);
            const W3dGroundChunk* gc = reinterpret_cast<const W3dGroundChunk*>(p + sizeof(W3dGroundHeader));
            uint32_t present = 0, holes = 0;
            for (int k = 0; k < 256; ++k) { present += (gc[k].flags & W3D_GROUND_CHUNK_PRESENT) != 0; holes += gc[k].holes != 0; }
            std::printf("  ground %u_%u: %u of 256 chunks, %u with holes, zScale %.4f\n", gh->tileX, gh->tileY, present, holes, gh->zScale);
        } else if (e[i].type == W3D_ENTRY_COLLISION) {
            W3dCollHeader c; std::memcpy(&c, p, sizeof c);
            uint64_t cells = (uint64_t)c.gridN * c.gridN + 1;
            if (c.gridN == 0 || c.gridN > 256 || c.offCells + cells * 4 > e[i].size || c.offIndex + (uint64_t)c.indexCount * 4 > e[i].size ||
                c.offTris + (uint64_t)c.triCount * sizeof(W3dCollTri) > e[i].size) return fail("collision range");
            const uint32_t* cs = reinterpret_cast<const uint32_t*>(p + c.offCells);
            const uint32_t* ix = reinterpret_cast<const uint32_t*>(p + c.offIndex);
            for (uint64_t k = 0; k + 1 < cells; ++k) if (cs[k] > cs[k + 1]) return fail("collision cells not monotonic");
            if (cs[cells - 1] != c.indexCount) return fail("collision cell end");
            for (uint32_t k = 0; k < c.indexCount; ++k) if (ix[k] >= c.triCount) return fail("collision index");
            if (!byKey.count({W3D_ENTRY_TERRAIN, e[i].id})) return fail("collision without terrain tile");
            const W3dCollTri* ct = reinterpret_cast<const W3dCollTri*>(p + c.offTris);
            uint32_t walls = 0, fromM2 = 0;
            for (uint32_t k = 0; k < c.triCount; ++k) {
                walls += (ct[k].flags & W3D_COLL_WALL) != 0;
                fromM2 += (ct[k].flags & W3D_COLL_M2) != 0;
            }
            std::printf("  floors+walls %u_%u: %u triangles (%u walls, %u from doodads), grid %u, %u cell refs\n", c.tileX,
                        c.tileY, c.triCount, walls, fromM2, c.gridN, c.indexCount);
        } else if (e[i].type == W3D_ENTRY_CREATURE) {
            W3dCreatureHeader c; std::memcpy(&c, p, sizeof c);
            auto in = [&](uint64_t off, uint64_t n) { return off + n <= e[i].size; };
            if (!in(c.offIndices, c.indexCount * 2ull) || !in(c.offVertices, c.vertexCount * (uint64_t)sizeof(W3dSkinVertex)) ||
                !in(c.offSubmeshes, c.submeshCount * (uint64_t)sizeof(W3dSkinSubmesh)) ||
                !in(c.offPalettes, c.paletteCount * (uint64_t)sizeof(W3dBonePalette)) ||
                !in(c.offBones, c.boneCount * (uint64_t)sizeof(W3dBone)) || !in(c.offSeqs, c.seqCount * (uint64_t)sizeof(W3dAnimSeq)) ||
                !in(c.offKeys, c.keyCount * 2ull) || !in(c.offTex, c.texCount * (uint64_t)sizeof(W3dCreatureTex)))
                return fail("creature range");
            const uint16_t* idx = reinterpret_cast<const uint16_t*>(p + c.offIndices);
            for (uint32_t k = 0; k < c.indexCount; ++k) if (idx[k] >= c.vertexCount) return fail("creature index");
            if (c.offTex >= sizeof(W3dCreatureHeader) && c.lodCount) {   // stage 6.12
                if (!w3d_creature_lods(p, e[i].size)) return fail("creature LOD table");
                ++lodCreatures;
            }
            const W3dSkinSubmesh* sm = reinterpret_cast<const W3dSkinSubmesh*>(p + c.offSubmeshes);
            const W3dBonePalette* pal = reinterpret_cast<const W3dBonePalette*>(p + c.offPalettes);
            const W3dSkinVertex* vx = reinterpret_cast<const W3dSkinVertex*>(p + c.offVertices);
            for (uint32_t k = 0; k < c.submeshCount; ++k) {
                if ((uint64_t)sm[k].firstIndex + sm[k].indexCount > c.indexCount) return fail("creature submesh range");
                if (sm[k].palette >= c.paletteCount) return fail("creature palette index");
                if (sm[k].texSlot != W3D_NO_TEXTURE && sm[k].texSlot >= c.texCount) return fail("creature tex slot");
                const W3dBonePalette& pp = pal[sm[k].palette];
                if (pp.count == 0 || pp.count > W3D_PALETTE_BONES) return fail("palette size");
                for (uint32_t q = sm[k].firstIndex; q < sm[k].firstIndex + sm[k].indexCount; ++q) {
                    const W3dSkinVertex& v = vx[idx[q]];
                    for (int j = 0; j < 2; ++j)
                        if (v.bone[j] % 3 || v.bone[j] / 3 >= pp.count) return fail("vertex bone outside its palette");
                    if (v.weight[0] + v.weight[1] != 255) return fail("weights don't sum to 255");
                }
                for (int j = 0; j < pp.count; ++j) if (pp.bones[j] >= c.boneCount) return fail("palette bone index");
            }
            const W3dBone* bo = reinterpret_cast<const W3dBone*>(p + c.offBones);
            for (uint32_t k = 0; k < c.boneCount; ++k) if (bo[k].parent >= (int)k) return fail("bone parent order");
            const W3dAnimSeq* sq = reinterpret_cast<const W3dAnimSeq*>(p + c.offSeqs);
            for (uint32_t k = 0; k < c.seqCount; ++k) {
                if (!in(sq[k].offChannels, c.boneCount * 12ull) || sq[k].frames == 0) return fail("sequence range");
                const uint32_t* ch = reinterpret_cast<const uint32_t*>(p + sq[k].offChannels);
                for (uint32_t q = 0; q < c.boneCount * 3; ++q) {
                    if (ch[q] == W3D_CHAN_NONE) continue;
                    uint64_t n = (ch[q] & W3D_CHAN_ANIM) ? sq[k].frames : 1, comps = q % 3 == 0 ? 4 : 3;
                    if ((ch[q] & ~W3D_CHAN_ANIM) + n * comps > c.keyCount) return fail("channel keys out of range");
                }
            }
            const W3dCreatureTex* ct = reinterpret_cast<const W3dCreatureTex*>(p + c.offTex);
            for (uint32_t k = 0; k < c.texCount; ++k) if (ct[k].kind == W3D_CTEX_FIXED && !texOk(ct[k].texId)) ++missingTex;
            if (c.attachCount) {
                if (!in(c.offAttach, (uint64_t)c.attachCount * sizeof(W3dAttach))) return fail("attachment range");
                const W3dAttach* at = reinterpret_cast<const W3dAttach*>(p + c.offAttach);
                for (uint32_t k = 0; k < c.attachCount; ++k) if (at[k].bone < 0 || (uint32_t)at[k].bone >= c.boneCount) return fail("attachment bone");
            }
        } else if (e[i].type == W3D_ENTRY_DISPLAY) {
            W3dDisplay d; std::memcpy(&d, p, sizeof d);
            if (!byKey.count({W3D_ENTRY_CREATURE, d.modelId})) return fail("display without model");
            for (uint32_t t : d.tex) if (t && !texOk(t)) ++missingTex;
            if (d.geosetCount > W3D_DISPLAY_GEOSETS) return fail("display geosets");
        } else if (e[i].type == W3D_ENTRY_CHARACTER) {
            W3dCharHeader c; std::memcpy(&c, p, sizeof c);
            if (c.offSections + (uint64_t)c.sectionCount * sizeof(W3dCharSection) > e[i].size ||
                c.offStyles + (uint64_t)c.styleCount * sizeof(W3dCharStyle) > e[i].size) return fail("character range");
            if (!byKey.count({W3D_ENTRY_CREATURE, c.modelId})) return fail("character without model");
            const W3dCharSection* cs = reinterpret_cast<const W3dCharSection*>(p + c.offSections);
            for (uint32_t k = 0; k < c.sectionCount; ++k) {
                if (cs[k].kind == W3D_CSEC_HAIR) { if (cs[k].img[0] && !texOk(cs[k].img[0])) ++missingTex; continue; }
                for (uint32_t im : cs[k].img)
                    if (im && !byKey.count({W3D_ENTRY_CHARIMG, im})) return fail("character layer not packed");
            }
            std::printf("  player race %u sex %u: %u CharSections rows, %u styles, scale %.2f\n", c.race, c.sex,
                        c.sectionCount, c.styleCount, c.scale);
        } else if (e[i].type == W3D_ENTRY_CHARIMG) {
            W3dCharImage c; std::memcpy(&c, p, sizeof c);
            if (sizeof c + (uint64_t)c.w * c.h * 2 > e[i].size) return fail("character layer size");
            if (c.x + c.w > W3D_CHAR_ATLAS || c.y + c.h > W3D_CHAR_ATLAS) return fail("character layer outside the atlas");
        } else if (e[i].type == W3D_ENTRY_ITEM) {
            if (!w3d_item_valid(p, e[i].size)) return fail("item size");
            W3dItemDisplay d; std::memcpy(&d, p, sizeof d);
            for (int sx = 0; sx < 2; ++sx) {
                for (uint32_t im : d.img[sx])
                    if (im && !byKey.count({W3D_ENTRY_CHARIMG, im})) return fail("item layer not packed");
                if (d.cape[sx] && !texOk(d.cape[sx])) ++missingTex;
                if (d.model[sx] && !byKey.count({W3D_ENTRY_MODEL, d.model[sx]})) return fail("item model not packed");
            }
            for (uint32_t m : d.helm)
                if (m && !byKey.count({W3D_ENTRY_MODEL, m})) return fail("item helmet model not packed");
        } else if (e[i].type == W3D_ENTRY_GLUE) {
            if (e[i].size < sizeof(W3dGlueScene)) return fail("glue scene size");
            W3dGlueScene g; std::memcpy(&g, p, sizeof g);
            if (g.modelId && !byKey.count({W3D_ENTRY_MODEL, g.modelId})) return fail("glue scene model not packed");
            std::printf("  glue scene %u: model %08x, camera %s, stand %s, radius %.1f\n", e[i].id, g.modelId,
                        (g.flags & W3D_GLUE_HAS_CAMERA) ? "yes" : "no", (g.flags & W3D_GLUE_HAS_STAND) ? "yes" : "no", g.radius);
        } else if (e[i].type == W3D_ENTRY_PLACEMENT) {
            W3dPlacementHeader ph; std::memcpy(&ph, p, sizeof ph);
            if (sizeof ph + ph.count * sizeof(W3dPlacement) > e[i].size) return fail("placement range");
            const W3dPlacement* pl = reinterpret_cast<const W3dPlacement*>(p + sizeof ph);
            size_t missingModels = 0;
            for (uint32_t k = 0; k < ph.count; ++k) if (!byKey.count({W3D_ENTRY_MODEL, pl[k].modelId})) ++missingModels;
            totalPlacements += ph.count;
            if (missingModels) return fail("placement references missing model");
        }
    }
    std::printf("  placements: %zu\n", totalPlacements);
    std::printf("  LODs (6.12): %zu models, %zu creature models with lighter versions\n", lodModels, lodCreatures);
    if (fxModels) std::printf("  effects: %zu models, %zu particle emitters, %zu glow sprites, %zu with bone animation\n", fxModels, fxEmitters, fxGlows, fxAnimated);
    std::printf("  texture GPU memory: %.1f KB (", texGpu / 1024.0);
    for (auto& [fmt, n] : fmtCount) std::printf(" %s:%zu", fmtName(fmt), n);
    std::printf(" )\n");
    if (missingTex) return fail("texture id referenced but not packed");
    std::printf("OK\n");
    return 0;
}

}  // namespace w3d
