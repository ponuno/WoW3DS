// fx.hpp — particles and glow sprites of the world's doodads (stage 5.32), without any 3DS API: the PC test runs it as is.
//
// Every frame the renderer hands over the model instances with effects near the camera (Source); the system emits from
// their emitters (W3dFxEmitter, common/w3d_fx.h) in world space, moves what is alive and returns camera-facing quads in
// view space (Quad), which the renderer writes into a vertex buffer and draws with the particle shader.
//
// What keeps it inside the 3DS's budget (the cost is mostly fill rate - every pixel of a blended quad - and then the CPU):
//   - a fixed pool (Budget::maxParticles), emission thinned as it fills up, never more than that alive;
//   - emitters run only near the camera (Budget::emitRange), and at a lower rate farther away;
//   - sizes are capped (flames small, smoke at most 1.5 yd), lives are short (the converter caps them at 8 s);
//   - glows are one quad per lamp, drawn up to Budget::glowRange.
// A model freed by the streamer takes its particles with it (forget): they point into its blob and its textures.
#pragma once
#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "w3d_fx.h"

namespace fx {

struct Source {                   // one instance with effects near the camera, this frame
    const void* key = nullptr;    // identifies the instance between frames (its emitters' accumulators)
    const void* owner = nullptr;  // the model: forget(owner) when it is freed
    const uint8_t* blob = nullptr;
    const W3dFxHeader* h = nullptr;
    const void* const* tex = nullptr;   // the model's handle of each fx texture (h->texCount)
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};     // as the mesh: world = pos + Rz(rot[2]) * Ry(rot[1]) * Rx(rot[0]) * (scale * local)
    float scale = 1;
    bool priority = false;        // a spell effect (stage 5.33): no thinning by distance or a filling pool
    W3dFxAnimView anim = {0, 0, 0, 0, 0, 0};   // stage 5.35: baked bone animation of the model (a == null: still)
    uint32_t animMs = 0;          // ... and how far into it
};

struct Quad {                     // camera-facing, view space (x right, y up, the camera looks along -z)
    float c[3];
    float half;                   // half the side, yards
    float u0, v0, u1, v1;         // PICA texture coordinates (t = 1 at the top of the image)
    uint32_t color;               // RGBA8, r in the low byte
    const void* tex;              // a texture handle, or null: the built-in soft dot
    uint8_t blend;                // W3D_BLEND_ALPHA or W3D_BLEND_ADD
};

struct Budget {
    int maxParticles = 500;
    float emitRange = 50.0f;      // yards: emitters farther away do not run
    float glowRange = 140.0f;
    int maxGlows = 160;
};
// The settings' "effects" level: 0 off, 1 low, 2 high.
Budget budgetFor(int level);

struct Stats {
    uint32_t alive = 0, emitted = 0, dropped = 0, glows = 0, sources = 0, quads = 0;
};

class System {
public:
    void setBudget(const Budget& b) { budget_ = b; }
    const Budget& budget() const { return budget_; }
    // dt seconds; cam: world position; view: the view matrix rows (rotation, world -> view); timeSec for flicker.
    void frame(float dt, float timeSec, const float cam[3], const float view[3][3], const std::vector<Source>& sources,
               std::vector<Quad>& out);
    void forget(const void* owner);   // drop every particle of this model (it is being freed)
    void clear();
    const Stats& stats() const { return stats_; }
    size_t alive() const { return parts_.size(); }

private:
    struct Particle {
        float p[3], v[3];
        float age, life;
        const W3dFxEmitter* e;
        const void* owner;
        const void* tex;
        uint8_t tile;
    };
    struct EmitterState { float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0}; uint32_t frame = 0; };
    float rnd();                    // 0..1
    float rndS() { return rnd() * 2.0f - 1.0f; }
    void emit(const Source& s, const W3dFxEmitter& e, const float R[3][3], float scale, const void* tex,
              const float* bone = nullptr);   // bone: 3x4 of the emitter's bone (stage 5.35), null = still
    Budget budget_;
    Stats stats_;
    std::vector<Particle> parts_;
    std::unordered_map<const void*, EmitterState> state_;
    uint32_t frame_ = 0, rng_ = 0x2545F491u;
};

System& global();                  // the one the renderer uses

}  // namespace fx

// Called by the streamer before a model's blob and textures are freed.
void fxForgetModel(const void* model);
