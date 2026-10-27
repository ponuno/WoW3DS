#include "action_layout.hpp"

#include <stdio.h>

#include <algorithm>

const char* padFixedName(int button) {
    switch (button) {
        case 4: return "Menu";       // X (top): context menu of the target
        case 5: return "Cancel";     // A (right)
        case 6: return "Jump";       // B (bottom)
        case 7: return "Use";        // Y (left): interact / attack
        default: return "";
    }
}

void ActionLayout::clear() {
    for (auto& p : slots_)
        for (auto& s : p) s = ActionSlot();
    page = 0;
}

bool ActionLayout::empty() const {
    for (auto& p : slots_)
        for (auto& s : p)
            if (s.type != ActionSlot::Empty) return false;
    return true;
}

bool ActionLayout::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    clear();
    int pg, slot, type;
    unsigned long id;
    while (fscanf(f, "%d %d %d %lu", &pg, &slot, &type, &id) == 4) {
        if (pg < 0 || pg >= kPages || !padSlotBindable(slot) || type < 1 || type > 2) continue;
        slots_[pg][slot].type = (uint8_t)type;
        slots_[pg][slot].id = (uint32_t)id;
    }
    fclose(f);
    return true;
}

bool ActionLayout::save(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    for (int p = 0; p < kPages; ++p)
        for (int s = 0; s < kPadSlots; ++s)
            if (slots_[p][s].type != ActionSlot::Empty)
                fprintf(f, "%d %d %d %lu\n", p, s, slots_[p][s].type, (unsigned long)slots_[p][s].id);
    fclose(f);
    return true;
}

bool ActionLayout::autofill(const w3n::GameSession& g) {
    std::vector<uint32_t> spells = g.barSpells();
    if (spells.empty()) return false;
    // the D-pad first (no modifier), then ZR (the right trigger is the natural one), ZL, both
    static const int kOrder[] = {0, 1, 2, 3, 2 * 8 + 0, 2 * 8 + 1, 2 * 8 + 2, 2 * 8 + 3, 2 * 8 + 4, 2 * 8 + 5, 2 * 8 + 6,
                                 2 * 8 + 7, 8 + 0, 8 + 1, 8 + 2, 8 + 3, 8 + 4, 8 + 5, 8 + 6, 8 + 7, 24, 25, 26, 27, 28,
                                 29, 30, 31};
    size_t n = 0;
    for (int slot : kOrder) {
        if (n >= spells.size()) break;
        slots_[0][slot].type = ActionSlot::Spell;
        slots_[0][slot].id = spells[n++];
    }
    return n > 0;
}

void ActionLayout::bind(int slot, ActionSlot a) {
    if (!padSlotBindable(slot)) return;
    if (a.type != ActionSlot::Empty)
        for (auto& s : slots_[page])
            if (s.type == a.type && s.id == a.id) s = ActionSlot();
    slots_[page][slot] = a;
}

bool ActionLayout::findItem(const w3n::GameSession& g, uint32_t entry, w3n::InvItem* out) const {
    for (const w3n::InvItem& it : g.inventory()) {
        if (it.entry != entry || it.equipped()) continue;
        if (out) *out = it;
        return true;
    }
    return false;
}

std::string ActionLayout::label(w3n::GameSession& g, int slot) const {
    const ActionSlot& a = at(slot);
    if (a.type == ActionSlot::Spell) return g.spellNames.name(a.id);
    if (a.type == ActionSlot::Item) {
        uint32_t count = 0;
        for (const w3n::InvItem& it : g.inventory())
            if (it.entry == a.id && !it.equipped()) count += it.count;
        return g.itemName(a.id) + " x" + std::to_string(count);
    }
    return "";
}

uint32_t ActionLayout::cooldownMs(const w3n::GameSession& g, int slot, uint32_t nowMs) const {
    const ActionSlot& a = at(slot);
    if (a.type != ActionSlot::Spell) return 0;
    uint32_t cd = g.cooldownLeftMs(a.id, nowMs);
    if (a.id != w3n::kSpellAttack) cd = std::max(cd, g.gcdLeftMs(nowMs));
    return cd;
}
