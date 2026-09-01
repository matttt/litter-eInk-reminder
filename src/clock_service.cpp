#include "clock_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <SensorPCF8563.hpp>

#include <limits.h>

#include "app_config.h"

namespace {

SensorPCF8563 rtc;
bool timeSynced = false;

constexpr uint32_t SchedulerMagic = 0x53434844;  // "SCHD"
constexpr uint16_t SchedulerVersion = 1;
constexpr int64_t SecondsPerDay = 24LL * 60 * 60;
constexpr int64_t SecondsPerMinute = 60;
constexpr uint8_t MaxAlarmArmAttempts = 3;
constexpr uint8_t RtcI2cAddress = 0x51;
constexpr uint8_t RtcControlStatus2Register = 0x01;
constexpr uint8_t RtcTimerControlRegister = 0x0E;
constexpr uint8_t RtcTimerValueRegister = 0x0F;
constexpr uint8_t RtcAlarmFlag = 1 << 3;
constexpr uint8_t RtcTimerFlag = 1 << 2;
constexpr uint8_t RtcAlarmInterruptEnable = 1 << 1;
constexpr uint8_t RtcTimerInterruptEnable = 1 << 0;
constexpr uint8_t RtcInterruptStateMask =
    RtcAlarmFlag | RtcTimerFlag |
    RtcAlarmInterruptEnable | RtcTimerInterruptEnable;
constexpr uint8_t CountdownPowerSavingFrequency = 0x03;
constexpr ClockService::ScheduledEventMask KnownScheduledEvents =
    ClockService::ScheduledEventMidnight |
    ClockService::ScheduledEventCleaningReminder;

struct RetainedSchedulerState {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    int64_t nextMidnightEpoch;
    int64_t cleaningReminderEpoch;
    int64_t armedAlarmEpoch;
    uint8_t cleaningReminderPending;
    ClockService::ScheduledEventMask deferredEvents;
    ClockService::ScheduledEventMask armedEvents;
    uint8_t reserved[5];
};

RTC_DATA_ATTR RetainedSchedulerState retainedScheduler = {};

bool schedulerStateIsValid()
{
    if (retainedScheduler.magic != SchedulerMagic ||
        retainedScheduler.version != SchedulerVersion ||
        retainedScheduler.size != sizeof(RetainedSchedulerState)) {
        return false;
    }

    if (retainedScheduler.nextMidnightEpoch <= 0 ||
        retainedScheduler.cleaningReminderPending > 1) {
        return false;
    }

    if ((retainedScheduler.deferredEvents & ~KnownScheduledEvents) != 0 ||
        (retainedScheduler.armedEvents & ~KnownScheduledEvents) != 0) {
        return false;
    }

    return !retainedScheduler.cleaningReminderPending ||
           retainedScheduler.cleaningReminderEpoch > 0;
}

bool localTimeToEpoch(const struct tm *localTime, int64_t *epoch)
{
    if (!localTime || !epoch) return false;

    struct tm value = *localTime;
    // Preserve tm_isdst when this came from localtime_r(); it distinguishes
    // the two occurrences of a repeated wall-clock time at the autumn fold.
    // Fall back to automatic selection only for an invalid hint.
    if (value.tm_isdst < -1 || value.tm_isdst > 1) value.tm_isdst = -1;
    time_t converted = mktime(&value);
    if (converted == (time_t)-1) return false;

    *epoch = static_cast<int64_t>(converted);
    return true;
}

bool epochToLocalTime(int64_t epoch, struct tm *localTime)
{
    if (!localTime) return false;

    time_t converted = static_cast<time_t>(epoch);
    if (static_cast<int64_t>(converted) != epoch) return false;
    return localtime_r(&converted, localTime) != nullptr;
}

bool epochToUtcTime(int64_t epoch, struct tm *utcTime)
{
    if (!utcTime) return false;

    time_t converted = static_cast<time_t>(epoch);
    if (static_cast<int64_t>(converted) != epoch) return false;
    return gmtime_r(&converted, utcTime) != nullptr;
}

bool isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t daysInMonth(int year, uint8_t month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && isLeapYear(year)) return 29;
    return days[month - 1];
}

// Howard Hinnant's proleptic-Gregorian civil-date conversion, with the Unix
// epoch offset folded in. Unlike mktime(), this calculation is independent of
// the configured local time zone and does not require non-portable timegm().
int64_t daysFromCivil(int year, uint8_t month, uint8_t day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
    unsigned dayOfYear =
        (153 * adjustedMonth + 2) / 5 + day - 1;
    unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 -
                         yearOfEra / 100 + dayOfYear;
    return static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
}

bool utcFieldsToEpoch(int year, uint8_t month, uint8_t day,
                      uint8_t hour, uint8_t minute, uint8_t second,
                      int64_t *epoch)
{
    if (!epoch || year < 1970 || year > 2099 ||
        month < 1 || month > 12 ||
        day < 1 || day > daysInMonth(year, month) ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    int64_t days = daysFromCivil(year, month, day);
    *epoch = days * SecondsPerDay +
             static_cast<int64_t>(hour) * 60 * 60 +
             static_cast<int64_t>(minute) * 60 + second;
    return true;
}

bool readRtcUtcEpoch(int64_t *epoch, RTC_DateTime *rawTime = nullptr)
{
    if (!epoch) return false;

    if (!rtc.isClockIntegrityGuaranteed()) {
        Serial.println("PCF8563 clock integrity is not guaranteed (VL set).");
        return false;
    }

    RTC_DateTime raw = rtc.getDateTime();
    if (rawTime) *rawTime = raw;

    if (raw.getWeek() > 6 ||
        !utcFieldsToEpoch(raw.getYear(), raw.getMonth(), raw.getDay(),
                          raw.getHour(), raw.getMinute(), raw.getSecond(),
                          epoch)) {
        Serial.printf(
            "Invalid PCF8563 UTC value: %04u-%02u-%02u %02u:%02u:%02u week=%u\n",
            static_cast<unsigned>(raw.getYear()),
            static_cast<unsigned>(raw.getMonth()),
            static_cast<unsigned>(raw.getDay()),
            static_cast<unsigned>(raw.getHour()),
            static_cast<unsigned>(raw.getMinute()),
            static_cast<unsigned>(raw.getSecond()),
            static_cast<unsigned>(raw.getWeek())
        );
        return false;
    }
    return true;
}

bool calculateNextLocalMidnight(int64_t nowEpoch, int64_t *midnightEpoch)
{
    if (!midnightEpoch) return false;

    struct tm midnight;
    if (!epochToLocalTime(nowEpoch, &midnight)) return false;

    midnight.tm_hour = 0;
    midnight.tm_min = 0;
    midnight.tm_sec = 0;
    midnight.tm_mday++;
    midnight.tm_isdst = -1;

    // The loop also covers unusual time-zone transitions that normalize a
    // nominal midnight to an instant which is not strictly after `now`.
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        time_t converted = mktime(&midnight);
        if (converted == (time_t)-1) return false;

        int64_t candidate = static_cast<int64_t>(converted);
        if (candidate > nowEpoch) {
            *midnightEpoch = candidate;
            return true;
        }

        midnight.tm_mday++;
        midnight.tm_hour = 0;
        midnight.tm_min = 0;
        midnight.tm_sec = 0;
        midnight.tm_isdst = -1;
    }

    return false;
}

bool ceilToMinute(int64_t logicalEpoch, int64_t *alarmEpoch)
{
    if (!alarmEpoch || logicalEpoch < 0) return false;

    int64_t remainder = logicalEpoch % SecondsPerMinute;
    if (remainder == 0) {
        *alarmEpoch = logicalEpoch;
        return true;
    }

    int64_t increment = SecondsPerMinute - remainder;
    if (logicalEpoch > INT64_MAX - increment) return false;
    *alarmEpoch = logicalEpoch + increment;
    return true;
}

bool writeRtcRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(RtcI2cAddress);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool readRtcRegister(uint8_t reg, uint8_t *value)
{
    if (!value) return false;

    Wire.beginTransmission(RtcI2cAddress);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(RtcI2cAddress, static_cast<uint8_t>(1)) != 1) {
        return false;
    }

    *value = static_cast<uint8_t>(Wire.read());
    return true;
}

void clearPhysicalAlarm()
{
    // Control_status_2 flags are write-zero-to-clear. A known zero write both
    // releases INT and disables AIE/TIE without SensorLib's read-modify-write
    // helpers accidentally preserving AF/TF (or turning a failed read into
    // 0xFF). The countdown timer is unused and shares the same INT output.
    bool statusCleared =
        writeRtcRegister(RtcControlStatus2Register, 0x00);
    bool timerDisabled = writeRtcRegister(
        RtcTimerControlRegister, CountdownPowerSavingFrequency
    );
    bool timerCleared = writeRtcRegister(RtcTimerValueRegister, 0x00);
    if (!statusCleared || !timerDisabled || !timerCleared) {
        Serial.println("Failed to fully clear PCF8563 interrupt state.");
    }

    retainedScheduler.armedAlarmEpoch = 0;
    retainedScheduler.armedEvents = ClockService::ScheduledEventNone;
}

bool initialiseSchedulerState(int64_t nowEpoch)
{
    int64_t nextMidnight;
    if (!calculateNextLocalMidnight(nowEpoch, &nextMidnight)) return false;

    RetainedSchedulerState fresh = {};
    fresh.version = SchedulerVersion;
    fresh.size = sizeof(RetainedSchedulerState);
    fresh.nextMidnightEpoch = nextMidnight;
    fresh.magic = SchedulerMagic;
    retainedScheduler = fresh;
    return true;
}

bool reconcileSchedulerState(
    int64_t nowEpoch,
    ClockService::ScheduledEventMask *newlyDue)
{
    if (!newlyDue) return false;
    *newlyDue = ClockService::ScheduledEventNone;

    if (!schedulerStateIsValid()) {
        return initialiseSchedulerState(nowEpoch);
    }

    // Preserve a due rollover before canonicalizing the retained deadline.
    // A long sleep therefore emits one catch-up event, not one per missed day.
    if (nowEpoch >= retainedScheduler.nextMidnightEpoch) {
        *newlyDue |= ClockService::ScheduledEventMidnight;
    }

    int64_t nextMidnight;
    if (!calculateNextLocalMidnight(nowEpoch, &nextMidnight)) return false;

    // Recompute on every service so forward/backward NTP corrections cannot
    // leave a stale calendar deadline in RTC memory.
    retainedScheduler.nextMidnightEpoch = nextMidnight;

    if (retainedScheduler.cleaningReminderPending &&
        nowEpoch >= retainedScheduler.cleaningReminderEpoch) {
        *newlyDue |= ClockService::ScheduledEventCleaningReminder;
        retainedScheduler.cleaningReminderPending = false;
        retainedScheduler.cleaningReminderEpoch = 0;
    }

    retainedScheduler.deferredEvents |= *newlyDue;
    return true;
}

bool armEarliestScheduledEvent(int64_t nowEpoch)
{
    int64_t schedulingNowEpoch = nowEpoch;

    for (uint8_t attempt = 0; attempt < MaxAlarmArmAttempts; ++attempt) {
        int64_t midnightAlarmEpoch;
        if (!ceilToMinute(retainedScheduler.nextMidnightEpoch,
                          &midnightAlarmEpoch)) {
            return false;
        }

        int64_t earliestAlarmEpoch = midnightAlarmEpoch;
        ClockService::ScheduledEventMask earliestEvents =
            ClockService::ScheduledEventMidnight;

        if (retainedScheduler.cleaningReminderPending &&
            retainedScheduler.cleaningReminderEpoch > schedulingNowEpoch) {
            int64_t reminderAlarmEpoch;
            if (!ceilToMinute(retainedScheduler.cleaningReminderEpoch,
                              &reminderAlarmEpoch)) {
                return false;
            }

            if (reminderAlarmEpoch < earliestAlarmEpoch) {
                earliestAlarmEpoch = reminderAlarmEpoch;
                earliestEvents =
                    ClockService::ScheduledEventCleaningReminder;
            } else if (reminderAlarmEpoch == earliestAlarmEpoch) {
                earliestEvents |=
                    ClockService::ScheduledEventCleaningReminder;
            }
        }

        // Logical deadlines at or before now must have been consumed before
        // arm. PCF8563 AF is raised only on the transition into a matching
        // minute, not merely because the programmed fields currently match.
        if (earliestAlarmEpoch <= schedulingNowEpoch) return false;

        struct tm alarmUtc;
        if (!epochToUtcTime(earliestAlarmEpoch, &alarmUtc)) return false;

        // Alarm fields use the same UTC basis as the PCF8563 calendar
        // registers.
        rtc.setAlarm(alarmUtc.tm_hour, alarmUtc.tm_min,
                     alarmUtc.tm_mday, 0xFF);

        // AF and TF are write-zero-to-clear, write-one-to-preserve bits. Do not
        // use SensorLib's generic read-modify-write alarm helpers here: a stale
        // flag is copied back as one, and a failed register read becomes 0xFF.
        // This known write atomically clears both flags, disables the unused
        // timer interrupt, and enables only the calendar alarm interrupt.
        if (!writeRtcRegister(RtcControlStatus2Register,
                              RtcAlarmInterruptEnable)) {
            clearPhysicalAlarm();
            Serial.println("Failed to enable the PCF8563 calendar alarm.");
            return false;
        }

        retainedScheduler.armedAlarmEpoch = earliestAlarmEpoch;
        retainedScheduler.armedEvents = earliestEvents;

        Serial.printf(
            "RTC scheduler armed for %04d-%02d-%02d %02d:%02d UTC "
            "(events=0x%02x)\n",
            alarmUtc.tm_year + 1900, alarmUtc.tm_mon + 1,
            alarmUtc.tm_mday, alarmUtc.tm_hour, alarmUtc.tm_min,
            earliestEvents
        );

        // Closing the xx:59:59 race requires checking the hardware after all
        // I2C writes. If the RTC entered (or passed) the alarm minute before
        // AIE was safely active, consume the logical event while already awake
        // and retry with the next future deadline.
        int64_t actualNowEpoch;
        if (!readRtcUtcEpoch(&actualNowEpoch)) {
            // Keep the PCF's best-effort programming in place, but do not
            // persist it as a verified arm. The zero snapshot makes main add
            // the independent ESP hourly recovery wake before deep sleep.
            retainedScheduler.armedAlarmEpoch = 0;
            retainedScheduler.armedEvents =
                ClockService::ScheduledEventNone;
            Serial.println(
                "Could not verify RTC alarm after arming; leaving the PCF "
                "alarm enabled with hourly recovery."
            );
            return false;
        }
        if (actualNowEpoch < earliestAlarmEpoch) {
            uint8_t interruptState;
            if (!readRtcRegister(RtcControlStatus2Register,
                                 &interruptState)) {
                // As with a failed time verification, retain the best-effort
                // PCF programming but make main add the recovery timer.
                retainedScheduler.armedAlarmEpoch = 0;
                retainedScheduler.armedEvents =
                    ClockService::ScheduledEventNone;
                Serial.println(
                    "Could not verify PCF8563 interrupt state after arming; "
                    "leaving the alarm enabled with hourly recovery."
                );
                return false;
            }

            bool interruptStateValid =
                (interruptState & RtcInterruptStateMask) ==
                RtcAlarmInterruptEnable;
            bool interruptReleased =
                digitalRead(AppConfig::RtcIrqPin) == HIGH;
            if (interruptStateValid && interruptReleased) return true;

            Serial.printf(
                "PCF8563 alarm arm left INT active "
                "(status2=0x%02x, pin=%s); retrying.\n",
                static_cast<unsigned>(interruptState),
                interruptReleased ? "HIGH" : "LOW"
            );
            clearPhysicalAlarm();

            ClockService::ScheduledEventMask newlyDue;
            if (!reconcileSchedulerState(actualNowEpoch, &newlyDue)) {
                return false;
            }
            schedulingNowEpoch = actualNowEpoch;
            continue;
        }

        Serial.println(
            "RTC crossed the armed minute during programming; "
            "servicing the elapsed event now."
        );
        clearPhysicalAlarm();

        ClockService::ScheduledEventMask newlyDue;
        if (!reconcileSchedulerState(actualNowEpoch, &newlyDue)) {
            return false;
        }
        schedulingNowEpoch = actualNowEpoch;
    }

    Serial.println("RTC alarm arm retry limit reached.");
    return false;
}

bool rearmAfterStateChange(int64_t nowEpoch)
{
    clearPhysicalAlarm();

    ClockService::ScheduledEventMask newlyDue;
    if (!reconcileSchedulerState(nowEpoch, &newlyDue)) {
        Serial.println("Failed to reconcile RTC scheduler state.");
        return false;
    }

    if (!armEarliestScheduledEvent(nowEpoch)) {
        Serial.println("Failed to arm next RTC scheduler event.");
        return false;
    }
    return true;
}

void timeAvailable(struct timeval *timeValue)
{
    (void)timeValue;
    Serial.println("[NTP] Got time adjustment");
    timeSynced = true;
}

}  // namespace

namespace ClockService {

void configureLocalTimezone()
{
    setenv("TZ", AppConfig::TimeZone, 1);
    tzset();
}

void syncFromNtp()
{
    timeSynced = false;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    sntp_set_time_sync_notification_cb(timeAvailable);
    configTzTime(AppConfig::TimeZone,
                 AppConfig::NtpServer1,
                 AppConfig::NtpServer2);
    WiFi.begin(AppConfig::WifiSsid, AppConfig::WifiPassword);

    Serial.print("Connecting to WiFi");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        Serial.print(".");
        delay(500);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected, waiting for NTP...");
        start = millis();
        while (!timeSynced && millis() - start < 15000) delay(250);
    }

    if (timeSynced) {
        time_t nowEpoch = time(nullptr);
        struct tm utcTime;
        if (nowEpoch != (time_t)-1 && gmtime_r(&nowEpoch, &utcTime)) {
            rtc.setDateTime(
                utcTime.tm_year + 1900, utcTime.tm_mon + 1,
                utcTime.tm_mday, utcTime.tm_hour,
                utcTime.tm_min, utcTime.tm_sec
            );
            Serial.println("External RTC set to UTC from NTP.");
        } else {
            Serial.println("NTP synced, but UTC conversion failed.");
        }
    } else {
        Serial.println("NTP sync failed.");
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(250);
}

bool initialise()
{
    if (!rtc.begin(Wire, AppConfig::RtcSdaPin, AppConfig::RtcSclPin)) {
        Serial.println("Failed to find PCF8563!");
        return false;
    }
    return true;
}

bool getLocalTime(struct tm *out)
{
    if (!out) {
        Serial.println("Cannot read local time into a null output.");
        return false;
    }
    *out = {};

    int64_t utcEpoch;
    if (!readRtcUtcEpoch(&utcEpoch)) return false;
    return epochToLocalTime(utcEpoch, out);
}

void logTime()
{
    RTC_DateTime raw;
    int64_t utcEpoch;
    if (!readRtcUtcEpoch(&utcEpoch, &raw)) {
        Serial.println("[RTC UTC]: invalid");
        return;
    }

    Serial.printf("[RTC UTC]: %04u-%02u-%02u %02u:%02u:%02u\n",
                  static_cast<unsigned>(raw.getYear()),
                  static_cast<unsigned>(raw.getMonth()),
                  static_cast<unsigned>(raw.getDay()),
                  static_cast<unsigned>(raw.getHour()),
                  static_cast<unsigned>(raw.getMinute()),
                  static_cast<unsigned>(raw.getSecond()));
}

void clearAlarmFlag()
{
    // AF and the countdown timer's TF share the active-low INT output. Clear
    // both before wake handling so neither source can immediately retrigger
    // EXT1 deep-sleep wake.
    clearPhysicalAlarm();
}

ScheduledEventMask serviceScheduledEvents(const struct tm *now)
{
    // Always release a latched AF/TF before inspecting or replacing the
    // retained schedule. This makes the operation safe on button wakes too.
    clearPhysicalAlarm();

    int64_t nowEpoch;
    if (!localTimeToEpoch(now, &nowEpoch)) {
        Serial.println("Failed to convert scheduler service time.");
        return ScheduledEventNone;
    }

    ScheduledEventMask newlyDue;
    if (!reconcileSchedulerState(nowEpoch, &newlyDue)) {
        Serial.println("Failed to reconcile RTC scheduler state.");
        return ScheduledEventNone;
    }

    if (!armEarliestScheduledEvent(nowEpoch)) {
        Serial.println("Failed to arm next RTC scheduler event.");
    }

    // Capture after arming too: post-arm verification can discover that the
    // RTC crossed a deadline during I2C programming. Return it in this same
    // service pass so callers repaint before sleeping.
    ScheduledEventMask due = retainedScheduler.deferredEvents;
    retainedScheduler.deferredEvents = ScheduledEventNone;
    return due;
}

ScheduledEventMask serviceScheduledEvents()
{
    struct tm now;
    if (!ClockService::getLocalTime(&now)) {
        clearPhysicalAlarm();
        Serial.println("Failed to read RTC time for scheduler service.");
        return ScheduledEventNone;
    }
    return serviceScheduledEvents(&now);
}

bool scheduleCleaningReminder24HoursAfter(const struct tm *baseTime)
{
    int64_t baseEpoch;
    if (!localTimeToEpoch(baseTime, &baseEpoch) ||
        baseEpoch > INT64_MAX - SecondsPerDay) {
        Serial.println("Failed to convert reminder base time.");
        return false;
    }

    struct tm now;
    int64_t nowEpoch;
    if (!ClockService::getLocalTime(&now) ||
        !localTimeToEpoch(&now, &nowEpoch)) {
        Serial.println("Failed to read RTC time for reminder scheduling.");
        return false;
    }

    if (!schedulerStateIsValid() && !initialiseSchedulerState(nowEpoch)) {
        clearPhysicalAlarm();
        Serial.println("Failed to initialise RTC scheduler state.");
        return false;
    }

    // A new cleaning supersedes both the previous logical reminder and a
    // reminder event deferred during an earlier state change.
    retainedScheduler.deferredEvents &=
        ~ScheduledEventCleaningReminder;
    int64_t reminderEpoch = baseEpoch + SecondsPerDay;
    retainedScheduler.cleaningReminderEpoch = reminderEpoch;
    retainedScheduler.cleaningReminderPending = true;

    if (!rearmAfterStateChange(nowEpoch)) return false;

    struct tm target;
    if (epochToLocalTime(reminderEpoch, &target)) {
        Serial.printf("Cleaning reminder due locally "
                      "%04d-%02d-%02d %02d:%02d:%02d\n",
                      target.tm_year + 1900, target.tm_mon + 1,
                      target.tm_mday, target.tm_hour, target.tm_min,
                      target.tm_sec);
    }
    return true;
}

bool scheduleCleaningReminder24HoursFromNow()
{
    struct tm now;
    if (!ClockService::getLocalTime(&now)) {
        Serial.println("Failed to read RTC time for reminder scheduling.");
        return false;
    }
    return scheduleCleaningReminder24HoursAfter(&now);
}

bool cancelCleaningReminder()
{
    struct tm now;
    int64_t nowEpoch;
    if (!ClockService::getLocalTime(&now) ||
        !localTimeToEpoch(&now, &nowEpoch)) {
        clearPhysicalAlarm();
        Serial.println("Failed to read RTC time while cancelling reminder.");
        return false;
    }

    if (schedulerStateIsValid()) {
        retainedScheduler.cleaningReminderEpoch = 0;
        retainedScheduler.cleaningReminderPending = false;
        retainedScheduler.deferredEvents &=
            ~ScheduledEventCleaningReminder;
    }
    return rearmAfterStateChange(nowEpoch);
}

bool resetScheduledEvents(const struct tm *now)
{
    clearPhysicalAlarm();

    int64_t nowEpoch;
    if (!localTimeToEpoch(now, &nowEpoch) ||
        !initialiseSchedulerState(nowEpoch)) {
        Serial.println("Failed to reset RTC scheduler state.");
        return false;
    }
    if (!armEarliestScheduledEvent(nowEpoch)) {
        Serial.println("Failed to arm midnight after scheduler reset.");
        return false;
    }
    return true;
}

bool resetScheduledEvents()
{
    struct tm now;
    if (!ClockService::getLocalTime(&now)) {
        clearPhysicalAlarm();
        Serial.println("Failed to read RTC time while resetting scheduler.");
        return false;
    }
    return resetScheduledEvents(&now);
}

SchedulerSnapshot schedulerSnapshot()
{
    SchedulerSnapshot snapshot = {};
    snapshot.stateValid = schedulerStateIsValid();
    if (!snapshot.stateValid) return snapshot;

    snapshot.nextMidnightEpoch = retainedScheduler.nextMidnightEpoch;
    snapshot.cleaningReminderPending =
        retainedScheduler.cleaningReminderPending;
    snapshot.cleaningReminderEpoch =
        retainedScheduler.cleaningReminderEpoch;
    snapshot.deferredEvents = retainedScheduler.deferredEvents;
    snapshot.armedEvents = retainedScheduler.armedEvents;
    snapshot.armedAlarmEpoch = retainedScheduler.armedAlarmEpoch;
    return snapshot;
}

}  // namespace ClockService
