// Text tables for the 3DS client (UI stages 4/6), next to spells.txt / factions.txt:
//   talents.txt  "B tab classMask order name" per TalentTab.dbc row, "T id tab row col reqTalent reqRank rank1..5"
//                per Talent.dbc row (1.12: ID, TabID, Row, Column, RankID[5] 4-8, PrereqTalent[3] 9-11,
//                PrereqRank[3] 12-14; TalentTab: ID, Name[8] 1-8, flags 9, icon 10, race mask 11, class mask 12,
//                pet mask 13, order 14, background 15)
//   taxi.txt     "N id map x y z name" per TaxiNodes.dbc row (ID, map, X, Y, Z, Name[8] 5-12) and
//                "P from to cost" per TaxiPath.dbc row (ID, from, to, cost)
#include "conv.h"

namespace w3d {

namespace {
std::string clean(std::string s) {
    for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}
std::string firstLocale(const DbcTable& t, uint32_t r, uint32_t c0, uint32_t c1) {
    for (uint32_t c = c0; c < c1 && c < t.fields; ++c) {
        std::string s = t.str(r, c);
        if (!s.empty()) return clean(s);
    }
    return {};
}
bool writeText(const std::string& path, const std::string& s) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return true;
}
}  // namespace

bool exportTalents(const DataDir& data, const std::string& path, bool verbose) {
    DbcTable tabs, tal;
    if (!tabs.load(data, "TalentTab.dbc") || !tal.load(data, "Talent.dbc") || tabs.fields < 15 || tal.fields < 15) {
        std::fprintf(stderr, "talents: TalentTab.dbc / Talent.dbc not found (no talents.txt)\n");
        return false;
    }
    std::string out = "# wow3ds talents: B tab classMask order name | T id tab row col reqTalent reqRank rank1..rank5\n";
    size_t nb = 0, nt = 0;
    for (uint32_t r = 0; r < tabs.records; ++r) {
        if (!tabs.u32(r, 0)) continue;
        out += "B\t" + std::to_string(tabs.u32(r, 0)) + "\t" + std::to_string(tabs.u32(r, 12)) + "\t" + std::to_string(tabs.u32(r, 14)) +
               "\t" + firstLocale(tabs, r, 1, 9) + "\n";
        ++nb;
    }
    for (uint32_t r = 0; r < tal.records; ++r) {
        if (!tal.u32(r, 0)) continue;
        out += "T";
        for (uint32_t c : {0u, 1u, 2u, 3u, 9u, 12u, 4u, 5u, 6u, 7u, 8u}) out += "\t" + std::to_string(tal.u32(r, c));
        out += "\n";
        ++nt;
    }
    if (!writeText(path, out)) return false;
    std::printf("  talents: %zu trees, %zu talents written to %s\n", nb, nt, path.c_str());
    (void)verbose;
    return true;
}

bool exportTaxi(const DataDir& data, const std::string& path, bool verbose) {
    DbcTable nodes, paths;
    if (!nodes.load(data, "TaxiNodes.dbc") || nodes.fields < 6) {
        std::fprintf(stderr, "taxi: TaxiNodes.dbc not found (no taxi.txt)\n");
        return false;
    }
    bool havePaths = paths.load(data, "TaxiPath.dbc") && paths.fields >= 4;
    std::string out = "# wow3ds flight masters: N id map x y z name | P from to cost\n";
    size_t nn = 0, np = 0;
    char b[96];
    for (uint32_t r = 0; r < nodes.records; ++r) {
        if (!nodes.u32(r, 0)) continue;
        std::snprintf(b, sizeof b, "N\t%u\t%u\t%.1f\t%.1f\t%.1f\t", nodes.u32(r, 0), nodes.u32(r, 1), nodes.f32(r, 2), nodes.f32(r, 3),
                      nodes.f32(r, 4));
        out += b + firstLocale(nodes, r, 5, 13) + "\n";
        ++nn;
    }
    if (havePaths)
        for (uint32_t r = 0; r < paths.records; ++r) {
            if (!paths.u32(r, 0)) continue;
            out += "P\t" + std::to_string(paths.u32(r, 1)) + "\t" + std::to_string(paths.u32(r, 2)) + "\t" + std::to_string(paths.u32(r, 3)) + "\n";
            ++np;
        }
    if (!writeText(path, out)) return false;
    std::printf("  taxi: %zu nodes, %zu paths written to %s\n", nn, np, path.c_str());
    (void)verbose;
    return true;
}

// transports.txt: "T path node map x y z flags delay" per TaxiPathNode.dbc row (1.12: ID 0, PathID 1, NodeIndex 2, MapID 3,
// X/Y/Z 4-6, Flags 7, Delay 8): ships and zeppelins move along these (net/transport.cpp), and "M display z x0 y0 x1 y1"
// for every transport model with a walkable deck (convertTransports). A run without models keeps the M lines of the
// existing file.
bool exportTransports(const DataDir& data, const std::string& path, bool verbose) {
    DbcTable nodes;
    if (!nodes.load(data, "TaxiPathNode.dbc") || nodes.fields < 9) {
        std::fprintf(stderr, "transports: TaxiPathNode.dbc not found (no transports.txt)\n");
        return false;
    }
    std::string out = "# wow3ds transports: T path node map x y z flags delay | M display deckZ x0 y0 x1 y1\n";
    size_t nt = 0, nm = 0;
    char b[160];
    for (uint32_t r = 0; r < nodes.records; ++r) {
        if (!nodes.u32(r, 1)) continue;
        std::snprintf(b, sizeof b, "T\t%u\t%u\t%u\t%.2f\t%.2f\t%.2f\t%u\t%u\n", nodes.u32(r, 1), nodes.u32(r, 2), nodes.u32(r, 3),
                      nodes.f32(r, 4), nodes.f32(r, 5), nodes.f32(r, 6), nodes.u32(r, 7), nodes.u32(r, 8));
        out += b;
        ++nt;
    }
    if (!transportDecks().empty()) {
        for (auto& kv : transportDecks()) {
            std::snprintf(b, sizeof b, "M\t%u\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\n", kv.first, kv.second.z, kv.second.x0, kv.second.y0,
                          kv.second.x1, kv.second.y1);
            out += b;
            ++nm;
        }
    } else if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::string old;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) old.append(buf, n);
        std::fclose(f);
        size_t at = 0;
        while (at < old.size()) {
            size_t e = old.find('\n', at);
            if (e == std::string::npos) e = old.size();
            if (old.compare(at, 2, "M\t") == 0) { out += old.substr(at, e - at) + "\n"; ++nm; }
            at = e + 1;
        }
    }
    if (!writeText(path, out)) return false;
    std::printf("  transports: %zu path nodes, %zu decks written to %s\n", nt, nm, path.c_str());
    (void)verbose;
    return true;
}

// locks.txt: "L id type0..7 index0..7 skill0..7" per Lock.dbc row (1.12: 33 fields, action 25-32 left out) and
// "S spell lockType" for every spell with effect 33 OPEN_LOCK (Effect 61-63, EffectMiscValue 106-108 in 1.12, taken
// relative to the name column): the 3DS opens herbs / ore / chests by casting the spell its lock asks for.
bool exportLocks(const DataDir& data, const std::string& path, bool verbose) {
    DbcTable lock, spell;
    if (!lock.load(data, "Lock.dbc") || lock.fields < 25) {
        std::fprintf(stderr, "locks: Lock.dbc not found (no locks.txt)\n");
        return false;
    }
    std::string out = "# wow3ds locks: L id type x8 index x8 skill x8 | S spell lockType\n";
    size_t nl = 0, ns = 0;
    for (uint32_t r = 0; r < lock.records; ++r) {
        if (!lock.u32(r, 0)) continue;
        out += "L";
        for (uint32_t c = 0; c < 25; ++c) out += "\t" + std::to_string(lock.u32(r, c));
        out += "\n";
        ++nl;
    }
    if (spell.load(data, "Spell.dbc")) {
        int nameCol = spellNameColumn(spell);
        if (nameCol < 0) nameCol = 120;
        const uint32_t effCol = (uint32_t)nameCol - 59, miscCol = (uint32_t)nameCol - 14;
        for (uint32_t r = 0; r < spell.records; ++r)
            for (uint32_t k = 0; k < 3; ++k)
                if (spell.u32(r, effCol + k) == 33) {
                    out += "S\t" + std::to_string(spell.u32(r, 0)) + "\t" + std::to_string(spell.u32(r, miscCol + k)) + "\n";
                    ++ns;
                    break;
                }
    }
    if (!writeText(path, out)) return false;
    std::printf("  locks: %zu locks, %zu opening spells written to %s\n", nl, ns, path.c_str());
    (void)verbose;
    return true;
}

// areatriggers.txt: "A id map x y z radius length width height yaw" per AreaTrigger.dbc row (1.12: ID, ContinentID, X, Y, Z,
// Radius, Box_length, Box_width, Box_height, Box_yaw). The server holds where each one leads (areatrigger_teleport); the
// 3DS only has to say that we walked into it.
bool exportAreaTriggers(const DataDir& data, const std::string& path, bool verbose) {
    DbcTable at;
    if (!at.load(data, "AreaTrigger.dbc") || at.fields < 10) {
        std::fprintf(stderr, "areatriggers: AreaTrigger.dbc not found (no areatriggers.txt: dungeon doors will not work)\n");
        return false;
    }
    std::string out = "# wow3ds area triggers: A id map x y z radius length width height yaw\n";
    size_t n = 0;
    char b[200];
    for (uint32_t r = 0; r < at.records; ++r) {
        if (!at.u32(r, 0)) continue;
        std::snprintf(b, sizeof b, "A\t%u\t%u\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.5f\n", at.u32(r, 0), at.u32(r, 1), at.f32(r, 2),
                      at.f32(r, 3), at.f32(r, 4), at.f32(r, 5), at.f32(r, 6), at.f32(r, 7), at.f32(r, 8), at.f32(r, 9));
        out += b;
        ++n;
    }
    if (!writeText(path, out)) return false;
    std::printf("  areatriggers: %zu triggers written to %s\n", n, path.c_str());
    (void)verbose;
    return true;
}

// emotes.txt: "E emote anim" per Emotes.dbc row (1.12: ID, EmoteSlashCommand, AnimID, Flags, ...): SMSG_EMOTE and
// UNIT_NPC_EMOTESTATE carry the emote, the 3DS plays its animation.
bool exportEmotes(const DataDir& data, const std::string& path, bool verbose) {
    DbcTable em;
    if (!em.load(data, "Emotes.dbc") || em.fields < 3) {
        std::fprintf(stderr, "emotes: Emotes.dbc not found (no emotes.txt; the 3DS uses its built-in list)\n");
        return false;
    }
    std::string out = "# wow3ds emotes: E emote anim\n";
    size_t n = 0;
    for (uint32_t r = 0; r < em.records; ++r) {
        uint32_t id = em.u32(r, 0), anim = em.u32(r, 2);
        if (!id || anim > 500) continue;
        out += "E\t" + std::to_string(id) + "\t" + std::to_string(anim) + "\n";
        ++n;
    }
    if (!writeText(path, out)) return false;
    std::printf("  emotes: %zu written to %s\n", n, path.c_str());
    (void)verbose;
    return true;
}

}  // namespace w3d
