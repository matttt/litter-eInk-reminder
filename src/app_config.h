#pragma once

#include <stdint.h>

namespace AppConfig {

constexpr char WifiSsid[] = "Wild Wild West";
constexpr char WifiPassword[] = "saddleup";

constexpr uint8_t OrianaButtonPin = 15;
constexpr uint8_t MatthewButtonPin = 21;
constexpr uint8_t ProgrammingButtonPin = 0;
constexpr uint8_t RtcSdaPin = 18;
constexpr uint8_t RtcSclPin = 17;
constexpr uint8_t RtcIrqPin = 9;

constexpr char CleaningsCsvPath[] = "/cleanings.csv";
constexpr char CleaningsTallyPath[] = "/cleanings.tally";

// Sync NTP every N alarm wakeups. Set to 1 to sync every time.
constexpr uint32_t NtpSyncInterval = 30;

// Retry NTP on the ESP32's independent deep-sleep timer when the external
// clock is invalid and no PCF8563 alarm can be armed.
constexpr uint64_t ClockRecoveryIntervalUs = 60ULL * 60 * 1000 * 1000;

constexpr char NtpServer1[] = "pool.ntp.org";
constexpr char NtpServer2[] = "time.nist.gov";
constexpr char TimeZone[] = "PST8PDT,M3.2.0,M11.1.0";

}  // namespace AppConfig
