// Spell names for the 3DS action bar (stage 5.8): Spell.dbc -> spells.txt, one "id<TAB>name<TAB>rank" line each.
// Faction reactions (stage 5.12): FactionTemplate.dbc + Faction.dbc -> factions.txt (see net/factions.hpp).
//
// Only spells that appear in SkillLineAbility.dbc (class, racial and profession spells a player can learn)
// are written, which is ~5 000 lines instead of ~24 000. Spell.dbc of 1.12 has 173 fields; the English name is
// column 120 and the rank 9 columns later (8 locales + a flags field per string group). To be safe against
// layout differences the name column is searched for: the column where spell 78 reads "Heroic Strike".
#include "conv.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <unordered_map>

namespace w3d {

bool DbcTable::load(const DataDir& data, const char* name) {
    std::string path = std::string("DBFilesClient\\") + name;
    if (!data.read(path, b) || b.size() < 20 || std::memcmp(b.data(), "WDBC", 4) != 0) return false;
    std::memcpy(&records, &b[4], 4); std::memcpy(&fields, &b[8], 4);
    std::memcpy(&recSize, &b[12], 4); std::memcpy(&strSize, &b[16], 4);
    return 20 + (size_t)records * recSize + strSize <= b.size();
}

uint32_t DbcTable::u32(uint32_t r, uint32_t f) const {
    if (r >= records || f >= fields || (f + 1) * 4 > recSize) return 0;
    uint32_t v;
    std::memcpy(&v, &b[20 + (size_t)r * recSize + f * 4], 4);
    return v;
}

float DbcTable::f32(uint32_t r, uint32_t f) const {
    uint32_t v = u32(r, f);
    float x;
    std::memcpy(&x, &v, 4);
    return x;
}

std::string DbcTable::str(uint32_t r, uint32_t f) const {
    uint32_t off = u32(r, f);
    size_t base = 20 + (size_t)records * recSize;
    if (off == 0 || off >= strSize) return {};
    const char* s = (const char*)&b[base + off];
    size_t n = 0;
    while (off + n < strSize && s[n]) ++n;
    return std::string(s, n);
}

namespace {
using Table = DbcTable;
}  // namespace

int spellNameColumn(const DbcTable& spell) {
    auto matches = [&](uint32_t c) {
        for (uint32_t r = 0; r < spell.records; ++r) {
            uint32_t id = spell.u32(r, 0);
            if (id != 78 && id != 133 && id != 2457) continue;
            std::string s = spell.str(r, c);
            if (s == "Heroic Strike" || s == "Fireball" || s == "Battle Stance") return true;
        }
        return false;
    };
    if (spell.fields > 120 && matches(120)) return 120;   // the 1.12 layout first: a small id column could alias a string
    uint32_t lo = spell.fields > 100 ? 100 : 0, hi = std::min<uint32_t>(spell.fields, 200);
    for (uint32_t c = lo; c < hi; ++c)
        for (uint32_t r = 0; r < spell.records; ++r) {
            uint32_t id = spell.u32(r, 0);
            if (id != 78 && id != 133 && id != 2457) continue;
            std::string s = spell.str(r, c);
            if (s == "Heroic Strike" || s == "Fireball" || s == "Battle Stance") return (int)c;
        }
    return -1;
}

bool exportSpellNames(const DataDir& data, const std::string& path, bool verbose) {
    Table spell;
    if (!spell.load(data, "Spell.dbc")) {
        std::fprintf(stderr, "spells: Spell.dbc not found (no spells.txt)\n");
        return false;
    }
    // Name column: where spell 78 is "Heroic Strike" (or 133 "Fireball", 2457 "Battle Stance").
    int nameCol = spellNameColumn(spell);
    if (nameCol < 0) {
        nameCol = 120;
        std::fprintf(stderr, "spells: name column not found by calibration, assuming 120\n");
    }
    int rankCol = nameCol + 9;

    std::set<uint32_t> wanted;
    Table sla;   // SkillLineAbility: ID 0, SkillLine 1, Spell 2
    bool haveSla = sla.load(data, "SkillLineAbility.dbc") && sla.fields > 2;
    if (haveSla)
        for (uint32_t r = 0; r < sla.records; ++r) wanted.insert(sla.u32(r, 2));

    std::string out;
    size_t n = 0;
    for (uint32_t r = 0; r < spell.records; ++r) {
        uint32_t id = spell.u32(r, 0);
        if (!id || (haveSla && !wanted.count(id))) continue;
        std::string name = spell.str(r, (uint32_t)nameCol);
        if (name.empty()) continue;
        std::string rank = (uint32_t)rankCol < spell.fields ? spell.str(r, (uint32_t)rankCol) : std::string();
        for (char& ch : name) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        for (char& ch : rank) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        out += std::to_string(id) + '\t' + name + '\t' + rank + '\n';
        ++n;
    }
    // Trainers (stage 5.13b) list the spells that teach (effect 36 LEARN_SPELL, the taught spell in
    // EffectTriggerSpell): write them under the name and rank of what they teach. 1.12 columns: Effect 61-63,
    // EffectTriggerSpell 109-111, name 120 - taken relative to the name column found above.
    size_t taught = 0;
    if (haveSla && nameCol >= 59) {
        std::unordered_map<uint32_t, uint32_t> rowOf;
        for (uint32_t r = 0; r < spell.records; ++r) rowOf[spell.u32(r, 0)] = r;
        const uint32_t effCol = (uint32_t)nameCol - 59, trigCol = (uint32_t)nameCol - 11;
        for (uint32_t r = 0; r < spell.records; ++r) {
            uint32_t id = spell.u32(r, 0);
            if (!id || wanted.count(id)) continue;
            for (uint32_t k = 0; k < 3; ++k) {
                if (spell.u32(r, effCol + k) != 36) continue;
                uint32_t t = spell.u32(r, trigCol + k);
                auto it = rowOf.find(t);
                if (!wanted.count(t) || it == rowOf.end()) continue;
                std::string name = spell.str(it->second, (uint32_t)nameCol);
                std::string rank = (uint32_t)rankCol < spell.fields ? spell.str(it->second, (uint32_t)rankCol) : std::string();
                if (name.empty()) continue;
                for (char& ch : name) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
                for (char& ch : rank) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
                out += std::to_string(id) + '\t' + name + '\t' + rank + '\n';
                ++taught;
                break;
            }
        }
    }
    // Buffs and debuffs (stage 5.16): the aura bar shows auras of any spell (food, NPC spells, items), so every
    // visible aura spell gets its name too: an effect 6 (APPLY_AURA) or 35 (APPLY_AREA_AURA), not passive (0x40)
    // and not hidden (0x80) in Attributes (column 6 in 1.12, i.e. name column - 114).
    size_t auras = 0;
    if (nameCol >= 114) {
        std::set<uint32_t> written(wanted.begin(), wanted.end());
        const uint32_t effCol = (uint32_t)nameCol - 59, attrCol = (uint32_t)nameCol - 114;
        for (uint32_t r = 0; r < spell.records; ++r) {
            uint32_t id = spell.u32(r, 0);
            if (!id || written.count(id) || (spell.u32(r, attrCol) & 0xC0)) continue;
            bool aura = false;
            for (uint32_t k = 0; k < 3; ++k) aura = aura || spell.u32(r, effCol + k) == 6 || spell.u32(r, effCol + k) == 35;
            if (!aura) continue;
            std::string name = spell.str(r, (uint32_t)nameCol);
            if (name.empty()) continue;
            for (char& ch : name) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            out += std::to_string(id) + '\t' + name + "\t\n";
            written.insert(id);
            ++auras;
        }
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "spells: cannot write %s\n", path.c_str());
        return false;
    }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("  spells: %zu names + %zu trainer spells + %zu auras written to %s (Spell.dbc %u records, name column %d%s)\n", n, taught, auras,
                path.c_str(), spell.records, nameCol, haveSla ? ", filtered by SkillLineAbility" : ", all spells");
    (void)verbose;
    return true;
}


// factions.txt: "T" = FactionTemplate.dbc (14 fields: id, faction, flags, our/friendly/hostile masks, 4 enemies,
// 4 friends), "F" = Faction.dbc (1.12: 37 fields: id, reputation index, race masks 2-5, class masks 6-9, base
// standings 10-13, flags 14-17, parent 18, names 19-26 (8 locales), name flags 27, descriptions 28-36).
bool exportFactions(const DataDir& data, const std::string& path, bool verbose) {
    Table ft, fa;
    if (!ft.load(data, "FactionTemplate.dbc") || ft.fields < 14) {
        std::fprintf(stderr, "factions: FactionTemplate.dbc not found (no factions.txt)\n");
        return false;
    }
    bool haveFa = fa.load(data, "Faction.dbc") && fa.fields >= 19;
    std::string out = "# wow3ds factions: T id faction flags our friendly hostile enemy x4 friend x4 | "
                      "F id repIndex raceMask x4 classMask x4 base x4 flags x4 name\n";
    size_t nt = 0, nf = 0;
    for (uint32_t r = 0; r < ft.records; ++r) {
        if (!ft.u32(r, 0)) continue;
        out += "T";
        for (uint32_t c = 0; c < 14; ++c) out += "\t" + std::to_string(ft.u32(r, c));
        out += "\n";
        ++nt;
    }
    if (haveFa)
        for (uint32_t r = 0; r < fa.records; ++r) {
            if (!fa.u32(r, 0)) continue;
            out += "F\t" + std::to_string(fa.u32(r, 0)) + "\t" + std::to_string((int32_t)fa.u32(r, 1));
            for (uint32_t c = 2; c < 18; ++c)
                out += "\t" + (c >= 10 && c < 14 ? std::to_string((int32_t)fa.u32(r, c)) : std::to_string(fa.u32(r, c)));
            std::string name;
            for (uint32_t c = 19; c < 27 && c < fa.fields && name.empty(); ++c) name = fa.str(r, c);   // first locale set
            for (char& ch : name) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            out += "\t" + name + "\n";
            ++nf;
        }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "factions: cannot write %s\n", path.c_str());
        return false;
    }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("  factions: %zu templates, %zu factions written to %s%s\n", nt, nf, path.c_str(),
                haveFa ? "" : " (Faction.dbc missing: no reputations)");
    (void)verbose;
    return true;
}

}  // namespace w3d
