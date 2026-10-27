// unit_state.hpp — what a unit of the session is doing, for its animation (CreatureSeen's state fields):
// movement flags, stand state, emotes, weapon, combat, casting, melee reactions, riding. Platform-free.
#pragma once
#include "creature_view.hpp"
#include "game.hpp"

void fillUnitState(w3n::GameSession& g, const w3n::WorldObject& o, uint32_t nowMs, CreatureSeen& c);
CreatureWeapon weaponFromSubclass(uint32_t itemClass, uint32_t subclass);
