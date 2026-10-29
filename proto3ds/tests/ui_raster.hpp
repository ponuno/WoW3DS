// ui_raster.hpp — PC stand-in for rendererDrawUi (UI stage 1/7): draws a HudList into an RGBA canvas with the
// textures of ui::assets() (the built-in font, or ui.w3dp decoded from its GPU formats) and writes PNGs, so the
// tests look at the very quads the 3DS would draw.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "hud.hpp"

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    Canvas(int w_, int h_, uint32_t color = 0xFF000000u);
    void fill(uint32_t color);
    void backdrop();                                    // a sky / grass picture standing in for the 3D world
    void draw(const HudList& l);
    bool png(const std::string& path) const;
    void blit(const Canvas& c, int x, int y);          // copy another canvas in (sheets of screenshots)
};

// Decodes level 0 of a TEX blob (any W3D_TEX_* format) to RGBA8, row 0 = top.
bool decodeTexBlob(const std::vector<uint8_t>& blob, int* w, int* h, std::vector<uint8_t>& rgba);
// UI stage 8: a stand-in for a texture made at run time on the console (ui::kTexExternal0 + n: the portraits), RGBA rows.
void rasterSetExternal(uint16_t id, int w, int h, const std::vector<uint8_t>& rgba);
