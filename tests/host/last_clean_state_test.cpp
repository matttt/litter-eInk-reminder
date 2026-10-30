#include "../../src/last_clean_state.cpp"
#include <cassert>
#include <cstdlib>
#include <iostream>

using namespace LastCleanState;

static State cleaning(int day, int hour = 12, int isdst = -1)
{
    State state = {};
    state.timestampValid = true;
    state.lastCleanTime.tm_year = 126;
    state.lastCleanTime.tm_mon = 10;
    state.lastCleanTime.tm_mday = day;
    state.lastCleanTime.tm_hour = hour;
    state.lastCleanTime.tm_isdst = isdst;
    mktime(&state.lastCleanTime);
    return state;
}

static time_t epoch(State state)
{
    return mktime(&state.lastCleanTime);
}

int main()
{
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    State restored = cleaning(1);
    assert(!load(&restored) && !restored.timestampValid);
    assert(nvsWrites == 0 && nvsBlob.empty());
    assert(!load(nullptr));

    // Simulate RTC loss: recover solely from the flash blob, with no writes
    // during restore or when an identical record is saved again.
    auto original = cleaning(1, 1, 1);  // First 01:00 in the DST fall-back fold.
    assert(save(original));
    assert(load(&restored) && restored.timestampValid);
    assert(epoch(restored) == epoch(original));
    assert(restored.lastCleanTime.tm_isdst == 1);
    assert(save(original) && nvsWrites == 1);
    auto repeatedHour = cleaning(1, 1, 0);
    assert(save(repeatedHour) && load(&restored));
    assert(epoch(restored) - epoch(original) == 3600);
    assert(restored.lastCleanTime.tm_isdst == 0);

    // A failed update leaves the previous committed state available.
    nvsWriteFails = true;
    assert(!save(cleaning(2)) && load(&restored));
    assert(epoch(restored) == epoch(repeatedHour));
    nvsWriteFails = false;
    nvsOpenFails = true;
    assert(!save(original) && !load(&restored));
    assert(!restored.timestampValid);
    nvsOpenFails = false;
    nvsReadFails = true;
    assert(!load(&restored));
    nvsReadFails = false;

    // Cleaner presses while the clock is unavailable also survive power loss.
    State pending = {};
    pending.pendingCleanerMask = CleaningHistory::Both;
    assert(save(pending) && load(&restored));
    assert(!restored.timestampValid && restored.pendingCleanerMask == CleaningHistory::Both);
    assert(save(original) && load(&restored));
    assert(restored.timestampValid && restored.pendingCleanerMask == CleaningHistory::None);

    // Reject malformed data without changing flash or creating a fake date.
    const auto validBlob = nvsBlob;
    for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7)}) {
        nvsBlob = validBlob;
        nvsBlob[offset] = 0xFF;
        assert(!load(&restored) && !restored.timestampValid);
    }
    nvsBlob = validBlob;
    nvsBlob.pop_back();
    assert(!load(&restored));
    nvsBlob = validBlob;
    std::fill(nvsBlob.begin() + 8, nvsBlob.end(), 0xFF);
    assert(!load(&restored));
    nvsBlob = validBlob;
    auto invalid = cleaning(1);
    invalid.lastCleanTime.tm_mday = 32;
    assert(!save(invalid));
    invalid = {};
    assert(!save(invalid));
    invalid.pendingCleanerMask = 4;
    assert(!save(invalid));
    assert(nvsBlob == validBlob && nvsOpenHandles == 0);
    std::cout << "Last-clean state host tests passed\n";
}
