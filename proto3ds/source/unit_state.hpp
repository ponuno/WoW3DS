// unit_state.hpp — what a unit of the session is doing, for its animation (CreatureSeen's state fields):
// movement flags, stand state, emotes, weapon, combat, casting, melee reactions, riding. Platform-free.
#pragma once
#include "creature_view.hpp"
#include "game.hpp"

void fillUnitState(w3n::GameSession& g, const w3n::WorldObject& o, uint32_t nowMs, CreatureSeen& c);
CreatureWeapon weaponFromSubclass(uint32_t itemClass, uint32_t subclass);

// Stage 5.41. A game object's state for the animator (door open / closed, chest opened) and its rotation: upright objects use the
// facing the caller passes as yaw; a tilted one (GAMEOBJECT_ROTATION with x / y) gets the full matrix.
void fillGameObjectState(const w3n::WorldObject& o, CreatureSeen& c);
// A corpse (type 7) as a lying player: race, sex, look from CORPSE_FIELD_BYTES_1/2, worn items from CORPSE_FIELD_ITEM. False for
// bones (no model) or when the race is unknown.
bool fillCorpse(const w3n::WorldObject& o, CreatureSeen& c);
