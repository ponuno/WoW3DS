// game_water.cpp — the server side of liquids (stage 5.26): the breath / fatigue / feign death bars, environmental
// damage (drowning, exhaustion, lava, slime, falling, fire) and fishing (the bobber, the bite, the catch).
// Formats from vMaNGOS (Player::SendMirrorTimerStart / Stop / Pause, WorldPackets::Combat::EnvironmentalDamageLog,
// GameObject::SendGameObjectCustomAnim, SMSG_FISH_NOT_HOOKED / SMSG_FISH_ESCAPED in GameObject::Use).
#include "game.hpp"

#include <cmath>
#include <cstdio>

namespace w3n {

namespace {
enum : uint16_t {
    SMSG_GAMEOBJECT_CUSTOM_ANIM = 0x0B3, SMSG_FISH_NOT_HOOKED = 0x1C8, SMSG_FISH_ESCAPED = 0x1C9,
    SMSG_START_MIRROR_TIMER = 0x1D9, SMSG_PAUSE_MIRROR_TIMER = 0x1DA, SMSG_STOP_MIRROR_TIMER = 0x1DB,
    SMSG_ENVIRONMENTALDAMAGELOG = 0x1FC,
};
constexpr uint32_t kGoFishingNode = 17;
}  // namespace

const char* mirrorTimerName(uint32_t type) {
    switch (type) {
        case kMirrorFatigue: return "Fatigue";
        case kMirrorBreath: return "Breath";
        case kMirrorFeign: return "Feign Death";
    }
    return "";
}

uint32_t MirrorTimer::valueAt(uint32_t nowMs) const {
    if (!active) return 0;
    if (paused || nowMs <= baseMs) return std::min(value, maxMs);
    const int64_t v = (int64_t)value + (int64_t)scale * (int64_t)(nowMs - baseMs);
    return (uint32_t)std::max<int64_t>(0, std::min<int64_t>(v, maxMs));
}

uint64_t GameSession::fishingBobber() const {
    for (auto& [g, o] : objects.all()) {
        if (o.type != ObjType::GameObject || o.field(UF::GAMEOBJECT_TYPE_ID) != kGoFishingNode) continue;
        const uint64_t by = (uint64_t)o.field(UF::GAMEOBJECT_CREATED_BY) | ((uint64_t)o.field(UF::GAMEOBJECT_CREATED_BY + 1) << 32);
        if (by == self_) return g;
    }
    return 0;
}

bool GameSession::fishBiting(uint32_t nowMs) const {
    return fishBiteGuid && fishBiteGuid == fishingBobber() && nowMs - fishBiteMs < kFishBiteMs;
}

bool GameSession::handleWater(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case SMSG_START_MIRROR_TIMER: {
            const uint32_t type = r.u32(), value = r.u32(), maxMs = r.u32();
            const int32_t scale = (int32_t)r.u32();
            const uint8_t paused = r.left() >= 1 ? r.u8() : 0;
            const uint32_t spell = r.left() >= 4 ? r.u32() : 0;
            if (r.bad || type >= kMirrorTimers) return true;
            MirrorTimer& m = mirror[type];
            m.active = true;
            m.value = value;
            m.maxMs = maxMs ? maxMs : value;
            m.scale = scale;
            m.paused = paused != 0;
            m.spell = spell;
            m.baseMs = nowMs;
            return true;
        }
        case SMSG_PAUSE_MIRROR_TIMER: {
            const uint32_t type = r.u32();
            const uint8_t paused = r.left() >= 1 ? r.u8() : 1;
            if (r.bad || type >= kMirrorTimers) return true;
            MirrorTimer& m = mirror[type];
            m.value = m.valueAt(nowMs);   // freeze (or go on) from where it is now
            m.baseMs = nowMs;
            m.paused = paused != 0;
            return true;
        }
        case SMSG_STOP_MIRROR_TIMER: {
            const uint32_t type = r.u32();
            if (!r.bad && type < kMirrorTimers) mirror[type] = MirrorTimer();
            return true;
        }
        case SMSG_ENVIRONMENTALDAMAGELOG: {
            const uint64_t victim = r.u64();
            const uint8_t type = r.u8();
            const uint32_t damage = r.u32();
            const uint32_t absorb = r.left() >= 4 ? r.u32() : 0;
            const uint32_t resist = r.left() >= 4 ? r.u32() : 0;
            if (r.bad) return true;
            // the texts of the original's combat log (GlobalStrings VSENVIRONMENTALDAMAGE_*_SELF / _OTHER)
            static const char* kSelf[6] = {"You are exhausted and lose %u health.", "You are drowning and lose %u health.",
                                           "You fall and lose %u health.", "You lose %u health for swimming in lava.",
                                           "You lose %u health for swimming in slime.", "You suffer %u points of fire damage."};
            static const char* kOther[6] = {"%s is exhausted and loses %u health.", "%s is drowning and loses %u health.",
                                            "%s falls and loses %u health.", "%s loses %u health for swimming in lava.",
                                            "%s loses %u health for swimming in slime.", "%s suffers %u points of fire damage."};
            const int t = type < 6 ? type : 5;
            char b[160];
            if (victim == self_) {
                std::snprintf(b, sizeof b, kSelf[t], (unsigned)damage);
                std::string line = b;
                if (absorb) line += " (" + std::to_string(absorb) + " absorbed)";
                if (resist) line += " (" + std::to_string(resist) + " resisted)";
                addLocal(line, nowMs);
                MeleeHit h;   // the red number over our head, like a blow
                h.ms = nowMs;
                h.attacker = 0;
                h.victim = self_;
                h.damage = (int32_t)damage;
                h.victimState = 1;
                hits.push_back(h);
                while (hits.size() > 32) hits.pop_front();
                envDamageTaken += damage;
                lastEnvDamageMs = nowMs ? nowMs : 1;
                lastEnvDamageType = type;
            } else if (const WorldObject* o = objects.find(victim)) {
                if (o->type == ObjType::Player && inParty(victim)) {
                    std::snprintf(b, sizeof b, kOther[t], o->name.empty() ? "Someone" : o->name.c_str(), (unsigned)damage);
                    addLocal(b, nowMs);
                }
            }
            return true;
        }
        case SMSG_GAMEOBJECT_CUSTOM_ANIM: {
            const uint64_t g = r.u64();
            if (r.bad) return true;
            if (g && g == fishingBobber()) {   // a fish bites: catch it now
                fishBiteGuid = g;
                fishBiteMs = nowMs ? nowMs : 1;
                ++fishBites;
            }
            if (WorldObject* o = objects.find(g)) o->customAnimMs = nowMs ? nowMs : 1;
            return true;
        }
        case SMSG_FISH_NOT_HOOKED:
            addLocal("No fish are hooked.", nowMs);
            ++fishNotHooked;
            fishBiteGuid = 0;
            return true;
        case SMSG_FISH_ESCAPED:
            addLocal("Your fish got away!", nowMs);
            ++fishEscaped;
            fishBiteGuid = 0;
            return true;
    }
    return false;
}

}  // namespace w3n
