// tables.hpp — static game data the 3DS client reads from text files written by w3dconv (UI stages 4/6):
// talents.txt (TalentTab.dbc + Talent.dbc) and taxi.txt (TaxiNodes.dbc + TaxiPath.dbc). See converter/src/texts.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace w3n {

struct TalentTree { uint32_t id = 0, classMask = 0, order = 0; std::string name; };
struct Talent {
    uint32_t id = 0, tree = 0, row = 0, col = 0, reqTalent = 0, reqRank = 0;   // reqRank: 0-based rank index of reqTalent
    uint32_t rankSpell[5] = {0, 0, 0, 0, 0};
    int maxRank() const;                    // ranks that exist (1..5)
};

class TalentTable {
public:
    bool load(const char* path);
    bool loaded() const { return !trees_.empty(); }
    void add(const TalentTree& t) { trees_.push_back(t); }
    void add(const Talent& t) { talents_.push_back(t); }
    // The trees of a class (1 warrior .. 11 druid), in their order (Arms, Fury, Protection).
    std::vector<const TalentTree*> treesFor(uint32_t classId) const;
    std::vector<const Talent*> talentsOf(uint32_t tree) const;   // by row, then column
    const Talent* find(uint32_t id) const;
    // Rank of a talent from the spells we know: the highest rank spell in 'known' (0 = not learned).
    template <class Known> int rankOf(const Talent& t, const Known& known) const {
        int r = 0;
        for (int k = 0; k < 5; ++k)
            if (t.rankSpell[k] && known(t.rankSpell[k])) r = k + 1;
        return r;
    }
private:
    std::vector<TalentTree> trees_;
    std::vector<Talent> talents_;
};

struct TaxiNode { uint32_t id = 0, map = 0; float x = 0, y = 0, z = 0; std::string name; };

class TaxiTable {
public:
    bool load(const char* path);
    bool loaded() const { return !nodes_.empty(); }
    void addNode(const TaxiNode& n) { nodes_[n.id] = n; }
    void addPath(uint32_t from, uint32_t to, uint32_t cost) { paths_.push_back({from, to, cost}); }
    const TaxiNode* node(uint32_t id) const;
    const std::unordered_map<uint32_t, TaxiNode>& nodes() const { return nodes_; }
    // The cheapest route from -> to over the nodes 'usable' allows (both ends included); empty when none.
    template <class Usable> std::vector<uint32_t> route(uint32_t from, uint32_t to, const Usable& usable, uint32_t* cost) const;
    uint32_t directCost(uint32_t from, uint32_t to) const;   // 0 when there is no direct path
private:
    struct Path { uint32_t from, to, cost; };
    std::unordered_map<uint32_t, TaxiNode> nodes_;
    std::vector<Path> paths_;
};

template <class Usable>
std::vector<uint32_t> TaxiTable::route(uint32_t from, uint32_t to, const Usable& usable, uint32_t* cost) const {
    // Dijkstra over the (small) path list
    std::unordered_map<uint32_t, uint32_t> dist, prev;
    std::vector<uint32_t> open{from};
    dist[from] = 0;
    while (!open.empty()) {
        size_t bi = 0;
        for (size_t i = 1; i < open.size(); ++i)
            if (dist[open[i]] < dist[open[bi]]) bi = i;
        uint32_t u = open[bi];
        open.erase(open.begin() + (long)bi);
        if (u == to) break;
        for (const Path& p : paths_) {
            if (p.from != u || !usable(p.to)) continue;
            uint32_t d = dist[u] + p.cost + 1;   // +1: fewer hops when costs tie
            auto it = dist.find(p.to);
            if (it == dist.end() || d < it->second) {
                if (it == dist.end()) open.push_back(p.to);
                dist[p.to] = d;
                prev[p.to] = u;
            }
        }
    }
    std::vector<uint32_t> out;
    if (!dist.count(to) || from == to) return out;
    for (uint32_t n = to; n != from; n = prev[n]) out.insert(out.begin(), n);
    out.insert(out.begin(), from);
    if (cost) {
        uint32_t c = 0;
        for (size_t i = 0; i + 1 < out.size(); ++i) c += directCost(out[i], out[i + 1]);
        *cost = c;
    }
    return out;
}

// emotes.txt (Emotes.dbc): emote id -> animation id; built-in values for the common ones without the file.
class EmoteTable {
public:
    bool load(const char* path);
    bool loaded() const { return !anims_.empty(); }
    uint32_t anim(uint32_t emote) const;   // 0 = none / unknown
    void add(uint32_t emote, uint32_t anim) { anims_[emote] = anim; }
private:
    std::unordered_map<uint32_t, uint32_t> anims_;
};

// locks.txt (Lock.dbc + the OPEN_LOCK spells of Spell.dbc): what opens a chest / herb / ore vein.
struct LockCase { uint32_t type = 0, index = 0, skill = 0; };   // type 1 item, 2 skill (index = LockType)
class LockTable {
public:
    bool load(const char* path);
    bool loaded() const { return !locks_.empty(); }
    void addLock(uint32_t id, const std::vector<LockCase>& c) { locks_[id] = c; }
    void addSpell(uint32_t spell, uint32_t lockType) { spells_.push_back({spell, lockType}); }
    const std::vector<LockCase>* lock(uint32_t id) const { auto it = locks_.find(id); return it == locks_.end() ? nullptr : &it->second; }
    const std::vector<std::pair<uint32_t, uint32_t>>& openSpells() const { return spells_; }   // spell, lock type
private:
    std::unordered_map<uint32_t, std::vector<LockCase>> locks_;
    std::vector<std::pair<uint32_t, uint32_t>> spells_;
};

}  // namespace w3n
