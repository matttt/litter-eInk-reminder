#pragma once
#include <cstdint>
#include <cstdio>

#define RTC_DATA_ATTR
#define IRAM_ATTR
#define BOARD_HAS_PSRAM
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
#define FALLING 3
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE *) {}
inline void portEXIT_CRITICAL(portMUX_TYPE *) {}
inline void portENTER_CRITICAL_ISR(portMUX_TYPE *) {}
inline void portEXIT_CRITICAL_ISR(portMUX_TYPE *) {}
inline void pinMode(int, int) {}
inline int digitalRead(int) { return HIGH; }
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterrupt(int, void (*)(), int) {}
inline void delay(unsigned) {}
struct TestSerial {
    void begin(int) {}
    void flush() {}
    void println(const char *) {}
    template <typename... Args>
    void printf(const char *, Args...) {}
};
static TestSerial Serial;
inline void yield() {}
inline uint32_t millis() { return 0; }
