#include "cleaning_history.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"

namespace {

constexpr uint32_t RetainedHistoryMagic = 0x4C484953;  // "LHIS"
constexpr uint16_t RetainedHistoryVersion = 2;
constexpr uint32_t ChecksumOffsetBasis = 2166136261U;
constexpr uint32_t ChecksumPrime = 16777619U;
constexpr char CleaningCsvHeader[] = "timestamp,who\n";

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
CleaningHistory::Totals allTimeTotals = {};
bool allTimeTotalsValid = false;

void addCleaning(uint32_t dateKey, uint8_t cleanerMask);

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

bool dateKeyFromIsoDate(const char *text, uint32_t *dateKey)
{
    if (!text || !dateKey || strlen(text) < 10 ||
        text[4] != '-' || text[7] != '-') {
        return false;
    }

    static const uint8_t digitPositions[] = {0, 1, 2, 3, 5, 6, 8, 9};
    for (uint8_t position : digitPositions) {
        if (!isdigit(static_cast<unsigned char>(text[position]))) {
            return false;
        }
    }

    uint32_t year = static_cast<uint32_t>(text[0] - '0') * 1000U +
                    static_cast<uint32_t>(text[1] - '0') * 100U +
                    static_cast<uint32_t>(text[2] - '0') * 10U +
                    static_cast<uint32_t>(text[3] - '0');
    uint32_t month = static_cast<uint32_t>(text[5] - '0') * 10U +
                     static_cast<uint32_t>(text[6] - '0');
    uint32_t day = static_cast<uint32_t>(text[8] - '0') * 10U +
                   static_cast<uint32_t>(text[9] - '0');
    uint32_t key = year * 10000U + month * 100U + day;
    if (!dateKeyIsValid(key)) return false;

    *dateKey = key;
    return true;
}

uint8_t cleanerMaskFromName(const char *name)
{
    using namespace CleaningHistory;

    if (!name) return None;
    if (strcasecmp(name, "Oriana") == 0) return Oriana;
    if (strcasecmp(name, "Matthew") == 0) return Matthew;
    if (strcasecmp(name, "Oriana+Matthew") == 0 ||
        strcasecmp(name, "Matthew+Oriana") == 0) {
        return Both;
    }
    return None;
}

uint8_t cleanerMaskFromCsvLine(char *line)
{
    if (!line) return CleaningHistory::None;

    size_t length = strlen(line);
    while (length > 0 &&
           isspace(static_cast<unsigned char>(line[length - 1]))) {
        line[--length] = '\0';
    }

    uint32_t ignoredDateKey;
    if (!dateKeyFromIsoDate(line, &ignoredDateKey)) {
        return CleaningHistory::None;
    }

    char *comma = strchr(line, ',');
    if (!comma) return CleaningHistory::None;
    return cleanerMaskFromName(comma + 1);
}

bool parseUnsignedField(const char **cursor, uint64_t maximum,
                        char terminator, uint64_t *value)
{
    if (!cursor || !*cursor || !value ||
        !isdigit(static_cast<unsigned char>(**cursor))) {
        return false;
    }

    uint64_t parsed = 0;
    while (isdigit(static_cast<unsigned char>(**cursor))) {
        uint8_t digit = static_cast<uint8_t>(**cursor - '0');
        if (parsed > (maximum - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
        (*cursor)++;
    }

    if (terminator == '\0') {
        if (**cursor != '\0') return false;
    } else {
        if (**cursor != terminator) return false;
        (*cursor)++;
    }

    *value = parsed;
    return true;
}

void incrementTotals(uint8_t cleanerMask)
{
    if (cleanerMask & CleaningHistory::Oriana) allTimeTotals.oriana++;
    if (cleanerMask & CleaningHistory::Matthew) allTimeTotals.matthew++;
}

// Keep the existing partition label so firmware uploads do not move storage.
constexpr char FlashPartitionLabel[] = "spiffs";

bool flashPartitionIsBlank()
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        FlashPartitionLabel
    );
    if (!partition) return false;

    // Only initialise erased flash. A mount failure on an existing filesystem
    // must never silently erase cleaning records. Check the entire partition,
    // not just the first block (a damaged filesystem may have an erased head).
    uint8_t buffer[256];
    for (size_t offset = 0; offset < partition->size;) {
        size_t length = partition->size - offset;
        if (length > sizeof(buffer)) length = sizeof(buffer);
        if (esp_partition_read(partition, offset, buffer, length) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < length; ++i) {
            if (buffer[i] != 0xFF) return false;
        }
        offset += length;
        yield();
    }
    return true;
}

bool mountFlash()
{
    if (!LittleFS.begin(false, "/littlefs", 4, FlashPartitionLabel)) {
        if (!flashPartitionIsBlank()) {
            Serial.println(
                "Flash mount failed; preserving existing storage. "
                "Cleaning history will remain in RTC memory only."
            );
            return false;
        }
        Serial.println("Initialising blank internal flash storage...");
        if (!LittleFS.format() ||
            !LittleFS.begin(false, "/littlefs", 4, FlashPartitionLabel)) {
            Serial.println("Could not initialise internal flash storage.");
            return false;
        }
    }

    Serial.printf("Flash storage: %lu / %lu bytes used.\n",
                  static_cast<unsigned long>(LittleFS.usedBytes()),
                  static_cast<unsigned long>(LittleFS.totalBytes()));
    return true;
}

bool writeFreshCleaningCsvHeader()
{
    File file = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_WRITE);
    if (!file) {
        Serial.println("Could not create cleaning CSV header.");
        return false;
    }

    constexpr size_t HeaderLength = sizeof(CleaningCsvHeader) - 1;
    bool success = file.write(
        reinterpret_cast<const uint8_t *>(CleaningCsvHeader), HeaderLength
    ) == HeaderLength;
    file.flush();
    file.close();

    File verification = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_READ);
    success = success && verification &&
              verification.size() == HeaderLength;
    if (verification) verification.close();
    if (!success) Serial.println("Could not create cleaning CSV header.");
    return success;
}

bool prepareCsvForAppend()
{
    File file = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_READ);
    if (!file) {
        if (LittleFS.exists(AppConfig::CleaningsCsvPath)) {
            Serial.println("Existing cleaning CSV could not be read.");
            return false;
        }
        return writeFreshCleaningCsvHeader();
    }
    if (file.isDirectory()) {
        file.close();
        Serial.println("Cleaning CSV path is a directory; refusing to append.");
        return false;
    }

    size_t size = file.size();
    if (size == 0) {
        file.close();
        return writeFreshCleaningCsvHeader();
    }

    constexpr size_t HeaderLength = sizeof(CleaningCsvHeader) - 1;
    if (size < HeaderLength) {
        char prefix[sizeof(CleaningCsvHeader)] = {};
        size_t bytesRead = file.read(
            reinterpret_cast<uint8_t *>(prefix), size
        );
        file.close();

        if (bytesRead != size ||
            memcmp(prefix, CleaningCsvHeader, size) != 0) {
            Serial.println(
                "Cleaning CSV is too short and is not a partial header; "
                "refusing to append."
            );
            return false;
        }

        file = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_APPEND);
        if (!file) {
            Serial.println("Could not complete partial cleaning CSV header.");
            return false;
        }
        size_t remaining = HeaderLength - size;
        bool success = file.write(
            reinterpret_cast<const uint8_t *>(CleaningCsvHeader) + size,
            remaining
        ) == remaining;
        file.flush();
        file.close();

        File verification = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_READ);
        success = success && verification &&
                  verification.size() == HeaderLength;
        if (verification) verification.close();
        if (success) {
            Serial.println("Completed a partial cleaning CSV header.");
        } else {
            Serial.println("Could not complete partial cleaning CSV header.");
        }
        return success;
    }

    bool seekSucceeded = file.seek(size - 1);
    int lastCharacter = seekSucceeded ? file.read() : -1;
    file.close();
    if (lastCharacter == '\n') return true;

    file = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_APPEND);
    if (!file) {
        Serial.println("Could not repair unterminated cleaning CSV row.");
        return false;
    }

    bool success = file.write(static_cast<uint8_t>('\n')) == 1;
    file.flush();
    file.close();
    if (success) {
        Serial.println("Terminated an incomplete cleaning CSV tail.");
    } else {
        Serial.println("Could not repair unterminated cleaning CSV row.");
    }
    return success;
}

bool appendCleaningToCsv(const struct tm *cleanedAt,
                         CleaningHistory::Cleaner cleaner)
{
    if (!cleanedAt || cleaner == CleaningHistory::None) return false;

    // Arduino-ESP32 2.0.x does not initialise cached stat data reliably when
    // FILE_APPEND creates a new file. Require the preparation step to have
    // created the header, and snapshot size through a separate read handle.
    File existing = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_READ);
    if (!existing) {
        Serial.println("Could not reopen cleaning CSV before append.");
        return false;
    }
    uint64_t initialSize = existing.size();
    existing.close();
    if (initialSize < sizeof(CleaningCsvHeader) - 1) {
        Serial.println("Cleaning CSV header is incomplete; refusing to append.");
        return false;
    }

    File file = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_APPEND);
    if (!file) {
        Serial.printf("Could not open %s for append.\n",
                      AppConfig::CleaningsCsvPath);
        return false;
    }

    uint64_t expectedBytes = 0;
    char timestamp[32] = {};
    if (strftime(timestamp, sizeof(timestamp),
                 "%Y-%m-%dT%H:%M:%S%z", cleanedAt) == 0) {
        file.close();
        Serial.println("Failed to format cleaning timestamp for CSV.");
        return false;
    }

    char row[64];
    int rowLength = snprintf(row, sizeof(row), "%s,%s\n", timestamp,
                             CleaningHistory::cleanerName(cleaner));
    if (rowLength <= 0 || static_cast<size_t>(rowLength) >= sizeof(row)) {
        file.close();
        Serial.println("Cleaning CSV row exceeded its buffer.");
        return false;
    }

    bool success = file.write(
        reinterpret_cast<const uint8_t *>(row),
        static_cast<size_t>(rowLength)
    ) == static_cast<size_t>(rowLength);
    expectedBytes += static_cast<size_t>(rowLength);

    file.flush();
    file.close();

    File verification = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_READ);
    success = success && verification &&
              verification.size() == initialSize + expectedBytes;
    if (verification) verification.close();

    if (success) {
        Serial.printf("Logged cleaning: %s, %s\n", timestamp,
                      CleaningHistory::cleanerName(cleaner));
    } else {
        Serial.println("Failed while writing cleaning record to internal flash.");
    }
    return success;
}

uint64_t cleaningCsvSize()
{
    File file = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_READ);
    if (!file) return 0;
    uint64_t size = file.size();
    file.close();
    return size;
}

bool writeTally(uint64_t csvSize)
{
    File file = LittleFS.open(AppConfig::CleaningsTallyPath, FILE_WRITE);
    if (!file) {
        Serial.printf("Could not write %s.\n", AppConfig::CleaningsTallyPath);
        return false;
    }

    bool success =
        file.print("oriana,matthew,csv_size\n") > 0 &&
        file.printf("%lu,%lu,%llu\n",
                    static_cast<unsigned long>(allTimeTotals.oriana),
                    static_cast<unsigned long>(allTimeTotals.matthew),
                    static_cast<unsigned long long>(csvSize)) > 0;
    file.flush();
    file.close();
    return success;
}

bool loadTally(uint64_t csvSize)
{
    File file = LittleFS.open(AppConfig::CleaningsTallyPath, FILE_READ);
    if (!file) return false;

    char header[40] = {};
    size_t headerLength =
        file.readBytesUntil('\n', header, sizeof(header) - 1);
    header[headerLength] = '\0';
    while (headerLength > 0 &&
           isspace(static_cast<unsigned char>(header[headerLength - 1]))) {
        header[--headerLength] = '\0';
    }

    char line[96] = {};
    size_t length = file.readBytesUntil('\n', line, sizeof(line) - 1);
    line[length] = '\0';
    file.close();

    while (length > 0 &&
           isspace(static_cast<unsigned char>(line[length - 1]))) {
        line[--length] = '\0';
    }
    if (strcmp(header, "oriana,matthew,csv_size") != 0) return false;

    const char *cursor = line;
    uint64_t oriana = 0;
    uint64_t matthew = 0;
    uint64_t storedCsvSize = 0;
    if (!parseUnsignedField(&cursor, UINT32_MAX, ',', &oriana) ||
        !parseUnsignedField(&cursor, UINT32_MAX, ',', &matthew) ||
        !parseUnsignedField(&cursor, UINT64_MAX, '\0', &storedCsvSize) ||
        storedCsvSize != csvSize) {
        return false;
    }

    allTimeTotals.oriana = static_cast<uint32_t>(oriana);
    allTimeTotals.matthew = static_cast<uint32_t>(matthew);
    allTimeTotalsValid = true;
    return true;
}

void rebuildTally(uint64_t csvSize)
{
    allTimeTotals = {};
    allTimeTotalsValid = false;
    bool scanSucceeded = true;

    File file = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_READ);
    if (file && file.isDirectory()) {
        file.close();
        Serial.println("Cleaning CSV path is a directory; tally unavailable.");
        scanSucceeded = false;
    } else if (file) {
        constexpr size_t ReadBlockSize = 512;
        uint8_t block[ReadBlockSize];
        char line[96];
        size_t lineLength = 0;
        bool discardLine = false;
        size_t fileSize = file.size();
        size_t totalRead = 0;

        while (totalRead < fileSize) {
            size_t requestLength = fileSize - totalRead;
            if (requestLength > ReadBlockSize) requestLength = ReadBlockSize;
            size_t bytesRead = file.read(block, requestLength);
            if (bytesRead == 0) {
                Serial.println("Failed while rebuilding cleaning tally.");
                scanSucceeded = false;
                break;
            }
            totalRead += bytesRead;

            for (size_t i = 0; i < bytesRead; ++i) {
                char character = static_cast<char>(block[i]);
                if (character == '\n') {
                    if (!discardLine && lineLength > 0) {
                        line[lineLength] = '\0';
                        incrementTotals(cleanerMaskFromCsvLine(line));
                    }
                    lineLength = 0;
                    discardLine = false;
                } else if (character == '\0') {
                    discardLine = true;
                } else if (!discardLine) {
                    if (lineLength + 1 < sizeof(line)) {
                        line[lineLength++] = character;
                    } else {
                        discardLine = true;
                    }
                }
            }
            yield();
        }

        if (scanSucceeded && !discardLine && lineLength > 0) {
            line[lineLength] = '\0';
            incrementTotals(cleanerMaskFromCsvLine(line));
        }
        file.close();
    } else if (csvSize != 0) {
        Serial.println("Could not open cleaning CSV to rebuild tally.");
        scanSucceeded = false;
    }

    if (!scanSucceeded) return;
    allTimeTotalsValid = true;
    if (!writeTally(csvSize)) {
        Serial.println("Tally cache rebuild succeeded, but cache write failed.");
    }
    Serial.printf("Cleaning tally: Oriana=%lu, Matthew=%lu\n",
                  static_cast<unsigned long>(allTimeTotals.oriana),
                  static_cast<unsigned long>(allTimeTotals.matthew));
}

bool loadOrRebuildTally()
{
    allTimeTotalsValid = false;
    uint64_t csvSize = cleaningCsvSize();
    if (loadTally(csvSize)) return true;

    Serial.println("Cleaning tally missing or stale; rebuilding from CSV.");
    rebuildTally(csvSize);
    return allTimeTotalsValid;
}

bool applyRecentCsvLine(char *reversedLine, size_t lineLength,
                        bool lineOverflowed, uint32_t oldestDateKey,
                        uint32_t newestDateKey)
{
    if (lineOverflowed || lineLength == 0) return false;

    for (size_t left = 0, right = lineLength - 1;
         left < right; ++left, --right) {
        char swap = reversedLine[left];
        reversedLine[left] = reversedLine[right];
        reversedLine[right] = swap;
    }
    reversedLine[lineLength] = '\0';

    uint32_t dateKey;
    if (!dateKeyFromIsoDate(reversedLine, &dateKey)) return false;
    if (dateKey < oldestDateKey) return true;
    if (dateKey > newestDateKey) return false;

    uint8_t cleanerMask = cleanerMaskFromCsvLine(reversedLine);
    if (cleanerMask != CleaningHistory::None) {
        addCleaning(dateKey, cleanerMask);
    }
    return false;
}

void loadRecentHistoryFromCsv()
{
    File file = LittleFS.open(AppConfig::CleaningsCsvPath, FILE_READ);
    if (!file) {
        Serial.printf("No %s history file yet.\n", AppConfig::CleaningsCsvPath);
        return;
    }
    if (file.isDirectory()) {
        file.close();
        Serial.println("Cleaning CSV path is a directory; history unavailable.");
        return;
    }

    constexpr size_t ReadBlockSize = 512;
    uint8_t block[ReadBlockSize];
    char reversedLine[96];
    size_t reversedLength = 0;
    bool lineOverflowed = false;
    bool stopScanning = false;
    bool ioFailed = false;
    size_t cursor = file.size();
    uint32_t newestDateKey = visibleDateKeys[0];
    uint32_t oldestDateKey =
        visibleDateKeys[CleaningHistory::DayCount - 1];
    uint32_t startedAt = millis();

    // Read blocks and parse their bytes backward in RAM to avoid a filesystem
    // seek for every character. Stop after the visible seven-day window.
    while (cursor > 0 && !stopScanning) {
        size_t blockStart = cursor > ReadBlockSize
            ? cursor - ReadBlockSize : 0;
        size_t blockLength = cursor - blockStart;
        if (!file.seek(blockStart) ||
            file.read(block, blockLength) != blockLength) {
            Serial.println("Failed while reading recent cleaning history.");
            ioFailed = true;
            break;
        }

        for (size_t i = blockLength; i > 0; --i) {
            char character = static_cast<char>(block[i - 1]);
            if (character == '\n' || character == '\r') {
                stopScanning = applyRecentCsvLine(
                    reversedLine, reversedLength, lineOverflowed,
                    oldestDateKey, newestDateKey
                );
                reversedLength = 0;
                lineOverflowed = false;
                if (stopScanning) break;
            } else if (character == '\0') {
                lineOverflowed = true;
            } else if (!lineOverflowed) {
                if (reversedLength + 1 < sizeof(reversedLine)) {
                    reversedLine[reversedLength++] = character;
                } else {
                    lineOverflowed = true;
                }
            }
        }

        cursor = blockStart;
        yield();
    }

    if (!stopScanning && !ioFailed) {
        applyRecentCsvLine(
            reversedLine, reversedLength, lineOverflowed,
            oldestDateKey, newestDateKey
        );
    }

    file.close();
    Serial.printf("Recent cleaning history loaded in %lums.\n",
                  static_cast<unsigned long>(millis() - startedAt));
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

const Totals &totals()
{
    return allTimeTotals;
}

bool totalsAreValid()
{
    return allTimeTotalsValid;
}

void refresh(const struct tm *now,
             const struct tm *newCleaningTime,
             Cleaner newCleaner)
{
    allTimeTotalsValid = false;
    historyValid = initialiseVisibleDays(now);
    if (!historyValid) return;

    reconcileRetainedHistory();

    uint8_t cleanerMask = static_cast<uint8_t>(newCleaner);
    bool newCleaningIsValid =
        newCleaningTime && newCleaner != None &&
        cleanerMaskIsValid(cleanerMask);
    uint32_t cleaningDateKey = 0;
    if (newCleaningIsValid &&
        dateKeyFromTime(newCleaningTime, &cleaningDateKey)) {
        // Update the live/RTC view whether or not persistent storage works.
        addCleaning(cleaningDateKey, cleanerMask);
    } else {
        newCleaningIsValid = false;
    }

    if (mountFlash()) {
        // A reset or power loss can leave a partial header or final write.
        // Repair it first and force the size-keyed tally cache to rebuild
        // against the repaired file before counting another event.
        Serial.println("Preparing cleaning CSV...");
        bool csvReady = prepareCsvForAppend();
        bool tallyReady = false;
        if (csvReady) {
            Serial.println("Loading cleaning tally...");
            tallyReady = loadOrRebuildTally();
        } else {
            allTimeTotalsValid = false;
            Serial.println("Skipping tally load because the CSV is not ready.");
        }

        if (newCleaningIsValid && csvReady &&
            appendCleaningToCsv(newCleaningTime, newCleaner)) {
            if (tallyReady) incrementTotals(cleanerMask);
            if (tallyReady && !writeTally(cleaningCsvSize())) {
                Serial.println(
                    "Cleaning logged, but tally cache update failed."
                );
            } else if (!tallyReady) {
                Serial.println(
                    "Cleaning logged, but its tally will be rebuilt on the "
                    "next successful flash read."
                );
            }
        } else if (newCleaningIsValid && !csvReady) {
            Serial.println(
                "Cleaning remains in RTC history, but its CSV write was "
                "skipped because the file could not be prepared safely."
            );
        }

        Serial.println("Loading recent cleaning history...");
        loadRecentHistoryFromCsv();
        LittleFS.end();
        Serial.println("Cleaning history flash session complete.");
    }

    retainVisibleHistory();
}

}  // namespace CleaningHistory
