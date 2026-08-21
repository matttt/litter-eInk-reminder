/**
 * Firmware entry point for the LILYGO T5 4.7-inch e-paper V2.3.
 *
 * Hardware-specific services live in separate modules; this file owns the
 * wake-reason workflow and the state that must survive deep sleep.
 */

#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM, Arduino IDE -> tools -> PSRAM -> OPI !!!"
#endif

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

#include "app_config.h"
#include "cleaning_history.h"
#include "clock_service.h"
#include "display_ui.h"
#include "esp32s3/rom/rtc.h"

// These values persist across deep-sleep cycles in RTC slow memory.
RTC_DATA_ATTR uint32_t alarmWakeCount = 0;
RTC_DATA_ATTR bool rtcCleanTimestampValid = false;
RTC_DATA_ATTR bool rtcReminderOverdue = false;
RTC_DATA_ATTR struct tm rtcLastCleanTime = {0};

namespace {

enum WakeReason {
    ColdBoot,
    CleanerButton,
    RtcAlarm
};

bool showMonthlySummary = false;

DisplayUi::State displayState()
{
    DisplayUi::State state = {};
    state.lastCleanTimestampValid = rtcCleanTimestampValid;
    state.lastCleanTime = rtcLastCleanTime;
    state.reminderOverdue = rtcReminderOverdue;
    state.showAllTimePieChart = showMonthlySummary;
    return state;
}

void configureWakePins()
{
    rtc_gpio_deinit((gpio_num_t)AppConfig::OrianaButtonPin);
    rtc_gpio_deinit((gpio_num_t)AppConfig::MatthewButtonPin);
    rtc_gpio_deinit((gpio_num_t)AppConfig::ProgrammingButtonPin);
    rtc_gpio_deinit((gpio_num_t)AppConfig::RtcIrqPin);

    pinMode(AppConfig::OrianaButtonPin, INPUT_PULLUP);
    pinMode(AppConfig::MatthewButtonPin, INPUT_PULLUP);
    pinMode(AppConfig::ProgrammingButtonPin, INPUT_PULLUP);
    pinMode(AppConfig::RtcIrqPin, INPUT_PULLUP);
}

WakeReason determineWakeReason()
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
        return ColdBoot;
    }

    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    if ((status & (1ULL << AppConfig::OrianaButtonPin)) ||
        (status & (1ULL << AppConfig::MatthewButtonPin))) {
        return CleanerButton;
    }
    if (status & (1ULL << AppConfig::RtcIrqPin)) {
        return RtcAlarm;
    }

    return RtcAlarm;
}

CleaningHistory::Cleaner determineCleaner()
{
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    bool orianaPressed = status & (1ULL << AppConfig::OrianaButtonPin);
    bool matthewPressed = status & (1ULL << AppConfig::MatthewButtonPin);

    if (orianaPressed && matthewPressed) return CleaningHistory::Both;
    if (orianaPressed) return CleaningHistory::Oriana;
    if (matthewPressed) return CleaningHistory::Matthew;
    return CleaningHistory::None;
}

void waitForCleaningButtonsToBeReleased()
{
    pinMode(AppConfig::OrianaButtonPin, INPUT_PULLUP);
    pinMode(AppConfig::MatthewButtonPin, INPUT_PULLUP);

    bool waitingLogged = false;
    while (digitalRead(AppConfig::OrianaButtonPin) == LOW ||
           digitalRead(AppConfig::MatthewButtonPin) == LOW) {
        if (!waitingLogged) {
            Serial.println("Waiting for cleaner button release before sleep...");
            waitingLogged = true;
        }
        delay(20);
    }
    delay(30);
}

bool programmingButtonIsHeld()
{
    // GPIO0 is also the e-paper controller's CFG_STR signal. Rendering leaves
    // it configured as an output, so return it to input mode before sampling
    // the physical BOOT button.
    rtc_gpio_deinit((gpio_num_t)AppConfig::ProgrammingButtonPin);
    pinMode(AppConfig::ProgrammingButtonPin, INPUT_PULLUP);
    delay(10);

    if (digitalRead(AppConfig::ProgrammingButtonPin) != LOW) return false;

    // Ignore reset/strapping transients and button bounce.
    delay(100);
    return digitalRead(AppConfig::ProgrammingButtonPin) == LOW;
}

void enterDeepSleep()
{
    if (programmingButtonIsHeld()) {
        Serial.println("Programming mode active; staying awake until reset.");
        return;
    }

    // Sleeping while a button is LOW would immediately log the same press again.
    waitForCleaningButtonsToBeReleased();

    // Catch GPIO0 presses made while waiting for a cleaner button release.
    if (programmingButtonIsHeld()) {
        Serial.println("Programming mode active; staying awake until reset.");
        return;
    }

    uint64_t mask = (1ULL << AppConfig::OrianaButtonPin) |
                    (1ULL << AppConfig::MatthewButtonPin) |
                    (1ULL << AppConfig::RtcIrqPin);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

    rtc_gpio_pullup_en((gpio_num_t)AppConfig::OrianaButtonPin);
    rtc_gpio_pulldown_dis((gpio_num_t)AppConfig::OrianaButtonPin);
    rtc_gpio_pullup_en((gpio_num_t)AppConfig::MatthewButtonPin);
    rtc_gpio_pulldown_dis((gpio_num_t)AppConfig::MatthewButtonPin);
    rtc_gpio_pullup_en((gpio_num_t)AppConfig::RtcIrqPin);
    rtc_gpio_pulldown_dis((gpio_num_t)AppConfig::RtcIrqPin);

    Serial.println("Entering deep sleep (wake on button or RTC alarm)");
    Serial.flush();
    delay(50);
    esp_deep_sleep_start();
}

void handleCleanerButtonWake()
{
    CleaningHistory::Cleaner cleaner = determineCleaner();
    Serial.printf("Woke from cleaner button: %s\n",
                  CleaningHistory::cleanerName(cleaner));

    rtcReminderOverdue = false;
    rtcCleanTimestampValid = ClockService::getLocalTime(&rtcLastCleanTime);
    if (rtcCleanTimestampValid) {
        showMonthlySummary = rtcLastCleanTime.tm_mday == 1;
        CleaningHistory::refresh(&rtcLastCleanTime,
                                 &rtcLastCleanTime, cleaner);
    }

    DisplayUi::showClean(displayState());

    if (rtcCleanTimestampValid) {
        ClockService::setReminder24HoursAfter(&rtcLastCleanTime);
    } else {
        ClockService::setReminder24HoursFromNow();
    }
}

void handleRtcAlarmWake()
{
    Serial.println("Woke from RTC ALARM (GPIO9)");
    alarmWakeCount++;
    Serial.printf("Alarm wake count: %u / %u\n",
                  alarmWakeCount, AppConfig::NtpSyncInterval);

    if (alarmWakeCount >= AppConfig::NtpSyncInterval) {
        Serial.println("Time for periodic NTP sync...");
        ClockService::syncFromNtp();
        alarmWakeCount = 0;
    }

    struct tm now;
    if (ClockService::getLocalTime(&now)) {
        showMonthlySummary = now.tm_mday == 1;
        CleaningHistory::refresh(&now);
    }
    DisplayUi::showDirty(displayState());
    ClockService::setReminder24HoursFromNow();

    // A subsequent alarm refresh will show the overdue marker.
    rtcReminderOverdue = true;
}

void handleColdBoot()
{
    Serial.println("Cold boot / reset");
    ClockService::syncFromNtp();
    alarmWakeCount = 0;
    rtcReminderOverdue = false;
    ClockService::disableAlarm();

    struct tm now;
    if (ClockService::getLocalTime(&now)) {
        showMonthlySummary = now.tm_mday == 1;
        CleaningHistory::refresh(&now);
    }
    DisplayUi::showDirty(displayState());
}

}  // namespace

void setup()
{
    // Work around brownouts caused by the e-paper power-on current spike.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(200);

    ClockService::configureLocalTimezone();
    configureWakePins();

    if (!DisplayUi::initialise()) {
        while (true) delay(1000);
    }

    Serial.printf("esp_reset_reason: %d\n", (int)esp_reset_reason());
    Serial.printf("esp_rom_get_reset_reason CPU0: %d\n",
                  esp_rom_get_reset_reason(0));

    if (!ClockService::initialise()) {
        Serial.println("External RTC init failed – halting.");
        while (true) delay(1000);
    }

    ClockService::clearAlarmFlag();

    switch (determineWakeReason()) {
        case CleanerButton:
            handleCleanerButtonWake();
            break;
        case RtcAlarm:
            handleRtcAlarmWake();
            break;
        case ColdBoot:
        default:
            handleColdBoot();
            break;
    }

    ClockService::logTime();
    delay(50);
    enterDeepSleep();
}

void loop()
{
    // Reached only when GPIO0 has latched programming mode.
    delay(100);
}
