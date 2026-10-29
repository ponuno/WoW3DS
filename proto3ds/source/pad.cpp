#include "pad.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "movement.hpp"

namespace {
constexpr u32 kDpad[4] = {KEY_DUP, KEY_DRIGHT, KEY_DDOWN, KEY_DLEFT};
constexpr u32 kFace[4] = {KEY_X, KEY_A, KEY_B, KEY_Y};   // top, right, bottom, left
constexpr float kDirCut = 0.38f;                         // ~22.5 deg: a direction counts from this share of the push
}  // namespace

const char* padButtonGlyph(int b) {
    static const char* n[8] = {"Up", "Rt", "Dn", "Lt", "X", "A", "B", "Y"};
    return b >= 0 && b < 8 ? n[b] : "?";
}

const char* padSetName(int set) {
    static const char* n[4] = {"", "ZL", "ZR", "ZL+ZR"};
    return set >= 0 && set < 4 ? n[set] : "";
}

// ------------------------------------------------------------------ config file: "key value" lines
bool PadConfig::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char key[32];
    float v;
    while (fscanf(f, "%31s %f", key, &v) == 2) {
        if (!strcmp(key, "scheme")) scheme = v >= 0.5f ? PadScheme::Classic : PadScheme::Forever;
        else if (!strcmp(key, "dead") && v >= 0.02f && v <= 0.4f) stickDead = v;
        else if (!strcmp(key, "run") && v >= 0.1f && v <= 1.0f) runThreshold = v;
        else if (!strcmp(key, "back") && v >= 0.1f && v <= 1.0f) backThreshold = v;
        else if (!strcmp(key, "face")) faceMovement = v != 0;
        else if (!strcmp(key, "toggle")) toggleModifiers = v != 0;
        else if (!strcmp(key, "invert")) invertY = v != 0;
        else if (!strcmp(key, "camspeed") && v >= 0.3f && v <= 6.0f) camSpeed = v;
        else if (!strcmp(key, "tracker")) questTracker = v != 0;
        else if (!strcmp(key, "language")) lang = v >= 0.5f ? 1 : 0;   // (6.20: the key "lang" of older files, where Russian was the default, is ignored)
        else if (!strcmp(key, "dist") && v >= 0 && v <= 3) drawDist = (int)v;
        else if (!strcmp(key, "crowd") && v >= 0 && v <= 3) crowd = (int)v;
        else if (!strcmp(key, "topmode") && v >= 0 && v <= 2) topMode = (int)v;
        else if (!strcmp(key, "doodads")) doodads = v != 0;
        else if (!strcmp(key, "stats")) showStats = v != 0;
        else if (!strcmp(key, "effects")) effects = v < 0 ? 0 : v > 2 ? 2 : v;
        else if (!strcmp(key, "bigtext")) bigText = v != 0;
        else if (!strcmp(key, "dimhud")) dimHud = v != 0;
        else if (!strcmp(key, "autotabs")) autoTabs = v != 0;
        else if (!strcmp(key, "music") && v >= 0 && v <= 10) musicVol = (int)v;
        else if (!strcmp(key, "sfx") && v >= 0 && v <= 10) sfxVol = (int)v;
        else if (!strcmp(key, "ambience") && v >= 0 && v <= 10) ambVol = (int)v;
        else if (!strcmp(key, "ground")) groundCast = v >= 0.5f ? 1 : 0;
        else if (!strcmp(key, "perflog")) perfLog = v != 0;
        else if (!strcmp(key, "gpuearly")) gpuKick = v != 0;   // (6.7: a new key - 'gpukick' files start with it off)
        else if (!strcmp(key, "vsync")) vsync = v != 0;
        else if (!strcmp(key, "fadein")) fadeIn = v != 0;
    }
    fclose(f);
    return true;
}

std::string PadConfig::text() const {
    char b[640];
    int n = snprintf(b, sizeof b, "scheme %d\ndead %.2f\nrun %.2f\nback %.2f\nface %d\ntoggle %d\ninvert %d\ncamspeed %.2f\ntracker %d\n",
            scheme == PadScheme::Classic ? 1 : 0, stickDead, runThreshold, backThreshold, faceMovement ? 1 : 0,
            toggleModifiers ? 1 : 0, invertY ? 1 : 0, camSpeed, questTracker ? 1 : 0);
    n += snprintf(b + n, sizeof b - (size_t)n, "language %d\ndist %d\ncrowd %d\ndoodads %d\nbigtext %d\ndimhud %d\nautotabs %d\n", lang, drawDist, crowd, doodads ? 1 : 0,
            bigText ? 1 : 0, dimHud ? 1 : 0, autoTabs ? 1 : 0);
    n += snprintf(b + n, sizeof b - (size_t)n, "music %d\nsfx %d\nambience %d\ntopmode %d\neffects %d\nstats %d\nground %d\nperflog %d\ngpuearly %d\nvsync %d\nfadein %d\n", musicVol, sfxVol,
            ambVol, topMode, effects, showStats ? 1 : 0, groundCast, perfLog ? 1 : 0, gpuKick ? 1 : 0, vsync ? 1 : 0, fadeIn ? 1 : 0);
    return std::string(b, (size_t)std::max(0, std::min(n, (int)sizeof b - 1)));
}

bool PadConfig::save(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    const std::string t = text();
    const bool ok = fwrite(t.data(), 1, t.size(), f) == t.size();
    fclose(f);
    return ok;
}

// ------------------------------------------------------------------ schemes
// SELECT (both schemes, UI stage 3): held + D-pad left/right turns the touch screen's tabs; a tap without the D-pad
// is the free camera, on release.
void Pad::selectKeys(const PadInput& in, PadOut& o) {
    if (in.down & KEY_SELECT) selectCombo_ = false;
    if (in.held & KEY_SELECT) {
        if (in.down & KEY_DLEFT) { o.tabDelta = -1; selectCombo_ = true; }
        if (in.down & KEY_DRIGHT) { o.tabDelta = 1; selectCombo_ = true; }
    }
    // Stage 5.31: a tap no longer switches to the free camera (it went on by accident, the user asked for it gone).
    o.freeCamera = false;
}

PadOut Pad::update(const PadInput& in, const PadConfig& cfg, bool inCombat, float camYaw, float dt) {
    PadInput i = in;
    PadOut tabs;
    selectKeys(in, tabs);
    if (in.held & KEY_SELECT) {   // the D-pad belongs to SELECT
        i.down &= ~(u32)(KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN);
        i.held &= ~(u32)(KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN);
    }
    PadOut o = cfg.scheme == PadScheme::Classic ? classic(i, cfg, dt) : forever(i, cfg, inCombat, camYaw, dt);
    o.tabDelta = tabs.tabDelta;
    o.freeCamera = tabs.freeCamera;
    return o;
}

PadOut Pad::forever(const PadInput& in, const PadConfig& cfg, bool inCombat, float camYaw, float dt) {
    PadOut o;
    const u32 d = in.down, h = in.held;
    // modifiers: held (default) or press-to-toggle
    if (cfg.toggleModifiers) {
        if (d & KEY_ZL) toggled_ ^= 1;
        if (d & KEY_ZR) toggled_ ^= 2;
        o.set = toggled_;
    } else {
        o.set = ((h & KEY_ZL) ? 1 : 0) | ((h & KEY_ZR) ? 2 : 0);
    }
    const bool L = (h & KEY_L) != 0, R = (h & KEY_R) != 0, both = L && R;
    o.mode = both ? PadOut::None : R ? PadOut::Enemy : L ? PadOut::Friend : PadOut::None;
    if (lastMode_ != PadOut::None && !L && !R) o.released = lastMode_;
    if (!both) lastMode_ = o.mode;

    auto pressed = [&](u32 k) { return (d & k) != 0; };
    if (both) {
        if (pressed(KEY_DLEFT)) o.pageDelta = -1;
        if (pressed(KEY_DRIGHT)) o.pageDelta = 1;
        if (h & KEY_DUP) o.zoom = -1;
        if (h & KEY_DDOWN) o.zoom = 1;
        if (pressed(KEY_A)) o.centerCamera = true;
    } else if (o.mode == PadOut::Enemy) {
        o.lastHostile = pressed(KEY_B);
        o.assist = pressed(KEY_A);
        o.attack = pressed(KEY_Y);
        o.cycle = pressed(KEY_X) || pressed(KEY_DUP) || pressed(KEY_DRIGHT) || pressed(KEY_DDOWN) || pressed(KEY_DLEFT);
    } else if (o.mode == PadOut::Friend) {
        o.selfTarget = pressed(KEY_B);
        o.assist = pressed(KEY_A);
        o.cycle = pressed(KEY_X);
        for (int b = 0; b < 4 && o.partyPick < 0; ++b)   // stage 5.17: party frames (the game falls back to cycling)
            if (pressed(kDpad[b])) o.partyPick = b;
    } else {
        for (int b = 0; b < 4 && o.slot < 0; ++b)
            if (pressed(kDpad[b])) o.slot = o.set * kPadButtons + b;
        if (o.set == 0) {
            o.jump = pressed(KEY_B);
            o.ascend = (in.held & KEY_B) != 0;
            o.cancel = pressed(KEY_A);
            o.interact = pressed(KEY_Y);
            o.context = pressed(KEY_X);
        } else {
            for (int b = 0; b < 4 && o.slot < 0; ++b)
                if (pressed(kFace[b])) o.slot = o.set * kPadButtons + 4 + b;
        }
    }
    o.menu = pressed(KEY_START);

    // C-Stick: aim while a targeting mode is held, else the camera
    float rm = sqrtf(in.rx * in.rx + in.ry * in.ry);
    if (o.mode != PadOut::None && rm > 0.3f) {
        o.aiming = true;
        o.aimYaw = camYaw + atan2f(-in.rx, in.ry);
    } else if (rm > cfg.stickDead) {
        o.camYaw = -in.rx * cfg.camSpeed * dt;
        o.camPitch = (cfg.invertY ? -in.ry : in.ry) * cfg.camSpeed * 0.66f * dt;
    }

    // Circle Pad: digital WoW movement from an analog stick
    float m = sqrtf(in.lx * in.lx + in.ly * in.ly);
    if (m < cfg.stickDead) {
        if (inCombat) { o.faceYaw = true; o.yaw = camYaw; }   // "turn player with camera" in combat
        return o;
    }
    o.walk = m < cfg.runThreshold;
    if (cfg.faceMovement) {                       // run where the stick points, in combat too: the walk animation
                                                  // and the turning follow the stick (strafing only with faceMovement off)
        o.dir = w3n::MoveFlag::Forward;
        o.faceYaw = true;
        o.yaw = camYaw + atan2f(-in.lx, in.ly);
        return o;
    }
    o.faceYaw = true;                            // strafe mode: face the camera
    o.yaw = camYaw;
    float nx = in.lx / m, ny = in.ly / m;
    if (ny > kDirCut) o.dir |= w3n::MoveFlag::Forward;
    else if (ny < -kDirCut && -in.ly >= cfg.backThreshold) o.dir |= w3n::MoveFlag::Backward;
    if (nx > kDirCut) o.dir |= w3n::MoveFlag::StrafeRight;
    else if (nx < -kDirCut) o.dir |= w3n::MoveFlag::StrafeLeft;
    return o;
}

PadOut Pad::classic(const PadInput& in, const PadConfig& cfg, float dt) {
    PadOut o;
    const u32 d = in.down, h = in.held;
    o.jump = d & KEY_B;
    o.ascend = (h & KEY_B) != 0;
    o.nextTarget = d & KEY_L;
    o.toggleAttack = d & KEY_R;
    o.chat = d & KEY_Y;
    o.interact = d & KEY_A;
    o.nextSet = d & KEY_X;
    o.menu = d & KEY_START;
    // camera = the character: C-Stick and D-pad left/right look, Circle Pad left/right turns
    float look = 0, lookY = 0;
    if (fabsf(in.rx) > cfg.stickDead) look = in.rx;
    if (fabsf(in.ry) > cfg.stickDead) lookY = in.ry;
    if (h & KEY_DLEFT) look = -1;
    if (h & KEY_DRIGHT) look = 1;
    if (h & KEY_DUP) o.zoom = -1;
    if (h & KEY_DDOWN) o.zoom = 1;
    float turn = fabsf(in.lx) > cfg.stickDead ? in.lx : 0.0f;
    o.camYaw = -look * cfg.camSpeed * dt - turn * 2.2f * dt;
    o.camPitch = (cfg.invertY ? -lookY : lookY) * 1.2f * dt;
    o.turn = -turn;
    if (in.ly > kDirCut) o.dir = w3n::MoveFlag::Forward;
    else if (in.ly < -kDirCut) o.dir = w3n::MoveFlag::Backward;
    return o;
}
