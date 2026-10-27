// PC check of the HUD: replays a recorded session (W3DL) into a GameSession up to a time, builds the HUD
// draw list and writes it as text (hud_render.py rasterises it to a PNG with the real font atlas).
//   hud_test fight.w3dl <ms> out.txt
#include <math.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hud_game.hpp"
#include "ui_raster.hpp"

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    std::vector<w3n::LoggedPacket> pk;
    if (!w3n::readPacketLog(argv[1], pk)) fprintf(stderr, "log truncated\n");
    uint32_t until = (uint32_t)atoi(argv[2]);
    w3n::GameSession s;
    if (const char* f = getenv("W3D_FACTIONS")) s.factions.load(f);   // stage 5.12: reaction colours
    uint32_t last = 0;
    for (auto& p : pk) {
        if (p.ms > until) break;
        last = p.ms;
        if (p.dir == 1) continue;
        if (p.opcode == 0x236 && p.data.size() >= 20) {
            w3n::Reader r(p.data);
            r.u32();
            float x = r.f32(), y = r.f32(), z = r.f32(), o = r.f32();
            s.mover.reset(x, y, z, o, p.ms);
        }
        s.handle(p.opcode, p.data, p.ms);
        s.objects.takeCreated();
    }
    if (const char* y = getenv("W3D_CAM_YAW")) {   // look somewhere else than the character faces
        const w3n::MovementInfo& m0 = s.mover.info();
        s.mover.reset(m0.x, m0.y, m0.z, (float)atof(y), last);
    }
    for (auto& [g, o] : s.objects.all())
        if (o.type == w3n::ObjType::Player && !o.name.empty() == false && o.hasPos) { s.objects.setSelf(g); s.objects.find(g)->self = true; break; }
    HudInput in;
    in.game = &s;
    in.nowMs = last;
    const w3n::MovementInfo& m = s.mover.info();
    in.camYaw = m.o;
    in.camPitch = -0.12f;
    in.camPos[0] = m.x - cosf(m.o) * 5.0f;
    in.camPos[1] = m.y - sinf(m.o) * 5.0f;
    in.camPos[2] = m.z + 2.6f;
    in.selfName = "Ponuno";
    in.showSelfPlate = true;
    if (argc > 4) {   // fake extras: a target and a cast
        uint64_t best = 0;
        for (auto& [g, o] : s.objects.all())
            if (!o.self && o.hasPos && o.isUnitLike() && !o.name.empty()) { best = g; break; }
        if (best) s.setTarget(best, last);
        s.cast.active = true; s.cast.spell = 78; s.cast.startMs = last - 900; s.cast.durMs = 2000;
    }
    static HudPadView pv;   // "pad": the Forever bars (stage 5.11) with a sample layout, ZR held, the aim ring
    if (argc > 5 && std::string(argv[5]) == "pad") {
        pv.show = true;
        pv.set = 2;
        static const char* names[8] = {"Charge", "Rend", "Hamstring", "", "Battle Shout", "Heroic Strike", "Bread x3", "Thunder Clap"};
        for (int i = 0; i < 8; ++i) pv.cells[i].name = names[i];
        pv.cells[5].active = true;
        pv.cells[0].cdMs = 7300;
        pv.aim = s.target();
        pv.mode = argc > 6 ? atoi(argv[6]) : 0;
        in.pad = &pv;
    }
    HudList l;
    hudBuildGame(l, in);
    Canvas c(kHudW, kHudH);   // UI stage 1: the PC rasteriser draws the quads over a sky / grass backdrop
    c.backdrop();
    c.draw(l);
    c.png(argv[3]);
    printf("%zu quads at %u ms, %zu objects\n", l.q.size(), last, s.objects.all().size());
    return 0;
}
