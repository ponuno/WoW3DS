#include "hud.hpp"

#include <algorithm>

namespace {
const W3dUiGlyph* glyphOf(const ui::FontView& f, uint32_t cp) {
    if (const W3dUiGlyph* g = f.find(cp)) return g;
    if (cp == 0xA0) return f.find(' ');
    if (cp == 0x401) return f.find(0x415);   // Yo without its dots rather than '?'
    if (cp == 0x451) return f.find(0x435);
    // 7.03: arrows the game's fonts lack (FRIZQT and its fallbacks have ▲ ▼ but no ◀ ▶: the pager showed '?'): the nearest one
    // the font has, an ASCII bracket at worst
    struct Alt { uint32_t cp, a, b; };
    static const Alt kAlt[] = {
        // 7.15: accented letters when the font (the stub one / an old ui.w3dp) has no Latin-1 glyphs: the base letter.
        {0xE1, 'a', 'a'}, {0xE0, 'a', 'a'}, {0xE2, 'a', 'a'}, {0xE4, 'a', 'a'}, {0xE3, 'a', 'a'}, {0xE5, 'a', 'a'},
        {0xC1, 'A', 'A'}, {0xC0, 'A', 'A'}, {0xC2, 'A', 'A'}, {0xC4, 'A', 'A'}, {0xC3, 'A', 'A'}, {0xC5, 'A', 'A'},
        {0xE9, 'e', 'e'}, {0xE8, 'e', 'e'}, {0xEA, 'e', 'e'}, {0xEB, 'e', 'e'},
        {0xC9, 'E', 'E'}, {0xC8, 'E', 'E'}, {0xCA, 'E', 'E'}, {0xCB, 'E', 'E'},
        {0xED, 'i', 'i'}, {0xEC, 'i', 'i'}, {0xEE, 'i', 'i'}, {0xEF, 'i', 'i'},
        {0xCD, 'I', 'I'}, {0xCC, 'I', 'I'}, {0xCE, 'I', 'I'}, {0xCF, 'I', 'I'},
        {0xF3, 'o', 'o'}, {0xF2, 'o', 'o'}, {0xF4, 'o', 'o'}, {0xF6, 'o', 'o'}, {0xF5, 'o', 'o'}, {0xF8, 'o', 'o'},
        {0xD3, 'O', 'O'}, {0xD2, 'O', 'O'}, {0xD4, 'O', 'O'}, {0xD6, 'O', 'O'}, {0xD5, 'O', 'O'}, {0xD8, 'O', 'O'},
        {0xFA, 'u', 'u'}, {0xF9, 'u', 'u'}, {0xFB, 'u', 'u'}, {0xFC, 'u', 'u'},
        {0xDA, 'U', 'U'}, {0xD9, 'U', 'U'}, {0xDB, 'U', 'U'}, {0xDC, 'U', 'U'},
        {0xF1, 'n', 'n'}, {0xD1, 'N', 'N'}, {0xE7, 'c', 'c'}, {0xC7, 'C', 'C'}, {0xDF, 's', 's'},
        {0xE6, 'a', 'a'}, {0xC6, 'A', 'A'}, {0x153, 'o', 'o'}, {0x152, 'O', 'O'}, {0xBF, '?', '?'}, {0xA1, '!', '!'},
        {0x25C0, 0x2190, '<'}, {0x25B6, 0x2192, '>'}, {0x25B2, 0x2191, '^'}, {0x25BC, 0x2193, 'v'},
        {0x2190, 0x25C0, '<'}, {0x2192, 0x25B6, '>'}, {0x2191, 0x25B2, '^'}, {0x2193, 0x25BC, 'v'}};
    for (const Alt& a : kAlt)
        if (a.cp == cp) {
            if (const W3dUiGlyph* g = f.find(a.a)) return g;
            if (const W3dUiGlyph* g = f.find(a.b)) return g;
        }
    return f.find('?');
}
}  // namespace

void HudList::quad(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, uint32_t color, uint16_t tex, uint8_t mode) {
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < cx0 || y0 < cy0 || x1 > cx1 || y1 > cy1) {   // clip, moving the texture coordinates along
        if (x1 <= cx0 || y1 <= cy0 || x0 >= cx1 || y0 >= cy1) return;
        const float du = (float)(u1 - u0) / (float)(x1 - x0), dv = (float)(v1 - v0) / (float)(y1 - y0);
        if (x0 < cx0) { u0 += (int)((cx0 - x0) * du + 0.5f); x0 = cx0; }
        if (x1 > cx1) { u1 -= (int)((x1 - cx1) * du + 0.5f); x1 = cx1; }
        if (y0 < cy0) { v0 += (int)((cy0 - y0) * dv + 0.5f); y0 = cy0; }
        if (y1 > cy1) { v1 -= (int)((y1 - cy1) * dv + 0.5f); y1 = cy1; }
    }
    if (fade < 255) color = hudAlpha(color, (int)(color >> 24) * fade / 255);
    if ((color >> 24) == 0) return;
    q.push_back({(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (uint16_t)u0, (uint16_t)v0, (uint16_t)u1, (uint16_t)v1,
                 color, tex, mode, 0});
}

void HudList::rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    quad(x, y, x + w, y + h, 0, 0, 0, 0, color, ui::kTexNone, kHudFlat);
}

void HudList::outline(int x, int y, int w, int h, uint32_t color) {
    rect(x, y, w, 1, color);
    rect(x, y + h - 1, w, 1, color);
    rect(x, y + 1, 1, h - 2, color);
    rect(x + w - 1, y + 1, 1, h - 2, color);
}

void HudList::bar(int x, int y, int w, int h, float fill, uint32_t fillColor, uint32_t back) {
    rect(x - 1, y - 1, w + 2, h + 2, hudRgba(0, 0, 0, 200));
    rect(x, y, w, h, back);
    fill = std::clamp(fill, 0.0f, 1.0f);
    int fw = (int)(fill * (float)w + 0.5f);
    if (fill > 0 && fw < 1) fw = 1;
    ui::Image tex = ui::assets().sprite("bar");
    if (tex.ok()) imagePart(x, y, fw, h, tex, 0, 0, w ? (float)fw / (float)w : 0, 1, fillColor);
    else {
        rect(x, y, fw, h, fillColor);
        if (h >= 5) rect(x, y, fw, 1, hudAlpha(0xFFFFFFFFu, 70));   // a highlight: reads as a bar, not a box
    }
}

int HudList::lineHeight(ui::Font f) { return ui::assets().font(f).lineHeight; }

int HudList::width(const std::string& s, ui::Font f) {
    const ui::FontView& fv = ui::assets().font(f);
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        const W3dUiGlyph* g = glyphOf(fv, ui::nextCodepoint(s, i));
        if (g) w += g->advance;
    }
    return w;
}

int HudList::text(int x, int y, const std::string& s, uint32_t color, bool shadow, ui::Font f) {
    const ui::FontView& fv = ui::assets().font(f);
    const uint32_t black = hudRgba(0, 0, 0, (int)(color >> 24) * 200 / 255);
    int width = 0;
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
        int pen = x + (pass == 0 ? 1 : 0), py = y + (pass == 0 ? 1 : 0);
        for (size_t i = 0; i < s.size();) {
            const W3dUiGlyph* g = glyphOf(fv, ui::nextCodepoint(s, i));
            if (!g) continue;
            if (g->w && g->h) {
                int gx = pen + g->xoff, gy = py + g->yoff;
                quad(gx, gy, gx + g->w, gy + g->h, g->x, g->y, g->x + g->w, g->y + g->h, pass == 0 ? black : color, fv.tex, kHudAlpha);
            }
            pen += g->advance;
        }
        width = pen - x - (pass == 0 ? 1 : 0);
    }
    return width;
}

int HudList::textCentered(int cx, int y, const std::string& s, uint32_t color, bool shadow, ui::Font f) {
    return text(cx - width(s, f) / 2, y, s, color, shadow, f);
}

int HudList::textRight(int xRight, int y, const std::string& s, uint32_t color, bool shadow, ui::Font f) {
    return text(xRight - width(s, f), y, s, color, shadow, f);
}

std::string HudList::fit(const std::string& s, int maxW, ui::Font f) {
    if (width(s, f) <= maxW) return s;
    const int dots = width("..", f);
    std::string out;
    int w = 0;
    const ui::FontView& fv = ui::assets().font(f);
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        const W3dUiGlyph* g = glyphOf(fv, ui::nextCodepoint(s, i));
        int a = g ? g->advance : 0;
        if (w + a + dots > maxW) break;
        w += a;
        out.append(s, start, i - start);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + "..";
}

std::string HudList::textFit(int x, int y, const std::string& s, int maxW, uint32_t color, bool shadow, ui::Font f) {
    std::string t = fit(s, maxW, f);
    text(x, y, t, color, shadow, f);
    return t;
}

std::vector<std::string> HudList::wrap(const std::string& s, int maxW, ui::Font f) {
    std::vector<std::string> out;
    std::string line, word;
    auto cutPoint = [&](const std::string& w) {   // bytes of w that fit maxW (at least one character)
        size_t i = 0, last = 0;
        while (i < w.size()) {
            size_t j = i;
            ui::nextCodepoint(w, j);
            if (width(w.substr(0, j), f) > maxW && last) break;
            last = j;
            i = j;
        }
        return last ? last : w.size();
    };
    auto flush = [&] {
        if (word.empty()) return;
        while (width(word, f) > maxW) {   // cut an overlong word
            size_t k = cutPoint(word);
            if (k >= word.size()) break;
            if (!line.empty()) { out.push_back(line); line.clear(); }
            out.push_back(word.substr(0, k));
            word = word.substr(k);
        }
        std::string joined = line.empty() ? word : line + " " + word;
        if (!line.empty() && width(joined, f) > maxW) { out.push_back(line); line = word; }
        else line = joined;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flush();
        else if (c == '\n') { flush(); out.push_back(line); line.clear(); }
        else word += c;
    }
    flush();
    if (!line.empty() || out.empty()) out.push_back(line);
    return out;
}

void HudList::image(int x, int y, int w, int h, const ui::Image& im, uint32_t color) {
    if (!im.ok()) return;
    quad(x, y, x + w, y + h, im.x, im.y, im.x + im.w, im.y + im.h, color, im.tex, kHudImage);
}

void HudList::imagePart(int x, int y, int w, int h, const ui::Image& im, float fx0, float fy0, float fx1, float fy1, uint32_t color) {
    if (!im.ok()) return;
    quad(x, y, x + w, y + h, im.x + (int)(im.w * fx0 + 0.5f), im.y + (int)(im.h * fy0 + 0.5f), im.x + (int)(im.w * fx1 + 0.5f),
         im.y + (int)(im.h * fy1 + 0.5f), color, im.tex, kHudImage);
}

void HudList::nine(int x, int y, int w, int h, const ui::Image& im, uint32_t color, bool centre) {
    if (!im.ok()) return;
    if (!im.l && !im.t && !im.r && !im.b) { image(x, y, w, h, im, color); return; }
    // screen margins = texture margins, shrunk when the frame is smaller than its corners
    int l = im.l, r = im.r, t = im.t, b = im.b;
    if (l + r > w) { l = w * l / std::max(1, l + r); r = w - l; }
    if (t + b > h) { t = h * t / std::max(1, t + b); b = h - t; }
    const int xs[4] = {x, x + l, x + w - r, x + w}, ys[4] = {y, y + t, y + h - b, y + h};
    const int us[4] = {im.x, im.x + im.l, im.x + im.w - im.r, im.x + im.w};
    const int vs[4] = {im.y, im.y + im.t, im.y + im.h - im.b, im.y + im.h};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (i == 1 && j == 1 && !centre) continue;
            quad(xs[i], ys[j], xs[i + 1], ys[j + 1], us[i], vs[j], us[i + 1], vs[j + 1], color, im.tex, kHudImage);
        }
}
