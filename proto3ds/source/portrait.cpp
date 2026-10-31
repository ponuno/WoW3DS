#include "portrait.hpp"
#include "vram_guard.hpp"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "creature_view.hpp"
#include "portrait_calib.hpp"
#include "ui_render.hpp"

namespace portrait {
namespace {

enum class State : uint8_t { Off, Calibrate, Waiting, Ready, Failed };

struct Slot {
    C3D_Tex tex;
    C3D_RenderTarget* target = nullptr;
    bool ok = false;
    uint64_t guid = 0;              // who the texture shows now (0: nothing)
    const void* disp = nullptr;     // ... with which display (a new one: armour or shape changed)
    int worn = -1;                  // ... and how many worn models were ready (a helmet arriving)
    uint32_t drawnMs = 0, changedMs = 0;
};

Slot g_slot[2];
Slot g_doll;                    // UI stage 14: the character window's full figure (kDollSize square), external texture 2
State g_state = State::Off;
float g_corr[4] = {1, 0, 0, 1};
uint32_t g_frame = 0, g_calFrame = 0;
char g_status[96] = "portraits: off";

int readyWorn(const CreatureInstance& ci) {
    int n = 0;
    for (int w = 0; w < ci.wornCount; ++w)
        if (ci.worn[w].model && ci.worn[w].model->state == AssetState::Ready && ci.worn[w].model->texturesReady()) ++n;
    return n;
}

}  // namespace

bool init() {
    shutdown();
    for (int k = 0; k < 2; ++k) {
        Slot& s = g_slot[k];
        // 6.43: colour + depth, never the last bytes of VRAM (vram_guard.hpp)
        if (!vramRoomFor(2u * ui::kPortraitSize * ui::kPortraitSize * 4u)) break;
        if (!C3D_TexInitVRAM(&s.tex, (u16)ui::kPortraitSize, (u16)ui::kPortraitSize, GPU_RGBA8)) break;
        s.target = C3D_RenderTargetCreateFromTex(&s.tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(GPU_RB_DEPTH24));
        if (!s.target) { C3D_TexDelete(&s.tex); break; }
        C3D_TexSetFilter(&s.tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&s.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s.ok = true;
    }
    if (!g_slot[0].ok || !g_slot[1].ok) {
        shutdown();
        g_state = State::Failed;
        snprintf(g_status, sizeof g_status, "portraits: off (no video memory)");
        return false;
    }
    if (vramRoomFor(2u * ui::kDollSize * ui::kDollSize * 4u) && C3D_TexInitVRAM(&g_doll.tex, (u16)ui::kDollSize, (u16)ui::kDollSize, GPU_RGBA8)) {
        g_doll.target = C3D_RenderTargetCreateFromTex(&g_doll.tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(GPU_RB_DEPTH24));
        if (g_doll.target) {
            C3D_TexSetFilter(&g_doll.tex, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&g_doll.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            g_doll.ok = true;
        } else {
            C3D_TexDelete(&g_doll.tex);
        }
    }
    g_state = State::Calibrate;
    snprintf(g_status, sizeof g_status, "portraits: calibrating");
    return true;
}

void shutdown() {
    for (int k = 0; k < 2; ++k) {
        Slot& s = g_slot[k];
        uiSetExternalTexture(k, nullptr, 0, 0);
        if (s.target) C3D_RenderTargetDelete(s.target);
        if (s.ok) C3D_TexDelete(&s.tex);
        s = Slot();
    }
    uiSetExternalTexture(2, nullptr, 0, 0);
    if (g_doll.target) C3D_RenderTargetDelete(g_doll.target);
    if (g_doll.ok) C3D_TexDelete(&g_doll.tex);
    g_doll = Slot();
    g_state = State::Off;
}

void frame(const CreatureInstance* const units[2], const uint64_t guids[2], uint32_t nowMs, FrameCounters& fc) {
    ++g_frame;
    switch (g_state) {
        case State::Off:
        case State::Failed:
            return;
        case State::Calibrate:
            C3D_RenderTargetClear(g_slot[0].target, C3D_CLEAR_ALL, 0, 0);
            C3D_FrameDrawOn(g_slot[0].target);
            uiDrawOrientationTest();
            g_calFrame = g_frame;
            g_state = State::Waiting;
            return;
        case State::Waiting: {
            if (g_frame - g_calFrame < 3) return;   // C3D_FRAME_SYNCDRAW: the GPU has long finished that frame
            const bool ok = portraitCalibrate((const uint8_t*)g_slot[0].tex.data, ui::kPortraitSize, ui::kPortraitSize, g_corr);
            if (!ok) {
                g_state = State::Failed;
                snprintf(g_status, sizeof g_status, "portraits: off (test pattern not understood)");
                return;
            }
            g_state = State::Ready;
            snprintf(g_status, sizeof g_status, "portraits: on, fix %+.0f %+.0f %+.0f %+.0f", g_corr[0], g_corr[1], g_corr[2], g_corr[3]);
            for (int k = 0; k < 2; ++k) uiSetExternalTexture(k, &g_slot[k].tex, ui::kPortraitSize, ui::kPortraitSize);
            if (g_doll.ok) uiSetExternalTexture(2, &g_doll.tex, ui::kDollSize, ui::kDollSize);
            return;
        }
        case State::Ready:
            break;
    }
    // at most one portrait a frame: the one that is wrong, else the one waited for longest (a refresh: textures and worn
    // models of a new unit arrive over the first seconds, so the picture is redrawn once a second for a while)
    int pick = -1;
    uint32_t pickAge = 0;
    for (int k = 0; k < 2; ++k) {
        Slot& s = g_slot[k];
        const CreatureInstance* u = units[k];
        if (!u || !u->disp || !u->disp->ready()) continue;
        const int worn = readyWorn(*u);
        const bool changed = s.guid != guids[k] || s.disp != (const void*)u->disp || s.worn != worn;
        if (changed) { pick = k; pickAge = ~0u; break; }
        const uint32_t age = nowMs - s.drawnMs;
        const uint32_t every = nowMs - s.changedMs < 6000 ? 1000 : 10000;
        if (age >= every && age > pickAge) { pick = k; pickAge = age; }
    }
    if (pick < 0) return;
    Slot& s = g_slot[pick];
    const CreatureInstance& u = *units[pick];
    C3D_RenderTargetClear(s.target, C3D_CLEAR_ALL, 0, 0);
    C3D_FrameDrawOn(s.target);
    CreatureInstance pose = u;
    float rate = 1;
    bool loop = true;
    pose.seq = creaturePickSeq(u.disp->model->blob, false, 0, false, false, &rate, &loop);   // standing
    pose.timeMs = 0;
    pose.loop = loop;
    if (rendererDrawPortrait(pose, g_corr, fc)) {
        const int worn = readyWorn(u);
        if (s.guid != guids[pick] || s.disp != (const void*)u.disp || s.worn != worn) s.changedMs = nowMs;
        s.guid = guids[pick];
        s.disp = u.disp;
        s.worn = worn;
        s.drawnMs = nowMs;
    }
}

void frameDoll(const CreatureInstance* unit, uint64_t guid, float turn, uint32_t nowMs, FrameCounters& fc) {
    if (g_state != State::Ready || !g_doll.ok || !unit || !guid || !unit->disp || !unit->disp->ready()) return;
    if (g_doll.guid == guid && nowMs - g_doll.drawnMs < 66) return;   // ~15 pictures a second is plenty for an idle pose
    C3D_RenderTargetClear(g_doll.target, C3D_CLEAR_ALL, 0, 0);
    C3D_FrameDrawOn(g_doll.target);
    CreatureInstance pose = *unit;
    float rate = 1;
    bool loop = true;
    pose.seq = creaturePickSeq(unit->disp->model->blob, false, 0, false, false, &rate, &loop);   // standing
    pose.timeMs = nowMs;
    pose.loop = loop;
    if (rendererDrawPortrait(pose, g_corr, fc, true, turn)) {
        g_doll.guid = guid;
        g_doll.drawnMs = nowMs;
    }
}

ui::Image doll(uint64_t guid) {
    ui::Image im;
    if (g_state != State::Ready || !g_doll.ok || !guid || g_doll.guid != guid) return im;
    im.tex = (uint16_t)(ui::kTexExternal0 + 2);
    im.x = im.y = 0;
    im.w = im.h = (uint16_t)ui::kDollSize;
    return im;
}

void dollForget() { g_doll.guid = 0; }   // the window closed: the next opening draws a fresh picture first

ui::Image image(int k, uint64_t guid) {
    ui::Image im;
    if (g_state != State::Ready || k < 0 || k > 1 || !guid || g_slot[k].guid != guid) return im;
    im.tex = (uint16_t)(ui::kTexExternal0 + k);
    im.x = im.y = 0;
    im.w = im.h = (uint16_t)ui::kPortraitSize;
    return im;
}

const char* status() { return g_status; }

}  // namespace portrait
