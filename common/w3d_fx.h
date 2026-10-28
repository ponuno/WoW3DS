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
    uint16_t texCount, pad;
    uint32_t offEmitters, offGlows, offTexIds;   // from the start of the blob
} W3dFxHeader;                                    // 20 bytes

enum W3dFxEmitterType { W3D_FX_PLANE = 1, W3D_FX_SPHERE = 2 };
#define W3D_FXF_RANDOM_TILE 0x01u   // each particle a random cell of the texture atlas (else: cells along its life)
#define W3D_FXF_FIRE        0x02u   // a flame (lamp / torch / brazier / candle): kept dense, small and bright
#define W3D_FXF_LIT         0x04u   // follows the day light (smoke, dust); unlit particles glow on their own

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
