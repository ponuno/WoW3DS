// zone_banner.hpp — the zone name over the top screen when the player enters a new zone or subzone (stage 5.45), as the
// original's ZoneTextFrame / SubZoneTextFrame: the zone's name large, the subzone's name smaller under it (a subzone alone
// when only the subzone changed). Fades in, holds, fades out. Pure logic (the names come from glue.txt, the area from the
// sound module's area grid), so it runs on the PC in a test.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

class ZoneBanner {
public:
    // Name of an AreaTable id (empty = unknown) and its parent zone (0 = a zone itself).
    using NameFn = std::function<std::string(uint32_t)>;
    using ParentFn = std::function<uint32_t(uint32_t)>;

    // Every frame: the map and the area the player is in (0 = unknown / outside the area grid).
    void update(uint32_t mapId, uint32_t area, uint32_t nowMs, const NameFn& name, const ParentFn& parent);
    void reset();                                     // logged out / loading: the next area shows again
    int alpha(uint32_t nowMs) const;                  // 0..255, 0 = nothing to draw
    uint32_t zone() const { return zone_; }          // the zone the player is in (0 = not known yet)
    const std::string& big() const { return big_; }   // the zone (or the subzone when only that changed)
    const std::string& small() const { return small_; }   // the subzone under a new zone's name

    static constexpr uint32_t kHoldMs = 1000;         // an area must be held this long before it is announced
    static constexpr uint32_t kFadeInMs = 700, kShowMs = 2300, kFadeOutMs = 1500;

private:
    uint32_t cand_ = 0, candMs_ = 0, shown_ = 0, zone_ = 0, map_ = ~0u, startMs_ = 0;
    bool active_ = false;
    std::string big_, small_;
};
