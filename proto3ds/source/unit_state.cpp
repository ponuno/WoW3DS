#include "unit_state.hpp"

#include "w3d_character.h"
#include "w3d_worldobj.h"

using namespace w3n;

CreatureWeapon weaponFromSubclass(uint32_t cls, uint32_t sub) {
    if (cls != 2) return CreatureWeapon::Unarmed;
    switch (sub) {
        case 1: case 5: case 8: return CreatureWeapon::TwoHand;               // axe, mace, sword (2H)
        case 6: case 10: case 20: return CreatureWeapon::TwoHandLoose;        // polearm, staff, fishing pole
        case 2: case 18: return CreatureWeapon::Bow;                          // bow, crossbow
        case 3: return CreatureWeapon::Rifle;                                 // gun
        case 16: return CreatureWeapon::Thrown;
        default: return CreatureWeapon::OneHand;                              // 1H axe / mace / sword, dagger, fist, wand
    }
}

namespace {
constexpr uint32_t kInCombat = 0x80000, kStunned = 0x40000;

// class / subclass of equipment slot 'slot' (15 main hand, 17 ranged): players show item entries, creatures carry
// class and subclass in UNIT_VIRTUAL_ITEM_INFO (main hand, off hand, ranged).
bool itemOf(GameSession& g, const WorldObject& o, int slot, uint32_t* cls, uint32_t* sub) {
    if (o.type == ObjType::Player) {
        const uint32_t entry = o.field((uint16_t)(UF::PLAYER_VISIBLE_ITEM_1_0 + slot * UF::PLAYER_VISIBLE_ITEM_STRIDE));
        if (!entry) return false;
        const ItemInfo* it = g.itemInfo(entry);   // asks the server the first time
        if (!it || it->missing) { *cls = 2; *sub = slot == 17 ? 2 : 7; return true; }   // until it answers
        *cls = it->cls;
        *sub = it->subclass;
        return true;
    }
    const int v = slot == 15 ? 0 : slot == 16 ? 1 : 2;
    const uint32_t info = o.field((uint16_t)(UF::UNIT_VIRTUAL_ITEM_INFO + v * 2));
    if (!info) return false;
    *cls = info & 0xFF;
    *sub = (info >> 8) & 0xFF;
    return true;
}

constexpr uint32_t kPlayerHideHelm = 0x400, kPlayerHideCloak = 0x800;   // PLAYER_FLAGS (/helm, /cloak)

// Slots drawn from the worn items: armour that changes the body, plus the models (helmet, shoulders, weapons).
constexpr int kEquipSlots[] = {W3D_EQ_HEAD, W3D_EQ_SHOULDERS, W3D_EQ_SHIRT, W3D_EQ_CHEST, W3D_EQ_WAIST, W3D_EQ_LEGS, W3D_EQ_FEET,
                               W3D_EQ_WRISTS, W3D_EQ_HANDS, W3D_EQ_BACK, W3D_EQ_MAINHAND, W3D_EQ_OFFHAND, W3D_EQ_RANGED, W3D_EQ_TABARD};

void fillEquipment(GameSession& g, const WorldObject& o, CreatureSeen& c) {
    for (uint32_t& e : c.equip) e = 0;
    for (uint8_t& e : c.equipInv) e = 0;
    if (o.type == ObjType::Player) {
        const uint32_t flags = o.field(UF::PLAYER_FLAGS);
        for (int slot : kEquipSlots) {
            if (slot == W3D_EQ_HEAD && (flags & kPlayerHideHelm)) continue;
            if (slot == W3D_EQ_BACK && (flags & kPlayerHideCloak)) continue;
            const uint32_t entry = o.field((uint16_t)(UF::PLAYER_VISIBLE_ITEM_1_0 + slot * UF::PLAYER_VISIBLE_ITEM_STRIDE));
            if (!entry) continue;
            const ItemInfo* it = g.itemInfo(entry);   // asks the server the first time; the unit is redrawn when it answers
            if (!it || it->missing) continue;
            c.equip[slot] = it->displayId;
            c.equipInv[slot] = (uint8_t)it->inventoryType;
        }
        return;
    }
    static const int kSlot[3] = {W3D_EQ_MAINHAND, W3D_EQ_OFFHAND, W3D_EQ_RANGED};   // UNIT_VIRTUAL_ITEM slots
    for (int v = 0; v < 3; ++v) {
        c.equip[kSlot[v]] = o.field((uint16_t)(UF::UNIT_VIRTUAL_ITEM_SLOT_DISPLAY + v));
        c.equipInv[kSlot[v]] = (uint8_t)(o.field((uint16_t)(UF::UNIT_VIRTUAL_ITEM_INFO + v * 2 + 1)) & 0xFF);
    }
}
}  // namespace

void fillUnitState(GameSession& g, const WorldObject& o, uint32_t nowMs, CreatureSeen& c) {
    const bool self = o.guid == g.selfGuid();
    c.moveFlags = self ? g.mover.info().flags : o.mv.flags;
    c.dead = o.dead();
    c.swimming = (c.moveFlags & MoveFlag::Swimming) != 0;
    const uint32_t flags = o.field(UF::UNIT_FLAGS);
    c.inCombat = (flags & kInCombat) != 0;
    c.stunned = (flags & kStunned) != 0;
    c.standState = o.standState();
    c.loopAnim = (uint16_t)g.emotes.anim(o.emoteState());
    c.emoteAnim = (uint16_t)g.emotes.anim(o.emote);
    c.emoteMs = o.emoteMs;
    c.looting = self && g.lootWindow.open;
    uint32_t cls = 0, sub = 0;
    c.weapon = itemOf(g, o, 15, &cls, &sub) ? weaponFromSubclass(cls, sub) : CreatureWeapon::Unarmed;
    c.swingMs = o.swingMs;
    c.swingOff = o.swingOff;
    c.hitMs = o.hitMs;
    c.hitReact = o.hitReact;
    // casting: a channel (UNIT_CHANNEL_SPELL, at UNIT_FIELD_CHANNEL_OBJECT) or a spell with a cast time.
    // 6.36: at a game object (Opening a chest, gathering a herb, a quest object) the unit kneels: casting = 5
    auto isObject = [&](uint64_t guid) {
        const WorldObject* t = guid ? g.objects.find(guid) : nullptr;
        return t && t->type == ObjType::GameObject;
    };
    c.casting = 0;
    if (o.field(UF::UNIT_CHANNEL_SPELL)) {
        const uint64_t at = (uint64_t)o.field(UF::UNIT_FIELD_CHANNEL_OBJECT) | ((uint64_t)o.field(UF::UNIT_FIELD_CHANNEL_OBJECT + 1) << 32);
        c.casting = isObject(at) ? 5 : at && at != o.guid ? 3 : 4;
    } else {
        const CastState cs = g.castOf(o.guid, nowMs);
        if (cs.active && cs.durMs && nowMs - cs.startMs < cs.durMs + 300)
            c.casting = isObject(cs.target) ? 5 : cs.target && cs.target != o.guid ? 1 : 2;
    }
    c.castGoMs = o.castGoMs;
    c.castGoDirected = o.castGoDirected;
    c.castGoObject = o.castGoAtObject;
    c.castGoLook = CastLook::Spell;
    switch (o.castGoSpell) {
        case 75: case 3018: case 2480: case 7918: case 7919: {   // Auto Shot, Shoot (bow / gun / crossbow)
            uint32_t rc = 0, rs = 0;
            const bool have = itemOf(g, o, 17, &rc, &rs);
            c.castGoLook = have && rs == 3 ? CastLook::Rifle : CastLook::Bow;
            break;
        }
        case 2764: case 3137: case 10277:   // Throw
            c.castGoLook = CastLook::Thrown;
            break;
        default: break;
    }
    c.mountDisplay = o.field(UF::UNIT_MOUNTDISPLAYID);
    c.flying = o.taxiFlight();
    fillEquipment(g, o, c);
}

void fillGameObjectState(const WorldObject& o, CreatureSeen& c) {
    c.isGo = true;
    c.goState = (uint8_t)std::min<uint32_t>(o.goState(), 255);
    c.goStateMs = o.stateChangeMs;
    float q[4];
    c.tilted = o.goTilt(q);
    if (c.tilted) w3d_obj_rot_from_quat(q, c.rot);
}

bool fillCorpse(const WorldObject& o, CreatureSeen& c) {
    if (o.type != ObjType::Corpse || !o.hasPos || o.corpseBones()) return false;
    const uint32_t b1 = o.field(UF::CORPSE_FIELD_BYTES_1), b2 = o.field(UF::CORPSE_FIELD_BYTES_2);
    // vMaNGOS Corpse::Create: BYTES_1 = race << 8 | gender << 16 | skin << 24; BYTES_2 = face | hairstyle << 8 | haircolor << 16 | facial << 24
    W3dLook l{};
    l.race = (uint8_t)((b1 >> 8) & 0xFF);
    l.sex = (uint8_t)((b1 >> 16) & 0xFF);
    l.skin = (uint8_t)((b1 >> 24) & 0xFF);
    l.face = (uint8_t)(b2 & 0xFF);
    l.hairStyle = (uint8_t)((b2 >> 8) & 0xFF);
    l.hairColor = (uint8_t)((b2 >> 16) & 0xFF);
    l.facialHair = (uint8_t)((b2 >> 24) & 0xFF);
    if (!l.race || l.race > 8) return false;   // the eight races of 1.12
    c.lookParts = l;
    c.look = w3d_look_key(&c.lookParts);
    c.displayId = o.corpseDisplay();
    c.dead = true;
    const uint32_t flags = o.corpseFlags();
    for (uint32_t& e : c.equip) e = 0;
    for (uint8_t& e : c.equipInv) e = 0;
    for (int slot = 0; slot < W3D_EQ_SLOTS; ++slot) {   // slots 0..18 are the inventory slots, like PLAYER_VISIBLE_ITEM
        const uint32_t v = o.field((uint16_t)(UF::CORPSE_FIELD_ITEM + slot));
        if (!v) continue;
        if (slot == W3D_EQ_HEAD && (flags & 0x08)) continue;   // CORPSE_FLAG_HIDE_HELM
        if (slot == W3D_EQ_BACK && (flags & 0x10)) continue;   // CORPSE_FLAG_HIDE_CLOAK
        c.equip[slot] = v & 0x00FFFFFF;
        c.equipInv[slot] = (uint8_t)(v >> 24);
    }
    return true;
}
