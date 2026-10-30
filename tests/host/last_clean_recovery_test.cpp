// Exercise production wake handling with a persistent NVS fake and mocked
// clock/display services. No physical sleep or display driver is involved.
#include <Arduino.h>
#include "../../src/main.cpp"
#include "../../src/last_clean_state.cpp"
#include <cassert>
#include <cstdlib>
#include <iostream>

static bool clockValid = true;
static struct tm clockNow = {};
static time_t reminderBase = 0;
static bool reminderPending = false;
static bool showingClean = false;
static unsigned loggedCleanings = 0;
static CleaningHistory::Cleaner loggedCleaner = CleaningHistory::None;

static time_t toEpoch(struct tm value) { return mktime(&value); }
static void setClock(time_t epoch) { localtime_r(&epoch, &clockNow); }

namespace ClockService {
void configureLocalTimezone() {}
void syncFromNtp() {}
bool initialise() { return true; }
bool getLocalTime(struct tm *out) { *out = clockNow; return clockValid; }
void logTime() {}
void clearAlarmFlag() {}
bool resetScheduledEvents(const struct tm *) { reminderPending = false; return true; }
bool resetScheduledEvents() { reminderPending = false; return clockValid; }
bool scheduleCleaningReminder24HoursAfter(const struct tm *base) {
    reminderBase = toEpoch(*base);
    reminderPending = true;
    return clockValid;
}
ScheduledEventMask serviceScheduledEvents(const struct tm *) {
    if (reminderPending && toEpoch(clockNow) >= reminderBase + 86400) {
        reminderPending = false;
        return ScheduledEventCleaningReminder;
    }
    return ScheduledEventNone;
}
ScheduledEventMask serviceScheduledEvents() { return serviceScheduledEvents(&clockNow); }
SchedulerSnapshot schedulerSnapshot() { return {}; }
}

namespace DisplayUi {
bool initialise() { return true; }
void showDirty(const State &) { showingClean = false; }
void showClean(const State &) { showingClean = true; }
void prepareForDeepSleep() {}
}

namespace CleaningHistory {
const char *cleanerName(Cleaner) { return "test"; }
void refresh(const struct tm *, const struct tm *cleaned, Cleaner cleaner) {
    if (cleaned) {
        // Flash must have committed before history or display work.
        assert(!nvsBlob.empty() && nvsOpenHandles == 0);
        ++loggedCleanings;
        loggedCleaner = cleaner;
    }
}
}

static void loseRtcMemory()
{
    rtcCleanTimestampValid = false;
    rtcReminderOverdue = false;
    rtcLastCleanTime = {};
    rtcPendingCleanerMask = CleaningHistory::None;
    awakeCleanerMask = CleaningHistory::None;
    awakeCleanerCaptureActive = false;
    reminderBase = 0;
    reminderPending = false;
}

int main()
{
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    clockNow.tm_year = 126;
    clockNow.tm_mon = 9;
    clockNow.tm_mday = 30;
    clockNow.tm_hour = 12;
    clockNow.tm_isdst = -1;
    const time_t cleanedAt = toEpoch(clockNow);
    setClock(cleanedAt);

    handleColdBoot();
    assert(!showingClean && nvsWrites == 0);
    recordCleanerMask(CleaningHistory::Oriana);
    assert(showingClean && nvsWrites == 1 && loggedCleanings == 1);

    // An ordinary reset and complete loss of RTC memory both retain the same
    // timestamp/deadline, without adding a CSV entry or writing NVS again.
    loseRtcMemory();
    setClock(cleanedAt + 3600);
    handleColdBoot();
    assert(showingClean && rtcCleanTimestampValid && !rtcReminderOverdue);
    assert(toEpoch(rtcLastCleanTime) == cleanedAt && reminderBase == cleanedAt);
    assert(nvsWrites == 1 && loggedCleanings == 1);
    loseRtcMemory();
    setClock(cleanedAt + 86400);
    handleColdBoot();
    assert(!showingClean && rtcReminderOverdue && reminderBase == cleanedAt);
    assert(nvsWrites == 1 && loggedCleanings == 1);

    // If NTP/RTC is unavailable at boot, keep the timestamp and recover its
    // original reminder once the hourly retry obtains a valid time.
    loseRtcMemory();
    clockValid = false;
    handleColdBoot();
    assert(rtcCleanTimestampValid && !showingClean);
    clockValid = true;
    setClock(cleanedAt + 7200);
    WakeSources recovery = {};
    recovery.clockRecovery = true;
    handleClockRecoveryWake(recovery);
    assert(showingClean && !rtcReminderOverdue && reminderBase == cleanedAt);

    // Undated cleaner masks persist through multiple power losses, including
    // a second button press arriving on an unsuccessful recovery wake.
    clockValid = false;
    recordCleanerMask(CleaningHistory::Oriana);
    loseRtcMemory();
    handleColdBoot();
    assert(showingClean && !rtcCleanTimestampValid);
    assert(rtcPendingCleanerMask == CleaningHistory::Oriana);
    recovery.ext1Status = 1ULL << AppConfig::MatthewButtonPin;
    handleClockRecoveryWake(recovery);
    assert(rtcPendingCleanerMask == CleaningHistory::Both);
    loseRtcMemory();
    clockValid = true;
    setClock(cleanedAt + 10000);
    handleColdBoot();
    assert(showingClean && rtcCleanTimestampValid);
    assert(rtcPendingCleanerMask == CleaningHistory::None);
    assert(loggedCleaner == CleaningHistory::Both && loggedCleanings == 2);
    assert(toEpoch(rtcLastCleanTime) == cleanedAt + 10000);
    assert(nvsOpenHandles == 0);
    std::cout << "Last-clean recovery host tests passed\n";
}
