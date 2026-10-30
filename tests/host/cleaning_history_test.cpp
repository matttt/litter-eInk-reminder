// Compile the production module against a persistent fake filesystem so tests
// can simulate RTC loss and inject failures without an attached ESP32.
#include "../../src/cleaning_history.cpp"
#include <cassert>
#include <cstdlib>
#include <iostream>

using namespace CleaningHistory;

static struct tm date(int day)
{
    struct tm result = {};
    result.tm_year = 126;
    result.tm_mon = 8;
    result.tm_mday = day;
    result.tm_hour = 12;
    result.tm_isdst = -1;
    mktime(&result);
    return result;
}

static void resetRam(bool cold)
{
    if (cold) retainedHistory = {};
    memset(history, 0, sizeof(history));
    memset(visibleDateKeys, 0, sizeof(visibleDateKeys));
    historyValid = false;
    allTimeTotals = {};
    allTimeTotalsValid = false;
}

static void freshDevice()
{
    resetRam(true);
    LittleFS = {};
    std::fill(flashBytes.begin(), flashBytes.end(), 0xFF);
    partitionMissing = partitionReadFails = fileWritesFail = false;
    writtenBytes = 0;
}

int main()
{
    setenv("TZ", "UTC0", 1);
    tzset();
    auto today = date(14);
    auto tomorrow = date(15);
    auto nextDay = date(16);

    freshDevice();
    refresh(&today, &today, Oriana);
    assert(LittleFS.formats == 1 && !LittleFS.mounted);
    assert(*LittleFS.files.at(AppConfig::CleaningsCsvPath) ==
           "timestamp,who\n2026-09-14T12:00:00+0000,Oriana\n");
    assert(totalsAreValid() && totals().oriana == 1 && totals().matthew == 0);

    // Deep sleep retains the view, while the flash file survives a cold reset.
    resetRam(false);
    refresh(&today, &today, Matthew);
    assert(days()[0].cleanerMask == Both);
    resetRam(true);
    size_t beforeRead = writtenBytes;
    refresh(&tomorrow);
    assert(days()[0].cleanerMask == None && days()[1].cleanerMask == Both);
    assert(totalsAreValid() && totals().oriana == 1 && totals().matthew == 1);
    assert(writtenBytes == beforeRead && LittleFS.formats == 1);
    assert(!LittleFS.mounted);

    // An interrupted cache write must rebuild totals from the CSV.
    *LittleFS.files.at(AppConfig::CleaningsTallyPath) = "oriana,";
    resetRam(true);
    refresh(&tomorrow, &tomorrow, Both);
    assert(totalsAreValid() && totals().oriana == 2 && totals().matthew == 2);

    // Write exhaustion retains the new event in RTC without inventing a
    // persisted count, and existing records remain recoverable after reset.
    const auto savedCsv = *LittleFS.files.at(AppConfig::CleaningsCsvPath);
    fileWritesFail = true;
    refresh(&nextDay, &nextDay, Oriana);
    assert(isValid() && days()[0].cleanerMask == Oriana);
    assert(*LittleFS.files.at(AppConfig::CleaningsCsvPath) == savedCsv);
    assert(totals().oriana == 2 && !LittleFS.mounted);
    resetRam(false);
    refresh(&nextDay);
    assert(days()[0].cleanerMask == Oriana);
    fileWritesFail = false;
    resetRam(true);
    refresh(&nextDay);
    assert(days()[0].cleanerMask == None);
    assert(totals().oriana == 2 && totals().matthew == 2);

    // Nonblank storage is never formatted in response to a mount failure.
    LittleFS.mountFails = true;
    refresh(&nextDay, &nextDay, Matthew);
    assert(isValid() && days()[0].cleanerMask == Matthew);
    assert(!totalsAreValid() && LittleFS.formats == 1);
    assert(*LittleFS.files.at(AppConfig::CleaningsCsvPath) == savedCsv);
    resetRam(false);
    refresh(&nextDay);
    assert(days()[0].cleanerMask == Matthew);

    // Check beyond the filesystem's first block before deciding it is blank.
    freshDevice();
    flashBytes.back() = 0;
    refresh(&today, &today, Oriana);
    assert(LittleFS.formats == 0 && days()[0].cleanerMask == Oriana);
    freshDevice();
    partitionReadFails = true;
    refresh(&today, &today, Matthew);
    assert(LittleFS.formats == 0 && days()[0].cleanerMask == Matthew);
    freshDevice();
    partitionMissing = true;
    refresh(&today);
    assert(LittleFS.formats == 0 && !totalsAreValid());
    freshDevice();
    LittleFS.formatFails = true;
    refresh(&today, &today, Both);
    assert(LittleFS.formats == 1 && days()[0].cleanerMask == Both);
    assert(!LittleFS.mounted && !totalsAreValid());

    // Invalid time must not create storage or write an undated cleaning.
    freshDevice();
    refresh(nullptr, &today, Oriana);
    assert(!isValid() && LittleFS.formats == 0 && writtenBytes == 0);
    std::cout << "Cleaning history host tests passed\n";
}
