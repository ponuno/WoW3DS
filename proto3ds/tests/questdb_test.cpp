// questdb_test — 6.63: questdb.bin (tools/questdb/questie_import.py over a small Questie-format sample) through questdb::Db, and the
// quest watch (questwatch::Watch). questdb_test <questdb.bin> [<questdb_v2.bin>]  (version 2: world coordinates, from tdb_import.py)
#include <cmath>
#include <cstdio>

#include "../source/questdb.hpp"
#include "../source/questwatch.hpp"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: questdb_test <questdb.bin>"); return 2; }
    questdb::Db db;
    CHECK(!db.opened() && !db.has(1), "nothing before open");
    CHECK(!questdb::Db().open("/nonexistent/questdb.bin"), "a missing file");
    CHECK(db.open(argv[1]) && db.count() == 4, "open: %zu quests", db.count());
    questdb::Quest q;
    CHECK(!db.find(999, &q) && !db.has(999), "an unknown quest");
    // quest 6: given and handed in by NPC 823 (zone 12, 48.17 42.94); asks for 10 of item 182, dropped by NPC 103 (two packs of spawns)
    CHECK(db.find(6, &q), "quest 6");
    CHECK(q.start.size() == 1 && q.start[0].area == 12 && q.start[0].x == 4817 && q.start[0].y == 4294, "starter (%u %u %u)", q.start.empty() ? 0 : q.start[0].area, q.start.empty() ? 0 : q.start[0].x, q.start.empty() ? 0 : q.start[0].y);
    CHECK(q.end.size() == 1 && q.end[0].x == 4817, "finisher");
    CHECK(q.obj.size() == 1 && q.obj[0].kind == questdb::kItem && q.obj[0].id == 182, "the item objective");
    CHECK(q.obj.size() == 1 && q.obj[0].blobs.size() == 2, "two packs of the dropper's spawns -> two blobs (%zu)", q.obj.empty() ? 0 : q.obj[0].blobs.size());
    if (q.obj.size() == 1 && q.obj[0].blobs.size() == 2) {
        bool a = false, b = false;
        for (const questdb::Blob& bl : q.obj[0].blobs) {
            a = a || (bl.area == 12 && std::abs((int)bl.x - 5750) < 150 && std::abs((int)bl.y - 4800) < 150);
            b = b || (bl.area == 12 && std::abs((int)bl.x - 2000) < 150 && std::abs((int)bl.y - 7000) < 150);
            CHECK(bl.r >= 70 && bl.r <= 1400, "a sensible radius (%u)", bl.r);
        }
        CHECK(a && b, "the blobs lie on the two packs");
    }
    // quest 7: kill 8 creature 6 (12 spawns in one cluster)
    CHECK(db.find(7, &q) && q.obj.size() == 1 && q.obj[0].kind == questdb::kKill && q.obj[0].id == 6 && !q.obj[0].blobs.empty() && q.obj[0].blobs.size() <= 4, "quest 7: kill, a few small dots (6.76)");
    // quest 8: an object objective + an explore area
    CHECK(db.find(8, &q) && q.obj.size() == 2 && q.obj[0].kind == questdb::kObject && q.obj[0].id == 31 && q.obj[1].kind == questdb::kExplore, "quest 8: object + explore");
    CHECK(q.obj.size() == 2 && !q.obj[1].blobs.empty() && q.obj[1].blobs[0].area == 44, "the explore area");
    // quest 9: its only spawns lie outside 0..100 (dropped), so there is nothing to keep; so is quest 11 (nobody known)
    CHECK(!db.has(9) && !db.has(11), "quests without a known place are left out");
    // a kill-credit objective (quest 10) uses its base creature
    CHECK(db.find(10, &q) && q.obj.size() == 1 && q.obj[0].kind == questdb::kKill && q.obj[0].id == 6 && !q.obj[0].blobs.empty(), "quest 10: kill credit");
    // decode refuses garbage
    const uint8_t cut[] = {1, 0, 1, 0, 12, 0};
    CHECK(!questdb::Db::decode(cut, sizeof cut, &q), "a cut record");
    const uint8_t bad[] = {0, 0, 1, 0, 9, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!questdb::Db::decode(bad, sizeof bad, &q), "an objective of an unknown kind");

    // ---- the watch
    questwatch::Watch w;
    CHECK(!w.tracked(5) && w.ids().empty(), "empty");
    CHECK(w.toggle(5) && w.tracked(5) && w.slot(5) == 0, "follow");
    CHECK(w.toggle(5) && !w.tracked(5), "unfollow");
    for (uint32_t i = 1; i <= 5; ++i) CHECK(w.toggle(i * 10), "follow %u", i * 10);
    CHECK(!w.toggle(99) && !w.tracked(99), "the sixth is refused");
    CHECK(w.toggle(30) && !w.tracked(30) && w.toggle(99) && w.tracked(99), "room again after one is dropped");
    CHECK(w.slot(99) == 4, "the newest takes the last slot (%d)", w.slot(99));
    // sync: a log not ready changes nothing; leaving quests drop; the first ready call only learns
    questwatch::Watch s;
    s.parse("10\n20\n");
    CHECK(s.tracked(10) && s.tracked(20), "restored");
    CHECK(!s.sync({}, false) && s.tracked(10), "an empty log that has not arrived keeps the watch");
    CHECK(s.sync({10, 30, 40}, true) && s.tracked(10) && !s.tracked(20) && !s.tracked(30), "20 left the log; the first ready call does not follow 30 / 40");
    CHECK(s.sync({10, 30, 40, 50}, true) && s.tracked(50), "a new quest is followed");
    CHECK(!s.sync({10, 30, 40, 50}, true), "no change, no change");
    questwatch::Watch f;
    f.sync({1, 2, 3, 4, 5, 6}, true);
    f.sync({1, 2, 3, 4, 5, 6, 7, 8}, true);
    CHECK(f.ids().size() == 2 && f.tracked(7) && f.tracked(8), "new ones are followed while there is room (%zu)", f.ids().size());
    f.sync({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, true);
    CHECK(f.ids().size() == 5, "never more than five (%zu)", f.ids().size());
    // save / load
    const std::string txt = f.serialize();
    questwatch::Watch r;
    r.parse(txt + "junk\n0\n123456789012\n");
    CHECK(r.ids() == f.ids(), "round trip (%zu)", r.ids().size());
    CHECK(db.version() == 1 && !q.world, "version 1 file");
    if (argc > 2) {   // version 2: signed world coordinates in yards
        questdb::Db d2;
        CHECK(d2.open(argv[2]) && d2.version() == 2 && d2.count() == 2, "open v2: %zu", d2.count());
        questdb::Quest w;
        CHECK(d2.find(100, &w) && w.world, "quest 100 (v2)");
        CHECK(w.start.size() == 1 && w.start[0].area == 0 && w.start[0].x == -8913 && w.start[0].y == -133, "negative world position (%d %d)", w.start.empty() ? 0 : w.start[0].x, w.start.empty() ? 0 : w.start[0].y);
        CHECK(w.end.size() == 1 && w.end[0].area == 1 && w.end[0].x == 10312 && w.end[0].y == -2000, "end on map 1");
        CHECK(w.obj.size() == 1 && w.obj[0].kind == questdb::kKill && w.obj[0].id == 299 && w.obj[0].blobs.size() == 2, "objective");
        if (w.obj.size() == 1 && w.obj[0].blobs.size() == 2) CHECK(w.obj[0].blobs[1].x == -9018 && w.obj[0].blobs[1].y == -164 && w.obj[0].blobs[1].r == 40, "blob");
        CHECK(!d2.find(101, &w) && d2.find(105, &w) && w.start.empty() && w.end.empty() && w.obj.empty() == false, "quest 105 / unknown 101");
        // the old reader rule: a version-1 file is not world
        questdb::Quest v1;
        CHECK(db.find(6, &v1) && !v1.world, "v1 not world");
    }
    std::printf("QUEST DB / WATCH: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
