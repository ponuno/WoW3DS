// spellfx.cpp — spell visuals for the 3DS (stage 5.33): Spell.dbc -> SpellVisual.dbc -> SpellVisualKit.dbc ->
// SpellVisualEffectName.dbc, as WoWee's SpellVisualSystem::loadSpellVisualDbc reads them (the same columns: SpellVisual
// 1 precast kit, 2 cast kit, 3 impact kit, 8 missile model; a kit's effect columns 3 head, 4 chest, 5 base, 6 left hand,
// 7 right hand, 8 breath, 11..13 special; SpellVisualEffectName 2 the model file). Each effect model goes into the pack
// as an ordinary MODL (small textures, few triangles, its particle emitters and glows behind the footer of w3d_fx.h),
// and one SVFX entry tells the client which models a spell shows at each stage and where on the unit.
#include "conv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "pipeline/m2_loader.hpp"
#include "w3d_fx.h"

namespace w3d {

namespace {
struct KitFx { uint32_t effect[2] = {0, 0}; uint8_t slot[2] = {0, 0}; };

// The column of Spell.dbc holding the missile speed: a float column where the spells whose visual has a missile mostly
// say 5..100 yd/s and the others mostly 0 (1.12 has no layout file here; the column is found, not assumed).
int speedColumn(const DbcTable& spell, uint32_t visCol, const std::set<uint32_t>& missileVisuals) {
    int best = -1;
    double bestScore = 0;
    for (uint32_t c = 10; c < std::min<uint32_t>(spell.fields, visCol); ++c) {
        uint32_t withM = 0, okM = 0, without = 0, zeroW = 0;
        for (uint32_t r = 0; r < spell.records; ++r) {
            const float v = spell.f32(r, c);
            if (missileVisuals.count(spell.u32(r, visCol))) {
                ++withM;
                okM += std::isfinite(v) && v >= 5.0f && v <= 100.0f;
            } else {
                ++without;
                zeroW += v == 0.0f;
            }
        }
        if (withM < 10 || !without) continue;
        const double a = (double)okM / withM, z = (double)zeroW / without;
        if (a < 0.6 || z < 0.8) continue;
        if (a + z > bestScore) { bestScore = a + z; best = (int)c; }
    }
    return best;
}
}  // namespace

bool convertSpellEffects(const Options& oIn, const DataDir& data, PackWriter& pack, ConvStats& st) {
    DbcTable spell, sv, svk, fxn;
    if (!spell.load(data, "Spell.dbc") || !sv.load(data, "SpellVisual.dbc") || !svk.load(data, "SpellVisualKit.dbc") ||
        !fxn.load(data, "SpellVisualEffectName.dbc")) {
        std::fputs("spell effects: Spell / SpellVisual / SpellVisualKit / SpellVisualEffectName.dbc missing\n", stderr);
        return false;
    }
    if (fxn.fields < 3 || sv.fields < 9 || svk.fields < 8) {
        std::fputs("spell effects: unexpected table layout\n", stderr);
        return false;
    }
    // ---- SpellVisualEffectName: id -> model path, scale (column 4 when it holds sensible floats)
    std::map<uint32_t, std::pair<std::string, float>> names;
    bool scaleOk = fxn.fields > 4;
    if (scaleOk) {
        uint32_t good = 0;
        for (uint32_t r = 0; r < fxn.records; ++r) { const float s = fxn.f32(r, 4); good += std::isfinite(s) && s >= 0.05f && s <= 20.0f; }
        scaleOk = good * 10 >= fxn.records * 8;
    }
    for (uint32_t r = 0; r < fxn.records; ++r) {
        std::string p = fxn.str(r, 2);
        if (p.empty()) continue;
        float s = scaleOk ? fxn.f32(r, 4) : 1.0f;
        if (!(s >= 0.05f && s <= 20.0f)) s = 1.0f;
        names[fxn.u32(r, 0)] = {p, s};
    }
    // ---- SpellVisualKit: up to two effects per kit and where they hang (both hands, else the most telling one)
    struct Col { uint32_t c; uint8_t slot; };
    const Col cols[] = {{6, W3D_SLOT_LHAND}, {7, W3D_SLOT_RHAND}, {11, W3D_SLOT_CHEST}, {5, W3D_SLOT_BASE}, {4, W3D_SLOT_CHEST},
                        {3, W3D_SLOT_HEAD}, {8, W3D_SLOT_HEAD}, {12, W3D_SLOT_CHEST}, {13, W3D_SLOT_CHEST}};
    std::map<uint32_t, KitFx> kits;
    for (uint32_t r = 0; r < svk.records; ++r) {
        KitFx k;
        int n = 0;
        for (const Col& c : cols) {
            if (c.c >= svk.fields || n >= 2) continue;
            const uint32_t e = svk.u32(r, c.c);
            if (!e || !names.count(e)) continue;
            if (n == 1 && k.effect[0] == e && !((k.slot[0] == W3D_SLOT_LHAND && c.slot == W3D_SLOT_RHAND))) continue;
            k.effect[n] = e;
            k.slot[n] = c.slot;
            ++n;
        }
        if (n) kits[svk.u32(r, 0)] = k;
    }
    // ---- SpellVisual: four stages
    struct Vis { uint32_t id; KitFx st[W3D_SFX_STAGES]; };
    std::map<uint32_t, Vis> visuals;
    std::set<uint32_t> missileVisuals;
    for (uint32_t r = 0; r < sv.records; ++r) {
        Vis v{};
        v.id = sv.u32(r, 0);
        bool any = false;
        // 1 precast, 2 cast, 3 impact kit; 4 state kit (the aura on a unit), 6 channel kit (stage 5.34)
        const struct { uint32_t col; int stage; } kitCol[5] = {{1, W3D_SFX_PRECAST}, {2, W3D_SFX_CAST}, {3, W3D_SFX_IMPACT},
                                                                {4, W3D_SFX_STATE}, {6, W3D_SFX_CHANNEL}};
        for (const auto& kc : kitCol) {
            if (kc.col >= sv.fields) continue;
            auto it = kits.find(sv.u32(r, kc.col));
            if (it != kits.end()) { v.st[kc.stage] = it->second; any = true; }
        }
        const uint32_t mis = sv.u32(r, 8);
        if (mis && names.count(mis)) {
            v.st[W3D_SFX_MISSILE].effect[0] = mis;
            v.st[W3D_SFX_MISSILE].slot[0] = W3D_SLOT_NONE;
            missileVisuals.insert(v.id);
            any = true;
        }
        if (any && v.id) visuals[v.id] = v;
    }
    // ---- Spell.dbc: spell -> visual (5 columns before the name, as the sounds), missile speed
    int nameCol = spellNameColumn(spell);
    if (nameCol < 0) nameCol = 120;
    const uint32_t visCol = (uint32_t)nameCol - 5;
    const int spCol = speedColumn(spell, visCol, missileVisuals);
    std::map<uint32_t, std::pair<uint32_t, float>> spells;   // spell -> (visual, speed)
    std::set<uint32_t> usedVisuals;
    for (uint32_t r = 0; r < spell.records; ++r) {
        const uint32_t v = spell.u32(r, visCol);
        if (!visuals.count(v)) continue;
        float sp = spCol >= 0 ? spell.f32(r, (uint32_t)spCol) : 0.0f;
        if (!(sp > 0.0f && sp < 200.0f)) sp = 0.0f;
        spells[spell.u32(r, 0)] = {v, sp};
        usedVisuals.insert(v);
    }
    // ---- the effect models: once each, small
    Options o = oIn;
    o.fxSpell = true;
    o.noFx = false;
    o.modelTex = std::min(o.modelTex, 64);
    o.maxModelTris = std::min(o.maxModelTris > 0 ? o.maxModelTris : 300, 300);
    std::map<uint32_t, uint16_t> effectIndex;   // SpellVisualEffectName id -> index in the effect table
    std::vector<W3dSpellEffect> effects;
    size_t failed = 0, done = 0, total = 0;
    for (uint32_t v : usedVisuals)
        for (const KitFx& k : visuals[v].st)
            for (uint32_t e : k.effect) total += e && !effectIndex.count(e) ? 1 : 0;
    for (uint32_t v : usedVisuals)
        for (const KitFx& k : visuals[v].st)
            for (uint32_t e : k.effect) {
                if (!e || effectIndex.count(e) || effects.size() >= 0xFFFE) continue;
                ++done;
                progress(false, "spell effects: %zu/%zu models", done, total);
                const auto& [path, scale] = names[e];
                const uint32_t id = addModelFromGame(pack, data, path, o, st);
                if (!id) { ++failed; effectIndex[e] = 0xFFFF; continue; }
                effectIndex[e] = (uint16_t)effects.size();
                effects.push_back({id, scale});
            }
    // ---- two effects that are not spells (stage 5.34): the level-up flash and the loot sparkle (paths as WoWee's
    // levelup_effect / loot_sparkles)
    auto special = [&](const char* gamePath) -> uint16_t {
        if (effects.size() >= 0xFFFE) return W3D_SFX_NONE;
        const uint32_t id = addModelFromGame(pack, data, gamePath, o, st);
        if (!id) return W3D_SFX_NONE;
        effects.push_back({id, 1.0f});
        return (uint16_t)(effects.size() - 1);
    };
    const uint16_t levelUp = special("Spells\\LevelUp\\LevelUp.m2");
    const uint16_t loot = special("Particles\\LootFX.m2");
    // ---- the table
    std::vector<W3dSpellVisual> outVis;
    std::map<uint32_t, uint16_t> visIndex;
    for (uint32_t v : usedVisuals) {
        const Vis& src = visuals[v];
        W3dSpellVisual o2{};
        o2.id = v;
        bool any = false;
        for (int s = 0; s < W3D_SFX_STAGES; ++s)
            for (int k = 0; k < 2; ++k) {
                o2.effect[s][k] = W3D_SFX_NONE;
                const uint32_t e = src.st[s].effect[k];
                auto it = e ? effectIndex.find(e) : effectIndex.end();
                if (it == effectIndex.end() || it->second == 0xFFFF) continue;
                o2.effect[s][k] = it->second;
                o2.slot[s][k] = src.st[s].slot[k];
                any = true;
            }
        if (!any || outVis.size() >= 0xFFFF) continue;
        visIndex[v] = (uint16_t)outVis.size();
        outVis.push_back(o2);
    }
    std::vector<W3dSpellRef> outSpells;
    for (const auto& [sid, vs] : spells) {
        auto it = visIndex.find(vs.first);
        if (it == visIndex.end()) continue;
        W3dSpellRef r{};
        r.spell = sid;
        r.visual = it->second;
        r.speed = vs.second;
        outSpells.push_back(r);
    }
    Blob b;
    W3dSpellFxHeader h{};
    h.magic = W3D_SVFX_MAGIC;
    b.put(h);
    b.align(4);
    const size_t oe = b.putArray(effects);
    b.align(4);
    const size_t ov = b.putArray(outVis);
    b.align(4);
    const size_t os = b.putArray(outSpells);
    auto* hp = b.at<W3dSpellFxHeader>(0);
    hp->effectCount = (uint32_t)effects.size();
    hp->visualCount = (uint32_t)outVis.size();
    hp->spellCount = (uint32_t)outSpells.size();
    hp->offEffects = (uint32_t)oe;
    hp->offVisuals = (uint32_t)ov;
    hp->offSpells = (uint32_t)os;
    hp->levelUp = levelUp;
    hp->loot = loot;
    pack.add(W3D_ENTRY_SPELLFX, 0, std::move(b.data));
    progressDone("spell effects: %zu spells, %zu visuals, %zu effect models (%zu failed), missile speed column %d, level-up %s, loot %s",
                 outSpells.size(), outVis.size(), effects.size(), failed, spCol, levelUp != W3D_SFX_NONE ? "yes" : "no",
                 loot != W3D_SFX_NONE ? "yes" : "no");
    return true;
}

}  // namespace w3d
