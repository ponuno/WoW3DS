// w3d_sound.h — the sound files w3dconv --sound writes for the 3DS client (sdmc:/3ds/w3dproto/sound/).
// Nothing of the game is in the repository: the converter takes every sound from the user's own client.
//
//   sfx.w3ds        interface / event sounds: W3dSfxHeader, W3dSfxEntry[count], then PCM16 mono (little endian).
//                   key = w3dSndHash(event name, e.g. "click", "levelup"); an event can have several variants
//                   (entries with the same key), the client picks one at random.
//   music.txt       "A area parent zoneMusic ambience"      per AreaTable.dbc row with music or ambience
//                   "Z zoneMusic silMinMs silMaxMs daySound nightSound"   ZoneMusic.dbc
//                   "S sound volume*100 file1 file2 ..."     SoundEntries.dbc rows the above use (files under music/)
//                   "B sound volume*100 file1 ..."           ambience sounds (SoundAmbience.dbc day / night: "Y amb day night")
//                   "M map area"                             the area of maps without terrain (dungeons)
//   music/*.mp3     the client's MP3s as they are (decoded on the 3DS with minimp3)
//   music/*.ima     WAV sources as IMA ADPCM mono: W3dImaHeader, then blocks of kW3dImaBlock samples
//   world.w3ds      sounds of the world by SoundEntries id (creatures, weapons, spells, footsteps): W3dSfxHeader
//                   (magic W3D_WORLD_MAGIC), W3dWorldEntry[count] sorted by key, then IMA ADPCM blocks (as in .ima files,
//                   without the header); entries with the same key are the variants of one sound
//   sounds.txt      what makes which sound (all SoundEntries ids; columns found by w3dconv, see sound.cpp):
//                   "D display soundData"                       CreatureDisplayInfo -> CreatureSoundData
//                   "K soundData aggro death wound woundCrit attack attackCrit footstep"
//                   "W weaponSubclass impact crit"               WeaponImpactSounds (flesh)
//                   "F footstep sound"                          FootstepTerrainLookup (dirt) / "F 0 sound": any creature
//                   "Q visual precast cast impact missile channel"   SpellVisual -> SpellVisualKit sounds
//                   "P spell visual"
//   areas_<map>.bin the area id of every terrain chunk: W3dAreaHeader, u16 slot[64*64] (0xFFFF none),
//                   then per slot u16 area[16*16] (index chunkY*16 + chunkX, chunk axes as the ADT tile axes)
#pragma once
#include <stdint.h>

#define W3D_SFX_MAGIC 0x46533357u   /* "W3SF" */
#define W3D_IMA_MAGIC 0x41493357u   /* "W3IA" */
#define W3D_AREA_MAGIC 0x52413357u  /* "W3AR" */

typedef struct { uint32_t magic, version, count, reserved; } W3dSfxHeader;
typedef struct { uint32_t key, offset, samples; uint16_t rate, volume; } W3dSfxEntry;   // offset from file start; volume x1000

#define W3D_WORLD_MAGIC 0x57533357u /* "W3SW" */
typedef struct { uint32_t key, offset, samples; uint16_t rate, volume, minDist, cutoff; } W3dWorldEntry;   // dist in yd x10

typedef struct { uint32_t magic, rate, samples, blocks; } W3dImaHeader;
// One block: s16 predictor, u8 step index, u8 0 (the decoder state before the block), then kW3dImaBlock / 2 bytes,
// low nibble first: kW3dImaBlock samples, all of them from the nibbles.
enum { kW3dImaBlock = 2048, kW3dImaBlockBytes = 4 + kW3dImaBlock / 2 };

typedef struct { uint32_t magic, map, tiles, reserved; } W3dAreaHeader;

static inline uint32_t w3dSndHash(const char* s) {   // FNV-1a over the lower-case name
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        uint8_t c = (uint8_t)*s;
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}

// IMA ADPCM (the classic DVI tables), shared by the encoder and the 3DS decoder.
static const int16_t kW3dImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t kW3dImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static inline int16_t w3dImaStep(uint8_t nib, int* pred, int* index) {
    int step = kW3dImaStep[*index];
    int diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    int p = *pred + ((nib & 8) ? -diff : diff);
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    *pred = p;
    int i = *index + kW3dImaIndex[nib & 15];
    *index = i < 0 ? 0 : i > 88 ? 88 : i;
    return (int16_t)p;
}

// Decodes one block (kW3dImaBlockBytes) into out[kW3dImaBlock].
static inline void w3dImaDecodeBlock(const uint8_t* b, int16_t* out) {
    int pred = (int16_t)(b[0] | (b[1] << 8)), index = b[2] > 88 ? 88 : b[2];
    for (int k = 0; k < kW3dImaBlock / 2; ++k) {
        uint8_t v = b[4 + k];
        out[2 * k] = w3dImaStep(v & 15, &pred, &index);
        out[2 * k + 1] = w3dImaStep(v >> 4, &pred, &index);
    }
}
