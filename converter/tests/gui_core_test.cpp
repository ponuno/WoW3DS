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
    CHECK(st.size() == 4 && st[0].logFile == "log_ui.txt" && st[1].logFile == "log_sound.txt" && st[2].logFile == "log_world.txt" &&
          st[3].logFile == "log_hlod_test.txt");   // (6.57: the horizon after the world)
    const auto& w = st[2].args;
    auto has = [&](const char* a) { for (auto& x : w) if (x == a) return true; return false; };
    CHECK(has("--creatures") && has("--players") && has("--transports") && !has("--no-items") && !has("--world") && has("--tiles"));
    CHECK(worldPackName("Kalimdor") == "map1.w3dp" && worldPackName("Azeroth") == "test.w3dp");
    CHECK(w.back() == "-v" && w[w.size() - 2] == "D:\\sd\\w3dproto\\test.w3dp" && w[w.size() - 3] == "-o");
    CHECK(commandLine("C:\\t\\w3dconv.exe", st[0]).find("--data \"E:\\Wow Classic\\Data\" --ui -o D:\\sd\\w3dproto\\ui.w3dp") != std::string::npos);
    // 7.14: a non-English game language goes to the converter; enUS leaves the line as before
    s.locale = "esES";
    auto stEs = buildSteps(s);
    auto hasEs = [&](const std::vector<std::string>& a) {
        for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--locale" && a[i + 1] == "esES") return true;
        return false;
    };
    CHECK(stEs.size() == 4 && hasEs(stEs[0].args) && hasEs(stEs[1].args) && hasEs(stEs[2].args));
    CHECK(parse(serialize(s)).locale == "esES");
    s.locale = "enUS";
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
    CHECK(es.size() == 3 && es[0].args[2] == "--all" && hasE("--no-sound-step") && hasE("--no-ui") && hasE("--no-items") && !hasE("--map"));
    CHECK(es[0].args[es[0].args.size() - 2] == "O" && parse(serialize(ev)).everything && !parse(serialize(dg)).everything);
    // 6.54e: the capitals' PVS after the packs (w3docc.exe), skipped when a pack step failed
    CHECK(!parse(serialize(ev)).pvs);
    ev.pvs = true;
    auto ps = buildSteps(parse(serialize(ev)));
    CHECK(ps.size() == 5 && ps[3].exe == "w3docc.exe" && ps[3].needsPack && ps[3].args[0] == "--pvs" && ps[3].args[1] == "O\\map0.w3dp" &&
          ps[4].args[1] == "O\\map1.w3dp" && ps[3].logFile == "log_pvs_map0.txt");
    int arounds = 0;
    for (auto& a : ps[3].args) arounds += a == "--around";
    CHECK(arounds == 3 && ps[3].args[3] == "-8700" && ps[3].args[4] == "700" && ps[3].args[5] == "550");
    CHECK(commandLine("C:\\t\\w3docc.exe", ps[4]).find("--pvs O\\map1.w3dp --around 1600 -4400 400") != std::string::npos);
    Settings pw; pw.data = "D"; pw.out = "O"; pw.ui = pw.sound = false; pw.pvs = true; pw.map = "Kalimdor";
    auto pws = buildSteps(pw);
    CHECK(pws.size() == 3 && pws[2].exe == "w3docc.exe" && pws[2].args[0] == "--pvs" && pws[2].args[1] == "O\\map1.w3dp" && pws[1].args[0] == "--hlod");
    pw.map = "Azeroth"; CHECK(buildSteps(pw)[2].args[1] == "O\\test.w3dp");
    pw.map = "Kalidar"; CHECK(buildSteps(pw).size() == 2);   // (no capitals there: the horizon only)
    pw.pvs = false; pw.map = "Azeroth"; CHECK(buildSteps(pw).size() == 2 && buildSteps(pw)[0].exe.empty() && buildSteps(pw)[1].args[0] == "--hlod");
    pw.hlod = false; CHECK(buildSteps(pw).size() == 1);
    // 6.57: the horizon after the packs, before the PVS; saved
    CHECK(ps.size() == 5 && ps[1].args[0] == "--hlod" && ps[1].args[1] == "O\\map0.w3dp" && ps[2].args[1] == "O\\map1.w3dp" && ps[3].args[0] == "--pvs" &&
          ps[1].logFile == "log_hlod_map0.txt" && ps[1].needsPack);
    CHECK(!parse(serialize(pw)).hlod && parse(serialize(ev)).hlod);
    printf("GUI CORE: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
