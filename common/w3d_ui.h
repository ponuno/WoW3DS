// w3d_ui.h — the interface pack ui.w3dp (UI stage 7): textures and tables for the two-screen game UI.
//
// A normal W3DP pack (w3d_format.h container) with two entry types:
//   UIDR id 0     the directory below (all tables, CPU memory)
//   UITX id = n   texture n of the directory: a TEX blob (W3dTexHeader + data), one mip level
// The converter makes it from the user's own client (Interface\Icons, Tooltips, DialogFrame, Buttons, PaperDoll,
// GossipFrame, MoneyFrame, WorldMap, Fonts + the DBCs that name them). Nothing of it is in the repository: without
// ui.w3dp the 3DS draws its own stub skin (flat frames, built-in font, initials instead of icons).
//
// Icons: square cells of iconCell pixels (iconSize of picture + an edge-replicated border) in pages of
// iconsPerRow x iconsPerRow cells; cell c of a page is at ((c % iconsPerRow) * iconCell, (c / iconsPerRow) * iconCell).
// Icon keys: W3D_UI_KEY(kind, id). Sprite keys (skin) and named icons: w3dUiHash(name) (FNV-1a, lower case).
// Maps: zone and continent maps of WorldMapArea.dbc, the picture in the top-left w x h of their texture;
// world (x, y) -> map u = (left - y) / (left - right), v = (top - x) / (top - bottom).
#pragma once
#include <stdint.h>

#include "w3d_format.h"

#define W3D_ENTRY_UI_DIR W3D_FOURCC('U', 'I', 'D', 'R')
#define W3D_ENTRY_UI_TEX W3D_FOURCC('U', 'I', 'T', 'X')
#define W3DU_MAGIC   W3D_FOURCC('W', '3', 'D', 'U')
#define W3DU_VERSION 1u

// Extra texture format for fonts (W3dTexFormat continues): 8-bit alpha, Morton tiles like the others.
#define W3D_TEX_A8 5

enum W3dUiTexKind { W3D_UITEX_SKIN = 0, W3D_UITEX_FONT = 1, W3D_UITEX_ICONS = 2, W3D_UITEX_MAP = 3 };
enum W3dUiKeyKind { W3D_UIKEY_SPELL = 1, W3D_UIKEY_ITEM = 2, W3D_UIKEY_NAMED = 3 };
#define W3D_UI_KEY(kind, id) (((uint32_t)(kind) << 24) | ((uint32_t)(id) & 0xFFFFFFu))

typedef struct {
    uint32_t magic, version;
    uint32_t texCount, offTex;          // W3dUiTex[texCount]; texture n = entry UITX id n
    uint32_t fontCount, offFonts;       // W3dUiFont[fontCount] (index = size class: 0 small, 1 normal, 2 large)
    uint32_t glyphCount, offGlyphs;     // W3dUiGlyph[], each font's run sorted by code point
    uint32_t spriteCount, offSprites;   // W3dUiSprite[], sorted by key
    uint32_t iconCount, offIcons;       // W3dUiIcon[], sorted by key
    uint32_t mapCount, offMaps;         // W3dUiMap[]
    uint32_t strSize, offStrings;       // zero-terminated UTF-8 strings (map names)
    uint16_t iconCell, iconSize, iconsPerRow, iconPages;
    uint32_t loadCount, offLoading;     // W3dUiLoading[] (loading screens per map; 0 in older packs)
    uint32_t reserved[2];
} W3dUiDir;

typedef struct {
    uint16_t w, h;                      // texture size (power of two)
    uint8_t format;                     // W3dTexFormat / W3D_TEX_A8
    uint8_t kind;                       // W3dUiTexKind
    uint16_t pad;
} W3dUiTex;                             // 8 bytes

typedef struct {
    uint16_t tex;                       // texture index (kind FONT)
    uint8_t pixelSize, lineHeight;      // em size, distance between lines
    int8_t ascent;                      // baseline below the line top
    uint8_t pad[3];
    uint32_t firstGlyph, glyphCount;
} W3dUiFont;                            // 16 bytes

typedef struct {
    uint32_t cp;                        // Unicode code point
    uint16_t x, y;                      // in the font texture
    uint8_t w, h;                       // 0 x 0 for a space
    int8_t xoff, yoff;                  // from the pen position / the line top
    uint8_t advance;
    uint8_t pad[3];
} W3dUiGlyph;                           // 16 bytes

typedef struct {
    uint32_t key;                       // w3dUiHash(name)
    uint16_t tex, x, y, w, h;           // the picture in texture tex
    uint8_t l, t, r, b;                 // 9-slice margins (0 = stretch the whole picture)
    uint16_t pad;
} W3dUiSprite;                          // 20 bytes

typedef struct {
    uint32_t key;                       // W3D_UI_KEY(kind, id)
    uint16_t page, cell;                // page = texture index of the icon page
} W3dUiIcon;                            // 8 bytes

typedef struct {
    uint32_t id, mapId, areaId;         // WorldMapArea ID, map, AreaTable id (0 = a continent)
    float left, right, top, bottom;     // world bounds: left/right are y, top/bottom are x
    uint16_t tex, w, h, pad;            // picture: the top-left w x h of texture tex
    uint32_t nameOff;                   // into strings (the WorldMapArea name, e.g. "Elwynn")
} W3dUiMap;                             // 40 bytes

// The in-game loading screen of a map (Map.dbc -> LoadingScreens.dbc): the picture is the top-left w x h of texture tex;
// several maps may share one texture.
typedef struct {
    uint32_t mapId;
    uint16_t tex, w, h, pad;
} W3dUiLoading;                         // 12 bytes

// FNV-1a over the lower-case name (ASCII), the key of sprites and named icons.
static inline uint32_t w3dUiHash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}
