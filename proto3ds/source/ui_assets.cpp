#include "ui_assets.hpp"

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "ui_font_builtin.h"

namespace ui {

namespace {
Assets g_assets;

void setErr(char* err, size_t n, const char* msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

template <class T>
bool copyTable(const std::vector<uint8_t>& b, uint32_t off, uint32_t count, std::vector<T>& out) {
    out.clear();
    if (!count) return true;
    if ((uint64_t)off + (uint64_t)count * sizeof(T) > b.size()) return false;
    out.resize(count);
    memcpy(out.data(), b.data() + off, (size_t)count * sizeof(T));
    return true;
}
}  // namespace

Assets& assets() { return g_assets; }

uint32_t nextCodepoint(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

const W3dUiGlyph* FontView::find(uint32_t cp) const {
    const W3dUiGlyph* b = glyphs;
    const W3dUiGlyph* e = glyphs + count;
    const W3dUiGlyph* it = std::lower_bound(b, e, cp, [](const W3dUiGlyph& g, uint32_t c) { return g.cp < c; });
    return it != e && it->cp == cp ? it : nullptr;
}

bool MapView::toUv(float x, float y, float* u, float* v) const {
    if (!m || m->left == m->right || m->top == m->bottom) return false;
    *u = (m->left - y) / (m->left - m->right);
    *v = (m->top - x) / (m->top - m->bottom);
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

void Assets::builtinFonts() {
    for (int i = 0; i < 3; ++i) {
        const W3dUiFont& f = kUiFontBuiltin[i];
        FontView& v = fonts_[i];
        v.tex = kTexBuiltinFont;
        v.pixelSize = f.pixelSize;
        v.lineHeight = f.lineHeight;
        v.ascent = f.ascent;
        v.glyphs = kUiGlyphBuiltin + f.firstGlyph;
        v.count = f.glyphCount;
    }
}

void Assets::close() {
    loaded_ = false;
    path_.clear();
    dir_.clear();
    tex_.clear();
    texLoc_.clear();
    sprites_.clear();
    icons_.clear();
    maps_.clear();
    loading_.clear();
    glyphs_.clear();
    strings_.clear();
    hdr_ = W3dUiDir{};
    builtinFonts();
}

bool Assets::open(const char* path, char* err, size_t errLen) {
    close();
    FILE* f = fopen(path, "rb");
    if (!f) { setErr(err, errLen, "no ui.w3dp (stub skin)"); return false; }
    W3dPackHeader ph{};
    bool ok = fread(&ph, sizeof ph, 1, f) == 1 && ph.magic == W3DP_MAGIC && ph.entryCount < 100000;
    std::vector<W3dEntry> idx;
    if (ok) {
        idx.resize(ph.entryCount);
        ok = fseek(f, (long)ph.indexOffset, SEEK_SET) == 0 &&
             fread(idx.data(), sizeof(W3dEntry), idx.size(), f) == idx.size();
    }
    const W3dEntry* dirE = nullptr;
    if (ok)
        for (const W3dEntry& e : idx)
            if (e.type == W3D_ENTRY_UI_DIR && e.id == 0) dirE = &e;
    if (ok && dirE && dirE->size >= sizeof(W3dUiDir) && dirE->size < (8u << 20)) {
        dir_.resize(dirE->size);
        ok = fseek(f, (long)dirE->offset, SEEK_SET) == 0 && fread(dir_.data(), 1, dir_.size(), f) == dir_.size();
    } else {
        ok = false;
    }
    fclose(f);
    if (!ok) { setErr(err, errLen, "ui.w3dp: not an interface pack"); dir_.clear(); return false; }
    memcpy(&hdr_, dir_.data(), sizeof hdr_);
    if (hdr_.magic != W3DU_MAGIC || hdr_.version != W3DU_VERSION) {
        setErr(err, errLen, "ui.w3dp: wrong version (convert it again)");
        dir_.clear();
        return false;
    }
    std::vector<W3dUiFont> fonts;
    ok = copyTable(dir_, hdr_.offTex, hdr_.texCount, tex_) && copyTable(dir_, hdr_.offFonts, hdr_.fontCount, fonts) &&
         copyTable(dir_, hdr_.offGlyphs, hdr_.glyphCount, glyphs_) && copyTable(dir_, hdr_.offSprites, hdr_.spriteCount, sprites_) &&
         copyTable(dir_, hdr_.offIcons, hdr_.iconCount, icons_) && copyTable(dir_, hdr_.offMaps, hdr_.mapCount, maps_) &&
         (hdr_.loadCount == 0 || copyTable(dir_, hdr_.offLoading, hdr_.loadCount, loading_)) &&
         (uint64_t)hdr_.offStrings + hdr_.strSize <= dir_.size();
    if (!ok) { setErr(err, errLen, "ui.w3dp: broken tables"); close(); return false; }
    strings_.assign((const char*)dir_.data() + hdr_.offStrings, hdr_.strSize);
    texLoc_.assign(tex_.size(), TexLoc());
    for (const W3dEntry& e : idx)
        if (e.type == W3D_ENTRY_UI_TEX && e.id < texLoc_.size()) texLoc_[e.id] = {e.offset, e.size};
    // fonts: a size class the pack has replaces the built-in one
    for (uint32_t i = 0; i < fonts.size() && i < 3; ++i) {
        const W3dUiFont& uf = fonts[i];
        if (!uf.glyphCount || (uint64_t)uf.firstGlyph + uf.glyphCount > glyphs_.size() || uf.tex >= tex_.size()) continue;
        FontView& v = fonts_[i];
        v.tex = (uint16_t)(kTexPack0 + uf.tex);
        v.pixelSize = uf.pixelSize;
        v.lineHeight = uf.lineHeight;
        v.ascent = uf.ascent;
        v.glyphs = glyphs_.data() + uf.firstGlyph;
        v.count = uf.glyphCount;
    }
    std::sort(sprites_.begin(), sprites_.end(), [](const W3dUiSprite& a, const W3dUiSprite& b) { return a.key < b.key; });
    std::sort(icons_.begin(), icons_.end(), [](const W3dUiIcon& a, const W3dUiIcon& b) { return a.key < b.key; });
    dir_.clear();
    dir_.shrink_to_fit();
    path_ = path;
    loaded_ = true;
    setErr(err, errLen, "");
    return true;
}

Image Assets::sprite(const char* name) const {
    const uint32_t key = w3dUiHash(name);
    auto it = std::lower_bound(sprites_.begin(), sprites_.end(), key, [](const W3dUiSprite& s, uint32_t k) { return s.key < k; });
    Image im;
    if (it == sprites_.end() || it->key != key || it->tex >= tex_.size()) return im;
    im.tex = (uint16_t)(kTexPack0 + it->tex);
    im.x = it->x; im.y = it->y; im.w = it->w; im.h = it->h;
    im.l = it->l; im.t = it->t; im.r = it->r; im.b = it->b;
    return im;
}

Image Assets::icon(uint32_t key) const {
    auto it = std::lower_bound(icons_.begin(), icons_.end(), key, [](const W3dUiIcon& s, uint32_t k) { return s.key < k; });
    Image im;
    if (it == icons_.end() || it->key != key || it->page >= tex_.size() || !hdr_.iconsPerRow) return im;
    const int cell = hdr_.iconCell, border = (hdr_.iconCell - hdr_.iconSize) / 2;
    im.tex = (uint16_t)(kTexPack0 + it->page);
    im.x = (uint16_t)((it->cell % hdr_.iconsPerRow) * cell + border);
    im.y = (uint16_t)((it->cell / hdr_.iconsPerRow) * cell + border);
    im.w = im.h = hdr_.iconSize;
    return im;
}

MapView Assets::view(const W3dUiMap& m) const {
    MapView v;
    v.m = &m;
    if (m.tex < tex_.size()) {
        v.image.tex = (uint16_t)(kTexPack0 + m.tex);
        v.image.w = m.w;
        v.image.h = m.h;
    }
    if (m.nameOff < strings_.size()) v.name = strings_.c_str() + m.nameOff;
    return v;
}

MapView Assets::zoneAt(uint32_t mapId, float x, float y) const {
    const W3dUiMap* best = nullptr;
    float bestArea = 0;
    for (const W3dUiMap& m : maps_) {
        if (m.mapId != mapId || !m.areaId) continue;
        if (y > m.left || y < m.right || x > m.top || x < m.bottom) continue;
        float area = (m.left - m.right) * (m.top - m.bottom);
        if (!best || area < bestArea) { best = &m; bestArea = area; }
    }
    return best ? view(*best) : continent(mapId);
}

Image Assets::loadingScreen(uint32_t mapId) const {
    Image im;
    for (const W3dUiLoading& l : loading_)
        if (l.mapId == mapId && l.tex < tex_.size()) {
            im.tex = (uint16_t)(kTexPack0 + l.tex);
            im.w = l.w;
            im.h = l.h;
            return im;
        }
    return im;
}

MapView Assets::continent(uint32_t mapId) const {
    for (const W3dUiMap& m : maps_)
        if (m.mapId == mapId && !m.areaId) return view(m);
    return MapView();
}

bool Assets::texInfo(uint16_t id, int* w, int* h, uint8_t* format, uint8_t* kind) const {
    if (id == kTexBuiltinFont) {
        *w = UI_FONT_BUILTIN_W; *h = UI_FONT_BUILTIN_H; *format = W3D_TEX_A8; *kind = W3D_UITEX_FONT;
        return true;
    }
    if (id < kTexPack0 || id - kTexPack0 >= (int)tex_.size()) return false;
    const W3dUiTex& t = tex_[id - kTexPack0];
    *w = t.w; *h = t.h; *format = t.format; *kind = t.kind;
    return true;
}

bool Assets::readTexture(uint16_t id, std::vector<uint8_t>& blob) const {
    if (!loaded_ || id < kTexPack0 || id - kTexPack0 >= (int)texLoc_.size()) return false;
    const TexLoc& l = texLoc_[id - kTexPack0];
    if (!l.size || l.size > (4u << 20)) return false;
    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) return false;
    blob.resize(l.size);
    bool ok = fseek(f, (long)l.offset, SEEK_SET) == 0 && fread(blob.data(), 1, blob.size(), f) == blob.size();
    fclose(f);
    if (ok && blob.size() >= W3D_TEX_DATA_OFFSET) {
        W3dTexHeader th;
        memcpy(&th, blob.data(), sizeof th);
        ok = th.width == tex_[id - kTexPack0].w && th.height == tex_[id - kTexPack0].h && W3D_TEX_DATA_OFFSET + th.dataSize <= blob.size();
    }
    return ok;
}

void Assets::builtinFontA8(std::vector<uint8_t>& out, int* w, int* h) {
    *w = UI_FONT_BUILTIN_W;
    *h = UI_FONT_BUILTIN_H;
    out.resize((size_t)UI_FONT_BUILTIN_W * UI_FONT_BUILTIN_H);
    for (size_t p = 0; p < out.size(); ++p) {
        uint8_t a = (p & 1) ? (kUiFontBuiltinA4[p >> 1] >> 4) : (kUiFontBuiltinA4[p >> 1] & 15);
        out[p] = (uint8_t)(a * 17);
    }
}

namespace {
struct Init { Init() { g_assets.close(); } } g_init;   // built-in fonts before anything draws text
}

}  // namespace ui
