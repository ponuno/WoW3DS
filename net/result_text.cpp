// result_text.cpp - client texts for SpellCastResult and InventoryResult codes of 1.12.1.
// GENERATED from vMaNGOS Spells/SpellDefines.h (enum SpellCastResult, comments = client strings) and
// Objects/ItemDefines.h (enum InventoryResult), with the #if SUPPORTED_CLIENT_BUILD branches of build 5875.
#include "game.hpp"

namespace w3n {

namespace {
const char* const kCastResult[] = {
    "You are in combat",   // 0 SPELL_FAILED_AFFECTING_COMBAT
    "You are already at full Health",   // 1 SPELL_FAILED_ALREADY_AT_FULL_HEALTH
    "You are already at full power",   // 2 SPELL_FAILED_ALREADY_AT_FULL_POWER
    "That creature is already being tamed",   // 3 SPELL_FAILED_ALREADY_BEING_TAMED
    "You already control a charmed creature",   // 4 SPELL_FAILED_ALREADY_HAVE_CHARM
    "You already control a summoned creature",   // 5 SPELL_FAILED_ALREADY_HAVE_SUMMON
    "Already open",   // 6 SPELL_FAILED_ALREADY_OPEN
    "A more powerful spell is already active",   // 7 SPELL_FAILED_AURA_BOUNCED
    "Autotrack interrupted",   // 8 SPELL_FAILED_AUTOTRACK_INTERRUPTED
    "You have no target",   // 9 SPELL_FAILED_BAD_IMPLICIT_TARGETS
    "Invalid target",   // 10 SPELL_FAILED_BAD_TARGETS
    "Target can't be charmed",   // 11 SPELL_FAILED_CANT_BE_CHARMED
    "Item cannot be disenchanted",   // 12 SPELL_FAILED_CANT_BE_DISENCHANTED
    "There are no gems in this",   // 13 SPELL_FAILED_CANT_BE_PROSPECTED
    "Target is tapped",   // 14 SPELL_FAILED_CANT_CAST_ON_TAPPED
    "You can't start a duel while invisible",   // 15 SPELL_FAILED_CANT_DUEL_WHILE_INVISIBLE
    "You can't start a duel while stealthed",   // 16 SPELL_FAILED_CANT_DUEL_WHILE_STEALTHED
    "You are too close to enemies",   // 17 SPELL_FAILED_CANT_STEALTH
    "You can't do that yet",   // 18 SPELL_FAILED_CASTER_AURASTATE
    "You are dead",   // 19 SPELL_FAILED_CASTER_DEAD
    "Can't do that while charmed",   // 20 SPELL_FAILED_CHARMED
    "That is already being used",   // 21 SPELL_FAILED_CHEST_IN_USE
    "Can't do that while confused",   // 22 SPELL_FAILED_CONFUSED
    "Dont report",   // 23 SPELL_FAILED_DONT_REPORT
    "Must have the proper item equipped",   // 24 SPELL_FAILED_EQUIPPED_ITEM
    "Must have the right weapon equipped",   // 25 SPELL_FAILED_EQUIPPED_ITEM_CLASS
    "Must have the right weapon in the main hand",   // 26 SPELL_FAILED_EQUIPPED_ITEM_CLASS_MAINHAND
    "Must have the right item in the off hand",   // 27 SPELL_FAILED_EQUIPPED_ITEM_CLASS_OFFHAND
    "Internal error",   // 28 SPELL_FAILED_ERROR
    "Fizzled",   // 29 SPELL_FAILED_FIZZLE
    "Can't do that while fleeing",   // 30 SPELL_FAILED_FLEEING
    "That food's level is not high enough for your pet",   // 31 SPELL_FAILED_FOOD_LOWLEVEL
    "Target is too high level",   // 32 SPELL_FAILED_HIGHLEVEL
    "Hunger satiated",   // 33 SPELL_FAILED_HUNGER_SATIATED
    "Immune",   // 34 SPELL_FAILED_IMMUNE
    "Interrupted",   // 35 SPELL_FAILED_INTERRUPTED
    "Interrupted",   // 36 SPELL_FAILED_INTERRUPTED_COMBAT
    "Item is already enchanted",   // 37 SPELL_FAILED_ITEM_ALREADY_ENCHANTED
    "Item is gone",   // 38 SPELL_FAILED_ITEM_GONE
    "Tried to enchant an item that didn't exist",   // 39 SPELL_FAILED_ITEM_NOT_FOUND
    "Item is not ready yet",   // 40 SPELL_FAILED_ITEM_NOT_READY
    "You are not high enough level",   // 41 SPELL_FAILED_LEVEL_REQUIREMENT
    "Target not in line of sight",   // 42 SPELL_FAILED_LINE_OF_SIGHT
    "Target is too low level",   // 43 SPELL_FAILED_LOWLEVEL
    "Skill not high enough",   // 44 SPELL_FAILED_LOW_CASTLEVEL
    "Your weapon hand is empty",   // 45 SPELL_FAILED_MAINHAND_EMPTY
    "Can't do that while moving",   // 46 SPELL_FAILED_MOVING
    "Ammo needs to be in the paper doll ammo slot before it can be fired",   // 47 SPELL_FAILED_NEED_AMMO
    "Requires an ammo pouch",   // 48 SPELL_FAILED_NEED_AMMO_POUCH
    "Requires exotic ammo",   // 49 SPELL_FAILED_NEED_EXOTIC_AMMO
    "No path available",   // 50 SPELL_FAILED_NOPATH
    "You must be behind your target",   // 51 SPELL_FAILED_NOT_BEHIND
    "Your cast didn't land in fishable water",   // 52 SPELL_FAILED_NOT_FISHABLE
    "You can't use that here",   // 53 SPELL_FAILED_NOT_HERE
    "You must be in front of your target",   // 54 SPELL_FAILED_NOT_INFRONT
    "You are not in control of your actions",   // 55 SPELL_FAILED_NOT_IN_CONTROL
    "Spell not learned",   // 56 SPELL_FAILED_NOT_KNOWN
    "You are mounted",   // 57 SPELL_FAILED_NOT_MOUNTED
    "You are in flight",   // 58 SPELL_FAILED_NOT_ON_TAXI
    "You are on a transport",   // 59 SPELL_FAILED_NOT_ON_TRANSPORT
    "Spell is not ready yet",   // 60 SPELL_FAILED_NOT_READY
    "You are in shapeshift form",   // 61 SPELL_FAILED_NOT_SHAPESHIFT
    "You must be standing to do that",   // 62 SPELL_FAILED_NOT_STANDING
    "You can only use this on an object you own",   // 63 SPELL_FAILED_NOT_TRADEABLE
    "Tried to enchant a trade item, but not trading",   // 64 SPELL_FAILED_NOT_TRADING
    "You have to be unsheathed to do that!",   // 65 SPELL_FAILED_NOT_UNSHEATHED
    "Can't cast as ghost",   // 66 SPELL_FAILED_NOT_WHILE_GHOST
    "Out of ammo",   // 67 SPELL_FAILED_NO_AMMO
    "No charges remain",   // 68 SPELL_FAILED_NO_CHARGES_REMAIN
    "You haven't selected a champion",   // 69 SPELL_FAILED_NO_CHAMPION
    "That ability requires combo points",   // 70 SPELL_FAILED_NO_COMBO_POINTS
    "Dueling isn't allowed here",   // 71 SPELL_FAILED_NO_DUELING
    "Not enough endurance",   // 72 SPELL_FAILED_NO_ENDURANCE
    "There aren't any fish here",   // 73 SPELL_FAILED_NO_FISH
    "Can't use items while shapeshifted",   // 74 SPELL_FAILED_NO_ITEMS_WHILE_SHAPESHIFTED
    "You can't mount here",   // 75 SPELL_FAILED_NO_MOUNTS_ALLOWED
    "You do not have a pet",   // 76 SPELL_FAILED_NO_PET
    "Not enough power",   // 77 SPELL_FAILED_NO_POWER
    "Nothing to dispel",   // 78 SPELL_FAILED_NOTHING_TO_DISPEL
    "Nothing to steal",   // 79 SPELL_FAILED_NOTHING_TO_STEAL
    "Cannot use while swimming",   // 80 SPELL_FAILED_ONLY_ABOVEWATER
    "Can only use during the day",   // 81 SPELL_FAILED_ONLY_DAYTIME
    "Can only use indoors",   // 82 SPELL_FAILED_ONLY_INDOORS
    "Can only use while mounted",   // 83 SPELL_FAILED_ONLY_MOUNTED
    "Can only use during the night",   // 84 SPELL_FAILED_ONLY_NIGHTTIME
    "Can only use outside",   // 85 SPELL_FAILED_ONLY_OUTDOORS
    "Must be in the right form",   // 86 SPELL_FAILED_ONLY_SHAPESHIFT
    "You must be in stealth mode",   // 87 SPELL_FAILED_ONLY_STEALTHED
    "Can only use while swimming",   // 88 SPELL_FAILED_ONLY_UNDERWATER
    "Out of range",   // 89 SPELL_FAILED_OUT_OF_RANGE
    "Can't use that ability while pacified",   // 90 SPELL_FAILED_PACIFIED
    "You are possessed",   // 91 SPELL_FAILED_POSSESSED
    "Reagents",   // 92 SPELL_FAILED_REAGENTS
    "You need to be in a certain area",   // 93 SPELL_FAILED_REQUIRES_AREA
    "Requires a spell focus nearby",   // 94 SPELL_FAILED_REQUIRES_SPELL_FOCUS
    "You are unable to move",   // 95 SPELL_FAILED_ROOTED
    "Can't do that while silenced",   // 96 SPELL_FAILED_SILENCED
    "Another action is in progress",   // 97 SPELL_FAILED_SPELL_IN_PROGRESS
    "You have already learned the spell",   // 98 SPELL_FAILED_SPELL_LEARNED
    "The spell is not available to you",   // 99 SPELL_FAILED_SPELL_UNAVAILABLE
    "Can't do that while stunned",   // 100 SPELL_FAILED_STUNNED
    "Your target is dead",   // 101 SPELL_FAILED_TARGETS_DEAD
    "Target is in combat",   // 102 SPELL_FAILED_TARGET_AFFECTING_COMBAT
    "You can't do that yet",   // 103 SPELL_FAILED_TARGET_AURASTATE
    "Target is currently dueling",   // 104 SPELL_FAILED_TARGET_DUELING
    "Target is hostile",   // 105 SPELL_FAILED_TARGET_ENEMY
    "Target is too enraged to be charmed",   // 106 SPELL_FAILED_TARGET_ENRAGED
    "Target is friendly",   // 107 SPELL_FAILED_TARGET_FRIENDLY
    "The target can't be in combat",   // 108 SPELL_FAILED_TARGET_IN_COMBAT
    "Can't target players",   // 109 SPELL_FAILED_TARGET_IS_PLAYER
    "Target is alive",   // 110 SPELL_FAILED_TARGET_NOT_DEAD
    "Target is not in your party",   // 111 SPELL_FAILED_TARGET_NOT_IN_PARTY
    "Creature must be looted first",   // 112 SPELL_FAILED_TARGET_NOT_LOOTED
    "Target is not a player",   // 113 SPELL_FAILED_TARGET_NOT_PLAYER
    "No pockets to pick",   // 114 SPELL_FAILED_TARGET_NO_POCKETS
    "Target has no weapons equipped",   // 115 SPELL_FAILED_TARGET_NO_WEAPONS
    "Creature is not skinnable",   // 116 SPELL_FAILED_TARGET_UNSKINNABLE
    "Thirst satiated",   // 117 SPELL_FAILED_THIRST_SATIATED
    "Target too close",   // 118 SPELL_FAILED_TOO_CLOSE
    "You have too many of that item already",   // 119 SPELL_FAILED_TOO_MANY_OF_ITEM
    "Totems",   // 120 SPELL_FAILED_TOTEMS
    "Not enough training points",   // 121 SPELL_FAILED_TRAINING_POINTS
    "Failed attempt",   // 122 SPELL_FAILED_TRY_AGAIN
    "Target needs to be behind you",   // 123 SPELL_FAILED_UNIT_NOT_BEHIND
    "Target needs to be in front of you",   // 124 SPELL_FAILED_UNIT_NOT_INFRONT
    "Your pet doesn't like that food",   // 125 SPELL_FAILED_WRONG_PET_FOOD
    "Can't cast while fatigued",   // 126 SPELL_FAILED_NOT_WHILE_FATIGUED
    "Target must be in this instance",   // 127 SPELL_FAILED_TARGET_NOT_IN_INSTANCE
    "Can't cast while trading",   // 128 SPELL_FAILED_NOT_WHILE_TRADING
    "Target is not in your party or raid group",   // 129 SPELL_FAILED_TARGET_NOT_IN_RAID
    "Cannot disenchant while looting",   // 130 SPELL_FAILED_DISENCHANT_WHILE_LOOTING
    "Cannot prospect while looting",   // 131 SPELL_FAILED_PROSPECT_WHILE_LOOTING
    "Prospect need more",   // 132 SPELL_FAILED_PROSPECT_NEED_MORE
    "Target is currently in free-for-all PvP combat",   // 133 SPELL_FAILED_TARGET_FREEFORALL
    "There are no nearby corpses to eat",   // 134 SPELL_FAILED_NO_EDIBLE_CORPSES
    "Can only use in battlegrounds",   // 135 SPELL_FAILED_ONLY_BATTLEGROUNDS
    "Target is not a ghost",   // 136 SPELL_FAILED_TARGET_NOT_GHOST
    "Your pet can't learn any more skills",   // 137 SPELL_FAILED_TOO_MANY_SKILLS
    "You can't use the new item",   // 138 SPELL_FAILED_TRANSFORM_UNUSABLE
    "The weather isn't right for that",   // 139 SPELL_FAILED_WRONG_WEATHER
    "You can't do that while you are immune",   // 140 SPELL_FAILED_DAMAGE_IMMUNE
    "Can't do that right now",   // 141 SPELL_FAILED_PREVENTED_BY_MECHANIC
    "Maximum play time exceeded",   // 142 SPELL_FAILED_PLAY_TIME
    "Your reputation isn't high enough",   // 143 SPELL_FAILED_REPUTATION
    "Your skill is not high enough",   // 144 SPELL_FAILED_MIN_SKILL
    "Unknown reason",   // 145 SPELL_FAILED_UNKNOWN
};
const char* const kInventoryResult[] = {
    "",   // 0 EQUIP_ERR_OK
    "You must reach a higher level to use that",   // 1 EQUIP_ERR_CANT_EQUIP_LEVEL_I
    "You lack the skill to use that",   // 2 EQUIP_ERR_CANT_EQUIP_SKILL
    "That item doesn't go in that slot",   // 3 EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT
    "That bag is full",   // 4 EQUIP_ERR_BAG_FULL
    "Can't put non-empty bags in other bags",   // 5 EQUIP_ERR_NONEMPTY_BAG_OVER_OTHER_BAG
    "Can't trade equipped bags",   // 6 EQUIP_ERR_CANT_TRADE_EQUIP_BAGS
    "Only ammo can go there",   // 7 EQUIP_ERR_ONLY_AMMO_CAN_GO_HERE
    "You do not have the required proficiency for that item",   // 8 EQUIP_ERR_NO_REQUIRED_PROFICIENCY
    "No equipment slot is available for that item",   // 9 EQUIP_ERR_NO_EQUIPMENT_SLOT_AVAILABLE
    "You can never use that item",   // 10 EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM
    "You can never use that item",   // 11 EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM2
    "No equipment slot is available for that item",   // 12 EQUIP_ERR_NO_EQUIPMENT_SLOT_AVAILABLE2
    "Cannot equip that with a two-handed weapon",   // 13 EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED
    "You cannot dual-wield",   // 14 EQUIP_ERR_CANT_DUAL_WIELD
    "That item doesn't go in that container",   // 15 EQUIP_ERR_ITEM_DOESNT_GO_INTO_BAG
    "That item doesn't go in that container",   // 16 EQUIP_ERR_ITEM_DOESNT_GO_INTO_BAG2
    "You can't carry any more of those items",   // 17 EQUIP_ERR_CANT_CARRY_MORE_OF_THIS
    "No equipment slot is available for that item",   // 18 EQUIP_ERR_NO_EQUIPMENT_SLOT_AVAILABLE3
    "This item cannot stack",   // 19 EQUIP_ERR_ITEM_CANT_STACK
    "This item cannot be equipped",   // 20 EQUIP_ERR_ITEM_CANT_BE_EQUIPPED
    "These items can't be swapped",   // 21 EQUIP_ERR_ITEMS_CANT_BE_SWAPPED
    "That slot is empty",   // 22 EQUIP_ERR_SLOT_IS_EMPTY
    "The item was not found",   // 23 EQUIP_ERR_ITEM_NOT_FOUND
    "You can't drop a soulbound item",   // 24 EQUIP_ERR_CANT_DROP_SOULBOUND
    "Out of range",   // 25 EQUIP_ERR_OUT_OF_RANGE
    "Tried to split more than number in stack",   // 26 EQUIP_ERR_TRIED_TO_SPLIT_MORE_THAN_COUNT
    "Couldn't split those items",   // 27 EQUIP_ERR_COULDNT_SPLIT_ITEMS
    "Missing reagent",   // 28 EQUIP_ERR_MISSING_REAGENT
    "You don't have enough money",   // 29 EQUIP_ERR_NOT_ENOUGH_MONEY
    "Not a bag",   // 30 EQUIP_ERR_NOT_A_BAG
    "You can only do that with empty bags",   // 31 EQUIP_ERR_CAN_ONLY_DO_WITH_EMPTY_BAGS
    "You don't own that item",   // 32 EQUIP_ERR_DONT_OWN_THAT_ITEM
    "You can only equip one quiver",   // 33 EQUIP_ERR_CAN_EQUIP_ONLY1_QUIVER
    "You must purchase that bag slot first",   // 34 EQUIP_ERR_MUST_PURCHASE_THAT_BAG_SLOT
    "You are too far away from a bank",   // 35 EQUIP_ERR_TOO_FAR_AWAY_FROM_BANK
    "Item is locked",   // 36 EQUIP_ERR_ITEM_LOCKED
    "You are stunned",   // 37 EQUIP_ERR_YOU_ARE_STUNNED
    "You can't do that when you're dead",   // 38 EQUIP_ERR_YOU_ARE_DEAD
    "You can't do that right now",   // 39 EQUIP_ERR_CANT_DO_RIGHT_NOW
    "Internal bag error",   // 40 EQUIP_ERR_INT_BAG_ERROR
    "You can only equip one bolt pouch",   // 41 EQUIP_ERR_CAN_EQUIP_ONLY1_BOLT
    "You can only equip one ammo pouch",   // 42 EQUIP_ERR_CAN_EQUIP_ONLY1_AMMOPOUCH
    "Stackable cant be wrapped",   // 43 EQUIP_ERR_STACKABLE_CANT_BE_WRAPPED
    "Equipped cant be wrapped",   // 44 EQUIP_ERR_EQUIPPED_CANT_BE_WRAPPED
    "Wrapped cant be wrapped",   // 45 EQUIP_ERR_WRAPPED_CANT_BE_WRAPPED
    "Bound cant be wrapped",   // 46 EQUIP_ERR_BOUND_CANT_BE_WRAPPED
    "Unique cant be wrapped",   // 47 EQUIP_ERR_UNIQUE_CANT_BE_WRAPPED
    "Bags cant be wrapped",   // 48 EQUIP_ERR_BAGS_CANT_BE_WRAPPED
    "Already looted",   // 49 EQUIP_ERR_ALREADY_LOOTED
    "Inventory is full",   // 50 EQUIP_ERR_INVENTORY_FULL
    "Your bank is full",   // 51 EQUIP_ERR_BANK_FULL
    "That item is currently sold out",   // 52 EQUIP_ERR_ITEM_IS_CURRENTLY_SOLD_OUT
    "That bag is full",   // 53 EQUIP_ERR_BAG_FULL3
    "The item was not found",   // 54 EQUIP_ERR_ITEM_NOT_FOUND2
    "This item cannot stack",   // 55 EQUIP_ERR_ITEM_CANT_STACK2
    "That bag is full",   // 56 EQUIP_ERR_BAG_FULL4
    "That item is currently sold out",   // 57 EQUIP_ERR_ITEM_SOLD_OUT
    "That object is busy",   // 58 EQUIP_ERR_OBJECT_IS_BUSY
    "None",   // 59 EQUIP_ERR_NONE
    "You can't do that while in combat",   // 60 EQUIP_ERR_NOT_IN_COMBAT
    "You can't do that while disarmed",   // 61 EQUIP_ERR_NOT_WHILE_DISARMED
    "That bag is full",   // 62 EQUIP_ERR_BAG_FULL6
    "You don't have the required rank for that item",   // 63 EQUIP_ERR_CANT_EQUIP_RANK
    "You don't have the required reputation for that item",   // 64 EQUIP_ERR_CANT_EQUIP_REPUTATION
    "You cannot equip another bag of that type",   // 65 EQUIP_ERR_TOO_MANY_SPECIAL_BAGS
    "You can't loot that item now",   // 66 EQUIP_ERR_LOOT_CANT_LOOT_THAT_NOW
};
}  // namespace

std::string castResultText(uint32_t code) {
    if (code < sizeof kCastResult / sizeof *kCastResult) return kCastResult[code];
    return "Unknown reason (" + std::to_string(code) + ")";
}

std::string inventoryResultText(uint32_t code) {
    if (code < sizeof kInventoryResult / sizeof *kInventoryResult) return kInventoryResult[code];
    return "That bag is full";   // the client shows any larger value as \"bag full\"
}

// ------------------------------------------------------------------ the character's voice (UI stage 8)
// Tokens of the original's error speech files (<Race><Gender>_err_<token>NN.wav, the same names WoWee probes).
namespace {
const char* const kVocalToken[(int)Vocal::Count] = {
    "", "nomana", "norage", "noenergy", "outofrange", "genericnotarget", "invalidattacktarget", "spellcooldown",
    "abilitycooldown", "itemcooldown", "noammo", "chestinuse", "inventoryfull", "bagfull", "itemmaxcount", "notabag",
    "cantuseitem", "itemlocked", "notequippable", "cantequiplevel", "cantequipskill", "cantequipever",
    "cantdropsoulbounditem", "notenoughmoney", "cantlearnspell", "loottoofar", "lootdidntkill", "cantloot",
};
}  // namespace

const char* vocalToken(Vocal v) { return (unsigned)v < (unsigned)Vocal::Count ? kVocalToken[(int)v] : ""; }

Vocal vocalFromToken(const std::string& t) {
    for (int i = 1; i < (int)Vocal::Count; ++i)
        if (t == kVocalToken[i]) return (Vocal)i;
    return Vocal::None;
}

Vocal vocalForCastResult(uint32_t code, uint8_t powerType, uint32_t cls) {
    const bool ability = cls == 1 || cls == 4;   // warriors and rogues: "That ability isn't ready yet"
    switch (code) {
        case 77:   // SPELL_FAILED_NO_POWER: "Not enough mana / rage / energy" by our power
            return powerType == 1 ? Vocal::NoRage : powerType == 3 ? Vocal::NoEnergy : powerType == 0 ? Vocal::NoMana : Vocal::None;
        case 89: return Vocal::OutOfRange;                      // SPELL_FAILED_OUT_OF_RANGE
        case 60: return ability ? Vocal::AbilityCooldown : Vocal::SpellCooldown;   // SPELL_FAILED_NOT_READY
        case 40: return Vocal::ItemCooldown;                    // SPELL_FAILED_ITEM_NOT_READY
        case 9: return Vocal::NoTarget;                         // SPELL_FAILED_BAD_IMPLICIT_TARGETS: "You have no target"
        case 10: case 101: case 105: case 107: case 113:        // BAD_TARGETS, TARGETS_DEAD, TARGET_ENEMY / FRIENDLY / NOT_PLAYER
            return Vocal::InvalidTarget;
        case 67: case 47: return Vocal::NoAmmo;                 // NO_AMMO, NEED_AMMO
        case 21: return Vocal::ChestInUse;                      // CHEST_IN_USE
        case 41: case 43: return Vocal::CantUseItem;            // LEVEL_REQUIREMENT, LOWLEVEL
        default: return Vocal::None;
    }
}

Vocal vocalForInventoryResult(uint32_t code) {
    switch (code) {
        case 50: return Vocal::InventoryFull;
        case 4: case 53: case 56: case 62: return Vocal::BagFull;
        case 17: return Vocal::ItemMaxCount;
        case 30: return Vocal::NotABag;
        case 1: return Vocal::CantEquipLevel;
        case 2: case 8: return Vocal::CantEquipSkill;
        case 10: case 11: return Vocal::CantEquipEver;
        case 3: case 20: return Vocal::NotEquippable;
        case 24: return Vocal::CantDropSoulbound;
        case 29: return Vocal::NotEnoughMoney;
        case 36: return Vocal::ItemLocked;
        case 25: return Vocal::OutOfRange;
        default: return Vocal::None;
    }
}

Vocal vocalForLootError(uint8_t e) {
    switch (e) {
        case 4: return Vocal::LootTooFar;
        case 0: return Vocal::LootDidntKill;   // no permission: "I didn't kill that"
        default: return Vocal::CantLoot;
    }
}

void GameSession::uiLine(const std::string& text, uint32_t nowMs, bool info) {
    if (text.empty()) return;
    // the same text again: renew it rather than stacking copies (spamming a key on cooldown)
    for (auto it = uiErrors.begin(); it != uiErrors.end(); ++it)
        if (it->text == text) { uiErrors.erase(it); break; }
    uiErrors.push_back({text, nowMs, info});
    while (uiErrors.size() > 4) uiErrors.pop_front();
}

void GameSession::error(const std::string& text, Vocal v, uint32_t nowMs, const std::string& chatLine) {
    addLocal(chatLine.empty() ? text : chatLine, nowMs);
    uiLine(text, nowMs, false);
    vocal(v);
}

void GameSession::notice(const std::string& text, uint32_t nowMs) {
    addLocal(text, nowMs);
    uiLine(text, nowMs, true);
}

uint64_t GameSession::npcWindow() const {
    if (gossip.open) return gossip.npc;
    if (vendor.open) return vendor.npc;
    if (trainer.open) return trainer.npc;
    if (questDialog.open()) return questDialog.npc;
    if (bank.open) return bank.npc;
    if (taxi.open) return taxi.npc;
    if (auction.open) return auction.npc;
    if (binderConfirm) return binderConfirm;
    return 0;
}

}  // namespace w3n
