#include "cleaning_history.h"

#include <Arduino.h>
#include <string.h>

namespace {

constexpr uint32_t RetainedHistoryMagic = 0x4C484953;  // "LHIS"
constexpr uint16_t RetainedHistoryVersion = 2;
constexpr uint32_t ChecksumOffsetBasis = 2166136261U;
constexpr uint32_t ChecksumPrime = 16777619U;

struct RetainedDay {
    uint32_t dateKey;
    uint8_t cleanerMask;
    uint8_t reserved[3];
};

struct RetainedHistory {
    uint32_t magic;
    uint16_t version;
    uint16_t dayCount;
    RetainedDay days[CleaningHistory::DayCount];
    uint32_t checksum;
};

// RTC slow memory survives deep-sleep wake cycles but not loss of power.
RTC_DATA_ATTR RetainedHistory retainedHistory = {};

CleaningHistory::Day history[CleaningHistory::DayCount] = {};
uint32_t visibleDateKeys[CleaningHistory::DayCount] = {};
bool historyValid = false;

bool isLeapYear(uint32_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool dateKeyIsValid(uint32_t dateKey)
{
    uint32_t year = dateKey / 10000;
    uint32_t month = (dateKey / 100) % 100;
    uint32_t day = dateKey % 100;

    if (year < 1900 || year > 9999 || month < 1 || month > 12 || day < 1) {
        return false;
    }

    static const uint8_t daysPerMonth[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    uint8_t maximumDay = daysPerMonth[month - 1];
    if (month == 2 && isLeapYear(year)) maximumDay = 29;
    return day <= maximumDay;
}

bool dateKeyFromTime(const struct tm *timeInfo, uint32_t *dateKey,
                     struct tm *normalisedTime = nullptr)
{
    if (!timeInfo || !dateKey) return false;

    // Calendar dates are normalised at noon. That keeps date arithmetic clear
    // of DST transitions, which normally happen in the early morning.
    struct tm day = *timeInfo;
    day.tm_hour = 12;
    day.tm_min = 0;
    day.tm_sec = 0;
    day.tm_isdst = -1;
    if (mktime(&day) == (time_t)-1) return false;

    int year = day.tm_year + 1900;
    int month = day.tm_mon + 1;
    int dayOfMonth = day.tm_mday;
    if (year < 1900 || year > 9999 || month < 1 || month > 12 ||
        dayOfMonth < 1 || dayOfMonth > 31) {
        return false;
    }

    uint32_t key = static_cast<uint32_t>(year * 10000 +
                                         month * 100 + dayOfMonth);
    if (!dateKeyIsValid(key)) return false;

    *dateKey = key;
    if (normalisedTime) *normalisedTime = day;
    return true;
}

bool cleanerMaskIsValid(uint8_t cleanerMask)
{
    return (cleanerMask & ~static_cast<uint8_t>(CleaningHistory::Both)) == 0;
}

void addChecksumByte(uint32_t *checksum, uint8_t value)
{
    *checksum ^= value;
    *checksum *= ChecksumPrime;
}

void addChecksumUint16(uint32_t *checksum, uint16_t value)
{
    addChecksumByte(checksum, static_cast<uint8_t>(value));
    addChecksumByte(checksum, static_cast<uint8_t>(value >> 8));
}

void addChecksumUint32(uint32_t *checksum, uint32_t value)
{
    addChecksumUint16(checksum, static_cast<uint16_t>(value));
    addChecksumUint16(checksum, static_cast<uint16_t>(value >> 16));
}

uint32_t retainedHistoryChecksum(const RetainedHistory &record)
{
    uint32_t checksum = ChecksumOffsetBasis;
    addChecksumUint32(&checksum, record.magic);
    addChecksumUint16(&checksum, record.version);
    addChecksumUint16(&checksum, record.dayCount);

    for (size_t i = 0; i < CleaningHistory::DayCount; ++i) {
        const RetainedDay &day = record.days[i];
        addChecksumUint32(&checksum, day.dateKey);
        addChecksumByte(&checksum, day.cleanerMask);
        for (uint8_t value : day.reserved) {
            addChecksumByte(&checksum, value);
        }
    }
    return checksum;
}

bool retainedHistoryIsValid()
{
    if (retainedHistory.magic != RetainedHistoryMagic ||
        retainedHistory.version != RetainedHistoryVersion ||
        retainedHistory.dayCount != CleaningHistory::DayCount ||
        retainedHistory.checksum !=
            retainedHistoryChecksum(retainedHistory)) {
        return false;
    }

    for (size_t i = 0; i < CleaningHistory::DayCount; ++i) {
        const RetainedDay &day = retainedHistory.days[i];
        if (!dateKeyIsValid(day.dateKey) ||
            !cleanerMaskIsValid(day.cleanerMask)) {
            return false;
        }

        for (size_t previous = 0; previous < i; ++previous) {
            if (retainedHistory.days[previous].dateKey == day.dateKey) {
                return false;
            }
        }
    }
    return true;
}

bool initialiseVisibleDays(const struct tm *now)
{
    if (!now) return false;

    for (size_t i = 0; i < CleaningHistory::DayCount; ++i) {
        struct tm day = *now;
        day.tm_mday -= static_cast<int>(i);

        if (!dateKeyFromTime(&day, &visibleDateKeys[i], &day)) {
            Serial.println("Failed to calculate cleaning-history date.");
            return false;
        }

        if (i == 0) {
            snprintf(history[i].label, sizeof(history[i].label), "Today");
        } else if (strftime(history[i].label, sizeof(history[i].label),
                            "%A", &day) == 0) {
            return false;
        }
        history[i].cleanerMask = CleaningHistory::None;
    }

    return true;
}

void reconcileRetainedHistory()
{
    if (!retainedHistoryIsValid()) {
        memset(&retainedHistory, 0, sizeof(retainedHistory));
        return;
    }

    for (size_t visible = 0; visible < CleaningHistory::DayCount; ++visible) {
        for (size_t retained = 0; retained < CleaningHistory::DayCount;
             ++retained) {
            if (visibleDateKeys[visible] ==
                retainedHistory.days[retained].dateKey) {
                history[visible].cleanerMask |=
                    retainedHistory.days[retained].cleanerMask;
            }
        }
    }
}

void addCleaning(uint32_t dateKey, uint8_t cleanerMask)
{
    for (size_t i = 0; i < CleaningHistory::DayCount; ++i) {
        if (visibleDateKeys[i] == dateKey) {
            history[i].cleanerMask |= cleanerMask;
            return;
        }
    }
}

void retainVisibleHistory()
{
    // Build the complete record before copying it into RTC memory. Validation
    // does not depend on write order: a torn copy will fail its checksum.
    RetainedHistory fresh = {};
    fresh.magic = RetainedHistoryMagic;
    fresh.version = RetainedHistoryVersion;
    fresh.dayCount =
        static_cast<uint16_t>(CleaningHistory::DayCount);

    for (size_t i = 0; i < CleaningHistory::DayCount; ++i) {
        fresh.days[i].dateKey = visibleDateKeys[i];
        fresh.days[i].cleanerMask = history[i].cleanerMask;
    }

    fresh.checksum = retainedHistoryChecksum(fresh);
    retainedHistory = fresh;
}

}  // namespace

namespace CleaningHistory {

const char *cleanerName(Cleaner cleaner)
{
    switch (cleaner) {
        case Oriana:  return "Oriana";
        case Matthew: return "Matthew";
        case Both:    return "Oriana+Matthew";
        default:      return "Unknown";
    }
}

const Day *days()
{
    return history;
}

bool isValid()
{
    return historyValid;
}

void refresh(const struct tm *now,
             const struct tm *newCleaningTime,
             Cleaner newCleaner)
{
    historyValid = initialiseVisibleDays(now);
    if (!historyValid) return;

    reconcileRetainedHistory();

    uint8_t cleanerMask = static_cast<uint8_t>(newCleaner);
    if (newCleaningTime && newCleaner != None &&
        cleanerMaskIsValid(cleanerMask)) {
        uint32_t cleaningDateKey = 0;
        if (dateKeyFromTime(newCleaningTime, &cleaningDateKey)) {
            addCleaning(cleaningDateKey, cleanerMask);
        }
    }

    retainVisibleHistory();
}

}  // namespace CleaningHistory
