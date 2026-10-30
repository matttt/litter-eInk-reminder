#pragma once
#include <cstdint>
#include <cstdio>

#define RTC_DATA_ATTR
struct TestSerial {
    void println(const char *) {}
    template <typename... Args>
    void printf(const char *, Args...) {}
};
static TestSerial Serial;
inline void yield() {}
inline uint32_t millis() { return 0; }
