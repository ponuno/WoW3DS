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
#include <cstring>
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
    // 1.12 (no weapon effects yet): 3 head, 4 chest, 5 base, 6 left hand, 7 right hand, 8 breath, 9..11 special, 12 world,
    // 13 the kit's sound (5.35b: WoWee's 11..13 for the specials read sound ids as effects). A column is used only when
    // most of its values name an effect: a column of sounds or flags never does.
    const Col cols[] = {{6, W3D_SLOT_LHAND}, {7, W3D_SLOT_RHAND}, {9, W3D_SLOT_CHEST}, {5, W3D_SLOT_BASE}, {4, W3D_SLOT_CHEST},
                        {3, W3D_SLOT_HEAD}, {8, W3D_SLOT_HEAD}, {10, W3D_SLOT_CHEST}, {11, W3D_SLOT_CHEST}, {12, W3D_SLOT_BASE}};
    bool colOk[64] = {};
    for (const Col& c : cols) {
        if (c.c >= svk.fields || c.c >= 64) continue;
        uint32_t nz = 0, ok = 0;
        for (uint32_t r = 0; r < svk.records; ++r)
            if (const uint32_t e = svk.u32(r, c.c); e && e != 0xFFFFFFFFu) { ++nz; ok += names.count(e) ? 1 : 0; }   // -1 = none
        colOk[c.c] = nz && ok * 2 >= nz;
    }
    std::map<uint32_t, KitFx> kits;
    for (uint32_t r = 0; r < svk.records; ++r) {
        KitFx k;
        int n = 0;
        for (const Col& c : cols) {
            if (c.c >= svk.fields || c.c >= 64 || !colOk[c.c] || n >= 2) continue;
            const uint32_t e = svk.u32(r, c.c);
            if (!e || !names.count(e)) continue;
            if (n == 1 && k.effect[0] == e && !((k.slot[0] == W3D_SLOT_LHAND && c.slot == W3D_SLOT_RHAND))) continue;
            k.effect[n] = e;
            k.slot[n] = c.slot;
            ++n;
        }
        if (n) kits[svk.u32(r, 0)] = k;
    }
    // ---- SpellVisual. 1.12 as the data shows it (--explain-spell on a real client): 1 precast, 2 cast, 3 impact, 4 state,
    // 5 state done kit, 6 "has a missile" (0/1), 7 the missile model, 8 path type, 9 destination attachment, 10 missile
    // sound. (5.35f: the missile was read from 8 and the flag in 6 as a channel kit - no Fireball had a missile.) The
    // missile column is the one of 7 / 8 whose values name effects; a channel kit column only when one really holds kits
    // (many different ones) - else a channel shows the precast kit.
    auto empty = [](uint32_t v) { return v == 0 || v == 0xFFFFFFFFu; };
    int misCol = -1;
    {
        uint32_t best = 0;
        for (uint32_t c : {7u, 8u}) {
            if (c >= sv.fields) continue;
            uint32_t nz = 0, ok = 0;
            for (uint32_t r = 0; r < sv.records; ++r)
                if (const uint32_t v = sv.u32(r, c); !empty(v)) { ++nz; ok += names.count(v) ? 1 : 0; }
            if (nz && ok * 2 >= nz && ok > best) { best = ok; misCol = (int)c; }
        }
    }
    int chanCol = -1;
    if (sv.fields > 6) {
        uint32_t nz = 0, ok = 0;
        std::set<uint32_t> distinct;
        for (uint32_t r = 0; r < sv.records; ++r)
            if (const uint32_t v = sv.u32(r, 6); !empty(v)) { ++nz; ok += kits.count(v) ? 1 : 0; distinct.insert(v); }
        if (nz && ok * 2 >= nz && distinct.size() >= 3) chanCol = 6;
    }
    struct Vis { uint32_t id; KitFx st[W3D_SFX_STAGES]; };
    std::map<uint32_t, Vis> visuals;
    std::set<uint32_t> missileVisuals;
    for (uint32_t r = 0; r < sv.records; ++r) {
        Vis v{};
        v.id = sv.u32(r, 0);
        bool any = false;
        const struct { int col; int stage; } kitCol[5] = {{1, W3D_SFX_PRECAST}, {2, W3D_SFX_CAST}, {3, W3D_SFX_IMPACT},
                                                           {4, W3D_SFX_STATE}, {chanCol, W3D_SFX_CHANNEL}};
        for (const auto& kc : kitCol) {
            if (kc.col < 0 || (uint32_t)kc.col >= sv.fields) continue;
            auto it = kits.find(sv.u32(r, (uint32_t)kc.col));
            if (it != kits.end()) { v.st[kc.stage] = it->second; any = true; }
        }
        const uint32_t mis = misCol >= 0 ? sv.u32(r, (uint32_t)misCol) : 0;
        if (!empty(mis) && names.count(mis)) {
            v.st[W3D_SFX_MISSILE].effect[0] = mis;
            v.st[W3D_SFX_MISSILE].slot[0] = W3D_SLOT_NONE;
            missileVisuals.insert(v.id);
            any = true;
        }
        if (any && v.id) visuals[v.id] = v;
    }
    std::printf("  spell effects: SpellVisual missile column %d, channel kit column %d\n", misCol, chanCol);
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
    std::printf("  spell effects: kit columns used:");
    for (const Col& c : cols) if (c.c < 64 && colOk[c.c]) std::printf(" %u", c.c);
    std::printf("; %zu kits with effects, %zu visuals, %zu spells with a visual\n", kits.size(), visuals.size(), spells.size());
    progressDone("spell effects: %zu spells, %zu visuals, %zu effect models (%zu failed), missile speed column %d, level-up %s, loot %s",
                 outSpells.size(), outVis.size(), effects.size(), failed, spCol, levelUp != W3D_SFX_NONE ? "yes" : "no",
                 loot != W3D_SFX_NONE ? "yes" : "no");
    std::printf("  spell effects: %zu spells, %zu visuals, %zu effect models (%zu failed), missile speed column %d, level-up %s, loot %s\n",
                 outSpells.size(), outVis.size(), effects.size(), failed, spCol, levelUp != W3D_SFX_NONE ? "yes" : "no",
                 loot != W3D_SFX_NONE ? "yes" : "no");
    std::printf("  spell effects: %zu particle emitters dropped (no rate / life), %zu models of particles only\n", st.fxEmittersDropped, st.fxParticleOnly);
    return true;
}

// 5.35f: what the chain Spell -> SpellVisual -> SpellVisualKit -> SpellVisualEffectName says for some spells (the
// conversion of real data showed no visual for Fireball): every column of each row on the way, to see where it breaks.
int explainSpells(const DataDir& data, const std::vector<uint32_t>& ids) {
    DbcTable spell, sv, svk, fxn;
    const bool ok = spell.load(data, "Spell.dbc") & sv.load(data, "SpellVisual.dbc") & svk.load(data, "SpellVisualKit.dbc") &
                    fxn.load(data, "SpellVisualEffectName.dbc");
    std::printf("Spell.dbc %u x %u, SpellVisual.dbc %u x %u, SpellVisualKit.dbc %u x %u, SpellVisualEffectName.dbc %u x %u\n",
                spell.records, spell.fields, sv.records, sv.fields, svk.records, svk.fields, fxn.records, fxn.fields);
    if (!ok) { std::puts("a table is missing"); return 1; }
    int nameCol = spellNameColumn(spell);
    std::printf("Spell name column %d -> visual column %d\n", nameCol, nameCol - 5);
    if (nameCol < 0) nameCol = 120;
    auto row = [](const DbcTable& t, uint32_t id) -> int {
        for (uint32_t r = 0; r < t.records; ++r) if (t.u32(r, 0) == id) return (int)r;
        return -1;
    };
    auto dump = [](const char* what, const DbcTable& t, int r, uint32_t from, uint32_t to) {
        std::printf("  %s row %d:", what, r);
        for (uint32_t c = from; c < std::min(to, t.fields); ++c) std::printf(" [%u]=%u", c, t.u32((uint32_t)r, c));
        std::printf("\n");
    };
    auto effect = [&](uint32_t e) {
        const int r = e ? row(fxn, e) : -1;
        if (r < 0) { std::printf("      effect %u: not in SpellVisualEffectName\n", e); return; }
        std::printf("      effect %u: name '%s' file '%s'\n", e, fxn.str((uint32_t)r, 1).c_str(), fxn.str((uint32_t)r, 2).c_str());
        // the model itself: geometry and particle emitters as the converter sees them
        const std::string m2 = wowee::pipeline::modelPathToM2(fxn.str((uint32_t)r, 2));
        std::vector<uint8_t> bytes;
        if (!data.read(m2, bytes)) { std::printf("        %s: not in the data\n", m2.c_str()); return; }
        wowee::pipeline::M2Model mdl = wowee::pipeline::M2Loader::load(bytes);
        uint32_t ver = 0;
        if (bytes.size() >= 8) std::memcpy(&ver, bytes.data() + 4, 4);
        std::printf("        %s: version %u, %zu vertices, %zu batches, %zu bones, %zu sequences, %zu particle emitters, %zu ribbons\n",
                    m2.c_str(), ver, mdl.vertices.size(), mdl.batches.size(), mdl.bones.size(), mdl.sequences.size(),
                    mdl.particleEmitters.size(), mdl.ribbonEmitters.size());
        auto tr = [](const wowee::pipeline::M2AnimationTrack& t) {
            char b[96];
            size_t seqs = t.sequences.size(), keys = 0;
            float first = 0;
            bool have = false;
            for (const auto& q : t.sequences) {
                keys += q.floatValues.size();
                if (!have && !q.floatValues.empty()) { first = q.floatValues[0]; have = true; }
            }
            std::snprintf(b, sizeof b, "%zu seq / %zu keys, first %s%.2f, global %d", seqs, keys, have ? "" : "none ", first,
                          (int)t.globalSequence);
            return std::string(b);
        };
        for (size_t k = 0; k < mdl.particleEmitters.size() && k < 6; ++k) {
            const auto& pe = mdl.particleEmitters[k];
            std::printf("          emitter %zu: bone %u, tex %u, blend %u, type %u, rate [%s], life [%s], speed [%s]\n", k, pe.bone,
                        pe.texture, pe.blendingType, pe.emitterType, tr(pe.emissionRate).c_str(), tr(pe.lifespan).c_str(),
                        tr(pe.emissionSpeed).c_str());
        }
    };
    for (uint32_t id : ids) {
        std::printf("spell %u\n", id);
        const int r = row(spell, id);
        if (r < 0) { std::puts("  not in Spell.dbc"); continue; }
        std::printf("  name '%s'\n", spell.str((uint32_t)r, (uint32_t)nameCol).c_str());
        dump("Spell", spell, r, (uint32_t)nameCol - 8, (uint32_t)nameCol);
        const uint32_t vis = spell.u32((uint32_t)r, (uint32_t)nameCol - 5);
        const int vr = row(sv, vis);
        if (vr < 0) { std::printf("  visual %u: not in SpellVisual.dbc\n", vis); continue; }
        dump("SpellVisual", sv, vr, 0, 16);
        for (uint32_t c : {1u, 2u, 3u, 4u, 6u}) {
            const uint32_t kit = sv.u32((uint32_t)vr, c);
            if (!kit) continue;
            const int kr = row(svk, kit);
            std::printf("    kit column %u = %u%s\n", c, kit, kr < 0 ? " (not in SpellVisualKit)" : "");
            if (kr < 0) continue;
            dump("  SpellVisualKit", svk, kr, 0, std::min<uint32_t>(svk.fields, 20));
            for (uint32_t k = 3; k <= 14 && k < svk.fields; ++k)
                if (const uint32_t e = svk.u32((uint32_t)kr, k); e && row(fxn, e) >= 0) { std::printf("    [%u] ", k); effect(e); }
        }
        for (uint32_t c = 7; c <= 8; ++c)
            if (const uint32_t m = sv.u32((uint32_t)vr, c); m && row(fxn, m) >= 0) { std::printf("    missile column %u ", c); effect(m); }
    }
    return 0;
}

}  // namespace w3d
