// Parses the 28 SMSG_UPDATE_OBJECT packets of a real WoW Forever login (Northshire, 2026-11-03; data/forever_update.fwl holds only those packets).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>

#include "../forever/fupdate.hpp"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "net/tests/data/forever_update.fwl";
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(d.size() > 4 && !memcmp(d.data(), "FWL1", 4), "log header");
    std::vector<wf::Bytes> packets;
    for (size_t i = 4; i + 13 <= d.size();) {
        uint32_t op, sz;
        memcpy(&op, &d[i + 1], 4);
        memcpy(&sz, &d[i + 9], 4);
        i += 13;
        if (i + sz > d.size()) break;
        if (op == 0x5E0000) packets.emplace_back(d.begin() + i, d.begin() + i + sz);
        i += sz;
    }
    CHECK(packets.size() == 28, "%zu packets", packets.size());
    int units = 0, gos = 0, items = 0, players = 0, values = 0, transports = 0, withSpline = 0, decoded = 0;
    const wf::FObject* firstWolf = nullptr;
    const wf::FObject* self = nullptr;
    std::vector<wf::FUpdate> all(packets.size());
    for (size_t i = 0; i < packets.size(); ++i) {
        std::string err;
        const bool ok = wf::parseUpdateObject(packets[i], all[i], err);
        CHECK(ok, "packet %zu: %s", i, err.c_str());
        values += (int)all[i].values.size();
        for (const wf::FObject& o : all[i].objects) {
            if (o.objType == wf::FOBJ_UNIT) { ++units; if (o.fieldsDecoded) ++decoded; if (!firstWolf && o.fieldsDecoded && o.entry == 69) firstWolf = &o; if (o.spline.has) ++withSpline; }
            else if (o.objType == wf::FOBJ_GAMEOBJECT) { ++gos; if (o.fieldsDecoded && o.goType == 15) ++transports; }
            else if (o.objType == wf::FOBJ_ITEM) ++items;
            else if (o.objType == wf::FOBJ_ACTIVE_PLAYER) { ++players; self = &o; }
        }
    }
    printf("units %d (decoded %d, splines %d), game objects %d (transports %d), items %d, players %d, values blocks %d\n", units, decoded, withSpline, gos, transports, items, players, values);
    CHECK(units == 59 && decoded == 59, "units");
    CHECK(gos == 49 && transports == 6, "game objects");
    CHECK(items == 7 && players == 1 && values == 69, "items / player / values");
    CHECK(self && self->thisIsYou && self->hasPos && std::fabs(self->x + 8949.95f) < 0.01f && std::fabs(self->y + 132.493f) < 0.01f && std::fabs(self->z - 83.531f) < 0.01f, "self position");
    CHECK(firstWolf != nullptr, "a wolf");
    if (firstWolf) {
        CHECK(firstWolf->display == 604 && firstWolf->level == 2 && firstWolf->health == 55 && firstWolf->maxHealth == 55 && firstWolf->faction == 32, "wolf fields %d %d %lld %d", firstWolf->display, firstWolf->level, (long long)firstWolf->health, firstWolf->faction);
        CHECK(std::fabs(firstWolf->x + 9028.41f) < 0.01f && std::fabs(firstWolf->y + 164.729f) < 0.01f && std::fabs(firstWolf->z - 79.026f) < 0.01f, "wolf position");
        CHECK(firstWolf->hasSpeeds && std::fabs(firstWolf->speeds[1] - 6.0f) < 0.01f, "run speed %f", firstWolf->speeds[1]);
    }
    // an NPC with NPC flags: Brother Sammuel (925), level 5, 102 HP
    bool sammuel = false;
    for (auto& u : all) for (auto& o : u.objects) if (o.objType == wf::FOBJ_UNIT && o.entry == 925) sammuel = o.level == 5 && o.health == 102 && o.npcFlags == 51;
    CHECK(sammuel, "entry 925");
    // damaged input: every truncation and a few thousand random bit flips must neither crash nor hang
    std::mt19937 rng(7);
    for (size_t k = 0; k < packets.size(); ++k) {
        const wf::Bytes& p = packets[k];
        if (p.size() > 5000) continue;
        for (size_t cut = 0; cut < p.size(); cut += 1 + p.size() / 300) {
            wf::FUpdate u; std::string e;
            if (wf::parseUpdateObject(p.data(), cut, u, e)) { ++fails; printf("FAIL: a truncated packet %zu at %zu parsed\n", k, cut); }
        }
        for (int t = 0; t < 400; ++t) {
            wf::Bytes q = p;
            for (int n = 0; n < 1 + (int)(rng() % 4); ++n) q[rng() % q.size()] ^= (uint8_t)(1u << (rng() % 8));
            wf::FUpdate u; std::string e;
            wf::parseUpdateObject(q, u, e);
        }
    }
    printf(fails ? "FOREVER UPDATE: FAIL\n" : "FOREVER UPDATE: PASS\n");
    return fails ? 1 : 0;
}
