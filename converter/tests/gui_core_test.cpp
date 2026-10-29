// w3dgui core: steps, quoting, settings round trip.
#include "../gui/gui_core.h"
#include <cstdio>
using namespace w3gui;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    Settings s;
    s.data = "E:\\Wow Classic\\Data"; s.out = "D:\\sd\\w3dproto";
    CHECK(validate(s).empty());
    auto st = buildSteps(s);
    CHECK(st.size() == 3 && st[0].logFile == "log_ui.txt" && st[1].logFile == "log_sound.txt" && st[2].logFile == "log_world.txt");
    const auto& w = st[2].args;
    auto has = [&](const char* a) { for (auto& x : w) if (x == a) return true; return false; };
    CHECK(has("--creatures") && has("--players") && has("--transports") && !has("--no-items") && !has("--world") && has("--tiles"));
    CHECK(worldPackName("Kalimdor") == "map1.w3dp" && worldPackName("Azeroth") == "test.w3dp");
    CHECK(w.back() == "-v" && w[w.size() - 2] == "D:\\sd\\w3dproto\\test.w3dp" && w[w.size() - 3] == "-o");
    CHECK(commandLine("C:\\t\\w3dconv.exe", st[0]).find("--data \"E:\\Wow Classic\\Data\" --ui -o D:\\sd\\w3dproto\\ui.w3dp") != std::string::npos);
    s.world = true; s.items = false; s.quality = 2; s.extra = "--creature-tris 600 --transport-filter \"ship,zeppelin\"";
    w3gui::Settings s2 = parse(serialize(s));
    CHECK(s2.world && !s2.items && s2.quality == 2 && s2.extra == s.extra && s2.data == s.data && s2.tx1 == 34);
    auto w2 = buildSteps(s2)[2].args;
    auto has2 = [&](const char* a) { for (auto& x : w2) if (x == a) return true; return false; };
    CHECK(has2("--world") && !has2("--tiles") && has2("--no-items") && has2("high") && has2("--creature-tris") && has2("ship,zeppelin"));
    s.out = "D:\\sd\\"; CHECK(buildSteps(s)[1].args[4] == "D:\\sd\\sound");
    CHECK(quoteArg("a b\\") == "\"a b\\\\\"");
    CHECK(quoteArg("x") == "x" && quoteArg("") == "\"\"");
    Settings e; CHECK(!validate(e).empty());
    e.data = "x"; e.out = "y"; e.ui = e.sound = e.packWorld = false; CHECK(!validate(e).empty());
    e.packWorld = true; e.tx1 = 99; CHECK(!validate(e).empty());
    Settings dg; dg.data = "D"; dg.out = "O"; dg.ui = dg.sound = dg.packWorld = false; dg.dungeons = true;
    CHECK(validate(dg).empty());
    auto ds = buildSteps(dg);
    CHECK(ds.size() == 1 && ds[0].args[2] == "--dungeons" && ds[0].args[ds[0].args.size() - 3] == "-o" && ds[0].args[ds[0].args.size() - 2] == "O");
    CHECK(parse(serialize(dg)).dungeons);
    Settings ev; ev.data = "D"; ev.out = "O"; ev.everything = true; ev.sound = false; ev.items = false; ev.packWorld = false; ev.ui = false;
    CHECK(validate(ev).empty());
    auto es = buildSteps(ev);
    auto hasE = [&](const char* a) { for (auto& x : es[0].args) if (x == a) return true; return false; };
    CHECK(es.size() == 1 && es[0].args[2] == "--all" && hasE("--no-sound-step") && hasE("--no-ui") && hasE("--no-items") && !hasE("--map"));
    CHECK(es[0].args[es[0].args.size() - 2] == "O" && parse(serialize(ev)).everything && !parse(serialize(dg)).everything);
    printf("GUI CORE: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
