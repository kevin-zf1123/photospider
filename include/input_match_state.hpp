#pragma once

#include <string>

namespace ps {

// A small shared state machine to keep a sticky prefix while navigating
// matches (e.g., history Up/Down or completion Tab cycles). The prefix remains
// constant until Reset() is called due to edits or cursor moves.
struct InputMatchState {
    bool active = false;
    std::string original_prefix;
    size_t original_cursor_pos = 0;

    void Begin(const std::string& prefix, size_t cursor_pos) {
        active = true;
        original_prefix = prefix;
        original_cursor_pos = cursor_pos;
    }

    void Reset() {
        active = false;
        original_prefix.clear();
        original_cursor_pos = 0;
    }
};

} // namespace ps

