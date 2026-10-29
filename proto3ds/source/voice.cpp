#include "voice.hpp"

#include <math.h>

using w3n::ObjType;
using w3n::WorldObject;

void VoiceDirector::reset() { *this = VoiceDirector(); }

void VoiceDirector::raceGender(const WorldObject& o, uint32_t* race, uint32_t* gender) {
    const uint32_t b = o.field(w3n::UF::UNIT_BYTES_0);
    *race = b & 0xFF;
    *gender = (b >> 16) & 0xFF;
    if (*gender > 1) *gender = 0;
}

void VoiceDirector::npcLine(w3n::GameSession& g, const snd::SoundTables& t, const WorldObject& o, int kind, uint32_t nowMs,
                            std::vector<SoundReq>& out) {
    const snd::NpcVoice* v = t.npcVoice(o.displayId());
    const uint32_t sound = !v ? 0 : kind == 0 ? v->greeting : kind == 1 ? v->farewell : v->pissed;
    if (!sound) { ++stats.silent; return; }
    SoundReq r;
    r.sound = sound;
    float p[3];
    o.positionAt(nowMs, p);
    r.x = p[0]; r.y = p[1]; r.z = p[2] + 1.6f;
    r.what = kind == 0 ? "greeting" : kind == 1 ? "farewell" : "pissed";
    out.push_back(r);
    (kind == 0 ? stats.greetings : kind == 1 ? stats.farewells : stats.pissed)++;
    npcs_[o.guid].lastMs = nowMs;
    (void)g;
}

void VoiceDirector::update(w3n::GameSession& g, const snd::SoundTables& t, uint32_t nowMs, std::vector<SoundReq>& out) {
    const bool on = t.loaded();
    // ---- NPCs: selecting one, opening and closing its window
    const uint64_t tg = g.target();
    if (started_ && tg != target_ && tg && on) {
        const WorldObject* o = g.objects.find(tg);
        // NPCs (not players, not ourselves), alive, not hostile: a hostile one does not greet
        // (with factions.txt: by reaction; without: the session's guess - a creature without NPC functions is an enemy)
        const bool hostile = o && (g.factions.loaded() ? (int)g.shownReaction(*o) <= (int)w3n::Rank::Hostile : g.enemyLike(*o));
        if (o && o->type == ObjType::Unit && !o->dead() && !hostile && t.npcVoice(o->displayId())) {
            Npc& n = npcs_[tg];
            if (nowMs - n.firstClickMs > kClickWindowMs || !n.clicks) { n.firstClickMs = nowMs; n.clicks = 0; }
            ++n.clicks;
            const snd::NpcVoice* v = t.npcVoice(o->displayId());
            if (n.clicks >= kPissedClicks && v->pissed) {
                n.clicks = 0;
                npcLine(g, t, *o, 2, nowMs, out);
            } else if (!n.lastMs || nowMs - n.lastMs >= kGreetCooldownMs) {
                npcLine(g, t, *o, 0, nowMs, out);
            }
        }
    }
    target_ = tg;
    const uint64_t w = g.npcWindow();
    if (started_ && w != window_ && on) {
        if (window_) {   // the window we had closed (or another NPC's opened): goodbye from the one we leave
            const WorldObject* o = g.objects.find(window_);
            if (o && o->type == ObjType::Unit && !o->dead()) {
                const w3n::MovementInfo& me = g.mover.info();
                float p[3];
                o->positionAt(nowMs, p);
                const float dx = p[0] - me.x, dy = p[1] - me.y;
                if (dx * dx + dy * dy < 30.0f * 30.0f && t.npcVoice(o->displayId())) npcLine(g, t, *o, 1, nowMs, out);
            }
        }
        if (w) {   // a window opened: the greeting, unless selecting it just said it
            const WorldObject* o = g.objects.find(w);
            if (o && o->type == ObjType::Unit && !o->dead() && t.npcVoice(o->displayId())) {
                const Npc& n = npcs_[w];
                if (!n.lastMs || nowMs - n.lastMs >= kGreetCooldownMs) npcLine(g, t, *o, 0, nowMs, out);
            }
        }
    }
    window_ = w;
    started_ = true;

    // ---- our character: what went wrong
    const WorldObject* me = g.selfObject();
    for (w3n::Vocal v : g.vocals) {
        if (!on || !me) break;
        if (errorMs_ && nowMs - errorMs_ < kErrorGapMs) continue;
        if (v == lastError_ && errorMs_ && nowMs - errorMs_ < kSameErrorMs) continue;
        uint32_t race, gender;
        raceGender(*me, &race, &gender);
        const uint32_t sound = t.errorVoice(race, gender, w3n::vocalToken(v));
        if (!sound) { ++stats.silent; continue; }
        SoundReq r;
        r.sound = sound;
        r.self = true;
        r.x = me->mv.x; r.y = me->mv.y; r.z = me->mv.z;
        r.what = "error";
        out.push_back(r);
        ++stats.errors;
        errorMs_ = nowMs;
        lastError_ = v;
    }
    g.vocals.clear();

    // ---- text emotes: the voice of who did it
    for (const w3n::GameSession::HeardEmote& e : g.heardEmotes) {
        if (!on) break;
        const WorldObject* o = g.objects.find(e.guid);
        if (!o || !o->isUnitLike()) continue;
        uint32_t race, gender;
        raceGender(*o, &race, &gender);
        const uint32_t sound = t.emoteVoice(e.textEmote, race, gender);
        if (!sound) { ++stats.silent; continue; }
        SoundReq r;
        r.sound = sound;
        r.self = o->self || e.guid == g.selfGuid();
        float p[3];
        o->positionAt(nowMs, p);
        r.x = p[0]; r.y = p[1]; r.z = p[2] + 1.6f;
        r.what = "emote";
        out.push_back(r);
        ++stats.emotes;
    }
    g.heardEmotes.clear();
    // forget NPCs we have not heard for a while (the map stays small in cities)
    if (npcs_.size() > 64)
        for (auto it = npcs_.begin(); it != npcs_.end();)
            it = nowMs - it->second.lastMs > 60000 && nowMs - it->second.firstClickMs > kClickWindowMs ? npcs_.erase(it) : std::next(it);
}
