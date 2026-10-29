// w3dgui core (portable, tested on Linux by tests/gui_core_test.cpp): the settings of the window, the steps it runs
// (w3dconv.exe calls) and the settings file. Everything the 3DS needs lands in ONE output folder, the one to copy
// to sdmc:/3ds/w3dproto/:  test.w3dp (the world), ui.w3dp, sound\, spells/factions/talents/taxi/locks/emotes/transports .txt
#pragma once
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace w3gui {

struct Settings {
    std::string data;                 // WoW 1.12 Data folder (MPQ)
    std::string out;                  // the one output folder
    std::string map = "Azeroth";      // Azeroth = Eastern Kingdoms, Kalimdor, or any other map name
    bool world = false;               // every tile of the map (0..63) instead of the rectangle below
    int tx0 = 30, ty0 = 46, tx1 = 34, ty1 = 50;
    bool packWorld = true;            // the terrain pack (test.w3dp for Azeroth, map1.w3dp for Kalimdor)
    bool creatures = true, players = true, items = true, transports = true;
    bool gameobjects = false;         // models of ordinary game objects (--gameobjects; a keyword list goes into Extra)
    bool dungeons = false;            // instance maps (--dungeons): one map<ID>.w3dp per dungeon, same creature/player options
    bool ui = true, sound = true;
    bool everything = false;          // stage 5.27: w3dconv --all, every map and dungeon with one shared asset pool
                                      // (map<ID>.w3dp + common.w3dp); the world/dungeon choices above are not used then
    int quality = 0;                  // texture encoder: 0 low (fast), 1 medium, 2 high
    std::string extra;                // more w3dconv options for the world pack, as typed
};

struct Step {
    std::string title;                // shown in the window
    std::string logFile;              // in the output folder
    std::vector<std::string> args;    // for w3dconv.exe
};

inline std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char c = dir.back();
    return (c == '\\' || c == '/') ? dir + name : dir + "\\" + name;
}

// Splits "a b \"c d\"" the way a command line would.
inline std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false, any = false;
    for (char c : s) {
        if (c == '"') { inq = !inq; any = true; }
        else if ((c == ' ' || c == '\t' || c == '\r' || c == '\n') && !inq) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        } else cur += c;
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

// One argument for CreateProcess: quoted when it holds spaces or quotes.
inline std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string r = "\"";
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') { ++backslashes; r += c; continue; }
        if (c == '"') { r.append(backslashes, '\\'); r += "\\\""; }
        else r += c;
        backslashes = 0;
    }
    r.append(backslashes, '\\');   // trailing backslashes before the closing quote
    return r + "\"";
}

// Problems that make a run pointless; empty = fine.
inline std::string validate(const Settings& s) {
    if (s.data.empty()) return "The Data folder of the 1.12 client is not set.";
    if (s.out.empty()) return "The output folder is not set.";
    if (s.everything) return "";
    if (!s.packWorld && !s.ui && !s.sound && !s.dungeons) return "Nothing is selected.";
    if (s.packWorld) {
        if (s.map.empty()) return "The map is not set.";
        if (!s.world && (s.tx0 < 0 || s.ty0 < 0 || s.tx1 > 63 || s.ty1 > 63)) return "Tiles: numbers from 0 to 63.";
    }
    return "";
}

// The client picks the pack by the server's map id: Eastern Kingdoms (0) = test.w3dp, Kalimdor (1) = map1.w3dp, other
// maps by their id as "mapN" (e.g. map530 for Expansion01) or "mapN.w3dp".
inline std::string worldPackName(const std::string& map) {
    if (map == "Azeroth") return "test.w3dp";
    if (map == "Kalimdor") return "map1.w3dp";
    if (map.size() > 3 && map.compare(0, 3, "map") == 0) return map + ".w3dp";
    return map + ".w3dp";
}

// UI and sound first (minutes), the world last (the long one): a wrong Data folder shows up at once.
inline std::vector<Step> buildSteps(const Settings& s) {
    std::vector<Step> v;
    if (s.everything) {   // one run: interface, sound, every map, creatures, players, ships into one pool
        Step a;
        a.title = "Everything: every map and dungeon, one shared asset pool (map<ID>.w3dp, common.w3dp)";
        a.logFile = "log_all.txt";
        a.args = {"--data", s.data, "--all"};
        if (!s.ui) a.args.push_back("--no-ui");
        if (!s.sound) a.args.push_back("--no-sound-step");
        if (!s.items) a.args.push_back("--no-items");
        if (s.gameobjects) a.args.push_back("--gameobjects");
        if (s.quality > 0) { a.args.push_back("--tex-quality"); a.args.push_back(s.quality == 1 ? "medium" : "high"); }
        for (auto& x : splitArgs(s.extra)) a.args.push_back(x);
        a.args.push_back("-o");
        a.args.push_back(s.out);
        a.args.push_back("-v");
        v.push_back(a);
        return v;
    }
    if (s.ui) {   // Extra options too (e.g. --consoleport "C:\\path")
        Step u{"Interface and texts (ui.w3dp, spells/talents/taxi ...)", "log_ui.txt", {"--data", s.data, "--ui"}};
        for (auto& x : splitArgs(s.extra)) u.args.push_back(x);
        for (const std::string& x : {std::string("-o"), joinPath(s.out, "ui.w3dp"), std::string("-v")}) u.args.push_back(x);
        v.push_back(u);
    }
    if (s.sound) v.push_back({"Sound and music (folder sound)", "log_sound.txt", {"--data", s.data, "--sound", "-o", joinPath(s.out, "sound"), "-v"}});
    if (s.packWorld) {
        Step w;
        w.title = s.world ? "World: all tiles of map " + s.map : "World: tiles " + std::to_string(s.tx0) + "," + std::to_string(s.ty0) + " - " +
                                                                std::to_string(s.tx1) + "," + std::to_string(s.ty1) + " of map " + s.map;
        w.logFile = "log_world.txt";
        w.args = {"--data", s.data, "--map", s.map};
        if (s.world) w.args.push_back("--world");
        else for (const char* f : {"--tiles"}) {
            w.args.push_back(f);
            for (int n : {s.tx0, s.ty0, s.tx1, s.ty1}) w.args.push_back(std::to_string(n));
        }
        if (s.creatures) w.args.push_back("--creatures");
        if (s.players) w.args.push_back("--players");
        if (s.players && !s.items) w.args.push_back("--no-items");
        if (s.transports) w.args.push_back("--transports");
        if (s.gameobjects) w.args.push_back("--gameobjects");
        if (s.quality > 0) { w.args.push_back("--tex-quality"); w.args.push_back(s.quality == 1 ? "medium" : "high"); }
        for (auto& a : splitArgs(s.extra)) w.args.push_back(a);
        w.args.push_back("-o");
        w.args.push_back(joinPath(s.out, worldPackName(s.map)));
        w.args.push_back("-v");
        v.push_back(w);
    }
    if (s.dungeons) {
        Step d;
        d.title = "Dungeons: every instance map (map<ID>.w3dp)";
        d.logFile = "log_dungeons.txt";
        d.args = {"--data", s.data, "--dungeons"};
        if (s.creatures) d.args.push_back("--creatures");
        if (s.players) d.args.push_back("--players");
        if (s.players && !s.items) d.args.push_back("--no-items");
        if (s.gameobjects) d.args.push_back("--gameobjects");
        if (s.quality > 0) { d.args.push_back("--tex-quality"); d.args.push_back(s.quality == 1 ? "medium" : "high"); }
        d.args.push_back("-o");
        d.args.push_back(s.out);
        d.args.push_back("-v");
        v.push_back(d);
    }
    return v;
}

inline std::string commandLine(const std::string& exe, const Step& st) {
    std::string c = quoteArg(exe);
    for (auto& a : st.args) c += " " + quoteArg(a);
    return c;
}

// Settings file: "key=value" lines.
inline std::string serialize(const Settings& s) {
    std::string r;
    auto put = [&](const char* k, const std::string& v) { r += std::string(k) + "=" + v + "\n"; };
    auto b = [](bool x) { return std::string(x ? "1" : "0"); };
    put("data", s.data); put("out", s.out); put("map", s.map); put("world", b(s.world));
    put("tx0", std::to_string(s.tx0)); put("ty0", std::to_string(s.ty0)); put("tx1", std::to_string(s.tx1)); put("ty1", std::to_string(s.ty1));
    put("packWorld", b(s.packWorld)); put("creatures", b(s.creatures)); put("players", b(s.players)); put("items", b(s.items));
    put("transports", b(s.transports)); put("gameobjects", b(s.gameobjects)); put("dungeons", b(s.dungeons)); put("ui", b(s.ui)); put("sound", b(s.sound));
    put("everything", b(s.everything));
    put("quality", std::to_string(s.quality)); put("extra", s.extra);
    return r;
}

inline Settings parse(const std::string& text) {
    Settings s;
    std::map<std::string, std::string> kv;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at);
        at = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    auto str = [&](const char* k, std::string& dst) { auto it = kv.find(k); if (it != kv.end()) dst = it->second; };
    auto num = [&](const char* k, int& dst) { auto it = kv.find(k); if (it != kv.end() && !it->second.empty()) dst = std::atoi(it->second.c_str()); };
    auto flag = [&](const char* k, bool& dst) { auto it = kv.find(k); if (it != kv.end()) dst = it->second == "1"; };
    str("data", s.data); str("out", s.out); str("map", s.map); str("extra", s.extra);
    num("tx0", s.tx0); num("ty0", s.ty0); num("tx1", s.tx1); num("ty1", s.ty1); num("quality", s.quality);
    flag("world", s.world); flag("packWorld", s.packWorld); flag("creatures", s.creatures); flag("players", s.players);
    flag("items", s.items); flag("transports", s.transports); flag("gameobjects", s.gameobjects); flag("dungeons", s.dungeons); flag("ui", s.ui); flag("sound", s.sound);
    flag("everything", s.everything);
    return s;
}

}  // namespace w3gui
