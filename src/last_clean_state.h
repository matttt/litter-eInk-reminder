#pragma once

#include <stdint.h>
#include <time.h>

namespace LastCleanState {

struct State {
    bool timestampValid;
    struct tm lastCleanTime;
    uint8_t pendingCleanerMask;
};

// Reads never create storage. Missing, incompatible, or invalid records return
// false and leave an empty state. Configure the local timezone before use.
bool load(State *state);
// Commits a single versioned NVS blob before returning. An undated cleaning is
// stored as a pending cleaner mask until the external clock recovers.
bool save(const State &state);

}  // namespace LastCleanState
