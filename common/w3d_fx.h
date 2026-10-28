// w3d_fx.h — particle emitters and glow sprites of a static model (stage 5.32), shared by the converter and the 3DS.
//
// They ride at the END of the model's MODL blob, behind everything W3dModelHeader points at, found through an 8-byte
// footer {offset of W3dFxHeader, W3D_FX_MAGIC}: an older client reads the blob as before and never looks there, an older
// pack simply has no footer. Positions are in model space (the instance's T * R * S places them, as the mesh).
//
// Emitters are the M2 particle emitters of the doodad (torches, braziers, candles, chimneys, fountains) with their
// animated tracks reduced to one value each (doodads stand in their bind pose; the converter averages the keys).
// Glow sprites replace the M2's "glow cards" (flat additive quads with a glow / halo / flare texture that the original
// turns to the camera through a billboarded bone): the converter takes the card out of the mesh and keeps its centre,
// size and colour; the client draws a camera-facing soft sprite there (the light of lamps and lanterns).
#pragma once
#include <stdint.h>
#include <string.h>

#define W3D_FX_MAGIC 0x31584D46u   // 'FMX1'

typedef struct {
    uint16_t emitterCount, glowCount;
    uint16_t texCount;
    uint16_t animOff4;            // stage 5.35: the bone animation block (W3dFxAnim) at animOff4 * 4, 0 = none
    uint32_t offEmitters, offGlows, offTexIds;   // from the start of the blob
} W3dFxHeader;                                    // 20 bytes

enum W3dFxEmitterType { W3D_FX_PLANE = 1, W3D_FX_SPHERE = 2 };
#define W3D_FXF_RANDOM_TILE 0x01u   // each particle a random cell of the texture atlas (else: cells along its life)
#define W3D_FXF_FIRE        0x02u   // a flame (lamp / torch / brazier / candle): kept dense, small and bright
#define W3D_FXF_LIT         0x04u   // follows the day light (smoke, dust); unlit particles glow on their own
#define W3D_FXF_SPELL       0x08u   // a spell effect's emitter (stage 5.33): bigger allowed, no flame floor

typedef struct {
    float pos[3];                 // model space
    float rate;                   // particles per second
    float life;                   // seconds
    float speed, speedVar;        // yd/s, fraction (+-)
    float spreadV, spreadH;       // spread of the direction around model +Z (0 = straight up)
    float gravity;                // yd/s^2 down (negative: rises)
    float areaL, areaW;           // plane: emission rectangle (yd); sphere: radius in areaL
    float drag;                   // speed lost per second (fraction)
    float scale[3];               // size (yd) at birth, at mid, at death
    uint32_t color[3];            // RGBA8 (r in the low byte) at birth, mid, death
    float mid;                    // where "mid" is along the life, 0..1
    uint16_t tex;                 // index into the fx texture table, 0xFFFF = the built-in soft dot
    uint8_t blend;                // W3D_BLEND_ALPHA or W3D_BLEND_ADD (w3d_format.h)
    uint8_t type;                 // W3dFxEmitterType
    uint8_t rows, cols;           // texture atlas cells
    uint16_t flags;               // W3D_FXF_*
} W3dFxEmitter;                   // 88 bytes

typedef struct {
    float pos[3];                 // model space
    float size;                   // radius (yd)
    uint32_t color;               // RGBA8, a = strength
} W3dFxGlow;                      // 20 bytes

// The fx header of a model blob, or null (no footer / broken footer).
static inline const W3dFxHeader* w3d_model_fx(const uint8_t* blob, uint32_t size) {
    if (!blob || size < 8 + sizeof(W3dFxHeader)) return 0;
    uint32_t off, magic;
    memcpy(&off, blob + size - 8, 4);
    memcpy(&magic, blob + size - 4, 4);
    if (magic != W3D_FX_MAGIC || (off & 3) || off + sizeof(W3dFxHeader) > size - 8) return 0;
    const W3dFxHeader* h = (const W3dFxHeader*)(blob + off);
    const uint64_t e = (uint64_t)h->offEmitters + (uint64_t)h->emitterCount * sizeof(W3dFxEmitter);
    const uint64_t g = (uint64_t)h->offGlows + (uint64_t)h->glowCount * sizeof(W3dFxGlow);
    const uint64_t t = (uint64_t)h->offTexIds + (uint64_t)h->texCount * 4u;
    if ((h->emitterCount && e > off) || (h->glowCount && g > off) || (h->texCount && t > off)) return 0;
    if ((h->offEmitters | h->offGlows | h->offTexIds) & 3) return 0;
    return h;
}

// ---------------------------------------------------------------- bone animation of effect models (stage 5.35)
// Spell effect models turn, pulse and sway (a ring that spins, a shield that breathes). The M2's first looping animation is
// baked per bone (at most W3D_FXA_MAX_BONES bones that something hangs on, W3D_FXA_MAX_FRAMES samples over the loop) as
// 3x4 matrices in model space (bind pose -> animated pose, rows of [R*S | t]); every mesh part (submesh of the blob) rides
// its dominant bone rigidly, as do the emitters and glows. A model with no motion has no block.
// Layout from the block's start: W3dFxAnim, uint8 subBone[subCount], emitterBone[emitterCount], glowBone[glowCount]
// (each padded to 4 bytes), then float mat[frames][boneCount][12].
#define W3D_FXA_MAGIC 0x31415846u   // 'FXA1'
#define W3D_FXA_MAX_BONES 12
#define W3D_FXA_MAX_FRAMES 32
typedef struct {
    uint32_t magic;
    uint16_t boneCount, frames;
    uint32_t durationMs;          // one loop
    uint16_t subCount, emitterCount, glowCount;
    uint16_t flags;               // W3D_FXA_TINT (5.35j; 0 in 5.35 blocks)
} W3dFxAnim;                      // 16 bytes
// 5.35j: after the matrices, uint32 tint[frames][subCount] (RGBA8, r in the low byte): each part's colour and opacity
// over the loop (its transparency track times its colour's alpha). The client multiplies the part by it (additive
// parts by the opacity too: they fade by getting darker).
#define W3D_FXA_TINT 1u

typedef struct {
    const W3dFxAnim* a;
    const uint8_t* subBone;
    const uint8_t* emitterBone;
    const uint8_t* glowBone;
    const float* mat;             // [frames][boneCount][12]
    const uint32_t* tint;         // [frames][subCount] or null
} W3dFxAnimView;

// The animation block of a model with fx (validated against the blob size), or a view with a == null.
static inline W3dFxAnimView w3d_fx_anim(const uint8_t* blob, uint32_t size, const W3dFxHeader* fx) {
    W3dFxAnimView v = {0, 0, 0, 0, 0, 0};
    if (!blob || !fx || !fx->animOff4) return v;
    const uint64_t off = (uint64_t)fx->animOff4 * 4u;
    if (off + sizeof(W3dFxAnim) > size - 8u) return v;
    const W3dFxAnim* a = (const W3dFxAnim*)(blob + off);
    if (a->magic != W3D_FXA_MAGIC || !a->boneCount || a->boneCount > W3D_FXA_MAX_BONES || !a->frames ||
        a->frames > W3D_FXA_MAX_FRAMES || !a->durationMs)
        return v;
    const uint64_t t1 = (a->subCount + 3u) & ~3u, t2 = (a->emitterCount + 3u) & ~3u, t3 = (a->glowCount + 3u) & ~3u;
    const uint64_t offMat = off + sizeof(W3dFxAnim) + t1 + t2 + t3;
    const uint64_t endMat = offMat + (uint64_t)a->frames * a->boneCount * 12u * 4u;
    const uint64_t end = endMat + ((a->flags & W3D_FXA_TINT) ? (uint64_t)a->frames * a->subCount * 4u : 0u);
    if (end > size - 8u || (offMat & 3)) return v;
    v.a = a;
    v.subBone = blob + off + sizeof(W3dFxAnim);
    v.emitterBone = v.subBone + t1;
    v.glowBone = v.emitterBone + t2;
    v.mat = (const float*)(blob + offMat);
    if (a->flags & W3D_FXA_TINT) v.tint = (const uint32_t*)(blob + endMat);
    return v;
}

// The bone's 3x4 matrix (rows [R*S | t]) at ageMs into the loop, blended between the two nearest samples.
static inline void w3d_fx_anim_matrix(const W3dFxAnimView* v, int bone, uint32_t ageMs, float out[12]) {
    static const float kId[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (!v->a || bone < 0 || bone >= v->a->boneCount) { memcpy(out, kId, sizeof kId); return; }
    const uint32_t d = v->a->durationMs, F = v->a->frames;
    const float pos = (float)(ageMs % d) / (float)d * (float)F;
    uint32_t i0 = (uint32_t)pos;
    if (i0 >= F) i0 = F - 1;
    const uint32_t i1 = (i0 + 1) % F;
    const float f = pos - (float)i0;
    const float* m0 = v->mat + ((size_t)i0 * v->a->boneCount + (size_t)bone) * 12;
    const float* m1 = v->mat + ((size_t)i1 * v->a->boneCount + (size_t)bone) * 12;
    for (int k = 0; k < 12; ++k) out[k] = m0[k] + (m1[k] - m0[k]) * f;
}

// A part's tint (r, g, b, a in 0..1) at ageMs into the loop; white when the block has none.
static inline void w3d_fx_anim_tint(const W3dFxAnimView* v, int sub, uint32_t ageMs, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 1.0f;
    if (!v->a || !v->tint || sub < 0 || sub >= v->a->subCount) return;
    const uint32_t d = v->a->durationMs, F = v->a->frames;
    const float pos = (float)(ageMs % d) / (float)d * (float)F;
    uint32_t i0 = (uint32_t)pos;
    if (i0 >= F) i0 = F - 1;
    const uint32_t i1 = (i0 + 1) % F;
    const float f = pos - (float)i0;
    const uint32_t c0 = v->tint[(size_t)i0 * v->a->subCount + (size_t)sub], c1 = v->tint[(size_t)i1 * v->a->subCount + (size_t)sub];
    for (int k = 0; k < 4; ++k) {
        const float a = (float)((c0 >> (8 * k)) & 255) / 255.0f, b = (float)((c1 >> (8 * k)) & 255) / 255.0f;
        out[k] = a + (b - a) * f;
    }
}

// ---------------------------------------------------------------- spell visuals (stage 5.33, 5.34)
// One SVFX entry (id 0) in the pool: what a spell shows, from Spell.dbc -> SpellVisual.dbc -> SpellVisualKit.dbc ->
// SpellVisualEffectName.dbc. Each visual has six stages - precast (while casting), cast (when it goes off), impact (on
// each target), missile (flying from the caster to each target), state (looping on a unit while the aura is on it) and
// channel (looping on the caster while it channels) - with up to two effect models each, hung on a point of the unit.
// The models are ordinary MODL entries (static mesh + particles + glows at the end of the blob). Two more effects are not
// spells: the level-up flash (Spells\LevelUp\LevelUp.m2) and the sparkle of a lootable corpse (Particles\LootFX.m2).
#define W3D_SVFX_MAGIC 0x32465653u   // 'SVF2' (stage 5.34; 'SVFX' of 5.33 had four stages and is not read)
enum W3dSpellStage { W3D_SFX_PRECAST = 0, W3D_SFX_CAST = 1, W3D_SFX_IMPACT = 2, W3D_SFX_MISSILE = 3, W3D_SFX_STATE = 4,
                     W3D_SFX_CHANNEL = 5, W3D_SFX_STAGES = 6 };
enum W3dSpellSlot {               // where on the unit (SpellVisualKit's effect columns)
    W3D_SLOT_NONE = 0, W3D_SLOT_HEAD = 1, W3D_SLOT_CHEST = 2, W3D_SLOT_BASE = 3, W3D_SLOT_LHAND = 4, W3D_SLOT_RHAND = 5
};
#define W3D_SFX_NONE 0xFFFFu
typedef struct {
    uint32_t magic;               // W3D_SVFX_MAGIC
    uint32_t effectCount, visualCount, spellCount;
    uint32_t offEffects, offVisuals, offSpells;   // from the start of the entry
    uint16_t levelUp, loot;       // effect indices of the level-up flash and the loot sparkle (W3D_SFX_NONE: none)
} W3dSpellFxHeader;               // 32 bytes
typedef struct {
    uint32_t model;               // MODL id
    float scale;                  // SpellVisualEffectName scale (1 when absent)
} W3dSpellEffect;                 // 8 bytes
typedef struct {
    uint32_t id;                  // SpellVisual id
    uint16_t effect[W3D_SFX_STAGES][2];   // index into the effect table per stage, W3D_SFX_NONE = none
    uint8_t slot[W3D_SFX_STAGES][2];      // W3dSpellSlot of each
} W3dSpellVisual;                 // 40 bytes, sorted by id
typedef struct {
    uint32_t spell;               // Spell.dbc id
    uint16_t visual;              // index into the visual table
    uint16_t pad;
    float speed;                  // missile speed, yd/s (0 = not known: the client takes 20)
} W3dSpellRef;                    // 12 bytes, sorted by spell
