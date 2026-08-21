#include "cleaning_history.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ctype.h>
#include <strings.h>

#include "app_config.h"
#include "utilities.h"

namespace {

CleaningHistory::Day history[CleaningHistory::DayCount] = {};
bool historyValid = false;
CleaningHistory::Totals allTimeTotals = {};
bool allTimeTotalsValid = false;

uint8_t cleanerMaskFromName(const char *name)
{
    using namespace CleaningHistory;

    if (strcasecmp(name, "Oriana") == 0) return Oriana;
    if (strcasecmp(name, "Matthew") == 0) return Matthew;
    if (strcasecmp(name, "Oriana+Matthew") == 0 ||
        strcasecmp(name, "Matthew+Oriana") == 0) {
        return Both;
    }
    return None;
}

bool initialise(const struct tm *now)
{
    if (!now) return false;

    for (size_t i = 0; i < CleaningHistory::DayCount; ++i) {
        struct tm day = *now;
        day.tm_hour = 12;
        day.tm_min = 0;
        day.tm_sec = 0;
        day.tm_mday -= static_cast<int>(i);
        day.tm_isdst = -1;

        if (mktime(&day) == (time_t)-1) {
            Serial.println("Failed to calculate cleaning-history date.");
            return false;
        }

        strftime(history[i].date, sizeof(history[i].date), "%Y-%m-%d", &day);

        strftime(history[i].label, sizeof(history[i].label), "%a", &day);
        history[i].cleanerMask = CleaningHistory::None;
    }

    return true;
}

void addCleaning(const char *date, uint8_t cleanerMask)
{
    for (size_t i = 0; i < CleaningHistory::DayCount; ++i) {
        if (strncmp(history[i].date, date, 10) == 0) {
            history[i].cleanerMask |= cleanerMask;
            return;
        }
    }
}

uint8_t cleanerMaskFromCsvLine(char *line)
{
    size_t length = strlen(line);
    while (length > 0 && isspace(static_cast<unsigned char>(line[length - 1]))) {
        line[--length] = '\0';
    }

    if (length < 12 || strncmp(line, "timestamp,", 10) == 0) {
        return CleaningHistory::None;
    }

    char *comma = strchr(line, ',');
    if (!comma) return CleaningHistory::None;
    return cleanerMaskFromName(comma + 1);
}

void incrementTotals(uint8_t cleanerMask)
{
    if (cleanerMask & CleaningHistory::Oriana) allTimeTotals.oriana++;
    if (cleanerMask & CleaningHistory::Matthew) allTimeTotals.matthew++;
}

bool waitForSdMosiToBeReleased()
{
    if (digitalRead(AppConfig::OrianaButtonPin) != LOW) return true;

    Serial.println("Waiting for GPIO15 button release before using SD card...");
    uint32_t start = millis();
    while (digitalRead(AppConfig::OrianaButtonPin) == LOW &&
           millis() - start < 5000) {
        delay(20);
    }

    if (digitalRead(AppConfig::OrianaButtonPin) == LOW) {
        Serial.println("GPIO15 is still LOW; skipping SD access to protect the write.");
        return false;
    }
    delay(20);
    return true;
}

bool mountSdCard()
{
    // GPIO15 doubles as the Oriana button and SD MOSI on this board.
    if (!waitForSdMosiToBeReleased()) return false;

    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI)) {
        Serial.println("SD card mount failed; cleaning history will not be persisted.");
        SPI.end();
        pinMode(AppConfig::OrianaButtonPin, INPUT_PULLUP);
        return false;
    }

    Serial.println("SD card mounted.");
    return true;
}

void unmountSdCard()
{
    SD.end();
    SPI.end();
    pinMode(AppConfig::OrianaButtonPin, INPUT_PULLUP);
}

bool appendCleaningToCsv(const struct tm *cleanedAt,
                         CleaningHistory::Cleaner cleaner)
{
    if (!cleanedAt || cleaner == CleaningHistory::None) return false;

    File file = SD.open(AppConfig::CleaningsCsvPath, FILE_APPEND);
    if (!file) {
        Serial.printf("Could not open %s for append.\n",
                      AppConfig::CleaningsCsvPath);
        return false;
    }

    bool success = true;
    if (file.size() == 0 && file.print("timestamp,who\n") == 0) {
        success = false;
    }

    char timestamp[32];
    if (strftime(timestamp, sizeof(timestamp),
                 "%Y-%m-%dT%H:%M:%S%z", cleanedAt) == 0) {
        success = false;
    } else if (file.printf("%s,%s\n", timestamp,
                           CleaningHistory::cleanerName(cleaner)) == 0) {
        success = false;
    }

    file.flush();
    file.close();

    if (success) {
        Serial.printf("Logged cleaning: %s, %s\n", timestamp,
                      CleaningHistory::cleanerName(cleaner));
    } else {
        Serial.println("Failed while writing cleaning record to SD card.");
    }
    return success;
}

uint64_t cleaningCsvSize()
{
    File file = SD.open(AppConfig::CleaningsCsvPath, FILE_READ);
    if (!file) return 0;
    uint64_t size = file.size();
    file.close();
    return size;
}

bool writeTally(uint64_t csvSize)
{
    File file = SD.open(AppConfig::CleaningsTallyPath, FILE_WRITE);
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
    File file = SD.open(AppConfig::CleaningsTallyPath, FILE_READ);
    if (!file) return false;

    char line[96];
    file.readBytesUntil('\n', line, sizeof(line) - 1);  // Header
    size_t length = file.readBytesUntil('\n', line, sizeof(line) - 1);
    line[length] = '\0';
    file.close();

    unsigned long oriana = 0;
    unsigned long matthew = 0;
    unsigned long long storedCsvSize = 0;
    if (sscanf(line, "%lu,%lu,%llu",
               &oriana, &matthew, &storedCsvSize) != 3 ||
        storedCsvSize != csvSize) {
        return false;
    }

    allTimeTotals.oriana = oriana;
    allTimeTotals.matthew = matthew;
    allTimeTotalsValid = true;
    return true;
}

void rebuildTally(uint64_t csvSize)
{
    allTimeTotals = {};

    File file = SD.open(AppConfig::CleaningsCsvPath, FILE_READ);
    if (file) {
        char line[96];
        while (file.available()) {
            size_t length = file.readBytesUntil('\n', line, sizeof(line) - 1);
            line[length] = '\0';

            if (length == sizeof(line) - 1) {
                while (file.available() && file.read() != '\n') {}
                continue;
            }

            incrementTotals(cleanerMaskFromCsvLine(line));
        }
        file.close();
    }

    allTimeTotalsValid = true;
    if (!writeTally(csvSize)) {
        Serial.println("Tally cache rebuild succeeded, but cache write failed.");
    }
    Serial.printf("Cleaning tally: Oriana=%lu, Matthew=%lu\n",
                  static_cast<unsigned long>(allTimeTotals.oriana),
                  static_cast<unsigned long>(allTimeTotals.matthew));
}

void loadOrRebuildTally()
{
    uint64_t csvSize = cleaningCsvSize();
    if (!loadTally(csvSize)) {
        Serial.println("Cleaning tally missing or stale; rebuilding from CSV.");
        rebuildTally(csvSize);
    }
}

bool readPreviousLine(File &file, size_t &cursor,
                      char *line, size_t lineCapacity)
{
    if (cursor == 0) return false;

    size_t end = cursor;
    while (end > 0) {
        file.seek(end - 1);
        int character = file.read();
        if (character != '\n' && character != '\r') break;
        end--;
    }
    if (end == 0) {
        cursor = 0;
        return false;
    }

    size_t start = end;
    while (start > 0) {
        file.seek(start - 1);
        if (file.read() == '\n') break;
        start--;
    }

    size_t lineLength = end - start;
    cursor = start;
    if (lineLength >= lineCapacity) {
        line[0] = '\0';
        return true;
    }

    file.seek(start);
    size_t bytesRead = file.read(
        reinterpret_cast<uint8_t *>(line), lineLength
    );
    line[bytesRead] = '\0';
    return true;
}

void loadRecentHistoryFromCsv()
{
    File file = SD.open(AppConfig::CleaningsCsvPath, FILE_READ);
    if (!file) {
        Serial.printf("No %s history file yet.\n", AppConfig::CleaningsCsvPath);
        return;
    }

    char line[96];
    size_t cursor = file.size();
    const char *oldestDate = history[CleaningHistory::DayCount - 1].date;
    while (readPreviousLine(file, cursor, line, sizeof(line))) {
        if (strlen(line) >= 10 && line[4] == '-' && line[7] == '-' &&
            strncmp(line, oldestDate, 10) < 0) {
            break;
        }

        uint8_t cleanerMask = cleanerMaskFromCsvLine(line);
        if (cleanerMask != CleaningHistory::None) {
            addCleaning(line, cleanerMask);
        }
    }

    file.close();
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
    historyValid = initialise(now);
    if (!historyValid) return;

    if (mountSdCard()) {
        loadOrRebuildTally();

        if (newCleaningTime && newCleaner != None) {
            if (appendCleaningToCsv(newCleaningTime, newCleaner)) {
                incrementTotals(newCleaner);
                if (!writeTally(cleaningCsvSize())) {
                    Serial.println("Cleaning logged, but tally cache update failed.");
                }
            }
        }
        loadRecentHistoryFromCsv();
        unmountSdCard();
    }

    // Always show the new event, even if the SD card was unavailable.
    if (newCleaningTime && newCleaner != None) {
        char cleaningDate[11];
        strftime(cleaningDate, sizeof(cleaningDate),
                 "%Y-%m-%d", newCleaningTime);
        addCleaning(cleaningDate, newCleaner);
    }
}

}  // namespace CleaningHistory
