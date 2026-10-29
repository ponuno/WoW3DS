// glue_data.hpp — what the login and character screens know about races and classes (stage 5.29): names, factions,
// which classes each race may be, the scenes, descriptions and racial traits (built in, English and Russian; the game's
// own texts from glue.txt replace the English ones), the start outfits (CharStartOutfit.dbc via glue.txt), the zone names of
// the character list and the appearance choices a race model really has (from its CHRS entry). Platform-free.
#pragma once
#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace glue {

enum class Faction : uint8_t { Alliance, Horde };

struct RaceInfo {
    uint8_t id;                  // ChrRaces
    const char* key;             // HUMAN, ORC, ... (GlueStrings: RACE_INFO_<key>)
    const char* en;
    const char* ru;
    Faction faction;
    uint16_t classMask;          // bit = class id
    float height[2];             // yards, male / female (camera framing before the model is in)
    const char* descEn;
    const char* descRu;
    const char* traitsEn;        // "\n"-separated
    const char* traitsRu;
};
struct ClassInfo {
    uint8_t id;                  // ChrClasses
    const char* key;             // WARRIOR, ... (GlueStrings: CLASS_INFO_<key>)
    const char* en;
    const char* ru;
    const char* roleEn;
    const char* roleRu;
    const char* descEn;
    const char* descRu;
};

// The eight races in the order of the creation screen: Alliance (Human, Dwarf, Night Elf, Gnome), Horde (Orc, Undead,
// Tauren, Troll); the nine classes in the game's button order.
const RaceInfo* races(int* count);
const ClassInfo* classes(int* count);
const RaceInfo* race(uint8_t id);
const ClassInfo* cls(uint8_t id);
bool allowed(uint8_t race, uint8_t cls);
const char* raceName(uint8_t race, int lang);
const char* className(uint8_t cls, int lang);

// Equipment slot (W3D_EQ_*) for an inventory type (ItemTemplate::InventoryType), -1 = not shown (bags, ammo).
int slotForInventoryType(uint8_t inv);

// glue.txt of w3dconv. Empty (built-in texts, no clothes, no zone names) without it.
class Texts {
public:
    bool load(const char* path);
    bool loaded() const { return loaded_; }
    // A GlueStrings.lua string ("\n" turned into line breaks), nullptr when the client has none.
    const std::string* text(const std::string& key) const;
    // The clothes of a new character: display id and inventory type per equipment slot (0 = nothing). False: none known.
    bool outfit(uint8_t race, uint8_t cls, uint8_t sex, uint32_t display[19], uint8_t inv[19]) const;
    const std::string* zone(uint32_t areaId) const;
    uint32_t zoneParent(uint32_t areaId) const;   // the zone of a subzone (0 = it is a zone, or glue.txt is older)
    void addText(const std::string& key, const std::string& v) { texts_[key] = v; }
    void addZone(uint32_t id, const std::string& v) { zones_[id] = v; }
    void addOutfit(uint8_t race, uint8_t cls, uint8_t sex, const std::vector<std::pair<uint32_t, uint8_t>>& items);
private:
    bool loaded_ = false;
    std::unordered_map<std::string, std::string> texts_;
    std::unordered_map<uint32_t, std::string> zones_;
    std::unordered_map<uint32_t, uint32_t> zoneParents_;
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint8_t>>> outfits_;
};

// The description shown on the creation screen: the game's text (English) when the client has it, else the built-in one.
std::string raceDescription(const Texts& t, uint8_t race, int lang);
std::string classDescription(const Texts& t, uint8_t cls, int lang);

// What a race / sex can look like: the values the CharSections / style rows of its CHRS entry have (sorted). Faces and
// hair colours depend on the chosen skin colour / hair style, as in the game.
struct Appearance {
    std::vector<uint8_t> skins, faces, hairStyles, hairColors, facials;
};
void appearanceFrom(const uint8_t* chrs, uint32_t size, uint8_t skin, uint8_t hairStyle, Appearance& out);

}  // namespace glue
