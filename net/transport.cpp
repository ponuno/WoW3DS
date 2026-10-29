#include "transport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace w3n {

namespace {
constexpr float kPi = 3.14159265358979f;
float normO(float a) {
    while (a < 0) a += 2 * kPi;
    while (a >= 2 * kPi) a -= 2 * kPi;
    return a;
}
// directions of segment s of a chunk at u
float dirO(const std::vector<Vec3f>& pts, size_t s, float u) {
    Vec3f p[4];
    crSegment(pts, s, p);
    float dw[4];
    crDerivWeights(u, dw);
    Vec3f d = crEval(p, dw);
    return normO(std::atan2(d.y, d.x) + kPi);
}
}  // namespace

bool TransportPath::build(const std::vector<TaxiPathNode>& nodes, float speed, float accel) {
    kf_.clear();
    chunks_.clear();
    period_ = 0;
    if (nodes.size() < 3 || !(speed > 0) || !(accel > 0)) return false;
    speed_ = speed;
    accel_ = accel;
    accelTime_ = speed / accel;
    accelDist_ = 0.5f * speed * speed / accel;

    std::vector<Vec3f> splinePath;
    bool mapChange = false;
    for (size_t i = 1; i + 1 < nodes.size(); ++i) {
        if (!mapChange) {
            if ((nodes[i].flags & 1) || nodes[i].map != nodes[i + 1].map) {
                if (kf_.empty()) return false;
                kf_.back().teleport = true;
                mapChange = true;
            } else {
                Frame f;
                f.node = nodes[i];
                kf_.push_back(f);
                splinePath.push_back({nodes[i].x, nodes[i].y, nodes[i].z});
            }
        } else {
            mapChange = false;
        }
    }
    if (kf_.empty()) return false;
    kf_.back().teleport = true;   // last to first is always a teleport, even for closed paths

    int firstStop = -1, lastStop = -1;
    kf_[0].distFromPrev = 0;
    kf_[0].index = 1;
    if (kf_[0].stop()) firstStop = lastStop = 0;

    const size_t K = kf_.size();
    size_t start = 0;
    for (size_t i = 1; i < K; ++i) {
        if (kf_[i - 1].teleport || i + 1 == K) {
            const size_t extra = !kf_[i - 1].teleport ? 1 : 0;
            const size_t n = i - start + extra;
            chunks_.emplace_back(splinePath.begin() + (long)start, splinePath.begin() + (long)(start + n));
            const int cid = (int)chunks_.size() - 1;
            const std::vector<Vec3f>& pts = chunks_.back();
            std::vector<double> L(n + 1, 0.0);   // lengths[abs index]: abs 1 = first control point
            for (size_t k = 1; k < n; ++k) L[k + 1] = L[k] + crSegLength(pts, k - 1);
            for (size_t j = start; j < i + extra; ++j) {
                kf_[j].index = (uint32_t)(j - start + 1);
                kf_[j].distFromPrev = (float)(L[j - start + 1] - L[j - start]);
                if (j > 0) kf_[j - 1].nextDistFromPrev = kf_[j].distFromPrev;
                kf_[j].chunk = cid;
            }
            if (kf_[i - 1].teleport) {
                kf_[i].index = (uint32_t)(i - start + 1);
                kf_[i].distFromPrev = 0.0f;
                kf_[i - 1].nextDistFromPrev = 0.0f;
                kf_[i].chunk = cid;
            }
            start = i;
        }
        if (kf_[i].stop()) {
            if (firstStop == -1) firstStop = (int)i;
            lastStop = (int)i;
        }
    }
    kf_.back().nextDistFromPrev = kf_.front().distFromPrev;
    if (firstStop == -1 || lastStop == -1) firstStop = lastStop = 0;

    // distances since the last / until the next stop
    float tmp = 0.0f;
    for (size_t i = 0; i < K; ++i) {
        size_t j = (i + (size_t)lastStop) % K;
        if (kf_[j].stop() || (int)j == lastStop) tmp = 0.0f;
        else tmp += kf_[j].distFromPrev;
        kf_[j].distSinceStop = tmp;
    }
    tmp = 0.0f;
    for (int i = (int)K - 1; i >= 0; --i) {
        size_t j = ((size_t)i + (size_t)firstStop) % K;
        tmp += kf_[(j + 1) % K].distFromPrev;
        kf_[j].distUntilStop = tmp;
        if (kf_[j].stop() || (int)j == firstStop) tmp = 0.0f;
    }

    for (Frame& f : kf_) {
        const float total = f.distSinceStop + f.distUntilStop;
        if (total < 2 * accelDist_) {                       // never reaches full speed
            if (f.distSinceStop < f.distUntilStop) {        // still accelerating
                float seg = 2.0f * std::sqrt((f.distUntilStop + f.distSinceStop) / accel_);
                f.timeTo = seg - std::sqrt(2 * f.distSinceStop / accel_);
            } else {
                f.timeTo = std::sqrt(2 * f.distUntilStop / accel_);
            }
        } else if (f.distSinceStop < accelDist_) {          // accelerating, will reach full speed
            float seg = (f.distUntilStop + f.distSinceStop) / speed_ + (speed_ / accel_);
            f.timeTo = seg - std::sqrt(2 * f.distSinceStop / accel_);
        } else if (f.distUntilStop < accelDist_) {          // slowing down
            f.timeTo = std::sqrt(2 * f.distUntilStop / accel_);
        } else {                                            // cruising
            f.timeTo = f.distUntilStop / speed_ + 0.5f * speed_ / accel_;
        }
    }

    float segTime = 0.0f;
    for (size_t i = 0; i < K; ++i) {
        size_t j = (i + (size_t)lastStop) % K;
        if (kf_[j].stop() || (int)j == lastStop) segTime = kf_[j].timeTo;
        kf_[j].timeFrom = segTime - kf_[j].timeTo;
    }

    kf_[0].arrive = 0;
    float cur = 0.0f;
    if (kf_[0].stop()) {
        cur = (float)kf_[0].node.delay;
        kf_[0].depart = (uint32_t)(cur * 1000.0f);
    }
    for (size_t i = 1; i < K; ++i) {
        cur += kf_[i - 1].timeTo;
        if (kf_[i].stop()) {
            kf_[i].arrive = (uint32_t)(cur * 1000.0f);
            kf_[i - 1].nextArrive = kf_[i].arrive;
            cur += (float)kf_[i].node.delay;
            kf_[i].depart = (uint32_t)(cur * 1000.0f);
        } else {
            cur -= kf_[i].timeTo;
            kf_[i].arrive = (uint32_t)(cur * 1000.0f);
            kf_[i - 1].nextArrive = kf_[i].arrive;
            kf_[i].depart = kf_[i].arrive;
        }
    }
    kf_.back().nextArrive = kf_.back().depart;
    period_ = kf_.back().depart;

    // heading at a frame: along its spline segment (the last frame of a chunk: the end of the previous one)
    for (Frame& f : kf_) {
        f.initO = 0;
        if (f.chunk < 0) continue;
        const std::vector<Vec3f>& pts = chunks_[(size_t)f.chunk];
        if (pts.size() < 2) continue;
        size_t m = f.index - 1;
        if (m + 1 < pts.size()) f.initO = dirO(pts, m, 0.0f);
        else f.initO = dirO(pts, pts.size() - 2, 1.0f);
    }
    return period_ != 0;
}

float TransportPath::segmentPos(const Frame& f, float now) const {
    // ShipTransport::CalculateSegmentPos
    const float sinceStop = f.timeFrom + (now - 0.001f * (float)f.depart);
    const float untilStop = f.timeTo - (now - 0.001f * (float)f.depart);
    float pos, dist;
    if (sinceStop < untilStop) {
        dist = sinceStop < accelTime_ ? 0.5f * accel_ * sinceStop * sinceStop : accelDist_ + (sinceStop - accelTime_) * speed_;
        pos = dist - f.distSinceStop;
    } else {
        dist = untilStop < accelTime_ ? 0.5f * accel_ * untilStop * untilStop : accelDist_ + (untilStop - accelTime_) * speed_;
        pos = f.distUntilStop - dist;
    }
    return f.nextDistFromPrev > 1e-6f ? pos / f.nextDistFromPrev : 0.0f;
}

TransportPose TransportPath::at(uint32_t progressMs) const {
    TransportPose out;
    if (!built()) return out;
    const uint32_t p = progressMs % period_;
    size_t i = 0;
    for (; i < kf_.size(); ++i) {
        const Frame& f = kf_[i];
        if ((p >= f.arrive && p < f.depart) || (p >= f.depart && p < f.nextArrive)) break;
    }
    if (i == kf_.size()) i = kf_.size() - 1;
    const Frame& f = kf_[i];
    out.valid = true;
    out.map = f.node.map;
    out.x = f.node.x; out.y = f.node.y; out.z = f.node.z;
    out.o = f.initO;
    if (p >= f.arrive && p < f.depart) return out;   // waiting at a stop
    if (f.teleport || f.chunk < 0 || p == 0) return out;
    const std::vector<Vec3f>& pts = chunks_[(size_t)f.chunk];
    if (pts.size() < 2 || f.index + 1 > pts.size()) return out;
    const float t = std::min(1.0f, std::max(0.0f, segmentPos(f, (float)p * 0.001f)));
    Vec3f c[4];
    crSegment(pts, f.index - 1, c);
    float w[4], dw[4];
    crWeights(t, w);
    crDerivWeights(t, dw);
    Vec3f at = crEval(c, w), d = crEval(c, dw);
    out.x = at.x; out.y = at.y; out.z = at.z;
    if (std::fabs(d.x) + std::fabs(d.y) > 1e-6f) out.o = normO(std::atan2(d.y, d.x) + kPi);
    out.moving = true;
    return out;
}

bool TransportTable::parse(const std::string& text) {
    paths_.clear();
    decks_.clear();
    staticDecks_.clear();
    elevators_.clear();
    built_.clear();
    std::map<uint32_t, std::map<uint32_t, TaxiPathNode>> tmp;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at);
        at = e + 1;
        if (line.size() >= 3 && line[0] == 'M' && line[1] == '\t') {
            unsigned id = 0;
            DeckInfo d;
            if (std::sscanf(line.c_str() + 2, "%u %f %f %f %f %f", &id, &d.z, &d.x0, &d.y0, &d.x1, &d.y1) == 6) decks_[id] = d;
            continue;
        }
        if (line.size() >= 3 && line[0] == 'S' && line[1] == '\t') {   // 6.14: a platform among the ordinary objects
            unsigned id = 0;
            DeckInfo d;
            if (std::sscanf(line.c_str() + 2, "%u %f %f %f %f %f", &id, &d.z, &d.x0, &d.y0, &d.x1, &d.y1) == 6) staticDecks_[id] = d;
            continue;
        }
        if (line.size() >= 3 && line[0] == 'A' && line[1] == '\t') {
            unsigned entry = 0, t = 0;
            ElevatorNode n;
            if (std::sscanf(line.c_str() + 2, "%u %u %f %f %f", &entry, &t, &n.x, &n.y, &n.z) == 5) { n.t = t; elevators_[entry].add(n); }
            continue;
        }
        if (line.size() < 3 || line[0] != 'T' || line[1] != '\t') continue;
        unsigned path = 0, idx = 0, map = 0, flags = 0, delay = 0;
        float x = 0, y = 0, z = 0;
        if (std::sscanf(line.c_str() + 2, "%u %u %u %f %f %f %u %u", &path, &idx, &map, &x, &y, &z, &flags, &delay) != 8) continue;
        TaxiPathNode n;
        n.map = map; n.x = x; n.y = y; n.z = z; n.flags = flags; n.delay = delay;
        tmp[path][idx] = n;
    }
    for (auto& kv : tmp) {
        auto& v = paths_[kv.first];
        for (auto& nd : kv.second) v.push_back(nd.second);
    }
    loaded_ = !paths_.empty() || !decks_.empty() || !staticDecks_.empty() || !elevators_.empty();
    return loaded_;
}

void ElevatorAnim::add(const ElevatorNode& n) {
    auto it = std::lower_bound(nodes_.begin(), nodes_.end(), n.t, [](const ElevatorNode& a, uint32_t t) { return a.t < t; });
    if (it != nodes_.end() && it->t == n.t) *it = n;
    else nodes_.insert(it, n);
}

bool ElevatorAnim::offsetAt(uint32_t progressMs, float out[3]) const {
    if (!valid()) return false;
    const uint32_t tm = progressMs % total();
    // vMaNGOS: next = first node with TimeSeg >= tm, prev = the node before it (none at the very start)
    auto nx = std::lower_bound(nodes_.begin(), nodes_.end(), tm, [](const ElevatorNode& a, uint32_t t) { return a.t < t; });
    if (nx == nodes_.end()) nx = nodes_.end() - 1;
    if (nx == nodes_.begin()) { out[0] = nx->x; out[1] = nx->y; out[2] = nx->z; return true; }   // the server keeps the old spot here
    const ElevatorNode& pv = *(nx - 1);
    const ElevatorNode& n = *nx;
    if (pv.x == n.x && pv.y == n.y && pv.z == n.z) { out[0] = pv.x; out[1] = pv.y; out[2] = pv.z; return true; }
    const float k = (float)(tm - pv.t) / (float)(n.t - pv.t);
    out[0] = pv.x + (n.x - pv.x) * k;
    out[1] = pv.y + (n.y - pv.y) * k;
    out[2] = pv.z + (n.z - pv.z) * k;
    return true;
}

TransportPose elevatorPose(const ElevatorAnim& a, uint32_t progressMs, const float st[4], const float q[4]) {
    TransportPose p;
    float off[3];
    if (!a.offsetAt(progressMs, off)) return p;
    // G3D: offset * Matrix3(quat) == transpose(R(quat)) * offset, R from the unit quaternion (x y z w)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    const float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 1e-6f) { x /= n; y /= n; z /= n; w /= n; } else { x = y = z = 0; w = 1; }
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    const float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    const float vx = r00 * off[0] + r10 * off[1] + r20 * off[2];
    const float vy = r01 * off[0] + r11 * off[1] + r21 * off[2];
    const float vz = r02 * off[0] + r12 * off[1] + r22 * off[2];
    p.x = st[0] + vx;
    p.y = st[1] - vy;
    p.z = st[2] + vz;
    p.o = st[3];
    p.moving = true;
    p.valid = true;
    return p;
}

bool TransportTable::load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return parse(text);
}

const std::vector<TaxiPathNode>* TransportTable::nodes(uint32_t pathId) const {
    auto it = paths_.find(pathId);
    return it == paths_.end() ? nullptr : &it->second;
}

const TransportPath* TransportTable::path(uint32_t pathId, float speed, float accel) {
    uint32_t sb, ab;
    std::memcpy(&sb, &speed, 4);
    std::memcpy(&ab, &accel, 4);
    auto key = std::make_pair(pathId, ((uint64_t)sb << 32) | ab);
    auto it = built_.find(key);
    if (it != built_.end()) return it->second && it->second->built() ? it->second.get() : nullptr;
    auto p = std::make_unique<TransportPath>();
    const auto* nd = nodes(pathId);
    if (!nd || !p->build(*nd, speed, accel)) p.reset();
    const TransportPath* r = p.get();
    built_[key] = std::move(p);
    return r;
}

void transportToWorld(const TransportPose& t, float lx, float ly, float lz, float lo, float out[3], float* oOut) {
    const float c = std::cos(t.o), s = std::sin(t.o);
    out[0] = t.x + lx * c - ly * s;
    out[1] = t.y + lx * s + ly * c;
    out[2] = t.z + lz;
    if (oOut) *oOut = normO(lo + t.o);
}

void worldToTransport(const TransportPose& t, float wx, float wy, float wz, float wo, float out[3], float* oOut) {
    const float c = std::cos(t.o), s = std::sin(t.o);
    const float dx = wx - t.x, dy = wy - t.y;
    out[0] = dx * c + dy * s;
    out[1] = -dx * s + dy * c;
    out[2] = wz - t.z;
    if (oOut) *oOut = normO(wo - t.o);
}

}  // namespace w3n
