// pvs.cpp — 6.54: the precomputed visibility (see pvs.hpp, common/w3d_pvs.h).
#include "pvs.hpp"

#include "world.h"
#include "w3d_pvs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pvs {
namespace {
// 6.54b: only the header, the tile table and the cell tables stay in memory (a town's sets are megabytes); the cells are read when needed
FILE* g_file = nullptr;
uint32_t g_fileSize = 0;
W3dPvsHeader g_hdr;
const W3dPvsHeader* g_h = nullptr;
std::vector<W3dPvsTile> g_tilesv;
std::vector<uint32_t> g_cellOff;     // per tile, cellsPerSide^2 offsets
uint32_t g_stamp = 0;
char g_status[256] = "no PVS";
std::vector<uint32_t> g_sortedOffs;  // every cell's offset, sorted: a cell's size = the next offset - its own

// 6.54c: the cells are read by a thread of their own, each in one read (6.54b read a cell with a dozen small freads on the main thread:
// 100-300 ms hitches at every cell crossing), cached (the last kCache cells), the 8 neighbours of the camera's cell asked for ahead.
// Until the camera's cell is in, the PVS is off for that frame (everything drawn, as without it).
struct Entry { std::vector<uint8_t> data; uint8_t state = 0; uint32_t used = 0; uint32_t off = 0, size = 0; };   // state 0 asked, 1 in, 2 failed
constexpr size_t kCache = 40;
std::unordered_map<int64_t, Entry> g_cache;
std::deque<int64_t> g_queue;
LightLock g_lock;
LightEvent g_wake;
Thread g_thread = nullptr;
std::atomic<bool> g_quit{false};
uint32_t g_useClock = 0;
uint32_t g_cellReads = 0;

int findTile(int tx, int ty) {
    uint32_t lo = 0, hi = (uint32_t)g_tilesv.size();   // sorted by (tx, ty)
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const W3dPvsTile& t = g_tilesv[mid];
        if (t.tx == tx && t.ty == ty) return (int)mid;
        if (t.tx < tx || (t.tx == tx && t.ty < ty)) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

bool readAt(uint32_t off, void* dst, uint32_t n) {
    return std::fseek(g_file, (long)off, SEEK_SET) == 0 && std::fread(dst, 1, n, g_file) == n;
}

void readerMain(void*) {
    std::vector<uint8_t> buf;
    while (!g_quit) {
        LightEvent_Wait(&g_wake);
        for (;;) {
            LightLock_Lock(&g_lock);
            int64_t key = -1;
            uint32_t off = 0, size = 0;
            while (!g_queue.empty() && key < 0) {
                const int64_t k = g_queue.front();
                g_queue.pop_front();
                auto it = g_cache.find(k);
                if (it != g_cache.end() && it->second.state == 0) { key = k; off = it->second.off; size = it->second.size; }
            }
            LightLock_Unlock(&g_lock);
            if (key < 0 || g_quit) break;
            buf.resize(size);
            const bool ok = size && size <= (2u << 20) && readAt(off, buf.data(), size);
            LightLock_Lock(&g_lock);
            auto it = g_cache.find(key);
            if (it != g_cache.end() && it->second.state == 0) {
                if (ok) it->second.data.assign(buf.begin(), buf.end());
                it->second.state = ok ? 1 : 2;
                ++g_cellReads;
            }
            LightLock_Unlock(&g_lock);
        }
    }
}

// ask for a cell (front: needed now); main thread, under no lock
void ask(int64_t key, uint32_t off, bool front) {
    auto it = g_cache.find(key);
    if (it != g_cache.end()) { it->second.used = g_useClock; return; }
    const auto nx = std::upper_bound(g_sortedOffs.begin(), g_sortedOffs.end(), off);
    const uint32_t end = nx != g_sortedOffs.end() ? *nx : g_fileSize;
    Entry e;
    e.off = off;
    e.size = end > off ? end - off : 0;
    e.used = g_useClock;
    if (g_cache.size() >= kCache) {   // drop the least recently used one that is in (or failed)
        auto victim = g_cache.end();
        for (auto i = g_cache.begin(); i != g_cache.end(); ++i)
            if (i->second.state != 0 && (victim == g_cache.end() || i->second.used < victim->second.used)) victim = i;
        if (victim != g_cache.end()) g_cache.erase(victim);
    }
    g_cache.emplace(key, std::move(e));
    if (front) g_queue.push_front(key);
    else g_queue.push_back(key);
}
}  // namespace

void unload() {
    if (g_thread) {   // (the reader first: it uses the file)
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    g_quit = false;
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    g_h = nullptr;
    g_tilesv.clear();
    g_cellOff.clear();
    g_sortedOffs.clear();
    g_cache.clear();
    g_queue.clear();
    std::snprintf(g_status, sizeof g_status, "no PVS");
}

bool loaded() { return g_h != nullptr; }
float occRange() { return g_h ? g_h->occRange : 0.0f; }
const char* status() { return g_status; }

bool load(const char* packPath, uint32_t buildId) {
    unload();
    char path[160];
    std::snprintf(path, sizeof path, "%s", packPath);
    char* dot = std::strrchr(path, '.');
    char* slash = std::strrchr(path, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    if (std::strlen(path) + 5 >= sizeof path) return false;
    std::strcat(path, ".pvs");
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::snprintf(g_status, sizeof g_status, "no %.150s", path); return false; }
    g_file = f;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    auto fail = [&](const char* why) {
        std::snprintf(g_status, sizeof g_status, "%.150s: %s - not used", path, why);
        unload();
        std::snprintf(g_status, sizeof g_status, "%.150s: %s - not used", path, why);
        return false;
    };
    if (n < (long)sizeof(W3dPvsHeader)) return fail("too short");
    g_fileSize = (uint32_t)n;
    W3dPvsHeader h;
    if (!readAt(0, &h, sizeof h)) return fail("cannot read");
    if (h.magic != W3D_PVS_MAGIC || (h.version != W3D_PVS_VERSION && h.version != 3)) return fail("not a PVS of this version (make it again with this w3docc)");
    if (!h.cellsPerSide || h.cellsPerSide > 256 || h.cellSize <= 0 || h.tileCount > 4096 ||
        (uint64_t)h.tileTableOff + (uint64_t)h.tileCount * sizeof(W3dPvsTile) > (uint64_t)n) return fail("damaged");
    if (h.buildId && buildId && h.buildId != buildId) return fail("made for another conversion of the packs");
    std::vector<W3dPvsTile> tiles(h.tileCount);
    if (h.tileCount && !readAt(h.tileTableOff, tiles.data(), h.tileCount * (uint32_t)sizeof(W3dPvsTile))) return fail("cannot read its tiles");
    const uint32_t per = h.cellsPerSide * h.cellsPerSide;
    std::vector<uint32_t> offs((size_t)per * h.tileCount);
    for (uint32_t i = 0; i < h.tileCount; ++i)
        if ((uint64_t)tiles[i].cellTableOff + per * 4ull > (uint64_t)n || !readAt(tiles[i].cellTableOff, offs.data() + (size_t)i * per, per * 4))
            return fail("damaged cell table");
    g_hdr = h;
    g_h = &g_hdr;
    g_tilesv.swap(tiles);
    g_cellOff.swap(offs);
    g_sortedOffs.clear();
    for (uint32_t o : g_cellOff) if (o) g_sortedOffs.push_back(o);
    std::sort(g_sortedOffs.begin(), g_sortedOffs.end());
    g_sortedOffs.erase(std::unique(g_sortedOffs.begin(), g_sortedOffs.end()), g_sortedOffs.end());
    // the reader thread: core 2 on the New 3DS (a lower priority than the main thread), else core 1 / 0
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    g_quit = false;
    {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        if (n3ds) g_thread = threadCreate(readerMain, nullptr, 16 * 1024, prio + 1, 2, false);
        if (!g_thread) g_thread = threadCreate(readerMain, nullptr, 16 * 1024, prio + 1, 1, false);
        if (!g_thread) g_thread = threadCreate(readerMain, nullptr, 16 * 1024, prio + 1, 0, false);
    }
    if (!g_thread) {
        unload();
        std::snprintf(g_status, sizeof g_status, "%.150s: no thread to read it - not used", path);
        return false;
    }
    std::snprintf(g_status, sizeof g_status, "%.120s: %lu tiles, %lu KB, cells %.1f yd, doodad range %.0f..%.0f, distance <= %.0f", path,
                  (unsigned long)h.tileCount, (unsigned long)(n / 1024), h.cellSize, h.occRange, h.maxRange, h.maxDist);
    return true;
}

FrameInfo frame(const float cam[3], float drawDist, const std::vector<Tile*>& tiles, bool enabled, RenderSettings& rs) {
    FrameInfo fi;
    rs.pvsOn = false;
    rs.pvsFarHidden = false;
    if (!g_h) { fi.why = 1; return fi; }
    if (!enabled || !rs.doodads) { fi.why = 2; return fi; }
    if (rs.doodadRange < g_h->occRange - 0.01f || rs.doodadRange > g_h->maxRange + 0.01f || drawDist > g_h->maxDist + 0.5f) { fi.why = 3; return fi; }
    // the camera's tile
    Tile* ct = nullptr;
    for (Tile* t : tiles) {
        if (!t->th) continue;
        const float* o = t->th->origin;
        if (cam[0] >= o[0] && cam[0] < o[0] + 533.33333f && cam[1] >= o[1] && cam[1] < o[1] + 533.33333f) { ct = t; break; }
    }
    if (!ct) { fi.why = 4; return fi; }
    // every tile around it that is in has every building shown (the occluders of the set must be there; checked below too)
    for (Tile* t : tiles)
        if (std::abs(t->tx - ct->tx) <= 1 && std::abs(t->ty - ct->ty) <= 1 && t->bldgsClosed > 0) { fi.why = 5; return fi; }
    const int ti = findTile(ct->tx, ct->ty);
    if (ti < 0) { fi.why = 4; return fi; }
    const W3dPvsTile* pt = &g_tilesv[(size_t)ti];
    const int N = (int)g_h->cellsPerSide;
    const int cx = (int)floorf((cam[0] - pt->origin[0]) / g_h->cellSize), cy = (int)floorf((cam[1] - pt->origin[1]) / g_h->cellSize);
    if (cx < 0 || cy < 0 || cx >= N || cy >= N) { fi.why = 4; return fi; }
    const uint32_t off = g_cellOff[(size_t)ti * N * N + (size_t)(cx + cy * N)];
    if (!off) { fi.why = 4; return fi; }
    const int64_t key = (int64_t)ti * N * N + cx + cy * N;
    // the cell from the cache (asked for when missing) and its 8 neighbours asked for ahead
    const Entry* cell = nullptr;
    {
        LightLock_Lock(&g_lock);
        ++g_useClock;
        const size_t before = g_queue.size();
        if (!g_cache.count(key)) fi.read = true;
        ask(key, off, true);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                int nx = cx + dx, ny = cy + dy, ntx = ct->tx, nty = ct->ty;
                if (nx < 0) { nx += N; --ntx; } else if (nx >= N) { nx -= N; ++ntx; }   // (cells along x lie in the tile next along x)
                if (ny < 0) { ny += N; --nty; } else if (ny >= N) { ny -= N; ++nty; }
                // (render x grows with the tile's y index, render y with its x index: the tile next along render x is ty - 1)
                int nti = ti;
                if (ntx != ct->tx || nty != ct->ty) {
                    const int otx = ct->tx - (nty - ct->ty), oty = ct->ty - (ntx - ct->tx);
                    nti = findTile(otx, oty);
                    if (nti < 0) continue;
                }
                const uint32_t noff = g_cellOff[(size_t)nti * N * N + (size_t)(nx + ny * N)];
                if (noff) ask((int64_t)nti * N * N + nx + ny * N, noff, false);
            }
        auto it = g_cache.find(key);
        if (it != g_cache.end() && it->second.state == 1) cell = &it->second;
        const bool signal = g_queue.size() != before || (it != g_cache.end() && it->second.state == 0);
        LightLock_Unlock(&g_lock);
        if (signal) LightEvent_Signal(&g_wake);
    }
    if (!cell) { fi.why = 6; return fi; }   // (being read: this frame without the PVS)
    W3dPvsLevel lv[16];
    // (only this thread drops cache entries, and the reader never touches one that is in: the data stays while it is used here)
    const int nl = w3d_pvs_levels(cell->data.data(), (uint32_t)cell->data.size(), 0, lv, 16);
    int use[16], nu = 0;
    for (int k = 0; k < nl; ++k)
        if (cam[2] >= lv[k].zlo && cam[2] <= lv[k].zhi) use[nu++] = k;
    if (!nu) { fi.why = 4; return fi; }
    // a tile the set names within one tile of the camera's that is not in yet: its buildings, which hid things in the set, are missing
    bool farHidden = true;   // 6.54d: every level used says nothing past the distance can be seen
    for (int u = 0; u < nu; ++u) {
        const uint8_t* p = lv[use[u]].data;
        const uint8_t* end = p + lv[use[u]].bytes;
        const uint32_t flags = w3d_pvs_varint(&p, end);
        if (!(flags & W3D_PVS_FAR_HIDDEN)) farHidden = false;
        const uint32_t groups = w3d_pvs_varint(&p, end);
        for (uint32_t g = 0; g < groups && end - p >= 2; ++g) {
            const int dtx = (int8_t)p[0], dty = (int8_t)p[1];
            p += 2;
            if (std::abs(dtx) <= 1 && std::abs(dty) <= 1) {
                bool have = false;
                for (Tile* x : tiles) if (x->tx == ct->tx + dtx && x->ty == ct->ty + dty) { have = true; break; }
                if (!have) { fi.why = 5; return fi; }
            }
            for (int kind = 0; kind < 3; ++kind) {
                const uint32_t n = w3d_pvs_varint(&p, end);
                for (uint32_t i = 0; i < n; ++i) w3d_pvs_varint(&p, end);
            }
        }
    }
    // mark
    if (++g_stamp == 0) g_stamp = 1;
    const uint32_t stamp = g_stamp;
    for (int u = 0; u < nu; ++u) {
        const uint8_t* p = lv[use[u]].data;
        const uint8_t* end = p + lv[use[u]].bytes;
        w3d_pvs_varint(&p, end);   // (the flags: read above)
        const uint32_t groups = w3d_pvs_varint(&p, end);
        for (uint32_t g = 0; g < groups && end - p >= 2; ++g) {
            const int tx = ct->tx + (int8_t)p[0], ty = ct->ty + (int8_t)p[1];
            p += 2;
            Tile* t = nullptr;
            for (Tile* x : tiles) if (x->tx == tx && x->ty == ty) { t = x; break; }
            if (t) {
                t->pvsStamp = stamp;
                if (t->th && t->pvsTerr.size() != t->th->batchCount) t->pvsTerr.assign(t->th->batchCount, 0);
            }
            for (int kind = 0; kind < 3; ++kind) {   // doodads, ground batches, WMO groups (6.54d)
                const uint32_t n = w3d_pvs_varint(&p, end);
                uint32_t prev = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t v = w3d_pvs_varint(&p, end);
                    const uint32_t idx = i == 0 ? v : prev + 1 + v;
                    prev = idx;
                    if (!t) continue;
                    if (kind != 1) {
                        if (idx < t->plToInst.size() && t->plToInst[idx] >= 0 && (size_t)t->plToInst[idx] < t->instances.size()) {
                            t->instances[(size_t)t->plToInst[idx]].pvsStamp = stamp;
                            ++fi.marked;
                        }
                    } else if (idx < t->pvsTerr.size()) {
                        t->pvsTerr[idx] = stamp;
                        ++fi.marked;
                    }
                }
            }
        }
    }
    rs.pvsOn = true;
    rs.pvsStamp = stamp;
    rs.pvsNear = g_h->nearAlways;
    rs.pvsMaxDist = g_h->maxDist;
    rs.pvsInteriors = g_h->version >= 4;   // 6.56: a v3 file never lists interior groups seen from afar
    rs.pvsFarHidden = farHidden;
    rs.pvsFarR = g_h->maxDist - 2.0f * g_h->cellSize;   // (the builder's: maxDist - 2 cells)
    fi.farHidden = farHidden;
    fi.on = true;
    fi.levels = (uint16_t)nu;
    return fi;
}

}  // namespace pvs
