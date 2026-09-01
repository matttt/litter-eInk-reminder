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

struct Totals {
    uint32_t oriana;
    uint32_t matthew;
};

using SdAccessHook = void (*)();

constexpr size_t DayCount = 7;

const char *cleanerName(Cleaner cleaner);
const Day *days();
bool isValid();
const Totals &totals();
bool totalsAreValid();

// GPIO15 is both the Oriana button and SD MOSI. Main uses these hooks to
// suspend only that button's awake interrupt while SPI owns the pin.
void setSdAccessHooks(SdAccessHook beforeAccess, SdAccessHook afterAccess);

// Undo the retained RTC clock-pin configuration at boot, and reapply a
// deterministic clock-low state immediately before deep sleep.
void beginWakeCycle();
bool prepareForDeepSleep();

void refresh(const struct tm *now,
             const struct tm *newCleaningTime = nullptr,
             Cleaner newCleaner = None);

}  // namespace CleaningHistory
