// recipe_test — 6.90: recipes.txt's item requirement column ("class:subclassMask:inventoryTypeMask") and Recipe::fits: an enchant
// for boots fits boots only, a weapon enchant weapons of its subclasses, an old file without the column fits anything.
#include <cstdio>

#include "../tables.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/tmp/w3d_recipe_test.txt";
    if (FILE* f = fopen(path, "w")) {
        fputs("# wow3ds recipes\n", f);
        fputs("R\t333\t7863\t0\t90\t110\t1\t0\t1\t-\t10940:1\t4:0:256\n", f);          // Enchant Boots: armour, feet
        fputs("R\t333\t7786\t0\t90\t110\t1\t0\t1\t-\t10940:2\t2:8192:0\n", f);         // a weapon enchant: subclass 13 (fist) only
        fputs("R\t164\t2663\t0\t1\t30\t0\t2852\t1\t5956:1\t2840:2\n", f);               // an old line: no column
        fclose(f);
    }
    RecipeTable t;
    CHECK(t.load(path) && t.all().size() == 3, "load");
    const Recipe* boots = t.find(7863);
    const Recipe* fist = t.find(7786);
    const Recipe* old = t.find(2663);
    CHECK(boots && boots->itemClass == 4 && boots->invMask == 256, "boots requirement parsed");
    if (boots) {
        CHECK(boots->fits(4, 2, 8), "leather boots fit");
        CHECK(!boots->fits(4, 2, 5), "a chest does not");
        CHECK(!boots->fits(2, 7, 13), "a sword does not");
    }
    if (fist) {
        CHECK(fist->fits(2, 13, 13), "a fist weapon fits");
        CHECK(!fist->fits(2, 7, 13), "a sword does not");
    }
    if (old) CHECK(old->itemClass == -1 && old->fits(2, 7, 13) && old->tools.size() == 1 && old->reagents.size() == 1, "an old line fits anything");
    std::printf("RECIPES: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
