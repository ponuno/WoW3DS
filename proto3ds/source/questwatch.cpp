// questwatch.cpp — 6.63: see questwatch.hpp.
#include "questwatch.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace questwatch {

static bool has(const std::vector<uint32_t>& v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); }

bool Watch::tracked(uint32_t id) const { return has(ids_, id); }

int Watch::slot(uint32_t id) const {
    for (size_t i = 0; i < ids_.size(); ++i) if (ids_[i] == id) return (int)i;
    return -1;
}

bool Watch::toggle(uint32_t id) {
    if (!id) return false;
    auto it = std::find(ids_.begin(), ids_.end(), id);
    if (it != ids_.end()) { ids_.erase(it); dirty_ = true; return true; }
    if ((int)ids_.size() >= kMax) return false;
    ids_.push_back(id);
    dirty_ = true;
    return true;
}

void Watch::remove(uint32_t id) {
    auto it = std::find(ids_.begin(), ids_.end(), id);
    if (it != ids_.end()) { ids_.erase(it); dirty_ = true; }
}

bool Watch::sync(const std::vector<uint32_t>& logIds, bool ready) {
    if (!ready) return false;
    bool changed = false;
    for (size_t i = ids_.size(); i-- > 0;)
        if (!has(logIds, ids_[i])) { ids_.erase(ids_.begin() + (long)i); changed = true; }
    if (learned_) {
        for (uint32_t id : logIds)
            if (!has(known_, id) && !has(ids_, id) && (int)ids_.size() < kMax) { ids_.push_back(id); changed = true; }
    }
    known_ = logIds;
    learned_ = true;
    if (changed) dirty_ = true;
    return changed;
}

std::string Watch::serialize() const {
    std::string s;
    for (uint32_t id : ids_) s += std::to_string(id) + "\n";
    return s;
}

void Watch::parse(const std::string& text) {
    ids_.clear();
    size_t i = 0;
    while (i < text.size() && (int)ids_.size() < kMax) {
        const unsigned long v = std::strtoul(text.c_str() + i, nullptr, 10);
        if (v && v < 0x10000000UL && !has(ids_, (uint32_t)v)) ids_.push_back((uint32_t)v);
        const size_t nl = text.find('\n', i);
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    dirty_ = false;
}

}  // namespace questwatch
