// Spell descriptions for the 3DS client (6.20): Spell.dbc Description / ToolTip with the variables of the original client
// filled in, written to spelldesc.bin (format in conv.h).
//
// The variables (Spell.dbc strings of 1.12, as the original client's tooltips show them):
//   $s1..3 effect value ("min to max" when the effect rolls a die), $m1 min, $M1 max, $o1 total of a periodic effect
//   (value * duration / period), $d duration, $a1 / $A1 radius (min / max, yards), $t1 period (s), $h proc chance, $n proc
//   charges, $u stack, $x1 chain targets, $i max targets, $r range, $b1 points per combo point, $e1 multiple value, $q1 misc
//   value; "$12345s1" asks another spell; ${expr} arithmetic (+ - * / and parentheses) with an optional ".N" decimals after it;
//   $lsingular:plural; by the last number, $gmale:female; (the first), $?s123[a][b] conditions (the player's own state is
//   unknown here: the else branch is taken). What cannot be resolved is left out, never printed as "$s1".
#include "conv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace w3d {

namespace {

// ------------------------------------------------------------------ the values
int32_t effMin(const SpellTextVals& v, int k) {
    const uint32_t lvl = v.maxLevel ? std::min(std::max(v.spellLevel, v.baseLevel), v.maxLevel) : std::max(v.spellLevel, v.baseLevel);
    int32_t b = v.base[k] + (int32_t)std::lround(v.perLevel[k] * (float)(lvl - v.baseLevel));
    return b + (v.dieSides[k] >= 1 ? 1 : 0);
}
int32_t effMax(const SpellTextVals& v, int k) {
    const int32_t lo = effMin(v, k);
    return v.dieSides[k] > 1 ? lo - 1 + v.dieSides[k] : lo;
}

std::string num(double x, int decimals = -1) {
    char b[48];
    if (decimals >= 0) std::snprintf(b, sizeof b, "%.*f", decimals, x);
    else if (std::fabs(x - std::round(x)) < 0.005) std::snprintf(b, sizeof b, "%lld", (long long)std::llround(x));
    else std::snprintf(b, sizeof b, "%.1f", x);
    return b;
}

std::string durationText(int32_t ms) {
    if (ms < 0) return "until cancelled";
    const double s = ms / 1000.0;
    if (s < 60) return num(s) + " sec";
    if (s < 3600) return num(s / 60.0) + " min";
    if (s < 86400) return num(s / 3600.0) + " hr";
    return num(s / 86400.0) + " days";
}

// ------------------------------------------------------------------ the resolver
struct Ctx {
    const SpellTextVals* v;
    const std::function<const SpellTextVals*(uint32_t)>* other;
    int depth;
    double last = 1;   // the last number printed ($l picks singular / plural by it)
};

std::string resolve(const std::string& t, Ctx& c);

// A variable letter (and effect digit) at t[i] after the "$" (and an optional spell id) -> its text and number. i advances.
bool variable(const std::string& t, size_t& i, const SpellTextVals& v, std::string* text, double* val) {
    if (i >= t.size()) return false;
    const char l = t[i++];
    int k = 0;   // effect index 0..2
    bool hasIdx = false;
    if (i < t.size() && t[i] >= '1' && t[i] <= '3') { k = t[i++] - '1'; hasIdx = true; }
    (void)hasIdx;
    double x = 0;
    std::string s;
    switch (l) {
        case 's': case 'S': {
            const int32_t lo = std::abs(effMin(v, k)), hi = std::abs(effMax(v, k));
            x = lo;
            s = hi != lo ? num(lo) + " to " + num(hi) : num(lo);
            break;
        }
        case 'm': x = std::abs(effMin(v, k)); break;
        case 'M': x = std::abs(effMax(v, k)); break;
        case 'o': case 'O': {
            x = std::abs(effMin(v, k));
            if (v.amplitudeMs[k] > 0 && v.durationMs > 0) x = x * ((double)v.durationMs / (double)v.amplitudeMs[k]);
            break;
        }
        case 'd': case 'D': s = durationText(v.durationMs); x = v.durationMs < 0 ? 0 : v.durationMs / 1000.0; break;
        case 'a': x = v.radiusMin[k]; break;
        case 'A': x = v.radiusMax[k]; break;
        case 't': case 'T': x = v.amplitudeMs[k] / 1000.0; break;
        case 'h': case 'H': x = v.procChance; break;
        case 'n': case 'N': x = v.procCharges; break;
        case 'u': case 'U': x = v.stack; break;
        case 'x': case 'X': x = v.chain[k]; break;
        case 'i': case 'I': x = v.maxTargets; break;
        case 'r': case 'R': x = v.rangeMax; break;
        case 'b': case 'B': x = std::fabs(v.combo[k]); break;
        case 'e': case 'E': x = v.multiple[k]; break;
        case 'q': case 'Q': x = v.misc[k]; break;
        default: return false;
    }
    *val = x;
    *text = s.empty() ? num(x) : s;
    return true;
}

// ${...}: + - * / ( ) and $variables, numbers
struct Expr {
    const std::string& s;
    size_t i = 0;
    Ctx& c;
    const SpellTextVals* v;
    explicit Expr(const std::string& str, Ctx& ctx) : s(str), c(ctx), v(ctx.v) {}
    void ws() { while (i < s.size() && s[i] == ' ') ++i; }
    double primary() {
        ws();
        if (i >= s.size()) return 0;
        if (s[i] == '(') { ++i; double r = sum(); ws(); if (i < s.size() && s[i] == ')') ++i; return r; }
        if (s[i] == '-') { ++i; return -primary(); }
        if (s[i] == '$') {
            ++i;
            const SpellTextVals* use = v;
            size_t j = i;
            uint32_t id = 0;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) id = id * 10 + (uint32_t)(s[j++] - '0');
            if (j > i && j < s.size() && std::isalpha((unsigned char)s[j])) {   // $12345s1: another spell
                use = c.other && *c.other ? (*c.other)(id) : nullptr;
                i = j;
            }
            std::string txt;
            double x = 0;
            if (use) variable(s, i, *use, &txt, &x);
            else ++i;
            return x;
        }
        size_t j = i;
        while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) ++j;
        double x = j > i ? std::atof(s.substr(i, j - i).c_str()) : 0;
        i = j > i ? j : i + 1;
        return x;
    }
    double term() {
        double r = primary();
        for (;;) {
            ws();
            if (i < s.size() && s[i] == '*') { ++i; r *= primary(); }
            else if (i < s.size() && s[i] == '/') { ++i; const double d = primary(); r = d != 0 ? r / d : 0; }
            else return r;
        }
    }
    double sum() {
        double r = term();
        for (;;) {
            ws();
            if (i < s.size() && s[i] == '+') { ++i; r += term(); }
            else if (i < s.size() && s[i] == '-') { ++i; r -= term(); }
            else return r;
        }
    }
};

// the end of a bracketed part starting at t[i] == open (nesting counted); returns the index of the closer or npos
size_t matching(const std::string& t, size_t i, char open, char close) {
    int depth = 0;
    for (size_t k = i; k < t.size(); ++k) {
        if (t[k] == open) ++depth;
        else if (t[k] == close && --depth == 0) return k;
    }
    return std::string::npos;
}

std::string resolve(const std::string& t, Ctx& c) {
    std::string out;
    size_t i = 0;
    while (i < t.size()) {
        const char ch = t[i];
        if (ch != '$') { out += ch; ++i; continue; }
        ++i;
        if (i >= t.size()) break;
        const char n = t[i];
        if (n == '$') { out += '$'; ++i; continue; }
        if (n == '{') {   // arithmetic
            const size_t e = matching(t, i, '{', '}');
            if (e == std::string::npos) break;
            const std::string inner = t.substr(i + 1, e - i - 1);
            Expr ex(inner, c);
            const double x = ex.sum();
            i = e + 1;
            int dec = -1;
            if (i + 1 < t.size() && t[i] == '.' && std::isdigit((unsigned char)t[i + 1])) { dec = t[i + 1] - '0'; i += 2; }
            c.last = x;
            out += dec >= 0 ? num(x, dec) : num(std::round(x));
            continue;
        }
        if (n == '?') {   // $?s123[then]?a45[then][else]: the player's state is unknown, so every condition fails: the plain [else]
            size_t k = i;
            std::string chosen;
            for (;;) {
                if (k < t.size() && t[k] == '?') {   // a condition and its [then]: skipped
                    while (k < t.size() && t[k] != '[') ++k;
                    const size_t e = k < t.size() ? matching(t, k, '[', ']') : std::string::npos;
                    if (e == std::string::npos) { k = t.size(); break; }
                    k = e + 1;
                } else if (k < t.size() && t[k] == '[') {   // the plain bracket: the else text
                    const size_t e = matching(t, k, '[', ']');
                    if (e == std::string::npos) { k = t.size(); break; }
                    chosen = t.substr(k + 1, e - k - 1);
                    k = e + 1;
                    break;
                } else break;
            }
            i = k;
            out += resolve(chosen, c);
            continue;
        }
        if (n == 'l' || n == 'L') {   // $lsingular:plural;
            const size_t e = t.find(';', i);
            if (e == std::string::npos) break;
            const std::string body = t.substr(i + 1, e - i - 1);
            const size_t colon = body.find(':');
            const std::string sing = colon == std::string::npos ? body : body.substr(0, colon);
            const std::string plur = colon == std::string::npos ? body : body.substr(colon + 1);
            out += std::fabs(c.last - 1.0) < 0.001 ? sing : plur;
            i = e + 1;
            continue;
        }
        if (n == 'g' || n == 'G') {   // $gmale:female;
            const size_t e = t.find(';', i);
            if (e == std::string::npos) break;
            const std::string body = t.substr(i + 1, e - i - 1);
            const size_t colon = body.find(':');
            out += colon == std::string::npos ? body : body.substr(0, colon);
            i = e + 1;
            continue;
        }
        // $12345s1 / $s1
        const SpellTextVals* use = c.v;
        size_t j = i;
        uint32_t id = 0;
        while (j < t.size() && std::isdigit((unsigned char)t[j])) id = id * 10 + (uint32_t)(t[j++] - '0');
        if (j > i && j < t.size() && std::isalpha((unsigned char)t[j])) {
            use = c.other && *c.other && c.depth < 3 ? (*c.other)(id) : nullptr;
            i = j;
            if (!use) { ++i; if (i < t.size() && t[i] >= '1' && t[i] <= '3') ++i; continue; }
        }
        std::string txt;
        double x = 0;
        if (variable(t, i, *use, &txt, &x)) { out += txt; c.last = x; }
        else if (i < t.size()) ++i;   // an unknown variable letter: dropped
    }
    return out;
}

std::string tidy(std::string s) {
    std::string o;
    bool space = false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '|' && i + 1 < s.size() && s[i + 1] == 'c') { i += 9; continue; }   // |cffRRGGBB
        if (ch == '|' && i + 1 < s.size() && (s[i + 1] == 'r' || s[i + 1] == 'n')) { ++i; continue; }
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') { space = true; continue; }
        if (space && !o.empty()) o += ' ';
        space = false;
        o += (char)ch;
    }
    return o;
}

void put32(std::string& s, uint32_t v) { s.append((const char*)&v, 4); }   // little endian hosts (the converter is x86)

}  // namespace

std::string resolveSpellText(const std::string& text, const SpellTextVals& v,
                             const std::function<const SpellTextVals*(uint32_t)>& other, int depth) {
    Ctx c{&v, &other, depth};
    return tidy(resolve(text, c));
}

bool exportSpellDescriptions(const DataDir& data, const std::string& path, bool verbose) {
    DbcTable spell;
    if (!spell.load(data, "Spell.dbc")) {
        std::fprintf(stderr, "spelldesc: Spell.dbc not found (no spelldesc.bin)\n");
        return false;
    }
    int nameCol = spellNameColumn(spell);
    if (nameCol < 0) nameCol = 120;
    const uint32_t nc = (uint32_t)nameCol;
    if (nc < 95 || nc + 27 >= spell.fields) {
        std::fprintf(stderr, "spelldesc: Spell.dbc layout not understood (no spelldesc.bin)\n");
        return false;
    }
    const uint32_t descCol = nc + 18, tipCol = nc + 27;
    DbcTable dur, rad, rng;
    std::unordered_map<uint32_t, int32_t> durOf;
    std::unordered_map<uint32_t, std::pair<float, float>> radOf;
    std::unordered_map<uint32_t, float> rngOf;
    if (dur.load(data, "SpellDuration.dbc") && dur.fields > 1)
        for (uint32_t r = 0; r < dur.records; ++r) durOf[dur.u32(r, 0)] = (int32_t)dur.u32(r, 1);
    if (rad.load(data, "SpellRadius.dbc") && rad.fields > 3)
        for (uint32_t r = 0; r < rad.records; ++r) radOf[rad.u32(r, 0)] = {rad.f32(r, 1), rad.f32(r, 3)};
    if (rng.load(data, "SpellRange.dbc") && rng.fields > 2)
        for (uint32_t r = 0; r < rng.records; ++r) rngOf[rng.u32(r, 0)] = rng.f32(r, 2);
    std::unordered_map<uint32_t, uint32_t> rowOf;
    for (uint32_t r = 0; r < spell.records; ++r) rowOf[spell.u32(r, 0)] = r;

    // Columns of the 1.12 Spell.dbc relative to the name column (120): the ones the other exports use, and
    // ProcChance 25, ProcCharges 26, MaxLevel 27, BaseLevel 28, SpellLevel 29, DurationIndex 30, RangeIndex 36, StackAmount 39,
    // DieSides 64, RealPointsPerLevel 73, BasePoints 76, RadiusIndex 88, Amplitude 94, MultipleValue 97, ChainTarget 100,
    // MiscValue 106, PointsPerComboPoint 112, MaxAffectedTargets 163.
    auto colOf = [&](uint32_t abs120) { return nc + abs120 - 120; };
    auto values = [&](uint32_t row) {
        SpellTextVals v;
        v.procChance = spell.u32(row, colOf(25));
        v.procCharges = spell.u32(row, colOf(26));
        v.maxLevel = spell.u32(row, colOf(27));
        v.baseLevel = spell.u32(row, colOf(28));
        v.spellLevel = spell.u32(row, colOf(29));
        auto d = durOf.find(spell.u32(row, colOf(30)));
        v.durationMs = d == durOf.end() ? 0 : d->second;
        auto g = rngOf.find(spell.u32(row, colOf(36)));
        v.rangeMax = g == rngOf.end() ? 0.0f : g->second;
        v.stack = spell.u32(row, colOf(39));
        v.maxTargets = colOf(163) < spell.fields ? spell.u32(row, colOf(163)) : 0;
        for (uint32_t k = 0; k < 3; ++k) {
            v.dieSides[k] = (int32_t)spell.u32(row, colOf(64) + k);
            v.perLevel[k] = spell.f32(row, colOf(73) + k);
            v.base[k] = (int32_t)spell.u32(row, colOf(76) + k);
            auto r = radOf.find(spell.u32(row, colOf(88) + k));
            if (r != radOf.end()) { v.radiusMin[k] = r->second.first; v.radiusMax[k] = r->second.second; }
            v.amplitudeMs[k] = spell.u32(row, colOf(94) + k);
            v.multiple[k] = spell.f32(row, colOf(97) + k);
            v.chain[k] = spell.u32(row, colOf(100) + k);
            v.misc[k] = (int32_t)spell.u32(row, colOf(106) + k);
            v.combo[k] = spell.f32(row, colOf(112) + k);
        }
        return v;
    };
    std::unordered_map<uint32_t, SpellTextVals> cache;
    std::function<const SpellTextVals*(uint32_t)> other = [&](uint32_t id) -> const SpellTextVals* {
        auto it = rowOf.find(id);
        if (it == rowOf.end()) return nullptr;
        auto c = cache.find(id);
        if (c == cache.end()) c = cache.emplace(id, values(it->second)).first;
        return &c->second;
    };

    struct E { uint32_t id; std::string text; };
    std::vector<E> list;
    size_t withTip = 0;
    for (uint32_t r = 0; r < spell.records; ++r) {
        const uint32_t id = spell.u32(r, 0);
        if (!id) continue;
        std::string raw = spell.str(r, descCol);
        if (raw.empty()) { raw = spell.str(r, tipCol); withTip += !raw.empty(); }
        if (raw.empty()) continue;
        const SpellTextVals* v = other(id);
        std::string t = resolveSpellText(raw, *v, other, 1);
        if (t.size() > 600) t.resize(600);
        if (!t.empty()) list.push_back({id, t});
    }
    std::sort(list.begin(), list.end(), [](const E& a, const E& b) { return a.id < b.id; });
    std::string out = "SDSC";
    put32(out, 1);
    put32(out, (uint32_t)list.size());
    uint32_t off = (uint32_t)(out.size() + list.size() * 12);
    for (const E& e : list) { put32(out, e.id); put32(out, off); put32(out, (uint32_t)e.text.size()); off += (uint32_t)e.text.size(); }
    for (const E& e : list) out += e.text;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("  spell descriptions: %zu written to %s (%zu from the buff tooltip), %zu KB\n", list.size(), path.c_str(), withTip, out.size() / 1024);
    (void)verbose;
    return true;
}

}  // namespace w3d
