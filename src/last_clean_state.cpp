#include "last_clean_state.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "cleaning_history.h"

namespace {

constexpr char StorageNamespace[] = "last-clean";
constexpr char StorageKey[] = "state";
constexpr uint32_t RecordMagic = 0x4C434C4E;  // "LCLN"
constexpr uint16_t RecordVersion = 1;

// Store an epoch, not struct tm (which contains platform-specific fields).
// Keep all fields in one blob so timestamp and pending mask change together.
struct Record {
    uint32_t magic;
    uint16_t version;
    uint8_t pendingCleanerMask;
    uint8_t reserved;
    int64_t epoch;
};
static_assert(sizeof(Record) == 16, "Last-clean storage layout changed");

bool decode(const Record &record, LastCleanState::State *state)
{
    if (record.magic != RecordMagic || record.version != RecordVersion ||
        record.reserved != 0 ||
        (record.pendingCleanerMask & ~CleaningHistory::Both) != 0) {
        return false;
    }

    LastCleanState::State decoded = {};
    decoded.pendingCleanerMask = record.pendingCleanerMask;
    if (record.epoch == 0) {
        if (decoded.pendingCleanerMask == CleaningHistory::None) return false;
    } else {
        if (record.epoch < 0 || decoded.pendingCleanerMask != CleaningHistory::None) {
            return false;
        }
        time_t epoch = static_cast<time_t>(record.epoch);
        if (static_cast<int64_t>(epoch) != record.epoch ||
            !localtime_r(&epoch, &decoded.lastCleanTime) ||
            decoded.lastCleanTime.tm_year < 70 ||
            decoded.lastCleanTime.tm_year > 199) {
            return false;
        }
        decoded.timestampValid = true;
    }
    *state = decoded;
    return true;
}

}  // namespace

namespace LastCleanState {

bool load(State *state)
{
    if (!state) return false;
    *state = {};
    Preferences storage;
    if (!storage.begin(StorageNamespace, true)) return false;

    Record record = {};
    bool loaded = storage.getBytesLength(StorageKey) == sizeof(record) &&
                  storage.getBytes(StorageKey, &record, sizeof(record)) == sizeof(record);
    storage.end();
    if (!loaded || !decode(record, state)) {
        Serial.println("No valid last-clean state in NVS.");
        return false;
    }
    return true;
}

bool save(const State &state)
{
    Record record = {};
    record.magic = RecordMagic;
    record.version = RecordVersion;
    record.pendingCleanerMask = state.pendingCleanerMask;
    if (state.timestampValid) {
        struct tm localTime = state.lastCleanTime;
        time_t epoch = mktime(&localTime);
        // Reject invalid calendar fields instead of silently normalising them.
        if (epoch == (time_t)-1 || localTime.tm_year != state.lastCleanTime.tm_year ||
            localTime.tm_mon != state.lastCleanTime.tm_mon ||
            localTime.tm_mday != state.lastCleanTime.tm_mday ||
            localTime.tm_hour != state.lastCleanTime.tm_hour ||
            localTime.tm_min != state.lastCleanTime.tm_min ||
            localTime.tm_sec != state.lastCleanTime.tm_sec) {
            return false;
        }
        record.epoch = static_cast<int64_t>(epoch);
    }
    State checked = {};
    if (!decode(record, &checked) || checked.timestampValid != state.timestampValid) {
        return false;
    }

    Preferences storage;
    if (!storage.begin(StorageNamespace)) return false;
    Record previous = {};
    bool unchanged = storage.getBytesLength(StorageKey) == sizeof(previous) &&
                     storage.getBytes(StorageKey, &previous, sizeof(previous)) == sizeof(previous) &&
                     memcmp(&previous, &record, sizeof(record)) == 0;
    bool saved = unchanged ||
                 storage.putBytes(StorageKey, &record, sizeof(record)) == sizeof(record);
    storage.end();
    return saved;
}

}  // namespace LastCleanState
