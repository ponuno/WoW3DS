// factions.cpp — factions.txt reader and the template rules (see factions.hpp).
#include "factions.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace w3n {

const char* rankName(Rank r) {
    static const char* n[] = {"Hated", "Hostile", "Unfriendly", "Neutral", "Friendly", "Honored", "Revered", "Exalted"};
    return (unsigned)r < 8 ? n[(unsigned)r] : "?";
}

namespace {
const int32_t kPointsInRank[8] = {36000, 3000, 3000, 3000, 6000, 12000, 21000, 1000};
constexpr int32_t kReputationCap = 42999;
}  // namespace

int32_t rankFloor(Rank r) {
    int32_t s = -42000;
    for (unsigned i = 0; i < (unsigned)r && i < 8; ++i) s += kPointsInRank[i];
    return s;
}

Rank standingToRank(int32_t standing) {
    int32_t limit = kReputationCap + 1;
    for (int i = 7; i >= 0; --i) {
        limit -= kPointsInRank[i];
        if (standing >= limit) return (Rank)i;
    }
    return Rank::Hated;
}

bool FactionTemplate::friendlyTo(const FactionTemplate& o) const {
    if (o.faction) {
        for (uint32_t e : enemies) if (e == o.faction) return false;
        for (uint32_t f : friends) if (f == o.faction) return true;
    }
    return (friendlyMask & o.ourMask) || (ourMask & o.friendlyMask);
}

bool FactionTemplate::hostileTo(const FactionTemplate& o) const {
    if (o.faction) {
        for (uint32_t e : enemies) if (e == o.faction) return true;
        for (uint32_t f : friends) if (f == o.faction) return false;
    }
    return (hostileMask & o.ourMask) != 0;
}

int FactionInfo::indexFor(uint32_t race, uint32_t cls) const {
    for (int i = 0; i < 4; ++i)
        if ((raceMask[i] == 0 || (raceMask[i] & race)) && (classMask[i] == 0 || (classMask[i] & cls))) return i;
    return -1;
}

const FactionTemplate* FactionTable::tmpl(uint32_t id) const {
    auto it = templates_.find(id);
    return it == templates_.end() ? nullptr : &it->second;
}

const FactionInfo* FactionTable::faction(uint32_t id) const {
    auto it = factions_.find(id);
    return it == factions_.end() ? nullptr : &it->second;
}

const FactionInfo* FactionTable::byRepIndex(int32_t idx) const {
    auto it = byRep_.find(idx);
    return it == byRep_.end() ? nullptr : faction(it->second);
}

void FactionTable::add(const FactionInfo& f) {
    factions_[f.id] = f;
    if (f.repIndex >= 0) byRep_[f.repIndex] = f.id;
}

bool FactionTable::parseLine(const std::string& line) {
    if (line.size() < 3 || (line[0] != 'T' && line[0] != 'F') || line[1] != '\t') return false;
    std::vector<std::string> col;
    size_t a = 2;
    while (true) {
        size_t b = line.find('\t', a);
        col.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    auto u = [&](size_t i) { return i < col.size() ? (uint32_t)std::strtoul(col[i].c_str(), nullptr, 10) : 0u; };
    auto s = [&](size_t i) { return i < col.size() ? (int32_t)std::strtol(col[i].c_str(), nullptr, 10) : 0; };
    if (line[0] == 'T') {
        if (col.size() < 14) return false;
        FactionTemplate t;
        t.id = u(0); t.faction = u(1); t.flags = u(2); t.ourMask = u(3); t.friendlyMask = u(4); t.hostileMask = u(5);
        for (int i = 0; i < 4; ++i) { t.enemies[i] = u(6 + i); t.friends[i] = u(10 + i); }
        if (!t.id) return false;
        add(t);
        return true;
    }
    if (col.size() < 18) return false;
    FactionInfo f;
    f.id = u(0);
    f.repIndex = s(1);
    for (int i = 0; i < 4; ++i) {
        f.raceMask[i] = u(2 + i); f.classMask[i] = u(6 + i); f.base[i] = s(10 + i); f.repFlags[i] = u(14 + i);
    }
    if (col.size() > 18) f.name = col[18];
    while (!f.name.empty() && (f.name.back() == '\r' || f.name.back() == '\n')) f.name.pop_back();
    if (!f.id) return false;
    add(f);
    return true;
}

bool FactionTable::load(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string line;
    char buf[512];
    while (std::fgets(buf, sizeof buf, f)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        parseLine(line);
    }
    std::fclose(f);
    return loaded();
}

}  // namespace w3n
