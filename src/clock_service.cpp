#include "clock_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <SensorPCF8563.hpp>

#include "app_config.h"

namespace {

SensorPCF8563 rtc;
bool timeSynced = false;

void timeAvailable(struct timeval *timeValue)
{
    (void)timeValue;
    Serial.println("[NTP] Got time adjustment");
    timeSynced = true;
}

}  // namespace

namespace ClockService {

void configureLocalTimezone()
{
    setenv("TZ", AppConfig::TimeZone, 1);
    tzset();
}

void syncFromNtp()
{
    timeSynced = false;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    sntp_set_time_sync_notification_cb(timeAvailable);
    configTzTime(AppConfig::TimeZone,
                 AppConfig::NtpServer1,
                 AppConfig::NtpServer2);
    WiFi.begin(AppConfig::WifiSsid, AppConfig::WifiPassword);

    Serial.print("Connecting to WiFi");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        Serial.print(".");
        delay(500);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected, waiting for NTP...");
        start = millis();
        while (!timeSynced && millis() - start < 15000) delay(250);
    }

    if (timeSynced) {
        struct tm timeInfo;
        if (::getLocalTime(&timeInfo, 1000)) {
            rtc.setDateTime(
                timeInfo.tm_year + 1900, timeInfo.tm_mon + 1,
                timeInfo.tm_mday, timeInfo.tm_hour,
                timeInfo.tm_min, timeInfo.tm_sec
            );
            Serial.println("External RTC set from NTP.");
        }
    } else {
        Serial.println("NTP sync failed.");
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(250);
}

bool initialise()
{
    if (!rtc.begin(Wire, AppConfig::RtcSdaPin, AppConfig::RtcSclPin)) {
        Serial.println("Failed to find PCF8563!");
        return false;
    }
    return true;
}

bool getLocalTime(struct tm *out)
{
    RTC_DateTime now = rtc.getDateTime();

    // The RTC stores local wall-clock time because NTP writes local time to it.
    struct tm local = {0};
    local.tm_year = now.getYear() - 1900;
    local.tm_mon  = now.getMonth() - 1;
    local.tm_mday = now.getDay();
    local.tm_hour = now.getHour();
    local.tm_min  = now.getMinute();
    local.tm_sec  = now.getSecond();
    local.tm_isdst = -1;

    *out = local;
    mktime(out);  // Populate derived fields such as tm_wday and tm_yday.
    return true;
}

void logTime()
{
    RTC_DateTime now = rtc.getDateTime();
    Serial.printf("[RTC]: %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.getYear(), now.getMonth(), now.getDay(),
                  now.getHour(), now.getMinute(), now.getSecond());
}

void clearAlarmFlag()
{
    rtc.resetAlarm();
}

void disableAlarm()
{
    rtc.disableAlarm();
}

bool setReminder24HoursAfter(const struct tm *baseTime)
{
    if (!baseTime) return false;

    struct tm target = *baseTime;
    target.tm_isdst = -1;

    time_t targetEpoch = mktime(&target);
    if (targetEpoch == (time_t)-1) {
        Serial.println("Failed to convert reminder base time.");
        return false;
    }

    targetEpoch += 24 * 60 * 60;
    localtime_r(&targetEpoch, &target);

    rtc.resetAlarm();
    rtc.disableAlarm();
    rtc.clearCountdownTimer();

    // PCF8563 alarm fields are minute, hour, day-of-month, weekday.
    rtc.setAlarm(target.tm_hour, target.tm_min, target.tm_mday, 0xFF);
    rtc.enableAlarm();

    Serial.printf("RTC reminder set for %04d-%02d-%02d %02d:%02d\n",
                  target.tm_year + 1900, target.tm_mon + 1, target.tm_mday,
                  target.tm_hour, target.tm_min);
    return true;
}

bool setReminder24HoursFromNow()
{
    struct tm now;
    if (!ClockService::getLocalTime(&now)) {
        Serial.println("Failed to read RTC time for reminder scheduling.");
        return false;
    }

    return setReminder24HoursAfter(&now);
}

}  // namespace ClockService
