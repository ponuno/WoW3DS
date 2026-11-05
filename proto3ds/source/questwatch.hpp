// questwatch.hpp — 6.63: the quests the player follows (the original's quest watch: up to five). Pure logic, shared with the PC test.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace questwatch {

constexpr int kMax = 5;

class Watch {
public:
    bool tracked(uint32_t id) const;
    // Follows / stops following. Following a sixth quest is refused (false); stopping always works.
    bool toggle(uint32_t id);
    void remove(uint32_t id);
    // Every frame with the ids in the quest log. 'ready' says the log has arrived (an empty log before that must not wipe the saved
    // watch). Quests that left the log are dropped; a quest that is new since the last call is followed while there is room
    // (the original does the same on accepting). The first ready call only learns what is there. Returns true when the watch changed.
    bool sync(const std::vector<uint32_t>& logIds, bool ready);
    const std::vector<uint32_t>& ids() const { return ids_; }
    // 0..kMax-1: the slot (colour) of a followed quest, -1 when not followed.
    int slot(uint32_t id) const;
    void clear() { ids_.clear(); known_.clear(); learned_ = false; }
    // One id a line.
    std::string serialize() const;
    void parse(const std::string& text);
    bool changedSinceSave() const { return dirty_; }
    void saved() { dirty_ = false; }

private:
    std::vector<uint32_t> ids_;     // in the order they were followed
    std::vector<uint32_t> known_;   // the log of the last ready call
    bool learned_ = false;
    bool dirty_ = false;
};

}  // namespace questwatch
