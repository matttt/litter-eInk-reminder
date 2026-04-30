/**
 * @copyright Copyright (c) 2024 Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2024-04-05
 * @note      Arduino Setting
 *            Tools ->
 *                  Board:"ESP32S3 Dev Module"
 *                  USB CDC On Boot:"Enable"
 *                  USB DFU On Boot:"Disable"
 *                  Flash Size : "16MB(128Mb)"
 *                  Flash Mode :"QIO 80MHz"
 *                  Partition Scheme:"16M Flash(3M APP/9.9MB FATFS)"
 *                  PSRAM:"OPI PSRAM"
 *                  Upload Mode:"UART0/Hardware CDC"
 *                  USB Mode:"Hardware CDC and JTAG"
 */

#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM, Arduino IDE -> tools -> PSRAM -> OPI !!!"
#endif

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <time.h>
#include <ctype.h>
#include <SensorPCF8563.hpp>

#include "epd_driver.h"
#include "utilities.h"
#include "firasans.h"

#include "src/opensans12b.h"
#include "src/opensans24b.h"

#include "src/dirtyImage.h"
#include "src/cleanImage.h"

#include "esp32s3/rom/rtc.h"


// ── Configuration ──────────────────────────────────────────────────────────
#define WIFI_SSID     "Wild Wild West"
#define WIFI_PASSWORD "saddleup"

#define CLEAN_BUTTON_PIN 15
#define CLEAN_BUTTON_PIN_2 0
#define SENSOR_SDA       18
#define SENSOR_SCL       17
#define SENSOR_IRQ       9

// Alarm fires daily at this hour (24-h format).
#define ALARM_HOUR   18
#define ALARM_MINUTE 00

// Sync NTP every N alarm wakeups. Set to 1 to sync every time.
#define NTP_SYNC_INTERVAL 30

const bool DISABLE_DEEP_SLEEP = false;

const char *ntpServer1 = "pool.ntp.org";
const char *ntpServer2 = "time.nist.gov";
const char *time_zone  = "PST8PDT,M3.2.0,M11.1.0";

// ── RTC-surviving state ────────────────────────────────────────────────────
// These persist across deep-sleep cycles in RTC slow memory.
RTC_DATA_ATTR uint32_t alarmWakeCount = 0;
RTC_DATA_ATTR bool     rtcInitialised = false;  // has NTP ever been synced?
RTC_DATA_ATTR bool     rtcCleanTimestampValid = false;
RTC_DATA_ATTR struct tm rtcLastCleanTimeInfo = {0};

// ── Globals ────────────────────────────────────────────────────────────────
uint8_t *framebuffer = nullptr;
SensorPCF8563 rtc;
bool timeSynced = false;

enum DisplayMode { DISPLAY_DIRTY, DISPLAY_CLEAN };
DisplayMode currentMode = DISPLAY_DIRTY;

bool      cleanTimestampValid = false;
struct tm lastCleanTimeInfo   = {0};

// ── Wake source detection ──────────────────────────────────────────────────
// We use ext1 with ANY_LOW on both GPIO 9 (RTC alarm) and GPIO 15 (button).
// After waking, check which GPIO triggered it.

enum WakeReason {
    WAKE_COLD_BOOT,
    WAKE_BUTTON,
    WAKE_RTC_ALARM
};


// ── WiFi / NTP ─────────────────────────────────────────────────────────────
void timeavailable(struct timeval *t)
{
    Serial.println("[NTP] Got time adjustment");
    timeSynced = true;
}

void syncTimeFromNTP()
{
    timeSynced = false;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    sntp_set_time_sync_notification_cb(timeavailable);
    configTzTime(time_zone, ntpServer1, ntpServer2);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 1000)) {
            // Push NTP time into the external RTC.
            rtc.setDateTime(
                timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                timeinfo.tm_mday, timeinfo.tm_hour,
                timeinfo.tm_min, timeinfo.tm_sec
            );
            Serial.println("External RTC set from NTP.");
            rtcInitialised = true;
        }
    } else {
        Serial.println("NTP sync failed.");
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(250);
}

// ── External RTC helpers ───────────────────────────────────────────────────
bool initExternalRTC()
{
    if (!rtc.begin(Wire, SENSOR_SDA, SENSOR_SCL)) {
        Serial.println("Failed to find PCF8563!");
        return false;
    }
    return true;
}

// Populate a struct tm from the external RTC, converting to local time zone
// by way of the POSIX TZ rule that was set via configTzTime().
bool getRTCLocalTime(struct tm *out)
{
    RTC_DateTime now = rtc.getDateTime();

    // Build a UTC-ish struct tm from the RTC values.
    struct tm utc = {0};
    utc.tm_year = now.getYear() - 1900;
    utc.tm_mon  = now.getMonth() - 1;
    utc.tm_mday = now.getDay();
    utc.tm_hour = now.getHour();
    utc.tm_min  = now.getMinute();
    utc.tm_sec  = now.getSecond();
    utc.tm_isdst = -1;

    // The external RTC stores local time (we wrote local time to it via NTP
    // + configTzTime).  Just copy it out directly.
    *out = utc;

    // Fill in derived fields (tm_wday, tm_yday) by round-tripping through
    // mktime.
    mktime(out);
    return true;
}

void logRTCTime()
{
    RTC_DateTime now = rtc.getDateTime();
    Serial.printf("[RTC]: %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.getYear(), now.getMonth(), now.getDay(),
                  now.getHour(), now.getMinute(), now.getSecond());
}

void setDailyAlarm()
{
    // Clear any previous alarm state.
    rtc.resetAlarm();
    rtc.disableAlarm();

    // PCF8563 alarm: hour, minute, day-of-month (0xFF = any), weekday (0xFF = any)
    rtc.setAlarm(ALARM_HOUR, ALARM_MINUTE, 0xFF, 0xFF);
    rtc.enableAlarm();
    Serial.printf("RTC alarm set for %02d:%02d daily\n", ALARM_HOUR, ALARM_MINUTE);
}

// ── Display helpers (unchanged from original) ──────────────────────────────
const char *getWeekdayShort(int wday)
{
    static const char *days[] = {
        "Sun", "Mon", "Tues", "Wed", "Thurs", "Fri", "Sat"
    };
    if (wday < 0 || wday > 6) return "???";
    return days[wday];
}

const char *getMonthShort(int mon)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (mon < 0 || mon > 11) return "???";
    return months[mon];
}

const char *getOrdinalSuffix(int day)
{
    if (day >= 11 && day <= 13) return "th";
    switch (day % 10) {
        case 1: return "st";
        case 2: return "nd";
        case 3: return "rd";
        default: return "th";
    }
}

void formatCleanStampStrings(
    char *line1, size_t line1Size,
    char *line2, size_t line2Size,
    char *line3, size_t line3Size,
    const struct tm *timeinfo)
{
    int hour12 = timeinfo->tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm = (timeinfo->tm_hour >= 12) ? "pm" : "am";

    snprintf(line1, line1Size, "last cleaned");
    snprintf(line2, line2Size, "%d:%02d%s", hour12, timeinfo->tm_min, ampm);
    snprintf(line3, line3Size, "%s %d/%d",
             getWeekdayShort(timeinfo->tm_wday),
             timeinfo->tm_mon + 1,
             timeinfo->tm_mday);
}

void drawTopLeftCleanStamp()
{
    if (!rtcCleanTimestampValid) return;

    char labelLine[40], timeLine[40], dateLine[24];
    formatCleanStampStrings(
        labelLine, sizeof(labelLine),
        timeLine, sizeof(timeLine),
        dateLine, sizeof(dateLine),
        &rtcLastCleanTimeInfo
    );

    FontProperties props = {
        .fg_color = 0,
        .bg_color = 15,
        .fallback_glyph = 0,
        .flags = 0
    };

    const int boxX = 14;
    const int boxY = 14;

    GFXfont *smallFont = (GFXfont *)&OpenSans12B;
    GFXfont *bigFont   = (GFXfont *)&OpenSans24B;

    const int paddingX = 0;
    const int line1Y = boxY + 30;
    const int line2Y = boxY + 85;
    const int line3Y = boxY + 125;

    int x1 = boxX + paddingX, y1 = line1Y;
    write_mode(smallFont, labelLine, &x1, &y1, framebuffer, BLACK_ON_WHITE, &props);

    int x2 = boxX + paddingX, y2 = line2Y;
    write_mode(bigFont, timeLine, &x2, &y2, framebuffer, BLACK_ON_WHITE, &props);

    int x3 = boxX + paddingX, y3 = line3Y;
    write_mode(smallFont, dateLine, &x3, &y3, framebuffer, BLACK_ON_WHITE, &props);
}

void copySelectedImageToFramebuffer()
{
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    Rect_t area = {
        .x = 0,
        .y = 0,
        .width  = (currentMode == DISPLAY_DIRTY) ? dirty_image_width : clean_image_width,
        .height = (currentMode == DISPLAY_DIRTY) ? dirty_image_height : clean_image_height
    };

    if (currentMode == DISPLAY_DIRTY) {
        epd_copy_to_framebuffer(area, (uint8_t *)dirty_image_data, framebuffer);
        drawTopLeftCleanStamp();
    } else {
        epd_copy_to_framebuffer(area, (uint8_t *)clean_image_data, framebuffer);
        drawTopLeftCleanStamp();
    }
}

void refreshDisplay()
{
    copySelectedImageToFramebuffer();
    epd_poweron();
    epd_clear();
    yield();
    uint32_t t1 = millis();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    uint32_t t2 = millis();
    Serial.printf("EPD draw took %lums\r\n", t2 - t1);
    epd_poweroff();
    yield();
}

void showDirtyNow()
{
    currentMode = DISPLAY_DIRTY;
    Serial.println("Rendering DIRTY image");
    refreshDisplay();
}

void showCleanNow()
{
    struct tm timeinfo;
    if (getRTCLocalTime(&timeinfo)) {
        rtcLastCleanTimeInfo = timeinfo;
        rtcCleanTimestampValid = true;
    } else {
        rtcCleanTimestampValid = false;
    }

    currentMode = DISPLAY_CLEAN;
    Serial.println("Rendering CLEAN image");
    refreshDisplay();
}


WakeReason determineWakeReason()
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause != ESP_SLEEP_WAKEUP_EXT1) {
        return WAKE_COLD_BOOT;
    }

    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    if ((status & (1ULL << CLEAN_BUTTON_PIN)) || (status & (1ULL << CLEAN_BUTTON_PIN_2))) {
        return WAKE_BUTTON;
    }
    if (status & (1ULL << SENSOR_IRQ)) {
        return WAKE_RTC_ALARM;
    }

    // Fallback – treat unknown ext1 trigger as alarm.
    return WAKE_RTC_ALARM;
}

// ── Deep sleep ─────────────────────────────────────────────────────────────
void enterDeepSleep()
{
    if (DISABLE_DEEP_SLEEP) return;

    // Configure ext1 wake on ANY_LOW for both pins.
    uint64_t mask = (1ULL << CLEAN_BUTTON_PIN) | (1ULL << CLEAN_BUTTON_PIN_2) | (1ULL << SENSOR_IRQ);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

    // Enable internal pull-ups so the pins idle HIGH during sleep.
    // On ESP32-S3, ext1 with ANY_LOW keeps RTC peripherals on for pull-ups
    // automatically, but we set them explicitly to be safe.
    rtc_gpio_pullup_en((gpio_num_t)CLEAN_BUTTON_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)CLEAN_BUTTON_PIN);
    rtc_gpio_pullup_en((gpio_num_t)CLEAN_BUTTON_PIN_2);
    rtc_gpio_pulldown_dis((gpio_num_t)CLEAN_BUTTON_PIN_2);
    rtc_gpio_pullup_en((gpio_num_t)SENSOR_IRQ);
    rtc_gpio_pulldown_dis((gpio_num_t)SENSOR_IRQ);

    Serial.println("Entering deep sleep (wake on button or RTC alarm)");
    Serial.flush();
    delay(50);
    esp_deep_sleep_start();
}

// ── setup() ────────────────────────────────────────────────────────────────
void setup()
{
    // hacky fix to disable brown out protection. 
    // the soldering of the battery could be to blame,
    // also seems that a 470µF capacitor across 3.3V and GND near the ESP32 to absorb the current spike from epd_poweron() could help
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(200);

    // De-init RTC IOs that may have been held during sleep.
    rtc_gpio_deinit((gpio_num_t)CLEAN_BUTTON_PIN);
    rtc_gpio_deinit((gpio_num_t)CLEAN_BUTTON_PIN_2);
    rtc_gpio_deinit((gpio_num_t)SENSOR_IRQ);
    pinMode(CLEAN_BUTTON_PIN, INPUT_PULLUP);
    pinMode(CLEAN_BUTTON_PIN_2, INPUT_PULLUP);
    pinMode(SENSOR_IRQ, INPUT_PULLUP);

    // ── Init e-paper ───────────────────────────────────────────────────────
    epd_init();
    framebuffer = (uint8_t *)heap_caps_malloc(
        EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM
    );
    if (!framebuffer) {
        Serial.println("alloc memory failed !!!");
        while (1) delay(1000);
    }
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.printf("esp_reset_reason: %d\n", (int)resetReason);


    int romResetReason = esp_rom_get_reset_reason(0);
    Serial.printf("esp_rom_get_reset_reason CPU0: %d\n", romResetReason);

    // ── Init external RTC ──────────────────────────────────────────────────
    if (!initExternalRTC()) {
        Serial.println("External RTC init failed – halting.");
        while (1) delay(1000);
    }

    // Clear any lingering alarm flag so the IRQ pin goes back HIGH.
    rtc.resetAlarm();

    // ── Determine why we woke up ───────────────────────────────────────────
    WakeReason reason = determineWakeReason();

    switch (reason) {
    case WAKE_BUTTON:
        Serial.println("Woke from BUTTON press (GPIO15)");
        // Button press = "just cleaned". Read time from external RTC (no WiFi).
        showCleanNow();
        break;

    case WAKE_RTC_ALARM:
        Serial.println("Woke from RTC ALARM (GPIO9)");
        alarmWakeCount++;
        Serial.printf("Alarm wake count: %u / %d\n", alarmWakeCount, NTP_SYNC_INTERVAL);

        // Periodically re-sync NTP to keep external RTC accurate.
        if (alarmWakeCount >= NTP_SYNC_INTERVAL) {
            Serial.println("Time for periodic NTP sync...");
            syncTimeFromNTP();
            alarmWakeCount = 0;
        }

        showDirtyNow();
        break;

    case WAKE_COLD_BOOT:
    default:
        Serial.println("Cold boot / reset");
        // First boot ever – must sync NTP to seed the external RTC.
        syncTimeFromNTP();
        alarmWakeCount = 0;
        showDirtyNow();
        break;
    }

    logRTCTime();

    // Only set the alarm on first boot; it persists across sleeps.
    // resetAlarm() at the top of setup() clears the flag each wake.
    if (reason == WAKE_COLD_BOOT) {
        setDailyAlarm();
    }

    delay(50);
    enterDeepSleep();
}

void loop()
{
    // Device always returns to deep sleep from setup().
}
