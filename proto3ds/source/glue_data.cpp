#include "glue_data.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "w3d_character.h"
#include "w3d_format.h"

namespace glue {

namespace {
constexpr uint16_t C(int id) { return (uint16_t)(1u << id); }
// classes: 1 Warrior 2 Paladin 3 Hunter 4 Rogue 5 Priest 7 Shaman 8 Mage 9 Warlock 11 Druid
const RaceInfo kRaces[] = {
    {1, "HUMAN", "Human", "Человек", Faction::Alliance, (uint16_t)(C(1) | C(2) | C(4) | C(5) | C(8) | C(9)), {1.85f, 1.75f},
     "Humans are a young, resilient race whose kingdom of Stormwind leads the Alliance. Versatile and determined, they make "
     "fine soldiers, priests and mages, and their paladins carry the Light into battle.",
     "Люди — молодая и стойкая раса; их королевство Штормград возглавляет Альянс. Разносторонние и решительные, они становятся "
     "отличными воинами, жрецами и магами, а их паладины несут Свет в бой.",
     "Diplomacy: +10% reputation gains\nPerception: see stealthed enemies better\nThe Human Spirit: +5% Spirit\n"
     "Sword and Mace Specialization: +5 weapon skill",
     "Дипломатия: +10% к получаемой репутации\nВосприятие: лучше видите незаметных врагов\nЧеловеческий дух: +5% к духу\n"
     "Владение мечами и дробящим оружием: +5 к навыку"},
    {3, "DWARF", "Dwarf", "Дворф", Faction::Alliance, (uint16_t)(C(1) | C(2) | C(3) | C(4) | C(5)), {1.40f, 1.35f},
     "The stout dwarves of Ironforge delve deep beneath the mountains of Dun Morogh. Hardy warriors and skilled smiths, they "
     "are fierce allies of humankind and tireless seekers of their ancient past.",
     "Крепкие дворфы Стальгорна живут в глубинах гор Дун Морога. Стойкие воины и искусные кузнецы, они верные союзники людей "
     "и неутомимые исследователи своего древнего прошлого.",
     "Stoneform: become immune to poison, disease and bleeding, +10% armor\nGun Specialization: +5 Guns skill\n"
     "Frost Resistance: +10\nTreasure Finding: track treasure",
     "Каменная форма: невосприимчивость к ядам, болезням и кровотечению, +10% к броне\nВладение огнестрельным оружием: +5 к навыку\n"
     "Сопротивление магии льда: +10\nПоиск сокровищ: показывает сокровища"},
    {4, "NIGHTELF", "Night Elf", "Ночной эльф", Faction::Alliance, (uint16_t)(C(1) | C(3) | C(4) | C(5) | C(11)), {2.15f, 2.05f},
     "The ancient night elves live in the forests of Teldrassil, the great world tree. Reclusive and proud, they guard nature "
     "with their druids and hunters and have joined the Alliance against the Burning Legion.",
     "Древние ночные эльфы живут в лесах Тельдрассила, великого древа мира. Замкнутые и гордые, они хранят природу вместе с "
     "друидами и охотниками и вступили в Альянс, чтобы противостоять Пылающему Легиону.",
     "Shadowmeld: become invisible while standing still\nQuickness: +1% dodge\nNature Resistance: +10\n"
     "Wisp Spirit: become a fast wisp when dead",
     "Слиться с тенью: невидимость, пока стоите на месте\nПроворство: +1% к уклонению\nСопротивление силам природы: +10\n"
     "Дух огонька: после смерти вы — быстрый огонёк"},
    {7, "GNOME", "Gnome", "Гном", Faction::Alliance, (uint16_t)(C(1) | C(4) | C(8) | C(9)), {1.05f, 1.00f},
     "Gnomes are brilliant, eccentric inventors who lost their city of Gnomeregan to a terrible invasion. Now guests of "
     "Ironforge, they put their sharp minds to arcane magic and engineering.",
     "Гномы — гениальные и эксцентричные изобретатели, потерявшие свой город Гномреган. Теперь они гостят в Стальгорне и "
     "направляют свой острый ум на тайную магию и инженерное дело.",
     "Escape Artist: escape from immobilizing effects\nExpansive Mind: +5% Intellect\nArcane Resistance: +10\n"
     "Engineering Specialization: +15 Engineering",
     "Мастер побега: освобождение от обездвиживания\nПытливый ум: +5% к интеллекту\nСопротивление тайной магии: +10\n"
     "Инженерное дело: +15 к навыку"},
    {2, "ORC", "Orc", "Орк", Faction::Horde, (uint16_t)(C(1) | C(3) | C(4) | C(7) | C(9)), {1.95f, 1.85f},
     "Freed from the demonic curse that once drove them, the orcs of Durotar follow their warchief Thrall. Born warriors with a "
     "proud shamanistic tradition, they lead the Horde.",
     "Освободившись от демонического проклятия, орки Дуротара следуют за своим вождём Траллом. Прирождённые воины с гордой "
     "шаманской традицией, они возглавляют Орду.",
     "Blood Fury: increases attack power\nHardiness: +25% chance to resist stuns\nCommand: +5% pet damage\n"
     "Axe Specialization: +5 Axes skill",
     "Кровавое неистовство: увеличивает силу атаки\nТвёрдость: +25% к шансу устоять при оглушении\nВластность: +5% к урону питомца\n"
     "Владение топорами: +5 к навыку"},
    {5, "SCOURGE", "Undead", "Нежить", Faction::Horde, (uint16_t)(C(1) | C(4) | C(5) | C(8) | C(9)), {1.75f, 1.70f},
     "Freed from the Lich King's will, the Forsaken of Undercity fight for their own survival under the banshee queen Sylvanas. "
     "They are cunning, ruthless and allied to the Horde out of necessity.",
     "Освободившись от воли Короля-лича, Отрёкшиеся из Подгорода сражаются за собственное выживание под предводительством "
     "Сильваны. Хитрые и безжалостные, они примкнули к Орде по необходимости.",
     "Will of the Forsaken: break fear, charm and sleep\nCannibalize: regain health from corpses\n"
     "Underwater Breathing: breathe four times longer\nShadow Resistance: +10",
     "Воля Отрёкшихся: снимает страх, подчинение и сон\nКаннибализм: восстанавливает здоровье, поедая трупы\n"
     "Подводное дыхание: в 4 раза дольше под водой\nСопротивление темной магии: +10"},
    {6, "TAUREN", "Tauren", "Таурен", Faction::Horde, (uint16_t)(C(1) | C(3) | C(7) | C(11)), {2.55f, 2.30f},
     "The peaceful tauren roam the plains of Mulgore and honor the Earth Mother. Massive and strong, they are steadfast "
     "allies of the orcs and gifted druids, shamans and hunters.",
     "Миролюбивые таурены кочуют по равнинам Мулгора и чтят Мать-Землю. Огромные и сильные, они верные союзники орков и "
     "талантливые друиды, шаманы и охотники.",
     "War Stomp: stun nearby enemies\nEndurance: +5% health\nCultivation: +15 Herbalism\nNature Resistance: +10",
     "Громовая поступь: оглушает врагов рядом\nВыносливость: +5% к здоровью\nСельское хозяйство: +15 к травничеству\n"
     "Сопротивление силам природы: +10"},
    {8, "TROLL", "Troll", "Тролль", Faction::Horde, (uint16_t)(C(1) | C(3) | C(4) | C(5) | C(7) | C(8)), {2.20f, 2.10f},
     "The Darkspear trolls, once outcasts, owe their lives to Thrall and now live in Durotar. Savage and cunning, they are "
     "skilled hunters and mystics with a dark voodoo tradition.",
     "Тролли племени Чёрного Копья, бывшие изгнанники, обязаны жизнью Траллу и теперь живут в Дуротаре. Дикие и хитрые, они "
     "искусные охотники и мистики с мрачной традицией вуду.",
     "Berserking: faster attacks and casting when wounded\nRegeneration: +10% health regeneration\n"
     "Beast Slaying: +5% damage against beasts\nBow and Throwing Specialization: +5 skill",
     "Берсерк: быстрее атаки и заклинания, когда вы ранены\nРегенерация: +10% к восстановлению здоровья\n"
     "Охота на зверей: +5% к урону по животным\nВладение луками и метательным оружием: +5 к навыку"},
};
const ClassInfo kClasses[] = {
    {1, "WARRIOR", "Warrior", "Воин", "Melee damage or tank. Uses rage. Plate armor at level 40.",
     "Урон в ближнем бою или танк. Ярость. Латы с 40 уровня.",
     "Warriors are masters of weapons and battle tactics. They build rage by dealing and taking blows, switch between "
     "stances, protect their allies with a shield or deal devastating blows with two-handed weapons.",
     "Воины — мастера оружия и боевой тактики. Они копят ярость, нанося и получая удары, переключают стойки, защищают союзников "
     "щитом или наносят сокрушительные удары двуручным оружием."},
    {2, "PALADIN", "Paladin", "Паладин", "Healer, tank or melee. Uses mana. Plate armor at level 40. Alliance only.",
     "Лекарь, танк или боец ближнего боя. Мана. Латы с 40 уровня. Только Альянс.",
     "Paladins are holy warriors of the Light. They heal and protect their allies with blessings and auras, wear heavy "
     "armor and smite the undead and demons.",
     "Паладины — святые воины Света. Они исцеляют и защищают союзников благословениями и аурами, носят тяжёлую броню и "
     "сокрушают нежить и демонов."},
    {3, "HUNTER", "Hunter", "Охотник", "Ranged damage with a pet. Uses mana. Mail armor at level 40.",
     "Урон издалека вместе с питомцем. Мана. Кольчуга с 40 уровня.",
     "Hunters fight from a distance with bows and guns while a tamed beast fights at their side. They track creatures, set "
     "traps and use aspects to suit the battle.",
     "Охотники сражаются издалека луком или ружьём, а рядом бьётся приручённый зверь. Они выслеживают существ, ставят "
     "ловушки и пользуются аспектами."},
    {4, "ROGUE", "Rogue", "Разбойник", "Melee damage from stealth. Uses energy and combo points. Leather armor.",
     "Урон в ближнем бою из незаметности. Энергия и приёмы серии. Кожаная броня.",
     "Rogues strike from the shadows. They sneak up unseen, poison their blades, stun and disable their foes and finish them "
     "with powerful combo moves.",
     "Разбойники наносят удары из тени. Они подкрадываются незаметно, отравляют клинки, оглушают врагов и добивают их "
     "мощными завершающими приёмами."},
    {5, "PRIEST", "Priest", "Жрец", "Healer or shadow damage. Uses mana. Cloth armor.",
     "Лекарь или урон темной магией. Мана. Тканевая броня.",
     "Priests are devoted to the spirit. They are the finest healers, shield their allies from harm, or turn to the shadow "
     "to destroy the minds of their enemies.",
     "Жрецы посвятили себя духу. Это лучшие лекари, они защищают союзников щитами или обращаются к тьме, чтобы сломить "
     "разум врагов."},
    {7, "SHAMAN", "Shaman", "Шаман", "Healer, caster or melee. Uses mana. Mail armor at level 40. Horde only.",
     "Лекарь, заклинатель или боец. Мана. Кольчуга с 40 уровня. Только Орда.",
     "Shamans call on the spirits of the elements. They place totems to aid their allies, hurl lightning, heal with the "
     "power of water and fight with enchanted weapons.",
     "Шаманы взывают к духам стихий. Они ставят тотемы в помощь союзникам, мечут молнии, исцеляют силой воды и сражаются "
     "зачарованным оружием."},
    {8, "MAGE", "Mage", "Маг", "Ranged spell damage and control. Uses mana. Cloth armor.",
     "Урон заклинаниями издалека и контроль. Мана. Тканевая броня.",
     "Mages wield the arcane, fire and frost. They destroy enemies from afar, turn them into sheep, conjure food and water "
     "and teleport across the world.",
     "Маги владеют тайной магией, огнём и льдом. Они уничтожают врагов издалека, превращают их в овец, создают еду и воду "
     "и телепортируются по миру."},
    {9, "WARLOCK", "Warlock", "Чернокнижник", "Ranged damage over time with a demon. Uses mana. Cloth armor.",
     "Урон со временем и демон-помощник. Мана. Тканевая броня.",
     "Warlocks bargain with demons. They summon minions to fight for them, curse and drain their enemies, strike fear into "
     "them and summon their allies from afar.",
     "Чернокнижники заключают сделки с демонами. Они призывают слуг, проклинают и истощают врагов, вселяют в них страх и "
     "призывают союзников издалека."},
    {11, "DRUID", "Druid", "Друид", "Healer, tank, melee or caster. Uses mana, rage or energy. Leather armor.",
     "Лекарь, танк, боец или заклинатель. Мана, ярость или энергия. Кожаная броня.",
     "Druids are guardians of nature who take the forms of animals: a bear to protect, a cat to strike, a seal to swim and "
     "a cheetah to travel; in their own form they heal and call down the wrath of nature.",
     "Друиды — хранители природы, принимающие облик животных: медведя для защиты, кошки для атаки, тюленя для плавания и "
     "гепарда для путешествий; в своём облике они исцеляют и обрушивают гнев природы."},
};
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
uint32_t outfitKey(uint8_t race, uint8_t cls, uint8_t sex) { return (uint32_t)race << 16 | (uint32_t)cls << 8 | (sex & 1); }
void sortUnique(std::vector<uint8_t>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}
}  // namespace

const RaceInfo* races(int* count) { *count = (int)(sizeof kRaces / sizeof kRaces[0]); return kRaces; }
const ClassInfo* classes(int* count) { *count = (int)(sizeof kClasses / sizeof kClasses[0]); return kClasses; }
const RaceInfo* race(uint8_t id) {
    for (const RaceInfo& r : kRaces) if (r.id == id) return &r;
    return nullptr;
}
const ClassInfo* cls(uint8_t id) {
    for (const ClassInfo& c : kClasses) if (c.id == id) return &c;
    return nullptr;
}
bool allowed(uint8_t r, uint8_t c) {
    const RaceInfo* ri = race(r);
    return ri && c < 16 && (ri->classMask & (1u << c));
}
const char* raceName(uint8_t r, int lang) {
    const RaceInfo* ri = race(r);
    return ri ? (lang ? ri->ru : ri->en) : "?";
}
const char* className(uint8_t c, int lang) {
    const ClassInfo* ci = cls(c);
    return ci ? (lang ? ci->ru : ci->en) : "?";
}

int slotForInventoryType(uint8_t inv) {
    switch (inv) {
        case 1: return W3D_EQ_HEAD;
        case 2: return W3D_EQ_NECK;
        case 3: return W3D_EQ_SHOULDERS;
        case 4: return W3D_EQ_SHIRT;
        case 5: case 20: return W3D_EQ_CHEST;
        case 6: return W3D_EQ_WAIST;
        case 7: return W3D_EQ_LEGS;
        case 8: return W3D_EQ_FEET;
        case 9: return W3D_EQ_WRISTS;
        case 10: return W3D_EQ_HANDS;
        case 11: return W3D_EQ_FINGER1;
        case 12: return W3D_EQ_TRINKET1;
        case 13: case 17: case 21: return W3D_EQ_MAINHAND;
        case 14: case 22: case 23: return W3D_EQ_OFFHAND;
        case 15: case 25: case 26: return W3D_EQ_RANGED;
        case 16: return W3D_EQ_BACK;
        case 19: return W3D_EQ_TABARD;
        default: return -1;   // 18 bag, 24 ammo, 27 quiver, 28 relic
    }
}

bool Texts::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    texts_.clear();
    zones_.clear();
    outfits_.clear();
    std::vector<char> line(8192);
    while (fgets(line.data(), (int)line.size(), f)) {
        std::vector<std::string> c = fields(line.data());
        if (c[0] == "T" && c.size() >= 3) {
            std::string v;
            for (size_t i = 0; i < c[2].size(); ++i) {   // "\n" written as two characters
                if (c[2][i] == '\\' && i + 1 < c[2].size() && c[2][i + 1] == 'n') { v += '\n'; ++i; }
                else v += c[2][i];
            }
            texts_[c[1]] = v;
        } else if (c[0] == "Z" && c.size() >= 3) {
            const uint32_t zid = (uint32_t)strtoul(c[1].c_str(), nullptr, 10);
            zones_[zid] = c[2];
            if (c.size() >= 4) zoneParents_[zid] = (uint32_t)strtoul(c[3].c_str(), nullptr, 10);
        } else if (c[0] == "O" && c.size() >= 4) {
            std::vector<std::pair<uint32_t, uint8_t>> items;
            for (size_t i = 4; i + 1 < c.size(); i += 2)
                items.push_back({(uint32_t)strtoul(c[i].c_str(), nullptr, 10), (uint8_t)atoi(c[i + 1].c_str())});
            addOutfit((uint8_t)atoi(c[1].c_str()), (uint8_t)atoi(c[2].c_str()), (uint8_t)atoi(c[3].c_str()), items);
        }
    }
    fclose(f);
    loaded_ = true;
    return true;
}

void Texts::addOutfit(uint8_t race, uint8_t cls, uint8_t sex, const std::vector<std::pair<uint32_t, uint8_t>>& items) {
    outfits_[outfitKey(race, cls, sex)] = items;
}

const std::string* Texts::text(const std::string& key) const {
    auto it = texts_.find(key);
    return it == texts_.end() ? nullptr : &it->second;
}

const std::string* Texts::zone(uint32_t id) const {
    auto it = zones_.find(id);
    return it == zones_.end() ? nullptr : &it->second;
}

uint32_t Texts::zoneParent(uint32_t id) const {
    auto it = zoneParents_.find(id);
    return it == zoneParents_.end() ? 0 : it->second;
}

bool Texts::outfit(uint8_t race, uint8_t cls, uint8_t sex, uint32_t display[19], uint8_t inv[19]) const {
    for (int k = 0; k < 19; ++k) { display[k] = 0; inv[k] = 0; }
    auto it = outfits_.find(outfitKey(race, cls, sex));
    if (it == outfits_.end()) return false;
    for (const auto& [d, t] : it->second) {
        const int s = slotForInventoryType(t);
        if (s < 0 || display[s]) continue;
        display[s] = d;
        inv[s] = t;
    }
    return true;
}

std::string raceDescription(const Texts& t, uint8_t r, int lang) {
    const RaceInfo* ri = race(r);
    if (!ri) return {};
    if (!lang)
        if (const std::string* s = t.text(std::string("RACE_INFO_") + ri->key)) return *s;
    return lang ? ri->descRu : ri->descEn;
}

std::string classDescription(const Texts& t, uint8_t c, int lang) {
    const ClassInfo* ci = cls(c);
    if (!ci) return {};
    if (!lang)
        if (const std::string* s = t.text(std::string("CLASS_INFO_") + ci->key)) return *s;
    return lang ? ci->descRu : ci->descEn;
}

void appearanceFrom(const uint8_t* chrs, uint32_t size, uint8_t skin, uint8_t hairStyle, Appearance& out) {
    out = Appearance();
    if (!chrs || !w3d_char_valid(chrs, size)) return;
    const W3dCharHeader* h = w3d_char(chrs);
    const W3dCharSection* s = (const W3dCharSection*)(chrs + h->offSections);
    const W3dCharStyle* st = (const W3dCharStyle*)(chrs + h->offStyles);
    for (uint32_t i = 0; i < h->sectionCount; ++i) {
        const W3dCharSection& c = s[i];
        if (c.kind == W3D_CSEC_SKIN && c.variation == 0) out.skins.push_back(c.color);
        else if (c.kind == W3D_CSEC_FACE && c.color == skin) out.faces.push_back(c.variation);
        else if (c.kind == W3D_CSEC_HAIR) {
            out.hairStyles.push_back(c.variation);
            if (c.variation == hairStyle) out.hairColors.push_back(c.color);
        }
    }
    for (uint32_t i = 0; i < h->styleCount; ++i) {
        if (st[i].kind == 0) out.hairStyles.push_back(st[i].variation);   // bald styles have no texture row
        else if (st[i].kind == 1) out.facials.push_back(st[i].variation);
    }
    if (out.hairColors.empty())   // a style without its own colour rows (bald): every colour the race has
        for (uint32_t i = 0; i < h->sectionCount; ++i)
            if (s[i].kind == W3D_CSEC_HAIR) out.hairColors.push_back(s[i].color);
    sortUnique(out.skins);
    sortUnique(out.faces);
    sortUnique(out.hairStyles);
    sortUnique(out.hairColors);
    sortUnique(out.facials);
    for (std::vector<uint8_t>* v : {&out.skins, &out.faces, &out.hairStyles, &out.hairColors, &out.facials})
        if (v->empty()) v->push_back(0);
}

}  // namespace glue
