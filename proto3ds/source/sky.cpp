// sky.cpp — 6.60: see sky.hpp.
#include "sky.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sky {

float clockHour(float baseMinutes, double elapsedSec, float minutesPerSec) {
    if (!(minutesPerSec > 0.0f) || minutesPerSec > 100.0f) minutesPerSec = 1.0f / 60.0f;
    double m = (double)baseMinutes + (elapsedSec > 0 ? elapsedSec : 0) * (double)minutesPerSec;
    m = std::fmod(m, 1440.0);
    if (m < 0) m += 1440.0;
    return (float)(m / 60.0);
}

bool Curve::sample(float hm, Rgb* out) const {
    if (!n) return false;
    if (n == 1) { *out = {(float)rgb[0][0], (float)rgb[0][1], (float)rgb[0][2]}; return true; }
    // the key before hm (the last one of the previous day when hm is before the first), the key after it
    int a = -1;
    for (int k = 0; k < n; ++k) if ((float)t[k] <= hm) a = k;
    float t0, t1;
    int ia, ib;
    if (a < 0) { ia = n - 1; ib = 0; t0 = (float)t[ia] - 2880.0f; t1 = (float)t[ib]; }
    else if (a == n - 1) { ia = a; ib = 0; t0 = (float)t[ia]; t1 = (float)t[ib] + 2880.0f; }
    else { ia = a; ib = a + 1; t0 = (float)t[ia]; t1 = (float)t[ib]; }
    float f = t1 > t0 ? (hm - t0) / (t1 - t0) : 0.0f;
    f = std::clamp(f, 0.0f, 1.0f);
    out->r = rgb[ia][0] + (rgb[ib][0] - rgb[ia][0]) * f;
    out->g = rgb[ia][1] + (rgb[ib][1] - rgb[ia][1]) * f;
    out->b = rgb[ia][2] + (rgb[ib][2] - rgb[ia][2]) * f;
    return true;
}

bool SkyTable::open(const char* path) {
    path_.clear();
    vols_.clear();
    sets_.clear();
    map_ = 0xFFFFFFFFu;
    FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return false;
    std::fclose(f);
    path_ = path;
    return true;
}

const SkyTable::Set* SkyTable::find(uint32_t params) const {
    for (const Set& s : sets_) if (s.params == params) return &s;
    return nullptr;
}

bool SkyTable::setMap(uint32_t map) {
    if (path_.empty()) return false;
    if (map == map_) return !vols_.empty();
    map_ = map;
    vols_.clear();
    sets_.clear();
    FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) return false;
    char line[512];
    // pass 1: the volumes of this map
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] != 'L' || line[1] != '\t') continue;
        unsigned m = 0, p = 0;
        float x = 0, y = 0, in = 0, out = 0;
        if (std::sscanf(line + 2, "%u %f %f %f %f %u", &m, &x, &y, &in, &out, &p) != 6 || m != map || !p) continue;
        vols_.push_back({x, y, in, out, p});
        bool have = false;
        for (const Set& s : sets_) have = have || s.params == p;
        if (!have) { Set s; s.params = p; sets_.push_back(s); }
    }
    // pass 2: their curves (and sky models)
    std::rewind(f);
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == 'B' && line[1] == '\t') {
            unsigned p2 = 0, box = 0;
            if (std::sscanf(line + 2, "%u %u", &p2, &box) == 2)
                for (Set& s : sets_) if (s.params == p2) s.skybox = box;
            continue;
        }
        if (line[0] != 'C' || line[1] != '\t') continue;
        char* p = line + 2;
        char* end = nullptr;
        const unsigned params = (unsigned)std::strtoul(p, &end, 10);
        if (end == p) continue;
        Set* set = nullptr;
        for (Set& s : sets_) if (s.params == params) set = &s;
        if (!set) continue;
        p = end;
        const unsigned ch = (unsigned)std::strtoul(p, &end, 10);
        if (end == p) continue;
        p = end;
        const unsigned n = (unsigned)std::strtoul(p, &end, 10);
        if (end == p || !n || n > 16) continue;
        p = end;
        const int slot = ch == 2 ? 0 : ch == 3 ? 1 : ch == 4 ? 2 : ch == 5 ? 3 : ch == 7 ? 4 : -1;
        if (slot < 0) continue;
        Curve c;
        for (unsigned k = 0; k < n; ++k) {
            while (*p == '\t' || *p == ' ') ++p;
            unsigned t = (unsigned)std::strtoul(p, &end, 10);
            if (end == p || *end != ':') { c.n = 0; break; }
            p = end + 1;
            unsigned rgb = (unsigned)std::strtoul(p, &end, 16);
            if (end == p) { c.n = 0; break; }
            p = end;
            c.t[k] = (uint16_t)std::min(t, 2879u);
            c.rgb[k][0] = (uint8_t)(rgb >> 16);
            c.rgb[k][1] = (uint8_t)(rgb >> 8);
            c.rgb[k][2] = (uint8_t)rgb;
            c.n = (uint8_t)(k + 1);
        }
        if (c.n == n) set->c[slot] = c;
    }
    std::fclose(f);
    return !vols_.empty();
}

uint32_t SkyTable::skyboxAt(float x, float y) const {
    const Vol* pick = nullptr;
    const Vol* global = nullptr;
    for (const Vol& v : vols_) {
        if (v.outer <= 0) { if (!global) global = &v; continue; }
        const float d = std::hypot(x - v.x, y - v.y);
        float w = 0;
        if (d <= v.inner) w = 1.0f;
        else if (d < v.outer) {
            const float t = std::clamp((d - v.inner) / (v.outer - v.inner), 0.0f, 1.0f);
            w = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        if (w >= 0.5f && (!pick || v.outer < pick->outer || (v.outer == pick->outer && v.params < pick->params))) pick = &v;
    }
    const Vol* v = pick ? pick : global;
    const Set* s = v ? find(v->params) : nullptr;
    return s ? s->skybox : 0;
}

bool SkyTable::colors(float x, float y, float hour, Colors* out) const {
    if (vols_.empty()) return false;
    float hm = std::fmod(hour, 24.0f) * 120.0f;
    if (hm < 0) hm += 2880.0f;
    // the volumes around the spot, smallest first (the most specific; the id breaks ties so the order cannot flicker); each takes its
    // smoothstep weight of what is left, the global volume (no radius) the rest
    struct W { const Vol* v; float w; };
    std::vector<W> near;
    const Vol* global = nullptr;
    for (const Vol& v : vols_) {
        if (v.outer <= 0) { if (!global) global = &v; continue; }
        const float d = std::hypot(x - v.x, y - v.y);
        float w = 0;
        if (d <= v.inner) w = 1.0f;
        else if (d < v.outer) {
            const float t = std::clamp((d - v.inner) / (v.outer - v.inner), 0.0f, 1.0f);
            w = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        if (w > 0) near.push_back({&v, w});
    }
    std::sort(near.begin(), near.end(), [](const W& a, const W& b) {
        if (a.v->outer != b.v->outer) return a.v->outer < b.v->outer;
        return a.v->params < b.v->params;
    });
    Colors acc{};
    float rest = 1.0f;
    bool any = false;
    auto add = [&](const Vol* v, float share) {
        const Set* s = find(v->params);
        if (!s) return;
        Rgb c[5];
        for (int k = 0; k < 5; ++k) if (!s->c[k].sample(hm, &c[k])) return;   // (a set without all five curves is no sky)
        Rgb* dst[5] = {&acc.top, &acc.mid, &acc.band1, &acc.band2, &acc.fog};
        for (int k = 0; k < 5; ++k) { dst[k]->r += c[k].r * share; dst[k]->g += c[k].g * share; dst[k]->b += c[k].b * share; }
        rest -= share;
        any = true;
    };
    for (const W& w : near) {
        if (rest <= 0.001f) break;
        add(w.v, rest * w.w);
    }
    if (global && rest > 0.001f) add(global, rest);
    if (!any) return false;
    if (rest > 0.001f) {   // no global light and the zones did not cover everything: scale up what there is
        const float k = 1.0f / (1.0f - rest);
        Rgb* dst[5] = {&acc.top, &acc.mid, &acc.band1, &acc.band2, &acc.fog};
        for (Rgb* d : dst) { d->r *= k; d->g *= k; d->b *= k; }
    }
    *out = acc;
    return true;
}

static Body arcAt(float hour, float* elDeg) {
    const float a = (hour - 6.0f) / 12.0f * 3.14159265f;
    const float sa = std::sin(a);
    float x = -0.3f * sa, y = -std::cos(a), z = 0.9f * sa;
    const float l = std::sqrt(x * x + y * y + z * z);
    Body b;
    b.dir[0] = x / l; b.dir[1] = y / l; b.dir[2] = z / l;
    *elDeg = std::asin(std::clamp(b.dir[2], -1.0f, 1.0f)) * 57.2957795f;
    b.strength = std::clamp((*elDeg + 2.0f) / 8.0f, 0.0f, 1.0f);
    return b;
}

Body sun(float hour) {
    float el;
    return arcAt(hour, &el);
}

Body moon(float hour) {
    float el, sunEl;
    arcAt(hour, &sunEl);
    Body b = arcAt(hour + 12.0f, &el);
    const float day = std::clamp((sunEl + 6.0f) / 12.0f, 0.0f, 1.0f);   // (the moon is still there at dawn, just faint)
    b.strength *= 1.0f - 0.75f * day;
    return b;
}

Rgb ghostGrade(const Rgb& c, float amount) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    const float grey = 0.30f * c.r + 0.59f * c.g + 0.11f * c.b;
    const float t = 0.85f * a, dark = 1.0f - 0.25f * a;
    Rgb o;
    o.r = std::clamp((c.r + (grey - c.r) * t) * dark, 0.0f, 255.0f);
    o.g = std::clamp((c.g + (grey - c.g) * t) * dark + 4.0f * a, 0.0f, 255.0f);
    o.b = std::clamp((c.b + (grey - c.b) * t) * dark + 12.0f * a, 0.0f, 255.0f);
    return o;
}

}  // namespace sky
