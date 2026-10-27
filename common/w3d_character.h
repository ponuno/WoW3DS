// w3d_character.h — player appearance (stage 5.5), shared by the 3DS client, the converter
// (humanoid geosets of NPCs use the same rule) and the PC tests.
//
//   * w3d_char_resolve: PLAYER_BYTES look -> CharSections layers + hair texture + geosets
//     (the rules of WoWee pipeline::resolveCharacterSections, with fallbacks for rows the
//     table does not carry);
//   * w3d_humanoid_geosets: which submeshes of a character model to draw without equipment
//     (WoWee EntitySpawner::normalizeHumanoidClothingGeosets);
//   * w3d_char_composite: body texture = base skin + overlays (alpha), RGB565 in PICA tile order.
#pragma once
#include <stdint.h>
#include <string.h>

#include "w3d_format.h"

typedef struct {
    uint8_t race, sex;                       // UNIT_FIELD_BYTES_0 bytes 0 and 2
    uint8_t skin, face, hairStyle, hairColor;   // PLAYER_BYTES bytes 0..3
    uint8_t facialHair;                      // PLAYER_BYTES_2 byte 0
} W3dLook;

static inline uint64_t w3d_look_key(const W3dLook* l) {
    return (1ull << 63) | ((uint64_t)l->race << 48) | ((uint64_t)(l->sex & 1) << 40) | ((uint64_t)l->skin << 32) |
           ((uint64_t)l->face << 24) | ((uint64_t)l->hairStyle << 16) | ((uint64_t)l->hairColor << 8) | l->facialHair;
}

static inline W3dLook w3d_look_from_fields(uint32_t unitBytes0, uint32_t playerBytes, uint32_t playerBytes2) {
    W3dLook l;
    l.race = (uint8_t)(unitBytes0 & 0xFF);
    l.sex = (uint8_t)((unitBytes0 >> 16) & 0xFF);
    l.skin = (uint8_t)(playerBytes & 0xFF);
    l.face = (uint8_t)((playerBytes >> 8) & 0xFF);
    l.hairStyle = (uint8_t)((playerBytes >> 16) & 0xFF);
    l.hairColor = (uint8_t)((playerBytes >> 24) & 0xFF);
    l.facialHair = (uint8_t)(playerBytes2 & 0xFF);
    return l;
}

typedef struct {
    uint32_t layers[7];   // CIMG ids in drawing order: base, pelvis, torso, face lower, face upper,
                          // facial hair lower, facial hair upper (0 = none)
    uint32_t hairTex;     // TEX id (0 = none)
    uint16_t scalp, f100, f200, f300;   // geoset choices for w3d_humanoid_geosets
    int exactFace, haveSkin, haveHair;
} W3dCharResolved;

static inline const W3dCharHeader* w3d_char(const uint8_t* blob) { return (const W3dCharHeader*)blob; }

// Bounds checks for entries read from the pack (the 3DS trusts nothing it did not check).
static inline int w3d_char_valid(const uint8_t* blob, uint32_t size) {
    if (size < sizeof(W3dCharHeader)) return 0;
    const W3dCharHeader* h = w3d_char(blob);
    return h->sectionCount < 4096 && h->styleCount < 4096 &&
           (uint64_t)h->offSections + (uint64_t)h->sectionCount * sizeof(W3dCharSection) <= size &&
           (uint64_t)h->offStyles + (uint64_t)h->styleCount * sizeof(W3dCharStyle) <= size;
}
static inline int w3d_charimg_valid(const uint8_t* blob, uint32_t size) {
    if (size < sizeof(W3dCharImage)) return 0;
    const W3dCharImage* im = (const W3dCharImage*)blob;
    return sizeof(W3dCharImage) + (uint64_t)im->w * im->h * 2 <= size;
}

static inline void w3d_char_resolve(const uint8_t* chrs, const W3dLook* look, W3dCharResolved* r) {
    const W3dCharHeader* h = w3d_char(chrs);
    const W3dCharSection* sec = (const W3dCharSection*)(chrs + h->offSections);
    const W3dCharStyle* sty = (const W3dCharStyle*)(chrs + h->offStyles);
    memset(r, 0, sizeof *r);
    const W3dCharSection *skin = 0, *skinAny = 0, *face = 0, *faceAlt = 0, *hair = 0, *hairStyleAny = 0, *hairAny = 0;
    const W3dCharSection *under = 0, *underAny = 0, *facial = 0, *facialAnyColor = 0;
    for (uint32_t i = 0; i < h->sectionCount; ++i) {
        const W3dCharSection* s = &sec[i];
        switch (s->kind) {
            case W3D_CSEC_SKIN:
                if (!skinAny) skinAny = s;
                if (!skin && s->color == look->skin) skin = s;
                break;
            case W3D_CSEC_FACE:
                if (!face && s->variation == look->face && s->color == look->skin) face = s;
                // the same face in another skin colour, or failing that any face in the right colour
                if (!faceAlt && (s->variation == look->face || s->color == look->skin)) faceAlt = s;
                break;
            case W3D_CSEC_HAIR:
                if (!hairAny) hairAny = s;
                if (!hairStyleAny && s->variation == look->hairStyle) hairStyleAny = s;
                if (!hair && s->variation == look->hairStyle && s->color == look->hairColor) hair = s;
                break;
            case W3D_CSEC_UNDERWEAR:
                if (!underAny) underAny = s;
                if (!under && s->color == look->skin) under = s;
                break;
            case W3D_CSEC_FACIAL_HAIR:
                if (!facialAnyColor && s->variation == look->facialHair) facialAnyColor = s;
                if (!facial && s->variation == look->facialHair && s->color == look->hairColor) facial = s;
                break;
        }
    }
    r->haveSkin = skin != 0;
    if (!skin) skin = skinAny;
    r->exactFace = face != 0;
    if (!face) face = faceAlt;
    r->haveHair = hair != 0;
    if (!hair) hair = hairStyleAny ? hairStyleAny : hairAny;
    if (!under) under = underAny;
    if (!facial) facial = facialAnyColor;
    if (skin) r->layers[0] = skin->img[0];
    if (under) { r->layers[1] = under->img[0]; r->layers[2] = under->img[1]; }
    if (face) { r->layers[3] = face->img[0]; r->layers[4] = face->img[1]; }
    if (facial) { r->layers[5] = facial->img[0]; r->layers[6] = facial->img[1]; }
    if (hair) r->hairTex = hair->img[0];
    r->scalp = 1;
    r->f100 = r->f200 = r->f300 = 0;   // no style row: no facial hair geometry (WoWee bareCharacterGeosets)
    for (uint32_t i = 0; i < h->styleCount; ++i) {
        const W3dCharStyle* s = &sty[i];
        if (s->kind == 0 && s->variation == look->hairStyle && s->g[0] > 0) r->scalp = s->g[0];
        if (s->kind == 1 && s->variation == look->facialHair) {
            r->f100 = s->g[0] ? (uint16_t)(100 + s->g[0]) : 0;   // variant 0 = none of that feature
            r->f200 = s->g[1] ? (uint16_t)(200 + s->g[1]) : 0;
            r->f300 = s->g[2] ? (uint16_t)(300 + s->g[2]) : 0;
        }
    }
}

// ---------------------------------------------------------------- geosets
static inline int w3d_geoset_has(const uint16_t* a, int n, uint16_t g) {
    for (int i = 0; i < n; ++i) if (a[i] == g) return 1;
    return 0;
}

// The variant 'want' if the model has it; "none" variants (x00) -> 0; else the lowest of its group.
static inline uint16_t w3d_geoset_resolve(const uint16_t* all, int n, uint16_t want) {
    if (w3d_geoset_has(all, n, want)) return want;
    if (want % 100 == 0) return 0;
    uint16_t lowest = 0;
    for (int i = 0; i < n; ++i)
        if (all[i] / 100 == want / 100 && (lowest == 0 || all[i] < lowest)) lowest = all[i];
    return lowest;
}

// Equipment that changes geometry (ItemDisplayInfo GeosetGroup values: 0 = slot empty / no change).
typedef struct {
    uint8_t sleeves;   // chest/shirt GeosetGroup1 -> group 8 (bracers when the chest has none)
    uint8_t robe;      // chest GeosetGroup3 -> group 13 kilt (wins over trousers)
    uint8_t legs;      // legs GeosetGroup1 -> group 13
    uint8_t boots;     // feet GeosetGroup1 -> group 5
    uint8_t gloves;    // hands GeosetGroup1 -> group 4
    uint8_t belt;      // waist GeosetGroup1 -> group 18
    uint8_t tabard;    // 1 = a tabard is worn -> 1201
    uint8_t cape;      // 0 = none, else 2 + cloak GeosetGroup1 -> group 15 (1502 + gg1; 1501 is the no-cloak panel)
} W3dEquipGeo;

// f100/f200/f300: facial hair variants (1xx/2xx/3xx ids), 0 = none of that feature.
// Submeshes to draw for a character model: an explicit set like WoWee bareCharacterGeosets +
// its equipment pass (entity_spawner_player), never "everything of a group" - character models
// carry every clothing variant at once (bare legs, robe skirts, kneepads, capes), and drawing
// more than one per group gives missing thighs or skirt planks. all = the model's submesh ids.
// eq may be null (no equipment). Writes sorted unique ids.
static inline int w3d_humanoid_geosets(const uint16_t* all, int n, uint32_t race, uint16_t scalp, uint16_t f100,
                                       uint16_t f200, uint16_t f300, const W3dEquipGeo* eq, uint16_t* out, int cap) {
    W3dEquipGeo none;
    memset(&none, 0, sizeof none);
    if (!eq) eq = &none;
    int k = 0;
    // pick: 'want' if the model has it, else 'fallback' if it has that, else 0
#define W3D_PICK(want, fallback) (w3d_geoset_has(all, n, (uint16_t)(want)) ? (uint16_t)(want) \
                                  : w3d_geoset_has(all, n, (uint16_t)(fallback)) ? (uint16_t)(fallback) : (uint16_t)0)
    uint16_t lowest[21];
    for (int g = 0; g < 21; ++g) lowest[g] = 0;
    for (int i = 0; i < n; ++i) {
        int g = all[i] / 100;
        if (g < 21 && all[i] % 100 != 0 && (lowest[g] == 0 || all[i] < lowest[g])) lowest[g] = all[i];
    }
    uint16_t pick[24];
    int np = 0;
    pick[np++] = scalp ? W3D_PICK(scalp, 1) : W3D_PICK(1, 1);                                  // hair / scalp
    {   // facial hair: the style's variant, else the default connector, else the group's lowest
        // 0 = the character has none of that feature (WoWee addFacialHairGeosets): nothing is added.
        if (f100) pick[np++] = W3D_PICK(f100, 101);
        if (f200) pick[np++] = W3D_PICK(f200, 201);
        if (f300) pick[np++] = W3D_PICK(f300, 301);
    }
    pick[np++] = W3D_PICK(401 + eq->gloves, 401);                                             // forearms / gloves
    {
        uint16_t b = W3D_PICK(501 + eq->boots, 501);                                          // shins / boots
        pick[np++] = b ? b : lowest[5];                                                        // gnome, tauren
    }
    {
        uint16_t e = W3D_PICK(702, 702);                                                       // ears
        pick[np++] = e;
    }
    pick[np++] = W3D_PICK(801 + eq->sleeves, 801);                                            // wrists / sleeves
    pick[np++] = W3D_PICK(902, 902);                                                           // kneepads
    {   // legs: trousers, a robe's kilt over them
        uint16_t p = W3D_PICK(1301 + eq->legs, 1301);
        if (eq->robe) p = W3D_PICK(1301 + eq->robe, p ? p : 1301);
        pick[np++] = p;
    }
    if (eq->tabard) pick[np++] = W3D_PICK(1201, 1201);
    pick[np++] = eq->cape ? W3D_PICK(1501 + (eq->cape - 1), 1502) : W3D_PICK(1501, 1501);   // cloak / no-cloak panel
    pick[np++] = W3D_PICK(1801 + eq->belt, 1801);                                             // belt (HD models only)
    pick[np++] = W3D_PICK(2001, 2001);                                                         // split feet (HD models)
    pick[np++] = W3D_PICK(2002, 2002);
    if (race == 4) pick[np++] = W3D_PICK(1701, 1701);                                         // night elf eye glow
#undef W3D_PICK
    if (w3d_geoset_has(all, n, 0) && k < cap) out[k++] = 0;   // the body
    for (int i = 0; i < np; ++i) {
        uint16_t g = pick[i];
        if (g == 0 || k >= cap || w3d_geoset_has(out, k, g)) continue;
        out[k++] = g;
    }
    for (int i = 1; i < k; ++i)   // insertion sort (a few dozen ids)
        for (int j = i; j > 0 && out[j - 1] > out[j]; --j) {
            uint16_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return k;
}

// ---------------------------------------------------------------- body texture
static inline uint32_t w3d_morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// Draws the layers (CIMG blobs, first = base, opaque) into rgb (W3D_CHAR_ATLAS^2 * 3 bytes of
// scratch) and writes the RGB565 texture in PICA order (8x8 tiles left to right, top to
// bottom, Morton order inside a tile; image row 0 = top, like the converter's TEX entries).
// Missing layers may be null. Pixels outside the atlas are clipped.
static inline void w3d_char_composite(const uint8_t* const* layers, int n, uint8_t* rgb, uint16_t* out565) {
    const int A = W3D_CHAR_ATLAS;
    memset(rgb, 0x80, (size_t)A * A * 3);   // grey where no layer covers (no base skin found)
    for (int li = 0; li < n; ++li) {
        if (!layers[li]) continue;
        const W3dCharImage* im = (const W3dCharImage*)layers[li];
        const uint16_t* px = (const uint16_t*)(layers[li] + sizeof(W3dCharImage));
        for (int y = 0; y < im->h; ++y) {
            int ay = im->y + y;
            if (ay < 0 || ay >= A) continue;
            for (int x = 0; x < im->w; ++x) {
                int ax = im->x + x;
                if (ax < 0 || ax >= A) continue;
                uint16_t v = px[y * im->w + x];
                uint32_t r = ((v >> 12) & 15) * 17, g = ((v >> 8) & 15) * 17, b = ((v >> 4) & 15) * 17, a = (v & 15) * 17;
                if (li == 0) a = 255;
                uint8_t* o = &rgb[((size_t)ay * A + ax) * 3];
                o[0] = (uint8_t)((r * a + o[0] * (255 - a) + 127) / 255);
                o[1] = (uint8_t)((g * a + o[1] * (255 - a) + 127) / 255);
                o[2] = (uint8_t)((b * a + o[2] * (255 - a) + 127) / 255);
            }
        }
    }
    for (int y = 0; y < A; ++y)
        for (int x = 0; x < A; ++x) {
            const uint8_t* p = &rgb[((size_t)y * A + x) * 3];
            uint16_t v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
            uint32_t tile = (uint32_t)(y >> 3) * (A >> 3) + (uint32_t)(x >> 3);
            out565[tile * 64 + w3d_morton8((uint32_t)x & 7, (uint32_t)y & 7)] = v;
        }
}


// ---------------------------------------------------------------- worn items (stage 5.19)
// Slots whose item changes the body itself - its texture layers, geosets, hair or cloak. Weapons, shields, shoulders
// and the helmet model hang on attachment points and do not change the body display.
#define W3D_BODY_EQ 10
static const uint8_t W3D_BODY_EQ_SLOTS[W3D_BODY_EQ] = {W3D_EQ_HEAD, W3D_EQ_SHIRT, W3D_EQ_CHEST, W3D_EQ_WAIST, W3D_EQ_LEGS,
                                                        W3D_EQ_FEET, W3D_EQ_WRISTS, W3D_EQ_HANDS, W3D_EQ_BACK, W3D_EQ_TABARD};

// Key of the body-relevant part of equipment[W3D_EQ_SLOTS] (ItemDisplayInfo ids, 0 = empty); 0 = nothing worn there.
static inline uint64_t w3d_equip_key(const uint32_t* equip) {
    uint64_t h = 1469598103934665603ull;
    int any = 0;
    for (int i = 0; i < W3D_BODY_EQ; ++i) {
        uint32_t v = equip[W3D_BODY_EQ_SLOTS[i]];
        any |= v != 0;
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    }
    return any ? h : 0;
}

// Order the slots' texture layers are painted over the skin (later = on top).
static const uint8_t W3D_LAYER_SLOTS[8] = {W3D_EQ_SHIRT, W3D_EQ_LEGS, W3D_EQ_FEET, W3D_EQ_CHEST, W3D_EQ_WAIST, W3D_EQ_WRISTS,
                                           W3D_EQ_HANDS, W3D_EQ_TABARD};
#define W3D_MAX_EQ_LAYERS (8 * W3D_ITEM_REGIONS)

typedef struct {
    W3dEquipGeo geo;
    int hideHair;                       // a helmet that takes the hair off
    uint32_t capeTex;                   // TEX id of the cloak (0 = none)
    uint32_t layers[W3D_MAX_EQ_LAYERS]; // CIMG ids to paint after the skin layers, in order
    int layerCount;
} W3dEquipResolved;

// items[slot] = the slot's ItemDisplay (null = empty or not in the pack).
static inline void w3d_equip_resolve(const W3dItemDisplay* const* items, int sex, W3dEquipResolved* r) {
    memset(r, 0, sizeof *r);
    sex &= 1;
    const W3dItemDisplay* chest = items[W3D_EQ_CHEST];
    const W3dItemDisplay* shirt = items[W3D_EQ_SHIRT];
    const W3dItemDisplay* wrists = items[W3D_EQ_WRISTS];
    r->geo.sleeves = chest && chest->gg[0] ? chest->gg[0] : shirt && shirt->gg[0] ? shirt->gg[0] : wrists ? wrists->gg[0] : 0;
    r->geo.robe = chest ? chest->gg[2] : 0;
    r->geo.legs = items[W3D_EQ_LEGS] ? items[W3D_EQ_LEGS]->gg[0] : 0;
    r->geo.boots = items[W3D_EQ_FEET] ? items[W3D_EQ_FEET]->gg[0] : 0;
    r->geo.gloves = items[W3D_EQ_HANDS] ? items[W3D_EQ_HANDS]->gg[0] : 0;
    r->geo.belt = items[W3D_EQ_WAIST] ? items[W3D_EQ_WAIST]->gg[0] : 0;
    r->geo.tabard = items[W3D_EQ_TABARD] ? 1 : 0;
    if (items[W3D_EQ_BACK] && items[W3D_EQ_BACK]->cape[sex]) {
        r->capeTex = items[W3D_EQ_BACK]->cape[sex];
        r->geo.cape = (uint8_t)(2 + items[W3D_EQ_BACK]->gg[0]);
    }
    r->hideHair = items[W3D_EQ_HEAD] ? (items[W3D_EQ_HEAD]->hideHair >> sex) & 1 : 0;
    for (int s = 0; s < 8; ++s) {
        const W3dItemDisplay* it = items[W3D_LAYER_SLOTS[s]];
        if (!it) continue;
        for (int reg = 0; reg < W3D_ITEM_REGIONS; ++reg)
            if (it->img[sex][reg]) r->layers[r->layerCount++] = it->img[sex][reg];
    }
}
