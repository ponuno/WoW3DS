// ui_assets.hpp — the interface pack (ui.w3dp, UI stage 7) on the 3DS and in the PC tests: fonts, skin sprites,
// icon atlases and maps, looked up by name / spell / item display; the texture data itself is read on demand
// (render.cpp keeps the GPU copies, the PC rasteriser decodes them). Without a pack the built-in stub skin is used:
// the fonts of ui_font_builtin.h (Latin + Cyrillic) and no sprites, icons or maps (callers draw flat frames).
//
// Texture ids in HudQuad.tex: 0 = none (flat colour), 1 = the built-in font atlas, 2 + n = texture n of the pack.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "w3d_ui.h"

namespace ui {

enum class Font : uint8_t { Small = 0, Normal = 1, Large = 2 };
constexpr uint16_t kTexNone = 0, kTexBuiltinFont = 1, kTexPack0 = 2;

// A picture in a texture: source rectangle in texels, 9-slice margins.
struct Image {
    uint16_t tex = kTexNone;
    uint16_t x = 0, y = 0, w = 0, h = 0;
    uint8_t l = 0, t = 0, r = 0, b = 0;
    bool ok() const { return tex != kTexNone; }
};

struct FontView {
    uint16_t tex = kTexBuiltinFont;
    int pixelSize = 9, lineHeight = 12, ascent = 9;
    const W3dUiGlyph* glyphs = nullptr;
    uint32_t count = 0;
    const W3dUiGlyph* find(uint32_t cp) const;   // nullptr when missing
};

struct MapView {
    const W3dUiMap* m = nullptr;
    Image image;                                  // the picture (w x h)
    std::string name;
    bool ok() const { return m != nullptr; }
    // world -> 0..1 over the picture; false when outside
    bool toUv(float x, float y, float* u, float* v) const;
};

class Assets {
public:
    // Opens ui.w3dp (the tables; textures stay in the file). False (and the stub skin) when missing / broken.
    bool open(const char* path, char* err = nullptr, size_t errLen = 0);
    void close();
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    const FontView& font(Font f) const { return fonts_[(int)f]; }
    Image sprite(const char* name) const;
    Image icon(uint32_t key) const;
    Image spellIcon(uint32_t spell) const { return icon(W3D_UI_KEY(W3D_UIKEY_SPELL, spell)); }
    Image itemIcon(uint32_t displayId) const { return displayId ? icon(W3D_UI_KEY(W3D_UIKEY_ITEM, displayId)) : Image(); }
    Image namedIcon(const char* name) const { return icon(W3D_UI_KEY(W3D_UIKEY_NAMED, w3dUiHash(name))); }

    // The smallest zone map of 'mapId' that contains (x, y); the continent map when no zone does.
    MapView zoneAt(uint32_t mapId, float x, float y) const;
    MapView continent(uint32_t mapId) const;
    size_t mapCount() const { return maps_.size(); }
    // The in-game loading screen of a map (the picture 512x256, stretch it); !ok() when the pack has none.
    Image loadingScreen(uint32_t mapId) const;

    // Textures: size / format / kind of an id (built-in font included); the raw TEX blob of a pack texture.
    bool texInfo(uint16_t id, int* w, int* h, uint8_t* format, uint8_t* kind) const;
    size_t packTextures() const { return tex_.size(); }
    bool readTexture(uint16_t id, std::vector<uint8_t>& blob) const;
    // The built-in font atlas as 8-bit alpha, row-major (w x h).
    static void builtinFontA8(std::vector<uint8_t>& out, int* w, int* h);

    // statistics for the debug page
    size_t iconCount() const { return icons_.size(); }
    size_t spriteCount() const { return sprites_.size(); }

private:
    MapView view(const W3dUiMap& m) const;
    bool loaded_ = false;
    std::string path_;
    std::vector<uint8_t> dir_;               // the UIDR blob
    W3dUiDir hdr_{};
    std::vector<W3dUiTex> tex_;
    struct TexLoc { uint32_t offset = 0, size = 0; };
    std::vector<TexLoc> texLoc_;
    std::vector<W3dUiSprite> sprites_;
    std::vector<W3dUiIcon> icons_;
    std::vector<W3dUiMap> maps_;
    std::vector<W3dUiLoading> loading_;
    std::vector<W3dUiGlyph> glyphs_;
    std::string strings_;
    FontView fonts_[3];
    void builtinFonts();
};

Assets& assets();

// UTF-8 -> code points (invalid bytes become U+FFFD); advances i.
uint32_t nextCodepoint(const std::string& s, size_t& i);

}  // namespace ui
