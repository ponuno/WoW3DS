#include "zone_banner.hpp"

void ZoneBanner::reset() {
    cand_ = candMs_ = shown_ = zone_ = startMs_ = 0;
    map_ = ~0u;
    active_ = false;
    big_.clear();
    small_.clear();
}

void ZoneBanner::update(uint32_t mapId, uint32_t area, uint32_t nowMs, const NameFn& name, const ParentFn& parent) {
    if (!area) { cand_ = 0; return; }
    if (area != cand_) { cand_ = area; candMs_ = nowMs; }
    if (area == shown_ && mapId == map_) return;
    if ((int32_t)(nowMs - candMs_) < (int32_t)kHoldMs) return;   // standing on a border: not yet
    // the zone: the area's top parent
    uint32_t zone = area;
    for (int i = 0; i < 3; ++i) {
        const uint32_t p = parent ? parent(zone) : 0;
        if (!p || p == zone) break;
        zone = p;
    }
    const std::string an = name ? name(area) : std::string(), zn = name ? name(zone) : std::string();
    const bool zoneChanged = zone != zone_ || mapId != map_;
    shown_ = area;
    zone_ = zone;
    map_ = mapId;
    big_.clear();
    small_.clear();
    if (zoneChanged) {
        big_ = zn.empty() ? an : zn;
        if (area != zone && !an.empty() && an != big_) small_ = an;
    } else {
        big_ = an;
    }
    if (big_.empty()) { active_ = false; return; }
    active_ = true;
    startMs_ = nowMs;
}

int ZoneBanner::alpha(uint32_t nowMs) const {
    if (!active_) return 0;
    const uint32_t t = nowMs - startMs_;
    if ((int32_t)t < 0) return 0;
    if (t < kFadeInMs) return (int)(255u * t / kFadeInMs);
    if (t < kFadeInMs + kShowMs) return 255;
    const uint32_t u = t - kFadeInMs - kShowMs;
    if (u < kFadeOutMs) return (int)(255u * (kFadeOutMs - u) / kFadeOutMs);
    return 0;
}
