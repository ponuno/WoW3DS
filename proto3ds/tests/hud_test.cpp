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
    if (getenv("W3D_PET")) {   // UI stage 13: a pet next to us (frame under the player's)
        using namespace w3n;
        auto fieldsW = [](Writer& w, std::initializer_list<std::pair<int, uint32_t>> f) {
            uint32_t mask[8] = {};
            int top = 0;
            for (auto& p : f) { mask[p.first / 32] |= 1u << (p.first % 32); top = std::max(top, p.first / 32 + 1); }
            w.u8((uint8_t)top);
            for (int i = 0; i < top; ++i) w.u32(mask[i]);
            for (int b = 0; b < top; ++b)
                for (int bit = 0; bit < 32; ++bit)
                    for (auto& p : f) if (p.first == b * 32 + bit) w.u32(p.second);
        };
        const uint64_t me = s.objects.self(), pg = 0xF130000000000555ull;
        const MovementInfo& m1 = s.mover.info();
        Writer w;
        w.u32(1); w.u8(0); w.u8(3); w.packedGuid(pg); w.u8(3);
        w.u8(0x60);
        w.u32(0); w.u32(0); w.f32(m1.x + 3); w.f32(m1.y); w.f32(m1.z); w.f32(0); w.u32(0);
        for (int k = 0; k < 6; ++k) w.f32(7);
        fieldsW(w, {{UF::UNIT_HEALTH, 300}, {UF::UNIT_MAXHEALTH, 400}, {UF::UNIT_LEVEL, 14}, {UF::UNIT_FIELD_PETNUMBER, 3},
                    {UF::UNIT_POWER1 + 2, 90}, {UF::UNIT_MAXPOWER1 + 2, 100}, {UF::UNIT_POWER1 + 4, 800000}, {UF::UNIT_MAXPOWER1 + 4, 1050000},
                    {UF::UNIT_BYTES_0, 2u << 24}, {UF::UNIT_BYTES_1, 5u << 8}});
        s.handle(0x0A9, w.b, last);
        Writer v;
        v.u32(1); v.u8(0); v.u8(0); v.packedGuid(me);
        fieldsW(v, {{UF::UNIT_FIELD_SUMMON, (uint32_t)pg}, {UF::UNIT_FIELD_SUMMON + 1, (uint32_t)(pg >> 32)}});
        s.handle(0x0A9, v.b, last);
        Writer nq; nq.u32(3); nq.cstr("Fang"); nq.u32(1);
        s.handle(0x053, nq.b, last);
        Writer ps;
        ps.u64(pg); ps.u32(0); ps.u8(1); ps.u8(2); ps.u8(0); ps.u8(0);
        for (int i = 0; i < 10; ++i) ps.u32(0);
        ps.u8(0); ps.u16(0);
        s.handle(0x179, ps.b, last);
    }
    if (getenv("W3D_SUBS")) {   // stage 5.42: <Guild> under players, <Subname> under creatures
        using namespace w3n;
        int n = 0;
        std::vector<uint64_t> gs;
        for (auto& [g, o] : s.objects.all()) gs.push_back(g);
        for (uint64_t g : gs) {
            WorldObject* o = s.objects.find(g);
            if (o->type == ObjType::Unit) o->subname = (n++ & 1) ? "Armorer" : "Flight Master";
            else if (o->type == ObjType::Player) { if (o->fields.size() <= 191) o->fields.resize(192, 0); o->fields[191] = 5; }
        }
        Writer q;
        q.u32(5); q.cstr("Knights of Ni");
        for (int i = 0; i < 10; ++i) q.cstr("");
        q.u32(0); q.u32(0); q.u32(0); q.u32(0); q.u32(0);
        s.handle(0x055, q.b, last);
    }
    HudInput in;
    in.game = &s;
    if (getenv("W3D_ZONE")) { in.zoneBig = "Elwynn Forest"; in.zoneSmall = "Goldshire"; in.zoneAlpha = 255; }   // stage 5.45
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
        pv.cells[0].cdKey = 100;
        pv.cells[0].cdTotalMs = 15000;   // UI stage 12: a clock at half, the GCD on two, a cooldown about to end
        pv.cells[1].gcdMs = 1000; pv.cells[1].cdKey = 772;
        pv.cells[2].cdMs = 3400; pv.cells[2].cdKey = 1715; pv.cells[2].cdTotalMs = 6000;
        pv.cells[7].gcdMs = 400; pv.cells[7].cdKey = 6343;
        in.cooldowns.push_back({ui::Image(), "Shield Wall", 1700000, 1800000});
        in.cooldowns.push_back({ui::Image(), "Healing Potion", 95000, 120000});
        in.cooldowns.push_back({ui::Image(), "Intercept", 4200, 25000});
        pv.aim = s.target();
        pv.mode = argc > 6 ? atoi(argv[6]) : 0;
        in.pad = &pv;
    }
    // UI stage 8: the interface pack (W3D_UI_PACK) for the frame art, stand-in portraits, a rested character with
    // experience, an elite target, an error line (W3D_HUD_EXTRAS=1)
    if (const char* pk = getenv("W3D_UI_PACK")) {
        char err[160] = "";
        if (!ui::assets().open(pk, err, sizeof err)) fprintf(stderr, "ui pack: %s\n", err);
    }
    if (getenv("W3D_HUD_EXTRAS")) {
        for (int k = 0; k < 2; ++k) {   // a face looking to the right of the picture (its nose on the right)
            const int n = ui::kPortraitSize;
            std::vector<uint8_t> px((size_t)n * n * 4);
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    uint8_t* o = &px[((size_t)y * n + x) * 4];
                    const float dx = x - 30.0f, dy = y - 30.0f;
                    uint8_t r = k ? 70 : 40, g = 60, b = k ? 40 : 110;
                    if (dx * dx / 300 + dy * dy / 420 < 1) { r = 225; g = 185; b = 150; }
                    if ((x - 34) * (x - 34) + (y - 24) * (y - 24) < 6 || (x - 44) * (x - 44) + (y - 24) * (y - 24) < 6) r = g = b = 20;
                    if (x >= 44 && x < 52 && y >= 30 && y < 34) { r = 200; g = 60; b = 60; }
                    if (y < 10) { r = 120; g = 70; b = 30; }   // hair on top: shows a vertical flip
                    o[0] = r; o[1] = g; o[2] = b; o[3] = 255;
                }
            rasterSetExternal((uint16_t)(ui::kTexExternal0 + k), n, n, px);
            in.portrait[k].tex = (uint16_t)(ui::kTexExternal0 + k);
            in.portrait[k].w = in.portrait[k].h = (uint16_t)n;
        }
        if (w3n::WorldObject* me = s.objects.find(s.objects.self())) {
            if (me->fields.size() < w3n::UF::PLAYER_END) me->fields.resize(w3n::UF::PLAYER_END);
            me->fields[w3n::UF::PLAYER_XP] = 620;
            me->fields[w3n::UF::PLAYER_NEXT_LEVEL_XP] = 1400;
            me->fields[w3n::UF::PLAYER_REST_STATE_EXPERIENCE] = 300;
            me->fields[w3n::UF::PLAYER_BYTES_2] = (me->fields[w3n::UF::PLAYER_BYTES_2] & 0x00FFFFFFu) | (1u << 24);
            me->fields[w3n::UF::PLAYER_FLAGS] |= 0x20;   // resting
        }
        if (w3n::WorldObject* t = s.objects.find(s.target())) {
            t->rank = 1;
            w3n::Writer w;   // SMSG_SPELL_START: the target casts a 2.5 s spell at us
            w.packedGuid(t->guid); w.packedGuid(t->guid); w.u32(133); w.u16(0); w.u32(2500); w.u16(0);
            s.handle(0x131, w.b, last - 1000);
        }
        static HudList warm;
        hudBuildGame(warm, in);   // (the first build only notes the experience; a change shows the numbers)
        if (w3n::WorldObject* me = s.objects.find(s.objects.self())) me->fields[w3n::UF::PLAYER_XP] = 655;
        s.error("Not enough rage", w3n::Vocal::NoRage, last);
        in.placeHint = "Blizzard";   // UI stage 10: aiming a ground spell
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
