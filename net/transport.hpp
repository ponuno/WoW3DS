// Transports (ships and zeppelins, GAMEOBJECT_TYPE_MO_TRANSPORT) for the 3DS client (stage 5.21).
// The server simulates them from TaxiPathNode.dbc (key frames, stops, acceleration; vMaNGOS Transports/TransportMgr.cpp
// and ShipTransport::Update) but only sends the position at creation and the path progress; the 1.12 client animates the
// object by itself. This is a port of that simulation: TransportPath::at(progressMs) gives the pose the server computes.
// transports.txt (converter/src/texts.cpp exportTransports): "T pathId nodeIndex map x y z flags delay" per node.
#pragma once
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "spline.hpp"

namespace w3n {

struct TaxiPathNode {
    uint32_t map = 0;
    float x = 0, y = 0, z = 0;
    uint32_t flags = 0;   // 1 = teleport, 2 = stop
    uint32_t delay = 0;   // seconds at a stop
};

struct TransportPose {
    float x = 0, y = 0, z = 0, o = 0;
    uint32_t map = 0;
    bool moving = false;
    bool valid = false;
};

class TransportPath {
public:
    // speed / accel: GO template data1 / data2 (yd/s, yd/s^2). False when the path cannot be a transport path.
    bool build(const std::vector<TaxiPathNode>& nodes, float speed, float accel);
    uint32_t period() const { return period_; }   // ms of one full round
    // progressMs: path progress (any value; taken modulo the period).
    TransportPose at(uint32_t progressMs) const;
    size_t frames() const { return kf_.size(); }
    bool built() const { return !kf_.empty() && period_ != 0; }

private:
    struct Frame {
        TaxiPathNode node;
        float initO = 0;
        uint32_t index = 0;     // 1-based position in its spline chunk
        int chunk = -1;
        float distFromPrev = -1, nextDistFromPrev = 0, distSinceStop = -1, distUntilStop = -1, timeFrom = 0, timeTo = 0;
        bool teleport = false;
        uint32_t arrive = 0, depart = 0, nextArrive = 0;
        bool stop() const { return node.flags == 2; }
    };
    float segmentPos(const Frame& f, float nowSec) const;
    std::vector<Frame> kf_;
    std::vector<std::vector<Vec3f>> chunks_;
    uint32_t period_ = 0;
    float speed_ = 0, accel_ = 0, accelTime_ = 0, accelDist_ = 0;
};

// Where a ship can be walked on: a flat deck at height z (relative to the transport origin) inside a rectangle
// (transport-local x / y), derived by the converter from the model ("M displayId z x0 y0 x1 y1").
struct DeckInfo {
    float z = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(float lx, float ly) const { return lx >= x0 && lx <= x1 && ly >= y0 && ly <= y1; }
};

// Does a deck at world height deckZ hold a walker whose feet are at zFeet? A deck up to `window` above the feet is a step
// up; one below holds the walker if it is within `window` + `drop` (the distance the walker falls this frame: a fast fall,
// or a slow frame, must not skip the deck between two samples).
inline bool deckReaches(float deckZ, float zFeet, float window, float drop) {
    return deckZ - zFeet <= window && zFeet - deckZ <= window + (drop > 0 ? drop : 0.0f);
}

// A platform among the ordinary objects (bridge, dock, raft, stage), placed by the server with a position, a facing and a scale:
// the deck of its model, turned and scaled with it. World (x, y) -> the deck's frame -> inside? Returns the world height of
// the deck there. (Same turn as worldToTransport: the object's facing is the angle from the model's x axis.)
inline bool staticDeckAt(const DeckInfo& d, const float pos[3], float yaw, float scale, float x, float y, float* worldZ) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float dx = x - pos[0], dy = y - pos[1];
    const float inv = scale > 0.05f ? 1.0f / scale : 1.0f;
    const float lx = (dx * c + dy * s) * inv, ly = (-dx * s + dy * c) * inv;
    if (!d.contains(lx, ly)) return false;
    *worldZ = pos[2] + d.z * (scale > 0.05f ? scale : 1.0f);
    return true;
}

// Elevators and trams (GAMEOBJECT_TYPE_TRANSPORT, type 11; vMaNGOS ElevatorTransport). The server moves them along
// TransportAnimation.dbc key points (offset from the object's stationary position, time in ms) and sends only the stationary
// position and the progress (time since creation mod TotalTime) in the create block. transports.txt:
// "A entry timeSeg x y z" per TransportAnimation.dbc row.
struct ElevatorNode { uint32_t t = 0; float x = 0, y = 0, z = 0; };
class ElevatorAnim {
public:
    void add(const ElevatorNode& n);          // keeps nodes sorted by time (a later row with the same time replaces)
    uint32_t total() const { return nodes_.empty() ? 0 : nodes_.back().t; }
    bool valid() const { return !nodes_.empty() && total() != 0; }
    size_t size() const { return nodes_.size(); }
    // Offset (before rotation) at progressMs (taken modulo total()), as ElevatorTransport::Update interpolates it.
    bool offsetAt(uint32_t progressMs, float out[3]) const;
private:
    std::vector<ElevatorNode> nodes_;
};
// Pose of an elevator: stationary position + offset rotated by the inverse of the object's rotation quaternion (x y z w),
// with the Y of the result negated before adding the stationary position (vMaNGOS: "magical sign flip, vanilla/tbc only").
// The orientation stays the stationary one.
TransportPose elevatorPose(const ElevatorAnim& a, uint32_t progressMs, const float stationary[4], const float quat[4]);

class TransportTable {
public:
    bool load(const std::string& path);                      // transports.txt
    bool parse(const std::string& text);
    bool loaded() const { return loaded_; }
    const std::vector<TaxiPathNode>* nodes(uint32_t pathId) const;
    // The built path for a GO template (cached); null when unknown / unbuildable.
    const TransportPath* path(uint32_t pathId, float speed, float accel);
    const ElevatorAnim* elevator(uint32_t goEntry) const {
        auto it = elevators_.find(goEntry);
        return it == elevators_.end() ? nullptr : &it->second;
    }
    // A transport's deck: its own "M" line, else the model's flat collision floor ("S": a transport whose model name the
    // converter's filter did not know still carries its passengers).
    const DeckInfo* deck(uint32_t gameObjectDisplayId) const {
        auto it = decks_.find(gameObjectDisplayId);
        if (it != decks_.end()) return &it->second;
        auto st = staticDecks_.find(gameObjectDisplayId);
        return st == staticDecks_.end() ? nullptr : &st->second;
    }
    // A platform among the ordinary objects (6.14, "S" lines): the display's flat collision floor, null if it has none.
    const DeckInfo* staticDeck(uint32_t gameObjectDisplayId) const {
        auto it = staticDecks_.find(gameObjectDisplayId);
        return it == staticDecks_.end() ? nullptr : &it->second;
    }
    size_t staticDeckCount() const { return staticDecks_.size(); }

private:
    bool loaded_ = false;
    std::map<uint32_t, std::vector<TaxiPathNode>> paths_;
    std::map<uint32_t, DeckInfo> decks_;
    std::map<uint32_t, DeckInfo> staticDecks_;
    std::map<uint32_t, ElevatorAnim> elevators_;
    std::map<std::pair<uint32_t, uint64_t>, std::unique_ptr<TransportPath>> built_;
};

// A passenger's offset on a transport to world coordinates and back (GenericTransport::CalculatePassengerPosition /
// CalculatePassengerOffset: rotation around Z by the transport's orientation).
void transportToWorld(const TransportPose& t, float lx, float ly, float lz, float lo, float out[3], float* oOut);
void worldToTransport(const TransportPose& t, float wx, float wy, float wz, float wo, float out[3], float* oOut);

}  // namespace w3n
