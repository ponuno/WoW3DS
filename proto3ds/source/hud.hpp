// hud.hpp — 2D draw list of both screens (stage 5.9, UI stage 1/7): coloured rectangles, UTF-8 text in three font
// sizes, pictures and 9-slice frames from the interface pack (ui_assets.hpp). No GPU code here: render.cpp turns the
// list into textured, vertex-coloured quads (rendererDrawUi), and the PC tests draw the same list with a software
// rasteriser (tests/ui_raster.cpp). Origin top-left, colours 0xAABBGGRR; the top screen is 400x240, the touch
// screen 320x240.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "ui_assets.hpp"

constexpr int kHudW = 400, kHudH = 240;
constexpr int kTouchW = 320, kTouchH = 240;

// mode: how the texture combines with the colour
enum HudMode : uint8_t { kHudFlat = 0, kHudAlpha = 1, kHudImage = 2 };

struct HudQuad {
    int16_t x0, y0, x1, y1;       // pixels
    uint16_t u0, v0, u1, v1;      // texels of 'tex'
    uint32_t color;               // 0xAABBGGRR
    uint16_t tex;                 // ui::kTex* id (0 = none)
    uint8_t mode;                 // HudMode: flat colour, alpha of the texture x colour (fonts), texture x colour
    uint8_t pad;
};

constexpr uint32_t hudRgba(int r, int g, int b, int a = 255) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}
constexpr uint32_t hudAlpha(uint32_t c, int a) { return (c & 0x00FFFFFFu) | ((uint32_t)a << 24); }

struct HudList {
    std::vector<HudQuad> q;
    // Everything added afterwards is clipped to this rectangle (pictures keep their texture mapping).
    void clip(int x, int y, int w, int h) { cx0 = x; cy0 = y; cx1 = x + w; cy1 = y + h; }
    void noClip() { cx0 = cy0 = -32768; cx1 = cy1 = 32767; }
    // Alpha of everything added afterwards is scaled by fade/255 (the top HUD dims out of combat).
    int fade = 255;

    void clear() { q.clear(); noClip(); fade = 255; }
    void rect(int x, int y, int w, int h, uint32_t color);
    void outline(int x, int y, int w, int h, uint32_t color);                 // 1 px frame inside the rectangle
    // A bar: dark border, background, fill 0..1 from the left.
    void bar(int x, int y, int w, int h, float fill, uint32_t fillColor, uint32_t back = hudRgba(0, 0, 0, 170));
    // Text (UTF-8) with the line top at y. Missing glyphs show as '?'. Returns the width in pixels.
    int text(int x, int y, const std::string& s, uint32_t color, bool shadow = true, ui::Font f = ui::Font::Small);
    int textCentered(int cx, int y, const std::string& s, uint32_t color, bool shadow = true, ui::Font f = ui::Font::Small);
    int textRight(int xRight, int y, const std::string& s, uint32_t color, bool shadow = true, ui::Font f = ui::Font::Small);
    // A line that does not fit maxW loses characters at the end and gets "..": returns what was drawn.
    std::string textFit(int x, int y, const std::string& s, int maxW, uint32_t color, bool shadow = true, ui::Font f = ui::Font::Small);
    static int width(const std::string& s, ui::Font f = ui::Font::Small);
    static int lineHeight(ui::Font f = ui::Font::Small);
    static std::string fit(const std::string& s, int maxW, ui::Font f = ui::Font::Small);   // cut with ".."
    // Wraps at spaces so that no line is wider than maxW pixels (overlong words are cut).
    static std::vector<std::string> wrap(const std::string& s, int maxW, ui::Font f = ui::Font::Small);

    // Pictures: the whole image stretched to w x h; a part of it (fractions of the source rectangle); a 9-slice
    // frame (corners kept, edges and centre stretched; without margins = image()). Nothing when !im.ok().
    void image(int x, int y, int w, int h, const ui::Image& im, uint32_t color = 0xFFFFFFFFu);
    void imagePart(int x, int y, int w, int h, const ui::Image& im, float fx0, float fy0, float fx1, float fy1,
                   uint32_t color = 0xFFFFFFFFu);
    void nine(int x, int y, int w, int h, const ui::Image& im, uint32_t color = 0xFFFFFFFFu, bool centre = true);

    // low level: one quad, clipped and faded
    void quad(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, uint32_t color, uint16_t tex, uint8_t mode);

private:
    int cx0 = -32768, cy0 = -32768, cx1 = 32767, cy1 = 32767;
};
