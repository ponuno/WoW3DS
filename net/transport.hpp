// Transports (ships and zeppelins, GAMEOBJECT_TYPE_MO_TRANSPORT) for the 3DS client (stage 5.21).
// The server simulates them from TaxiPathNode.dbc (key frames, stops, acceleration; vMaNGOS Transports/TransportMgr.cpp
// and ShipTransport::Update) but only sends the position at creation and the path progress; the 1.12 client animates the
// object by itself. This is a port of that simulation: TransportPath::at(progressMs) gives the pose the server computes.
// transports.txt (converter/src/texts.cpp exportTransports): "T pathId nodeIndex map x y z flags delay" per node.
#pragma once
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

class TransportTable {
public:
    bool load(const std::string& path);                      // transports.txt
    bool parse(const std::string& text);
    bool loaded() const { return loaded_; }
    const std::vector<TaxiPathNode>* nodes(uint32_t pathId) const;
    // The built path for a GO template (cached); null when unknown / unbuildable.
    const TransportPath* path(uint32_t pathId, float speed, float accel);
    const DeckInfo* deck(uint32_t gameObjectDisplayId) const {
        auto it = decks_.find(gameObjectDisplayId);
        return it == decks_.end() ? nullptr : &it->second;
    }

private:
    bool loaded_ = false;
    std::map<uint32_t, std::vector<TaxiPathNode>> paths_;
    std::map<uint32_t, DeckInfo> decks_;
    std::map<std::pair<uint32_t, uint64_t>, std::unique_ptr<TransportPath>> built_;
};

// A passenger's offset on a transport to world coordinates and back (GenericTransport::CalculatePassengerPosition /
// CalculatePassengerOffset: rotation around Z by the transport's orientation).
void transportToWorld(const TransportPose& t, float lx, float ly, float lz, float lo, float out[3], float* oOut);
void worldToTransport(const TransportPose& t, float wx, float wy, float wz, float wo, float out[3], float* oOut);

}  // namespace w3n
