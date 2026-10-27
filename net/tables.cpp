#include "tables.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace w3n {

namespace {
// Tab-separated lines; returns the fields (the last keeps its spaces).
std::vector<std::string> fields(const char* line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = line; *p && *p != '\n' && *p != '\r'; ++p) {
        if (*p == '\t') { out.push_back(cur); cur.clear(); }
        else cur += *p;
    }
    out.push_back(cur);
    return out;
}
uint32_t num(const std::string& s) { return (uint32_t)strtoul(s.c_str(), nullptr, 10); }
}  // namespace

int Talent::maxRank() const {
    int n = 0;
    for (int k = 0; k < 5; ++k)
        if (rankSpell[k]) n = k + 1;
    return std::max(1, n);
}

bool TalentTable::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    trees_.clear();
    talents_.clear();
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> c = fields(line);
        if (c[0] == "B" && c.size() >= 5) {
            trees_.push_back({num(c[1]), num(c[2]), num(c[3]), c[4]});
        } else if (c[0] == "T" && c.size() >= 12) {
            Talent t;
            t.id = num(c[1]); t.tree = num(c[2]); t.row = num(c[3]); t.col = num(c[4]);
            t.reqTalent = num(c[5]); t.reqRank = num(c[6]);
            for (int k = 0; k < 5; ++k) t.rankSpell[k] = num(c[7 + k]);
            talents_.push_back(t);
        }
    }
    fclose(f);
    return !trees_.empty();
}

std::vector<const TalentTree*> TalentTable::treesFor(uint32_t classId) const {
    std::vector<const TalentTree*> out;
    if (!classId || classId > 31) return out;
    for (const TalentTree& t : trees_)
        if (t.classMask & (1u << (classId - 1))) out.push_back(&t);
    std::sort(out.begin(), out.end(), [](const TalentTree* a, const TalentTree* b) { return a->order < b->order; });
    return out;
}

std::vector<const Talent*> TalentTable::talentsOf(uint32_t tree) const {
    std::vector<const Talent*> out;
    for (const Talent& t : talents_)
        if (t.tree == tree) out.push_back(&t);
    std::sort(out.begin(), out.end(), [](const Talent* a, const Talent* b) { return a->row != b->row ? a->row < b->row : a->col < b->col; });
    return out;
}

const Talent* TalentTable::find(uint32_t id) const {
    for (const Talent& t : talents_)
        if (t.id == id) return &t;
    return nullptr;
}

bool TaxiTable::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    nodes_.clear();
    paths_.clear();
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> c = fields(line);
        if (c[0] == "N" && c.size() >= 7) {
            TaxiNode n;
            n.id = num(c[1]); n.map = num(c[2]);
            n.x = (float)atof(c[3].c_str()); n.y = (float)atof(c[4].c_str()); n.z = (float)atof(c[5].c_str());
            n.name = c[6];
            nodes_[n.id] = n;
        } else if (c[0] == "P" && c.size() >= 4) {
            paths_.push_back({num(c[1]), num(c[2]), num(c[3])});
        }
    }
    fclose(f);
    return !nodes_.empty();
}

const TaxiNode* TaxiTable::node(uint32_t id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

uint32_t TaxiTable::directCost(uint32_t from, uint32_t to) const {
    for (const Path& p : paths_)
        if (p.from == from && p.to == to) return p.cost;
    return 0;
}

bool EmoteTable::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    anims_.clear();
    char line[128];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> c = fields(line);
        if (c[0] == "E" && c.size() >= 3) anims_[num(c[1])] = num(c[2]);
    }
    fclose(f);
    return !anims_.empty();
}

uint32_t EmoteTable::anim(uint32_t emote) const {
    if (!emote) return 0;
    auto it = anims_.find(emote);
    if (it != anims_.end()) return it->second;
    // Emotes.dbc of 1.12 for the usual ones (talk, bow, wave, cheer, exclamation, question, eat, dance, laugh,
    // sleep, sit, rude, roar, kneel, kiss, cry, chicken, beg, applaud, shout, flex, shy, point, work, salute, ...)
    switch (emote) {
        case 1: return 60; case 2: return 66; case 3: return 67; case 4: return 68; case 5: return 64; case 6: return 65;
        case 7: return 61; case 10: return 69; case 11: return 70; case 12: return 100; case 13: return 97;
        case 14: return 73; case 15: return 74; case 16: return 75; case 17: return 76; case 18: return 77;
        case 19: return 78; case 20: return 79; case 21: return 80; case 22: return 81; case 23: return 82;
        case 24: return 83; case 25: return 84; case 27: return 25; case 28: return 62; case 29: return 84;
        case 36: return 17; case 37: return 18; case 38: return 19; case 43: return 24; case 44: return 25;
        case 45: return 26; case 48: return 29; case 50: return 31; case 51: return 32; case 53: return 55;
        case 54: return 57; case 60: return 95; case 64: return 14; case 65: return 6; case 66: return 113;
        case 68: return 115; case 69: return 63; case 133: return 63; case 173: return 136; case 233: return 136;
        default: return 0;
    }
}

bool LockTable::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    locks_.clear();
    spells_.clear();
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> c = fields(line);
        if (c[0] == "L" && c.size() >= 26) {
            std::vector<LockCase> v;
            for (int k = 0; k < 8; ++k)
                if (num(c[2 + k])) v.push_back({num(c[2 + k]), num(c[10 + k]), num(c[18 + k])});
            locks_[num(c[1])] = v;
        } else if (c[0] == "S" && c.size() >= 3) {
            spells_.push_back({num(c[1]), num(c[2])});
        }
    }
    fclose(f);
    return !locks_.empty();
}

}  // namespace w3n
