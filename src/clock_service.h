#pragma once

#include <stdint.h>
#include <time.h>

namespace ClockService {

enum ScheduledEvent : uint8_t {
    ScheduledEventNone             = 0,
    ScheduledEventMidnight         = 1 << 0,
    ScheduledEventCleaningReminder = 1 << 1
};

using ScheduledEventMask = uint8_t;

struct SchedulerSnapshot {
    bool stateValid;
    int64_t nextMidnightEpoch;
    bool cleaningReminderPending;
    int64_t cleaningReminderEpoch;
    ScheduledEventMask deferredEvents;
    ScheduledEventMask armedEvents;
    int64_t armedAlarmEpoch;
};

void configureLocalTimezone();
// Synchronize system time and write UTC calendar fields into the PCF8563.
void syncFromNtp();

bool initialise();
// The PCF8563 calendar is stored in UTC. This API converts it to the configured
// local time zone and populates derived tm fields, including tm_isdst.
bool getLocalTime(struct tm *out);
void logTime();

void clearAlarmFlag();

// Reconcile all logical deadlines against `now`, return every event that is
// due, and program the PCF8563 for the earliest remaining event. This is safe
// to call on every wake, including button wakes. The no-argument overload
// reads `now` from the external RTC.
ScheduledEventMask serviceScheduledEvents(const struct tm *now);
ScheduledEventMask serviceScheduledEvents();

// The cleaning reminder is a one-shot logical deadline. Its exact epoch is
// retained; only the physical PCF8563 alarm is rounded up to a whole minute.
// These functions also re-arm the hardware alarm. Call
// serviceScheduledEvents() once more before sleeping to collect an event that
// may have become due while the display was being updated.
bool scheduleCleaningReminder24HoursAfter(const struct tm *baseTime);
bool scheduleCleaningReminder24HoursFromNow();
bool cancelCleaningReminder();

// Discard retained scheduler state, create the next local-midnight deadline,
// and arm it. The magic/version guard also causes serviceScheduledEvents() to
// perform this reset automatically when RTC slow-memory state is invalid.
bool resetScheduledEvents(const struct tm *now);
bool resetScheduledEvents();

SchedulerSnapshot schedulerSnapshot();

}  // namespace ClockService
