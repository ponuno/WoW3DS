// cinematic.cpp — 7.04: see cinematic.hpp.
#include "cinematic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cine {

void Sequence::at(uint32_t t, float pos[3], float target[3]) const {
    if (keys.empty()) {
        for (int k = 0; k < 3; ++k) pos[k] = target[k] = 0;
        return;
    }
    if (t <= keys.front().t || keys.size() == 1) {
        std::memcpy(pos, keys.front().pos, 12);
        std::memcpy(target, keys.front().target, 12);
        return;
    }
    if (t >= keys.back().t) {
        std::memcpy(pos, keys.back().pos, 12);
        std::memcpy(target, keys.back().target, 12);
        return;
    }
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](uint32_t v, const Key& k) { return v < k.t; });
    const Key& b = *it;
    const Key& a = *(it - 1);
    const float u = b.t > a.t ? (float)(t - a.t) / (float)(b.t - a.t) : 0.0f;
    for (int k = 0; k < 3; ++k) {
        pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * u;
        target[k] = a.target[k] + (b.target[k] - a.target[k]) * u;
    }
}

bool Table::load(const std::string& path) {
    seqs_.clear();
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[1] != '\t') continue;
        char* p = line + 2;
        const uint32_t id = (uint32_t)std::strtoul(p, &p, 10);
        Sequence& s = seqs_[id];
        s.id = id;
        if (line[0] == 'Q') {
            s.totalMs = (uint32_t)std::strtoul(p, &p, 10);
        } else if (line[0] == 'S') {
            Sound snd;
            snd.startMs = (uint32_t)std::strtoul(p, &p, 10);
            while (*p == '\t') ++p;
            char* e = p;
            while (*e && *e != '\t' && *e != '\n' && *e != '\r') ++e;
            snd.file.assign(p, e);
            if (*e == '\t') snd.volume = (int)std::strtol(e, nullptr, 10);
            if (!snd.file.empty()) s.sounds.push_back(snd);
        } else if (line[0] == 'K') {
            Key k;
            k.t = (uint32_t)std::strtoul(p, &p, 10);
            for (int c = 0; c < 3; ++c) k.pos[c] = std::strtof(p, &p);
            for (int c = 0; c < 3; ++c) k.target[c] = std::strtof(p, &p);
            s.keys.push_back(k);
        }
    }
    std::fclose(f);
    for (auto it = seqs_.begin(); it != seqs_.end();) {   // a sequence without a flight cannot be shown
        Sequence& s = it->second;
        std::stable_sort(s.keys.begin(), s.keys.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
        std::stable_sort(s.sounds.begin(), s.sounds.end(), [](const Sound& a, const Sound& b) { return a.startMs < b.startMs; });
        if (s.keys.empty()) { it = seqs_.erase(it); continue; }
        if (!s.totalMs) s.totalMs = s.keys.back().t;
        ++it;
    }
    return !seqs_.empty();
}

const Sequence* Table::find(uint32_t id) const {
    auto it = seqs_.find(id);
    return it == seqs_.end() ? nullptr : &it->second;
}

void lookAngles(const float pos[3], const float target[3], float* yaw, float* pitch) {
    const float dx = target[0] - pos[0], dy = target[1] - pos[1], dz = target[2] - pos[2];
    const float h = std::sqrt(dx * dx + dy * dy);
    *yaw = (std::fabs(dx) + std::fabs(dy) > 1e-5f) ? std::atan2(dy, dx) : 0.0f;
    *pitch = std::atan2(dz, h > 1e-5f ? h : 1e-5f);
}

}  // namespace cine
