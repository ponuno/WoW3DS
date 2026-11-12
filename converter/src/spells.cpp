// Spell names for the 3DS action bar (stage 5.8): Spell.dbc -> spells.txt, one "id<TAB>name<TAB>rank" line each.
// Faction reactions (stage 5.12): FactionTemplate.dbc + Faction.dbc -> factions.txt (see net/factions.hpp).
//
// Only spells that appear in SkillLineAbility.dbc (class, racial and profession spells a player can learn)
// are written, which is ~5 000 lines instead of ~24 000. Spell.dbc of 1.12 has 173 fields; the English name is
// column 120 and the rank 9 columns later (8 locales + a flags field per string group). To be safe against
// layout differences the name column is searched for: the column where spell 78 reads "Heroic Strike".
#include "conv.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <unordered_map>

namespace w3d {

namespace {
// 7.14: the order of the eight text columns in a 1.12 DBC (enUS first).
const char* const kLocaleCodes[8] = {"enUS", "koKR", "frFR", "deDE", "zhCN", "zhTW", "esES", "esMX"};
std::string g_localeDir = "enUS";
int g_localeIndex = 0;
}  // namespace

int gameLocaleIndex() { return g_localeIndex; }
const std::string& gameLocaleDir() { return g_localeDir; }

void setGameLocale(const std::string& code) {
    std::string c = code;
    for (char& ch : c) ch = (char)std::tolower((unsigned char)ch);
    if (c.empty() || c == "auto") {
        g_localeDir = c.empty() ? "enUS" : "auto";
        g_localeIndex = 0;
        return;
    }
    struct Entry { const char* code; int index; const char* dir; };
    static const Entry kMap[] = {
        {"enus", 0, "enUS"}, {"engb", 0, "enGB"}, {"kokr", 1, "koKR"}, {"frfr", 2, "frFR"},
        {"dede", 3, "deDE"}, {"zhcn", 4, "zhCN"}, {"zhtw", 5, "zhTW"}, {"eses", 6, "esES"},
        {"esmx", 7, "esMX"}, {"ruru", 0, "ruRU"},   // ruRU fan clients replace the English column
    };
    for (const Entry& e : kMap)
        if (c == e.code) {
            g_localeDir = e.dir;
            g_localeIndex = e.index;
            return;
        }
    std::fprintf(stderr, "locale: unknown '%s', using enUS\n", code.c_str());
    g_localeDir = "enUS";
    g_localeIndex = 0;
}

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

std::string DbcTable::loc(uint32_t r, uint32_t base, uint32_t count) const {
    // 7.14: the selected language's column, else the first non-empty one (a DBC may lack a translation).
    const int li = gameLocaleIndex();
    if (li >= 0 && (uint32_t)li < count) {
        std::string s = str(r, base + (uint32_t)li);
        if (!s.empty()) return s;
    }
    for (uint32_t c = 0; c < count; ++c) {
        std::string s = str(r, base + c);
        if (!s.empty()) return s;
    }
    return {};
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
        std::string name = spell.loc(r, (uint32_t)nameCol);
        if (name.empty()) continue;
        std::string rank = spell.loc(r, (uint32_t)rankCol);
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
                std::string name = spell.loc(it->second, (uint32_t)nameCol);
                std::string rank = spell.loc(it->second, (uint32_t)rankCol);
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
            std::string name = spell.loc(r, (uint32_t)nameCol);
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
                known += " " + std::to_string(id) + "(" + spell.loc(r, (uint32_t)nameCol) + ")";
        }
        std::printf("  ground-targeted spells: %zu (known among them:%s)\n", ground, known.empty() ? " none" : known.c_str());
    }
    // 7.16: items used on another item (Shiny Bauble on the fishing pole, a weapon oil or a sharpening stone on a weapon,
    // a poison): "@item<TAB>id<TAB>itemClass<TAB>subclassMask<TAB>inventoryTypeMask<TAB>mainHand". The spell's Targets mask
    // (name - 107) asks for an item (TARGET_FLAG_ITEM 0x10); the item requirements are Spell.dbc EquippedItemClass -62
    // (-1 = any), EquippedItemSubClassMask -61 and EquippedItemInventoryTypeMask -60 (the same columns recipes.txt uses).
    // mainHand = SPELL_ATTR_HELD_ITEM_ONLY (Attributes 0x200, name - 114): the original client puts the item from the main
    // hand in the cast by itself (vMaNGOS Spell::CheckCast requires that slot).
    size_t itemCasts = 0;
    if (nameCol >= 114) {
        const uint32_t tgtCol = (uint32_t)nameCol - 107, attrCol = (uint32_t)nameCol - 114, effCol = (uint32_t)nameCol - 59;
        std::string known;
        for (uint32_t r = 0; r < spell.records; ++r) {
            const uint32_t id = spell.u32(r, 0);
            if (!id) continue;
            const int32_t cls = (int32_t)spell.u32(r, (uint32_t)nameCol - 62);
            const uint32_t sub = spell.u32(r, (uint32_t)nameCol - 61), inv = spell.u32(r, (uint32_t)nameCol - 60);
            const bool mainHand = (spell.u32(r, attrCol) & 0x200) != 0;
            // The Targets mask is what the client uses to pick the cursor. Some item spells leave it 0 and only mark the
            // held item: their item requirements (a real class, not -1) and an enchant effect (53 permanent, 54 temporary,
            // 92 held item) tell them from ordinary spells (Fishing also names the main hand but summons a bobber).
            bool enchantsItem = false;
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t e = spell.u32(r, effCol + k);
                if (e == 53 || e == 54 || e == 92) enchantsItem = true;
            }
            if (!(spell.u32(r, tgtCol) & 0x10) && !(mainHand && cls >= 0 && enchantsItem)) continue;
            char line[96];
            std::snprintf(line, sizeof line, "@item\t%u\t%d\t%u\t%u\t%u\n", (unsigned)id, (int)cls, (unsigned)sub, (unsigned)inv,
                          (unsigned)(mainHand ? 1 : 0));
            out += line;
            ++itemCasts;
            if (id == 8087) known += " " + std::to_string(id) + "(" + spell.loc(r, (uint32_t)nameCol) + ")";
        }
        std::printf("  item-targeted spells: %zu (Shiny Bauble among them:%s)\n", itemCasts, known.empty() ? " no" : known.c_str());
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
    // 7.11: helpful spells at a friendly unit: "@friend<TAB>id" - an effect aims at a single friend (21) or party member (57)
    // and none at a single enemy (6, 25): Holy Light, Flash Heal, Rejuvenation ...; the client casts them at itself when the
    // selected target is an enemy or nothing.
    size_t friends = 0;
    if (nameCol >= 107) {
        const uint32_t effC = (uint32_t)nameCol - 59, aC = (uint32_t)nameCol - 38, bC = (uint32_t)nameCol - 35;
        for (uint32_t r = 0; r < spell.records; ++r) {
            const uint32_t id = spell.u32(r, 0);
            if (!id) continue;
            bool fr = false, foe = false;
            for (uint32_t k = 0; k < 3; ++k) {
                if (!spell.u32(r, effC + k)) continue;
                for (uint32_t t : {spell.u32(r, aC + k), spell.u32(r, bC + k)}) {
                    if (t == 21 || t == 57) fr = true;
                    if (t == 6 || t == 25) foe = true;
                }
            }
            if (!fr || foe) continue;
            out += "@friend\t" + std::to_string(id) + "\n";
            ++friends;
        }
        std::printf("  helpful spells at a friend: %zu\n", friends);
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
                known += " " + std::to_string(id) + "(" + spell.loc(r, (uint32_t)nameCol) + " " + std::to_string(rec ? rec : catRec) + " ms)";
        }
        std::printf("  spell cooldowns: %zu (known among them:%s)\n", cds, known.empty() ? " none" : known.c_str());
    }
    // 6.64: cast times, "@cast id ms": Spell.dbc CastingTimeIndex (column name-102) -> SpellCastTimes.dbc (ID, Base ms, PerLevel, Minimum).
    // The client starts its own cast bar and animation on send instead of waiting for SMSG_SPELL_START (ping).
    size_t casts = 0;
    {
        DbcTable ct;
        if (nameCol >= 102 && ct.load(data, "SpellCastTimes.dbc")) {
            std::unordered_map<uint32_t, uint32_t> baseOf;
            for (uint32_t r = 0; r < ct.records; ++r) {
                const int32_t base = (int32_t)ct.u32(r, 1);
                if (base > 0) baseOf[ct.u32(r, 0)] = (uint32_t)base;
            }
            const uint32_t idxCol = (uint32_t)nameCol - 102;
            for (uint32_t r = 0; r < spell.records; ++r) {
                const uint32_t id = spell.u32(r, 0);
                auto it = baseOf.find(spell.u32(r, idxCol));
                if (!id || it == baseOf.end() || it->second > 60000) continue;
                out += "@cast\t" + std::to_string(id) + "\t" + std::to_string(it->second) + "\n";
                ++casts;
            }
            std::printf("  spell cast times: %zu\n", casts);
        } else {
            std::fprintf(stderr, "spells: SpellCastTimes.dbc not found (no cast prediction)\n");
        }
    }
    // 6.87: auto-attack, "@combat id kind": 1 = a melee ability at an enemy (the original client turns auto-attack on with it),
    // 2 = fired with the ranged weapon (the shooting animation), 4 = SPELL_ATTR_STOP_ATTACK_TARGET (auto-attack ends).
    // 1.12 columns relative to the name (120): Attributes 6 (0x2 RANGED, 0x40 PASSIVE, 0x100000 STOP_ATTACK_TARGET),
    // AttributesEx2 8 (0x20 AUTOREPEAT), ImplicitTargetA 82-84, DmgClass 164 (1 magic, 2 melee, 3 ranged). The DmgClass column
    // is checked against Sinister Strike 1752 = 2, Arcane Shot 3044 = 3, Fireball 133 = 1, and searched for if it differs.
    size_t combat = 0;
    if (nameCol >= 114) {
        std::unordered_map<uint32_t, uint32_t> rowOf;
        for (uint32_t r = 0; r < spell.records; ++r) rowOf[spell.u32(r, 0)] = r;
        auto dmgOk = [&](uint32_t col) {
            auto v = [&](uint32_t id) -> int64_t { auto it = rowOf.find(id); return it == rowOf.end() ? -1 : (int64_t)spell.u32(it->second, col); };
            return v(1752) == 2 && v(3044) == 3 && v(133) == 1;
        };
        int dmgCol = -1;
        if ((uint32_t)nameCol + 44 < spell.fields && dmgOk((uint32_t)nameCol + 44)) dmgCol = nameCol + 44;
        for (uint32_t c = (uint32_t)nameCol + 20; dmgCol < 0 && c < spell.fields; ++c)
            if (dmgOk(c)) dmgCol = (int)c;
        const uint32_t attrCol = (uint32_t)nameCol - 114, attr2Col = (uint32_t)nameCol - 112, aCol = (uint32_t)nameCol - 38;
        size_t melee = 0, ranged = 0, stops = 0, passives = 0;
        // 6.95: "@passive id" - SPELL_ATTR_PASSIVE spells: the book shows them but they cannot be put on a bar
        for (uint32_t r = 0; r < spell.records; ++r) {
            const uint32_t id = spell.u32(r, 0);
            if (id && (spell.u32(r, attrCol) & 0x40)) { out += "@passive\t" + std::to_string(id) + "\n"; ++passives; }
        }
        std::printf("  passive spells: %zu\n", passives);
        // 6.99: "@hidden id" - spells the game's book never lists: SPELL_ATTR_DO_NOT_DISPLAY (Attributes 0x80), and the ones that
        // only grant a skill - weapon and armour proficiencies (Swords, Plate Mail), languages, skill steps (Effect 60 PROFICIENCY,
        // 39 LANGUAGE, 44 SKILL_STEP, 118 SKILL): those are skills (the Skills tab), not spells. Effect[3]: name - 56.
        size_t hidden = 0;
        for (uint32_t r = 0; r < spell.records; ++r) {
            const uint32_t id = spell.u32(r, 0);
            if (!id) continue;
            bool hide = (spell.u32(r, attrCol) & 0x80) != 0;
            if (!hide && nameCol >= 56) {
                bool any = false, onlySkill = true;
                for (uint32_t k = 0; k < 3; ++k) {
                    const uint32_t e = spell.u32(r, (uint32_t)nameCol - 56 + k);
                    if (!e) continue;
                    any = true;
                    if (e != 39 && e != 44 && e != 60 && e != 118) onlySkill = false;
                }
                hide = any && onlySkill;
            }
            if (hide) { out += "@hidden\t" + std::to_string(id) + "\n"; ++hidden; }
        }
        std::printf("  spells hidden from the book: %zu\n", hidden);
        for (uint32_t r = 0; r < spell.records; ++r) {
            const uint32_t id = spell.u32(r, 0);
            if (!id) continue;
            const uint32_t attr = spell.u32(r, attrCol), attr2 = spell.u32(r, attr2Col);
            const uint32_t dmg = dmgCol >= 0 ? spell.u32(r, (uint32_t)dmgCol) : 0;
            if (attr & 0x40) continue;   // passive
            bool atEnemy = false;
            for (uint32_t k = 0; k < 3; ++k) atEnemy = atEnemy || spell.u32(r, aCol + k) == 6;   // TARGET_CHAIN_DAMAGE: the enemy
            const bool learnable = !haveSla || wanted.count(id);
            uint32_t kind = 0;
            if (learnable && dmg == 2 && atEnemy && !(attr & 0x100000)) kind |= 1;
            if ((attr & 0x2) || dmg == 3 || (attr2 & 0x20)) kind |= 2;
            if (learnable && (attr & 0x100000)) kind |= 4;
            if (!kind) continue;
            out += "@combat\t" + std::to_string(id) + "\t" + std::to_string(kind) + "\n";
            ++combat;
            melee += (kind & 1) != 0; ranged += (kind & 2) != 0; stops += (kind & 4) != 0;
        }
        std::printf("  auto-attack spells: %zu (%zu melee abilities, %zu ranged, %zu that stop it; DmgClass column %d%s)\n", combat, melee,
                    ranged, stops, dmgCol, dmgCol < 0 ? " NOT FOUND: melee abilities will not start auto-attack" : "");
    }
    // 6.87: range, "@range id min max" (yards; SpellRange.dbc ID 0, MinRange 1, MaxRange 2; Spell.dbc RangeIndex = name - 84).
    // The original client refuses a cast at a unit out of range / too close itself, before sending it. Index 1 (self) and 2
    // (melee "combat range": the server checks reach with the bounding radii) are left out.
    size_t ranges = 0;
    if (nameCol >= 84) {
        Table rg;
        if (rg.load(data, "SpellRange.dbc") && rg.fields > 2) {
            std::unordered_map<uint32_t, std::pair<float, float>> mm;
            for (uint32_t r = 0; r < rg.records; ++r) mm[rg.u32(r, 0)] = {rg.f32(r, 1), rg.f32(r, 2)};
            const uint32_t rangeCol = (uint32_t)nameCol - 84;
            for (uint32_t r = 0; r < spell.records; ++r) {
                const uint32_t id = spell.u32(r, 0), idx = spell.u32(r, rangeCol);
                if (!id || idx <= 2 || (haveSla && !wanted.count(id))) continue;
                auto it = mm.find(idx);
                if (it == mm.end() || it->second.second <= 0.5f || it->second.second > 1000.0f) continue;
                char line[96];
                std::snprintf(line, sizeof line, "@range\t%u\t%.1f\t%.1f\n", id, it->second.first, it->second.second);
                out += line;
                ++ranges;
            }
        }
        std::printf("  spell ranges: %zu\n", ranges);
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
            std::string name = fa.loc(r, 19, 8);   // 7.14: the selected language
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
