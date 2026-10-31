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
        // 6.40: the school of magic (1.12 Spell.dbc column 1: 1 holy, 2 fire, 3 nature, 4 frost, 5 shadow, 6 arcane; 0 physical):
        // the client picks a cast sound by it for spells the sound data knows nothing of
        const uint32_t school = spell.u32(r, 1);
        if (school >= 1 && school <= 6) out += "@school\t" + std::to_string(id) + "\t" + std::to_string(school) + "\n";
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
    // UI stage 10: spells placed on the ground (Blizzard, Flamestrike, Rain of Fire, Volley, Hurricane, the engineers'
    // bombs ...): "@ground<TAB>id<TAB>max range<TAB>radius" (yards). Ground = the Targets mask asks for a destination
    // (TARGET_FLAG_DEST_LOCATION 0x40) or an effect hits the units around a chosen point (ImplicitTarget 16 / 28 / 31 / 34,
    // vMaNGOS SpellTarget). 1.12 columns relative to the name (120): Targets 13, RangeIndex 36, ImplicitTargetA 82-84,
    // ImplicitTargetB 85-87, EffectRadiusIndex 88-90; SpellRange.dbc MaxRange 2, SpellRadius.dbc Radius 1. Every spell
    // (items' spells are not in SkillLineAbility); the old client skips these lines (no id before the first tab).
    size_t ground = 0;
    if (nameCol >= 107) {
        Table range, radius;
        std::unordered_map<uint32_t, float> rangeOf, radiusOf;
        if (range.load(data, "SpellRange.dbc") && range.fields > 2)
            for (uint32_t r = 0; r < range.records; ++r) rangeOf[range.u32(r, 0)] = range.f32(r, 2);
        if (radius.load(data, "SpellRadius.dbc") && radius.fields > 1)
            for (uint32_t r = 0; r < radius.records; ++r) radiusOf[radius.u32(r, 0)] = radius.f32(r, 1);
        const uint32_t tgtCol = (uint32_t)nameCol - 107, rangeCol = (uint32_t)nameCol - 84, aCol = (uint32_t)nameCol - 38,
                       bCol = (uint32_t)nameCol - 35, radCol = (uint32_t)nameCol - 32;
        auto areaAtDest = [](uint32_t t) { return t == 16 || t == 28 || t == 31 || t == 34; };
        std::string known;
        for (uint32_t r = 0; r < spell.records; ++r) {
            const uint32_t id = spell.u32(r, 0);
            if (!id) continue;
            bool g = (spell.u32(r, tgtCol) & 0x40) != 0;
            float rad = 0;
            for (uint32_t k = 0; k < 3; ++k) {
                if (areaAtDest(spell.u32(r, aCol + k)) || areaAtDest(spell.u32(r, bCol + k))) g = true;
                auto it = radiusOf.find(spell.u32(r, radCol + k));
                if (it != radiusOf.end()) rad = std::max(rad, it->second);
            }
            if (!g) continue;
            auto rg = rangeOf.find(spell.u32(r, rangeCol));
            const float maxRange = rg != rangeOf.end() ? rg->second : 30.0f;
            if (maxRange <= 0.5f) continue;   // "self" range: centred on the caster, nothing to place
            char line[96];
            std::snprintf(line, sizeof line, "@ground\t%u\t%.1f\t%.1f\n", id, maxRange, rad > 0 ? rad : 5.0f);
            out += line;
            ++ground;
            if (id == 10 || id == 2120 || id == 5740 || id == 1510 || id == 16914 || id == 4064)
                known += " " + std::to_string(id) + "(" + spell.str(r, (uint32_t)nameCol) + ")";
        }
        std::printf("  ground-targeted spells: %zu (known among them:%s)\n", ground, known.empty() ? " none" : known.c_str());
    }
    // 5.48: spells that need no unit target: "@self<TAB>id". Every effect's ImplicitTargetA/B (vMaNGOS SpellTarget) is the
    // caster (1), its party / the friends or hostiles around it (20, 30, 33, 34, 36) - Battle Shout, Thorns-like self buffs,
    // Thunder Clap. Spells with any single-unit target (6 chain damage, 21 friend ...) or an unknown one are left out.
    size_t selfCasts = 0;
    if (nameCol >= 107) {
        const uint32_t effC = (uint32_t)nameCol - 59, aC = (uint32_t)nameCol - 38, bC = (uint32_t)nameCol - 35;
        auto selfKind = [](uint32_t t) { return t == 1 || t == 20 || t == 30 || t == 33 || t == 34 || t == 36; };
        for (uint32_t r = 0; r < spell.records; ++r) {
            const uint32_t id = spell.u32(r, 0);
            if (!id) continue;
            bool any = false, ok = true;
            for (uint32_t k = 0; k < 3 && ok; ++k) {
                if (!spell.u32(r, effC + k)) continue;   // no effect in this slot
                for (uint32_t t : {spell.u32(r, aC + k), spell.u32(r, bC + k)}) {
                    if (!t) continue;
                    if (selfKind(t)) any = true; else ok = false;
                }
            }
            if (!any || !ok) continue;
            out += "@self\t" + std::to_string(id) + "\n";
            ++selfCasts;
        }
        std::printf("  spells without a unit target: %zu\n", selfCasts);
    }
    // UI stage 12: cooldowns, "@cd id recovery category categoryRecovery onEvent" (ms): the 1.12 server sends none for an
    // ordinary cast, the client starts them itself from Spell.dbc. Columns relative to the name (120): Category 2,
    // Attributes 6 (0x02000000 SPELL_ATTR_COOLDOWN_ON_EVENT), RecoveryTime 19, CategoryRecoveryTime 20.
    size_t cds = 0;
    if (nameCol >= 118) {
        const uint32_t catCol = (uint32_t)nameCol - 118, attrCol = (uint32_t)nameCol - 114, recCol = (uint32_t)nameCol - 101,
                       catRecCol = (uint32_t)nameCol - 100;
        std::string known;
        for (uint32_t r = 0; r < spell.records; ++r) {
            const uint32_t id = spell.u32(r, 0), rec = spell.u32(r, recCol), catRec = spell.u32(r, catRecCol);
            const uint32_t cat = spell.u32(r, catCol);
            const bool onEvent = (spell.u32(r, attrCol) & 0x02000000u) != 0;
            if (!id || (!rec && !catRec && !onEvent) || rec > 86400000u || catRec > 86400000u) continue;
            char line[96];
            std::snprintf(line, sizeof line, "@cd\t%u\t%u\t%u\t%u\t%u\n", id, rec, cat, catRec, onEvent ? 1u : 0u);
            out += line;
            ++cds;
            if (id == 871 || id == 2139 || id == 1953 || id == 100 || id == 20271)
                known += " " + std::to_string(id) + "(" + spell.str(r, (uint32_t)nameCol) + " " + std::to_string(rec ? rec : catRec) + " ms)";
        }
        std::printf("  spell cooldowns: %zu (known among them:%s)\n", cds, known.empty() ? " none" : known.c_str());
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
