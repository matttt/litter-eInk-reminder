#pragma once

#include <time.h>

namespace ClockService {

void configureLocalTimezone();
void syncFromNtp();

bool initialise();
bool getLocalTime(struct tm *out);
void logTime();

void clearAlarmFlag();
void disableAlarm();
bool setReminder24HoursAfter(const struct tm *baseTime);
bool setReminder24HoursFromNow();

}  // namespace ClockService
