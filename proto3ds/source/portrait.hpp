// portrait.hpp — the unit frames' portraits (UI stage 8), like the original's: a picture of the unit's head drawn from its
// 3D model into a small texture (render to texture), redrawn when the unit, its looks or its gear change.
//
// Which way up a texture comes out of the GPU is not something to guess: the first frames draw a test pattern into the
// texture, read it back and work out the flip / turn that makes the picture upright (rendererDrawPortrait's corr).
// Without video memory for the two 64x64 textures, or with a pattern that makes no sense, portraits stay off and the HUD
// draws its fallback (race icon / initial).
#pragma once
#include <stdint.h>

#include "ui_assets.hpp"
#include "world.h"

namespace portrait {

bool init();        // two textures + render targets in VRAM; false: portraits off
void shutdown();
// Inside C3D_FrameBegin / C3D_FrameEnd, before the screens are drawn. units[k]: the unit to show in slot k (0 ours,
// 1 the target), nullptr when none / not drawn this frame; guids[k] who it is. At most one portrait is drawn per frame.
void frame(const CreatureInstance* const units[2], const uint64_t guids[2], uint32_t nowMs, FrameCounters& fc);
// The picture of slot k when it shows 'guid' (ok() false: not yet / not possible).
ui::Image image(int k, uint64_t guid);
const char* status();   // one line for the debug page / report
}  // namespace portrait
