// ui_render.hpp — draws HudLists on the GPU (UI stage 1/7): both screens, one vertex-coloured quad batch per texture
// and mode. Textures of ui.w3dp are uploaded when first drawn and the icon / map pages are evicted least recently
// used beyond a budget (the skin and fonts stay), so the interface stays inside its memory budget.
#pragma once
#include <3ds.h>
#include <citro3d.h>

#include "hud.hpp"

void uiRenderInit();
void uiRenderShutdown();
// Draws the list on the current render target; width = 400 (top) or 320 (touch screen).
void uiDraw(const HudList& list, int width);
// Call once per frame after C3D_FrameEnd: frees pages over the budget that were not drawn in the last frames.
void uiEndFrame();
// Drops every pack texture (after ui::assets() was reopened).
void uiReloadTextures();

struct UiTexStats { int resident = 0, loads = 0, evictions = 0, failed = 0; uint32_t bytes = 0; };
const UiTexStats& uiTexStats();
