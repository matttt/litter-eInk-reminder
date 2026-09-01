#pragma once

#include <time.h>

namespace DisplayUi {

struct State {
    bool lastCleanTimestampValid;
    struct tm lastCleanTime;
    bool reminderOverdue;
};

bool initialise();
void showDirty(const State &state);
void showClean(const State &state);

}  // namespace DisplayUi
