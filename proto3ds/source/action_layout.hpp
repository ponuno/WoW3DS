// action_layout.hpp — the controller hotbars (stage 5.11): pages x 32 slots (pad.hpp addressing, 28 bindable),
// each a spell or an item. Kept per character on the SD card (bars_<name>.txt), because the server's 120-slot
// action bar is laid out for a keyboard. A new character's layout is filled from the server bar / spell book.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "game.hpp"
#include "pad.hpp"

enum PetCmdId : uint32_t { kPetCmdAttack = 1, kPetCmdFollow = 2, kPetCmdStay = 3, kPetCmdPassive = 4, kPetCmdDefensive = 5, kPetCmdAggressive = 6 };
const char* petCmdName(uint32_t id);       // "Attack", "Follow", "Stay", "Passive", "Defensive", "Aggressive"
const char* petCmdIcon(uint32_t id);       // the named icon of the skin ("pet_attack" ...)
bool petCmdActive(const w3n::GameSession& g, uint32_t id);   // the pet's current order / stance (lit on the bars)

struct ActionSlot {
    // UI stage 13: PetCmd (id = PetCmdId) and PetSpell (id = the spell of the pet's book) run the pet's bar
    enum Type : uint8_t { Empty = 0, Spell = 1, Item = 2, PetCmd = 3, PetSpell = 4 };
    uint8_t type = Empty;
    uint32_t id = 0;                  // spell id or item entry (PetCmd: PetCmdId)
};

class ActionLayout {
public:
    static constexpr int kPages = 3;
    ActionLayout() { clear(); }
    void clear();
    bool empty() const;
    bool load(const char* path);
    bool save(const char* path) const;
    // Fills an empty layout: the server's bar spells (else the named spells of the book), the first four on the
    // D-pad, then ZR, ZL and ZL+ZR. Returns true when it placed anything.
    bool autofill(const w3n::GameSession& g);

    int page = 0;
    void turnPage(int delta) { page = ((page + delta) % kPages + kPages) % kPages; }
    const ActionSlot& at(int slot) const { return slots_[page][slot]; }
    const ActionSlot& atPage(int pg, int slot) const { return slots_[pg][slot]; }   // UI stage 12 (the cooldown strip)
    void bind(int slot, ActionSlot s);  // also removes the same action from other slots of this page

    // Short name for bars ("Heroic Strike", "Tough Hunk of Bread x3", "" when empty) and the cooldown left.
    std::string label(w3n::GameSession& g, int slot) const;
    uint32_t cooldownMs(const w3n::GameSession& g, int slot, uint32_t nowMs) const;
    // The item to use for an Item slot: the first stack of that entry in the bags (nullptr when none left).
    bool findItem(const w3n::GameSession& g, uint32_t entry, w3n::InvItem* out) const;

private:
    ActionSlot slots_[kPages][kPadSlots];
};

// Fixed actions of set 0's face buttons (Forever) for bars and prompts.
const char* padFixedName(int button);   // 4..7: "Context", "Cancel", "Jump", "Interact"
