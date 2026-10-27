// Glyph atlases for the interface pack (UI stage 7): the client's TTF (Fonts\FRIZQT__.TTF) rendered with
// stb_truetype at fixed pixel sizes into one 8-bit alpha atlas, so the 3DS never rasterises a font. Glyphs the main
// font lacks (Cyrillic in an English client) come from fallback fonts, scaled to the same em size and sitting on
// the main font's baseline.
#include "conv.h"

#include <algorithm>
#include <cmath>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace w3d {

std::vector<uint32_t> uiCodepoints() {
    std::vector<uint32_t> cps;
    for (uint32_t c = 32; c < 127; ++c) cps.push_back(c);
    for (uint32_t c : {0xA0u, 0xA9u, 0xABu, 0xB0u, 0xB7u, 0xBBu, 0xD7u, 0x401u}) cps.push_back(c);
    for (uint32_t c = 0x410; c < 0x450; ++c) cps.push_back(c);
    for (uint32_t c : {0x451u, 0x2013u, 0x2014u, 0x2022u, 0x2026u, 0x2116u, 0x2190u, 0x2191u, 0x2192u, 0x2193u, 0x25B2u,
                       0x25B6u, 0x25BCu, 0x25C0u, 0x25C6u, 0x2709u, 0x2713u, 0x2715u})
        cps.push_back(c);
    return cps;
}

bool bakeFonts(const std::vector<std::vector<uint8_t>>& ttfs, const std::vector<int>& sizes, const std::vector<uint32_t>& cps,
               int atlasW, std::vector<uint8_t>& atlas, int* atlasH, std::vector<BakedFont>& out, size_t* missing) {
    std::vector<stbtt_fontinfo> fonts;
    for (const std::vector<uint8_t>& t : ttfs) {
        stbtt_fontinfo f;
        if (t.empty() || !stbtt_InitFont(&f, t.data(), stbtt_GetFontOffsetForIndex(t.data(), 0))) {
            if (fonts.empty()) return false;   // the main font must load
            continue;
        }
        fonts.push_back(f);
    }
    if (fonts.empty()) return false;
    struct Pending { std::vector<uint8_t> px; int w, h; size_t font, glyph; };
    std::vector<Pending> pend;
    out.clear();
    if (missing) *missing = 0;
    for (int px : sizes) {
        BakedFont bf;
        bf.pixelSize = px;
        const float s0 = stbtt_ScaleForMappingEmToPixels(&fonts[0], (float)px);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&fonts[0], &asc, &desc, &gap);
        bf.ascent = (int)std::lround(asc * s0);
        bf.lineHeight = std::max(px + 2, (int)std::lround((asc - desc) * s0));
        for (uint32_t cp : cps) {
            size_t fi = 0;
            while (fi < fonts.size() && !stbtt_FindGlyphIndex(&fonts[fi], (int)cp)) ++fi;
            if (fi == fonts.size()) {
                if (missing && cp != 0xA0) ++*missing;
                continue;
            }
            const stbtt_fontinfo& f = fonts[fi];
            const float s = stbtt_ScaleForMappingEmToPixels(&f, (float)px);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&f, (int)cp, &adv, &lsb);
            BakedGlyph g{cp, 0, 0, 0, 0, 0, 0, std::max(1, (int)std::lround(adv * s))};
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&f, (int)cp, s, s, &x0, &y0, &x1, &y1);
            if (cp != 32 && cp != 0xA0 && x1 > x0 && y1 > y0) {
                Pending p;
                p.w = x1 - x0;
                p.h = y1 - y0;
                p.px.assign((size_t)p.w * p.h, 0);
                stbtt_MakeCodepointBitmap(&f, p.px.data(), p.w, p.h, p.w, s, s, (int)cp);
                // a little more contrast: unhinted small glyphs come out grey
                for (uint8_t& v : p.px) v = (uint8_t)std::min(255.0f, 255.0f * std::pow(v / 255.0f, 0.75f) + 0.5f);
                g.w = p.w;
                g.h = p.h;
                g.xoff = x0;
                g.yoff = bf.ascent + y0;
                p.font = out.size();
                p.glyph = bf.glyphs.size();
                pend.push_back(std::move(p));
            }
            bf.glyphs.push_back(g);
        }
        out.push_back(std::move(bf));
    }
    // shelf packing, tallest first
    std::vector<size_t> order(pend.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return pend[a].h > pend[b].h; });
    int x = 0, y = 0, rowH = 0;
    for (size_t k : order) {
        Pending& p = pend[k];
        if (x + p.w + 1 > atlasW) { x = 0; y += rowH + 1; rowH = 0; }
        BakedGlyph& g = out[p.font].glyphs[p.glyph];
        g.x = x;
        g.y = y;
        x += p.w + 1;
        rowH = std::max(rowH, p.h);
    }
    int h = 8;
    while (h < y + rowH + 1) h *= 2;
    *atlasH = h;
    atlas.assign((size_t)atlasW * h, 0);
    for (const Pending& p : pend) {
        const BakedGlyph& g = out[p.font].glyphs[p.glyph];
        for (int r = 0; r < p.h; ++r) std::memcpy(&atlas[(size_t)(g.y + r) * atlasW + g.x], &p.px[(size_t)r * p.w], p.w);
    }
    for (BakedFont& bf : out)
        std::sort(bf.glyphs.begin(), bf.glyphs.end(), [](const BakedGlyph& a, const BakedGlyph& b) { return a.cp < b.cp; });
    return true;
}

}  // namespace w3d
