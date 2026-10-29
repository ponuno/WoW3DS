// ui_render.hpp — draws HudLists on the GPU (UI stage 1/7): both screens, one vertex-coloured quad batch per texture
// and mode. Textures of ui.w3dp are uploaded when first drawn and the icon / map pages are evicted least recently
// used beyond a budget (the skin and fonts stay), so the interface stays inside its memory budget.
#pragma once
#include <3ds.h>
#include <citro3d.h>

#include "hud.hpp"

#include <string>

void uiRenderInit();
void uiRenderShutdown();
// Draws the list on the current render target; width = 400 (top) or 320 (touch screen).
void uiDraw(const HudList& list, int width);
// Call once per frame after C3D_FrameEnd: frees pages over the budget that were not drawn in the last frames.
void uiEndFrame();
// Drops every pack texture (after ui::assets() was reopened).
void uiReloadTextures();
// Stage 6.2: an icon / map page read by the loader thread became a texture since the last call: redraw the touch screen
// (it is drawn only when its list changes, and the page's quads were skipped while it was being read).
bool uiPagesArrived();
// Stage 6.5: writes a small file on the loader thread (the settings: an SD write froze the frame for 40-55 ms per button
// press). Files still waiting are written by uiRenderShutdown.
void uiWriteFileAsync(const char* path, const std::string& data);

// UI stage 8: a texture made at run time (the portraits) under id ui::kTexExternal0 + n; nullptr removes it.
void uiSetExternalTexture(int n, C3D_Tex* tex, int w, int h);
// Draws the orientation test pattern into the current render target: what clip space calls the top half in red, the
// left half in green (portrait.cpp reads the texture back to see where they landed).
void uiDrawOrientationTest();

struct UiTexStats {
    int resident = 0, loads = 0, evictions = 0, failed = 0; uint32_t bytes = 0;
    // stage 6 (perf log), cumulative: quads and draw batches drawn, quads dropped (vertex buffer full), time reading pages
    // from the SD card (on the main thread)
    uint32_t quads = 0, batches = 0, overflow = 0, uploadBytes = 0;
    uint32_t vertexBytes = 0, fontBytes = 0;      // the quad vertex buffer and the built-in font (linear, for good)
    // stage 6.2: icon / map pages are read by a thread (asyncLoads, queued = being read now); the skin and the fonts on
    // the main thread (syncLoads, syncLoadMs: frozen frames)
    uint32_t asyncLoads = 0, queued = 0, syncLoads = 0;
    double syncLoadMs = 0;
    double loadMs = 0;
};
const UiTexStats& uiTexStats();
