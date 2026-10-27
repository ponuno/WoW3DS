// UI stage 7 check: the interface pack converted from tests/gen_ui_data.py's synthetic client, read the way the 3DS
// reads it (ui_assets.cpp) and drawn by the PC rasteriser: icon lookups by spell / item display / name, the 9-slice
// frames, fonts with the Cyrillic fallback, the zone map under a position. Writes ui_pack.png.
//   ui_pack_test ui.w3dp out.png
#include <math.h>

#include <array>
#include <cstdio>
#include <string>

#include "ui_raster.hpp"

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    ui::Assets& a = ui::assets();
    char err[96];
    check(a.open(argv[1], err, sizeof err), "open ui.w3dp");
    // icons
    ui::Image fire = a.spellIcon(133), hs = a.spellIcon(78), hs2 = a.spellIcon(284), teach = a.spellIcon(7380);
    check(fire.ok() && hs.ok() && fire.w == 40, "spell icons (Fireball, Heroic Strike)");
    check(hs2.ok() && hs2.x == hs.x && hs2.y == hs.y && hs2.tex == hs.tex, "two spells, one picture: one cell");
    check(teach.ok() && teach.x == hs.x && teach.y == hs.y, "a trainer's teaching spell shows the taught spell's icon");
    check(!a.spellIcon(555).ok() && !a.spellIcon(99999).ok(), "missing picture / unknown spell: no icon");
    ui::Image food1 = a.itemIcon(1001), food3 = a.itemIcon(1003), shirt = a.itemIcon(1002);
    check(food1.ok() && shirt.ok() && food1.x == food3.x && food1.y == food3.y, "item displays -> icons (shared picture)");
    check(!a.itemIcon(1004).ok() && !a.itemIcon(0).ok(), "item display without an icon");
    check(a.namedIcon("tab_combat").ok() && a.namedIcon("tab_combat").x == a.namedIcon("attack").x &&
          a.namedIcon("tab_combat").y == a.namedIcon("attack").y, "tab_combat: the second path (INV_Sword_04, like 'attack')");
    check(a.namedIcon("tab_map").ok() && a.namedIcon("slot_0").ok() && a.namedIcon("class_8").ok() && !a.namedIcon("class_6").ok(),
          "named icons: tabs, paper doll, class grid");
    // skin
    ui::Image panel = a.sprite("panel"), dialog = a.sprite("dialog"), button = a.sprite("button");
    check(panel.ok() && panel.w == 24 && panel.l == 8 && panel.r == 8, "panel: 9-slice from the tooltip border strip");
    check(dialog.ok() && dialog.w == 36 && dialog.t == 12, "dialog: 9-slice from the dialog border");
    check(button.ok() && button.w == 80 && button.h == 22 && button.l == 10, "button: crop + margins");
    check(a.sprite("arrow0").ok() && a.sprite("arrow15").ok() && !a.sprite("arrow16").ok(), "map arrow: 16 rotations");
    check(a.sprite("coin_gold").ok() && a.sprite("gossip_1").ok() && a.sprite("quest_new").ok(), "coins, gossip, quest icons");
    // the tooltip border's parts land where the 9-slice expects them: left edge red, right green, top blue, bottom yellow
    {
        std::vector<uint8_t> blob, rgba;
        int w = 0, h = 0;
        bool ok = a.readTexture(panel.tex, blob) && decodeTexBlob(blob, &w, &h, rgba);
        auto at = [&](int x, int y) { const uint8_t* p = &rgba[((size_t)(panel.y + y) * w + panel.x + x) * 4]; return std::array<int, 4>{p[0], p[1], p[2], p[3]}; };
        ok = ok && at(1, 12)[0] > 200 && at(1, 12)[1] < 60;          // left: red
        ok = ok && at(22, 12)[1] > 200 && at(22, 12)[0] < 60;        // right: green
        ok = ok && at(12, 1)[2] > 200 && at(12, 1)[0] < 60;          // top: blue
        ok = ok && at(12, 22)[0] > 200 && at(12, 22)[1] > 200 && at(12, 22)[2] < 60;   // bottom: yellow
        ok = ok && at(12, 12)[3] < 30;                                // centre: transparent
        check(ok, "edge strip pieces in place (left/right/top/bottom/centre)");
    }
    // arrows: the tip (furthest opaque texel from the centre) turns 22.5 degrees counter-clockwise per step
    {
        bool ok = true;
        std::string angles;
        for (int k = 0; k < 16; ++k) {
            ui::Image ar = a.sprite(("arrow" + std::to_string(k)).c_str());
            std::vector<uint8_t> blob, rgba;
            int w = 0, h = 0;
            if (!ar.ok() || !a.readTexture(ar.tex, blob) || !decodeTexBlob(blob, &w, &h, rgba)) { ok = false; break; }
            float sx = 0, sy = 0, n = 0;
            for (int y = 0; y < ar.h; ++y)
                for (int x = 0; x < ar.w; ++x) {
                    int al = rgba[((size_t)(ar.y + y) * w + ar.x + x) * 4 + 3];
                    float dx = x + 0.5f - ar.w / 2.0f, dy = y + 0.5f - ar.h / 2.0f;
                    // the tip is thin: weight the texels near the apex by their distance from the centroid of a wide base
                    sx += al * dx; sy += al * dy; n += al;
                }
            // the triangle's area centroid lies towards its base: the tip points the other way
            float ang = atan2f(sy / n, -(sx / n)) * 57.29578f;   // screen y down; 0 = right? compute heading of -centroid
            float head = atan2f(-(-sy / n), -sx / n) * 57.29578f;   // heading of the tip, counter-clockwise from +x
            (void)ang;
            float expect = 90.0f + 22.5f * k;
            float d = fmodf(head - expect + 540.0f, 360.0f) - 180.0f;
            if (fabsf(d) > 8.0f) ok = false;
            angles += " " + std::to_string((int)lroundf(head));
        }
        check(ok, ("arrow headings" + angles).c_str());
    }
    // fonts: the pack's normal/large sizes, Latin from the client font, Cyrillic from the fallback; small = built-in
    check(a.font(ui::Font::Normal).tex >= ui::kTexPack0 && a.font(ui::Font::Large).tex >= ui::kTexPack0, "pack fonts for normal / large");
    check(a.font(ui::Font::Small).tex == ui::kTexBuiltinFont, "small font: built-in");
    check(a.font(ui::Font::Normal).find(0x416) != nullptr && a.font(ui::Font::Normal).find('W') != nullptr, "Cyrillic via the fallback font");
    // maps
    ui::MapView z = a.zoneAt(0, -9000.0f, 0.0f);
    check(z.ok() && z.name == "Elwynn" && z.image.w == 384 && z.image.h == 256, "zone map under (-9000, 0): Elwynn 384x256");
    float u, v;
    check(z.toUv(-9000.0f, 0.0f, &u, &v) && fabsf(u - 1535.0f / 3470.0f) < 0.01f, "world -> map uv");
    ui::MapView c = a.zoneAt(0, 5000.0f, 5000.0f);
    check(c.ok() && c.name == "Azeroth", "outside every zone: the continent");
    check(!a.zoneAt(1, 0, 0).ok(), "no map for Kalimdor in this pack");

    // picture
    Canvas cv(400, 340, 0xFF202020u);
    HudList l;
    l.clear();
    int x = 4;
    for (const char* n : {"tab_combat", "tab_map", "tab_bags", "tab_spells", "tab_quests", "tab_chat", "tab_social", "tab_settings"}) {
        l.image(x, 4, 40, 40, a.namedIcon(n));
        x += 44;
    }
    l.image(4, 50, 40, 40, fire);
    l.image(48, 50, 32, 32, hs);
    l.image(84, 50, 24, 24, food1);
    l.image(112, 50, 40, 40, a.namedIcon("class_8"));
    l.nine(160, 50, 120, 60, panel);
    l.nine(290, 50, 100, 60, dialog);
    l.nine(160, 116, 90, 26, button);
    l.text(170, 121, "Купить", 0xFFFFFFFFu, true, ui::Font::Normal);
    for (int k = 0; k < 16; ++k) l.image(4 + k * 18, 150, 16, 16, a.sprite(("arrow" + std::to_string(k)).c_str()));
    l.image(4, 170, 192, 128, z.image);
    l.text(200, 170, "Normal: Квесты Bags 12", 0xFF00D1FFu, true, ui::Font::Normal);
    l.text(200, 190, "Large: Торговец", 0xFFFFFFFFu, true, ui::Font::Large);
    l.text(200, 214, "Small: Сумки built-in", 0xFFFFFFFFu, true, ui::Font::Small);
    l.bar(200, 232, 150, 10, 0.7f, hudRgba(30, 200, 30));
    cv.draw(l);
    cv.png(argv[2]);
    printf("UI pack checks failed: %d\n", fails);
    return fails ? 1 : 0;
}
