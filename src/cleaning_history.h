#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace CleaningHistory {

enum Cleaner : uint8_t {
    None    = 0,
    Oriana  = 1 << 0,
    Matthew = 1 << 1,
    Both    = Oriana | Matthew
};

struct Day {
    char label[12];
    uint8_t cleanerMask;
};

constexpr size_t DayCount = 7;

const char *cleanerName(Cleaner cleaner);
const Day *days();
bool isValid();

void refresh(const struct tm *now,
             const struct tm *newCleaningTime = nullptr,
             Cleaner newCleaner = None);

}  // namespace CleaningHistory
