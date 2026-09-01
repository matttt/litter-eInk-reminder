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
RTC_DATA_ATTR uint8_t rtcPendingCleanerMask = CleaningHistory::None;
RTC_DATA_ATTR struct tm rtcLastCleanTime = {0};

namespace {

volatile uint8_t awakeCleanerMask = CleaningHistory::None;
bool awakeCleanerCaptureActive = false;
bool orianaCapturePausedForSd = false;
bool showMonthlySummary = false;
portMUX_TYPE awakeCleanerMux = portMUX_INITIALIZER_UNLOCKED;

struct WakeSources {
    bool coldBoot;
    bool clockRecovery;
    bool cleanerButton;
    bool rtcAlarm;
    uint64_t ext1Status;
};

void IRAM_ATTR captureOrianaPress()
{
    portENTER_CRITICAL_ISR(&awakeCleanerMux);
    awakeCleanerMask |= CleaningHistory::Oriana;
    portEXIT_CRITICAL_ISR(&awakeCleanerMux);
}

void IRAM_ATTR captureMatthewPress()
{
    portENTER_CRITICAL_ISR(&awakeCleanerMux);
    awakeCleanerMask |= CleaningHistory::Matthew;
    portEXIT_CRITICAL_ISR(&awakeCleanerMux);
}

void mergeAwakeCleanerMask(uint8_t cleanerMask)
{
    portENTER_CRITICAL(&awakeCleanerMux);
    awakeCleanerMask |= cleanerMask;
    portEXIT_CRITICAL(&awakeCleanerMux);
}

uint8_t readAwakeCleanerMask()
{
    portENTER_CRITICAL(&awakeCleanerMux);
    uint8_t cleanerMask = awakeCleanerMask;
    portEXIT_CRITICAL(&awakeCleanerMux);
    return cleanerMask;
}

uint8_t consumeAwakeCleanerMask()
{
    portENTER_CRITICAL(&awakeCleanerMux);
    uint8_t cleanerMask = awakeCleanerMask;
    awakeCleanerMask = CleaningHistory::None;
    portEXIT_CRITICAL(&awakeCleanerMux);
    return cleanerMask;
}

uint64_t sampleCleanerPins()
{
    uint64_t status = 0;
    if (digitalRead(AppConfig::OrianaButtonPin) == LOW) {
        status |= 1ULL << AppConfig::OrianaButtonPin;
    }
    if (digitalRead(AppConfig::MatthewButtonPin) == LOW) {
        status |= 1ULL << AppConfig::MatthewButtonPin;
    }
    return status;
}

void beginCapturingAwakeCleanerPresses(uint8_t initialMask)
{
    if (awakeCleanerCaptureActive) {
        mergeAwakeCleanerMask(initialMask);
    } else {
        portENTER_CRITICAL(&awakeCleanerMux);
        awakeCleanerMask = initialMask;
        portEXIT_CRITICAL(&awakeCleanerMux);
        attachInterrupt(digitalPinToInterrupt(AppConfig::OrianaButtonPin),
                        captureOrianaPress, FALLING);
        attachInterrupt(digitalPinToInterrupt(AppConfig::MatthewButtonPin),
                        captureMatthewPress, FALLING);
        awakeCleanerCaptureActive = true;
        orianaCapturePausedForSd = false;
    }

    // Close the small gap between the initial sample and ISR attachment.
    uint64_t status = sampleCleanerPins();
    if (status & (1ULL << AppConfig::OrianaButtonPin)) {
        mergeAwakeCleanerMask(CleaningHistory::Oriana);
    }
    if (status & (1ULL << AppConfig::MatthewButtonPin)) {
        mergeAwakeCleanerMask(CleaningHistory::Matthew);
    }
}

void pauseOrianaCaptureForSd()
{
    if (!awakeCleanerCaptureActive || orianaCapturePausedForSd) return;

    detachInterrupt(digitalPinToInterrupt(AppConfig::OrianaButtonPin));
    orianaCapturePausedForSd = true;
}

void resumeOrianaCaptureAfterSd()
{
    if (!awakeCleanerCaptureActive || !orianaCapturePausedForSd) return;

    pinMode(AppConfig::OrianaButtonPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(AppConfig::OrianaButtonPin),
                    captureOrianaPress, FALLING);
    orianaCapturePausedForSd = false;

    // Close the interrupt-detached handoff gap for a press that remains held.
    if (digitalRead(AppConfig::OrianaButtonPin) == LOW) {
        mergeAwakeCleanerMask(CleaningHistory::Oriana);
    }
}

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

void renderCurrentState()
{
    bool pendingCleaning =
        rtcPendingCleanerMask != CleaningHistory::None;
    if ((rtcCleanTimestampValid || pendingCleaning) &&
        !rtcReminderOverdue) {
        DisplayUi::showClean(displayState());
    } else {
        DisplayUi::showDirty(displayState());
    }
}

WakeSources determineWakeSources()
{
    WakeSources sources = {};
    esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    sources.clockRecovery = wakeCause == ESP_SLEEP_WAKEUP_TIMER;
    if (sources.clockRecovery) {
        // TIMER has priority in the reported wake cause. Sample the buttons so
        // a simultaneous EXT1 cleaner press is not discarded.
        sources.ext1Status = sampleCleanerPins();
        sources.cleanerButton = sources.ext1Status != 0;
        return sources;
    }

    sources.coldBoot = wakeCause != ESP_SLEEP_WAKEUP_EXT1;
    if (sources.coldBoot) return sources;

    sources.ext1Status = esp_sleep_get_ext1_wakeup_status();
    sources.cleanerButton =
        (sources.ext1Status & (1ULL << AppConfig::OrianaButtonPin)) ||
        (sources.ext1Status & (1ULL << AppConfig::MatthewButtonPin));
    sources.rtcAlarm =
        sources.ext1Status & (1ULL << AppConfig::RtcIrqPin);

    // Treat an unknown EXT1 source as an RTC wake. The logical scheduler still
    // verifies deadlines against wall-clock time before changing UI state.
    if (!sources.cleanerButton && !sources.rtcAlarm) sources.rtcAlarm = true;
    return sources;
}

CleaningHistory::Cleaner determineCleaner(uint64_t ext1Status)
{
    bool orianaPressed =
        ext1Status & (1ULL << AppConfig::OrianaButtonPin);
    bool matthewPressed =
        ext1Status & (1ULL << AppConfig::MatthewButtonPin);

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
    // GPIO0 is also the e-paper controller's CFG_STR signal. Explicitly return
    // it to input mode before sampling the physical BOOT button even if a
    // display operation was interrupted.
    rtc_gpio_deinit((gpio_num_t)AppConfig::ProgrammingButtonPin);
    pinMode(AppConfig::ProgrammingButtonPin, INPUT_PULLUP);
    delay(10);

    if (digitalRead(AppConfig::ProgrammingButtonPin) != LOW) return false;

    // Ignore reset/strapping transients and button bounce.
    delay(100);
    return digitalRead(AppConfig::ProgrammingButtonPin) == LOW;
}

bool scheduledEventOccurred(ClockService::ScheduledEventMask events,
                            ClockService::ScheduledEvent event)
{
    return (events & static_cast<ClockService::ScheduledEventMask>(event)) != 0;
}

ClockService::ScheduledEventMask serviceScheduledEventsAndRefreshTime(
    struct tm *now)
{
    if (!now) return ClockService::ScheduledEventNone;

    struct tm sampledNow = *now;
    ClockService::ScheduledEventMask events =
        ClockService::serviceScheduledEvents(now);

    // Alarm programming can itself straddle a minute or midnight. The
    // scheduler returns that event immediately; refresh the caller's time so
    // history and a simultaneous cleaner press are applied to the new day.
    struct tm refreshedNow;
    if (ClockService::getLocalTime(&refreshedNow)) {
        *now = refreshedNow;

        bool crossedDate =
            sampledNow.tm_year != refreshedNow.tm_year ||
            sampledNow.tm_mon != refreshedNow.tm_mon ||
            sampledNow.tm_mday != refreshedNow.tm_mday;
        bool midnightReturned =
            events & ClockService::ScheduledEventMidnight;
        if (crossedDate && !midnightReturned) {
            events |= ClockService::serviceScheduledEvents(now);
            struct tm finalNow;
            if (ClockService::getLocalTime(&finalNow)) *now = finalNow;
        }
    }
    return events;
}

void logScheduledEvents(ClockService::ScheduledEventMask events)
{
    if (scheduledEventOccurred(events,
                               ClockService::ScheduledEventMidnight)) {
        Serial.println("Midnight timer elapsed; advancing the history panel.");
    }
    if (scheduledEventOccurred(
            events, ClockService::ScheduledEventCleaningReminder)) {
        Serial.println("24-hour cleaning timer elapsed.");
    }
}

void refreshCleaningHistory(const struct tm *now,
                            const struct tm *newCleaningTime = nullptr,
                            CleaningHistory::Cleaner newCleaner =
                                CleaningHistory::None)
{
    showMonthlySummary = now && now->tm_mday == 1;
    CleaningHistory::refresh(now, newCleaningTime, newCleaner);
}

void applyScheduledEvents(ClockService::ScheduledEventMask events,
                          const struct tm *now)
{
    logScheduledEvents(events);
    if (scheduledEventOccurred(
            events, ClockService::ScheduledEventCleaningReminder)) {
        rtcReminderOverdue = true;
    }
    if (now) refreshCleaningHistory(now);
}

void recordCleanerMask(uint8_t cleanerMask)
{
    cleanerMask &= static_cast<uint8_t>(CleaningHistory::Both);
    if (cleanerMask == CleaningHistory::None) return;

    CleaningHistory::Cleaner cleaner =
        static_cast<CleaningHistory::Cleaner>(cleanerMask);
    Serial.printf("Recording cleaner: %s\n",
                  CleaningHistory::cleanerName(cleaner));

    struct tm now;
    bool nowValid = ClockService::getLocalTime(&now);
    rtcReminderOverdue = false;
    rtcCleanTimestampValid = nowValid;

    if (nowValid) {
        uint8_t combinedCleanerMask =
            rtcPendingCleanerMask | cleanerMask;
        cleaner = static_cast<CleaningHistory::Cleaner>(
            combinedCleanerMask &
            static_cast<uint8_t>(CleaningHistory::Both)
        );

        rtcPendingCleanerMask = CleaningHistory::None;
        rtcLastCleanTime = now;
        refreshCleaningHistory(&now, &now, cleaner);
        ClockService::scheduleCleaningReminder24HoursAfter(&now);
    } else {
        rtcPendingCleanerMask |= cleanerMask;
    }

    DisplayUi::showClean(displayState());
}

bool processCapturedAwakeCleaners()
{
    uint8_t cleanerMask = consumeAwakeCleanerMask();
    if (cleanerMask == CleaningHistory::None) return false;
    recordCleanerMask(cleanerMask);
    return true;
}

void drainScheduledEventsBeforeSleep()
{
    // Rendering can straddle midnight (or the cleaning deadline). Re-read the
    // clock after all normal wake work and repaint once more if that happened.
    constexpr uint8_t MaxDrainPasses = 3;
    for (uint8_t pass = 0; pass < MaxDrainPasses; ++pass) {
        struct tm now;
        if (!ClockService::getLocalTime(&now)) {
            Serial.println("Could not service timers before sleep.");
            return;
        }

        ClockService::ScheduledEventMask events =
            serviceScheduledEventsAndRefreshTime(&now);
        if (events == ClockService::ScheduledEventNone) return;

        applyScheduledEvents(events, &now);
        renderCurrentState();
    }

    Serial.println("Timer drain limit reached; the armed IRQ will wake us again.");
}

void enterDeepSleep()
{
    bool recoveryTimerEnabled = false;

    // Arduino-ESP32 2.0.17 / ESP-IDF 4.4 powers RTC_PERIPH down for EXT1 by
    // default. In that mode the sleep transition disables the internal pulls,
    // even though the pins read HIGH immediately beforehand. Keep the domain
    // on so the active-low cleaner and PCF8563 inputs cannot float LOW.
    esp_err_t rtcPowerResult = esp_sleep_pd_config(
        ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON
    );
    if (rtcPowerResult != ESP_OK) {
        Serial.printf(
            "Failed to keep RTC peripherals powered for sleep: 0x%x\n",
            static_cast<unsigned>(rtcPowerResult)
        );
        return;
    }

    while (true) {
        if (programmingButtonIsHeld()) {
            Serial.println(
                "Programming mode active; staying awake until reset."
            );
            return;
        }

        drainScheduledEventsBeforeSleep();

        // A second press can arrive while the e-paper is rendering. Consume it
        // as another same-day cleaning instead of silently waiting it out.
        if (processCapturedAwakeCleaners()) continue;

        // Sleeping while a button is LOW would immediately log the same press
        // again. Interrupt capture remains active while we wait, so a newly
        // pressed/released button is handled on the next loop pass.
        waitForCleaningButtonsToBeReleased();
        if (processCapturedAwakeCleaners()) continue;

        // GPIO0 may have been used by the last display refresh.
        if (programmingButtonIsHeld()) {
            Serial.println(
                "Programming mode active; staying awake until reset."
            );
            return;
        }

        ClockService::SchedulerSnapshot schedule =
            ClockService::schedulerSnapshot();
        bool rtcAlarmVerified =
            schedule.stateValid && schedule.armedAlarmEpoch > 0;
        bool rtcIrqReleased =
            digitalRead(AppConfig::RtcIrqPin) == HIGH;
        bool useRtcAlarmWake = rtcAlarmVerified && rtcIrqReleased;

        // Never enter active-low EXT1 sleep with the PCF8563 INT line already
        // asserted. If register verification or the physical line is bad,
        // omit only GPIO9 and let the independent ESP timer retry safely.
        uint64_t mask = (1ULL << AppConfig::OrianaButtonPin) |
                        (1ULL << AppConfig::MatthewButtonPin);
        if (useRtcAlarmWake) {
            mask |= 1ULL << AppConfig::RtcIrqPin;
        }
        esp_err_t ext1Result = esp_sleep_enable_ext1_wakeup(
            mask, ESP_EXT1_WAKEUP_ANY_LOW
        );
        if (ext1Result != ESP_OK) {
            Serial.printf(
                "Failed to configure EXT1 wake pins: 0x%x\n",
                static_cast<unsigned>(ext1Result)
            );
            return;
        }

        if (!useRtcAlarmWake) {
            esp_err_t timerResult = esp_sleep_enable_timer_wakeup(
                AppConfig::ClockRecoveryIntervalUs
            );
            if (timerResult != ESP_OK) {
                Serial.printf(
                    "Failed to configure ESP recovery timer: 0x%x\n",
                    static_cast<unsigned>(timerResult)
                );
                return;
            }
            recoveryTimerEnabled = true;
            Serial.printf(
                "RTC alarm wake unavailable (verified=%s, GPIO9=%s); "
                "using one-hour ESP recovery wake.\n",
                rtcAlarmVerified ? "yes" : "no",
                rtcIrqReleased ? "HIGH" : "LOW"
            );
        } else if (recoveryTimerEnabled) {
            // A previous pass through this loop may have enabled the fallback
            // before a just-captured cleaning successfully re-armed the RTC.
            esp_err_t timerResult = esp_sleep_disable_wakeup_source(
                ESP_SLEEP_WAKEUP_TIMER
            );
            if (timerResult != ESP_OK) {
                Serial.printf(
                    "Failed to disable ESP recovery timer: 0x%x\n",
                    static_cast<unsigned>(timerResult)
                );
                return;
            }
            recoveryTimerEnabled = false;
        }

        esp_err_t pullResult = ESP_OK;
        pullResult |= rtc_gpio_pullup_en(
            (gpio_num_t)AppConfig::OrianaButtonPin
        );
        pullResult |= rtc_gpio_pulldown_dis(
            (gpio_num_t)AppConfig::OrianaButtonPin
        );
        pullResult |= rtc_gpio_pullup_en(
            (gpio_num_t)AppConfig::MatthewButtonPin
        );
        pullResult |= rtc_gpio_pulldown_dis(
            (gpio_num_t)AppConfig::MatthewButtonPin
        );
        pullResult |= rtc_gpio_pullup_en(
            (gpio_num_t)AppConfig::RtcIrqPin
        );
        pullResult |= rtc_gpio_pulldown_dis(
            (gpio_num_t)AppConfig::RtcIrqPin
        );
        if (pullResult != ESP_OK) {
            Serial.printf(
                "Failed to configure RTC wake-pin pulls: 0x%x\n",
                static_cast<unsigned>(pullResult)
            );
            return;
        }

        Serial.printf(
            "Entering deep sleep (wake on buttons%s); "
            "pins O/M/RTC=%d/%d/%d\n",
            useRtcAlarmWake ? " or RTC alarm" : " plus ESP recovery timer",
            digitalRead(AppConfig::OrianaButtonPin),
            digitalRead(AppConfig::MatthewButtonPin),
            digitalRead(AppConfig::RtcIrqPin)
        );
        Serial.flush();
        delay(50);

        // Cover button presses that land during final wake-source setup or
        // serial flushing. A held press would wake immediately anyway; a short
        // press is recorded here without an unnecessary sleep/wake cycle.
        if (processCapturedAwakeCleaners()) continue;

        // The alarm may legitimately become due during the final serial
        // flush. Service it while awake instead of entering sleep on LOW and
        // immediately rebooting. A persistent LOW is rejected by the next
        // scheduler arm and falls back to the ESP timer above.
        if (useRtcAlarmWake &&
            digitalRead(AppConfig::RtcIrqPin) == LOW) {
            Serial.println(
                "RTC IRQ asserted during sleep handoff; servicing it awake."
            );
            continue;
        }

        // Clear the complete display power register only once all rendering
        // and last-chance input processing have finished.
        DisplayUi::prepareForDeepSleep();
        if (!CleaningHistory::prepareForDeepSleep()) {
            Serial.println(
                "Failed to retain SD clock LOW; entering deep sleep anyway."
            );
            Serial.flush();
        }

        // SD/display teardown is deliberately last, but it still takes a
        // finite amount of time. Drain one final button edge before sleeping;
        // restore the SD clock pad first because recording can mount the card.
        uint64_t lateButtonStatus = sampleCleanerPins();
        if (lateButtonStatus &
            (1ULL << AppConfig::OrianaButtonPin)) {
            mergeAwakeCleanerMask(CleaningHistory::Oriana);
        }
        if (lateButtonStatus &
            (1ULL << AppConfig::MatthewButtonPin)) {
            mergeAwakeCleanerMask(CleaningHistory::Matthew);
        }
        uint8_t lateCleanerMask = consumeAwakeCleanerMask();
        if (lateCleanerMask != CleaningHistory::None) {
            CleaningHistory::beginWakeCycle();
            recordCleanerMask(lateCleanerMask);
            continue;
        }

        if (useRtcAlarmWake &&
            digitalRead(AppConfig::RtcIrqPin) == LOW) {
            CleaningHistory::beginWakeCycle();
            Serial.println(
                "RTC IRQ asserted during final teardown; servicing it awake."
            );
            continue;
        }
        esp_deep_sleep_start();
    }
}

void countRtcAlarmWake(bool allowNtpSync)
{
    alarmWakeCount++;
    Serial.printf("Alarm wake count: %u / %u\n",
                  alarmWakeCount, AppConfig::NtpSyncInterval);

    if (allowNtpSync && alarmWakeCount >= AppConfig::NtpSyncInterval) {
        Serial.println("Time for periodic NTP sync...");
        ClockService::syncFromNtp();
        alarmWakeCount = 0;
    }
}

void handleDeepSleepWake(const WakeSources &sources)
{
    uint8_t capturedCleanerMask = readAwakeCleanerMask();
    if (sources.rtcAlarm) {
        Serial.println("Woke from RTC ALARM (GPIO9)");
        // Skip a potentially long Wi-Fi sync when a cleaner was already
        // captured. A press that begins during sync is latched by the ISR.
        countRtcAlarmWake(!sources.cleanerButton &&
                          capturedCleanerMask == CleaningHistory::None);
    }
    capturedCleanerMask |= consumeAwakeCleanerMask();
    bool cleanerButton =
        sources.cleanerButton ||
        capturedCleanerMask != CleaningHistory::None;

    struct tm now;
    bool nowValid = ClockService::getLocalTime(&now);
    ClockService::ScheduledEventMask events =
        nowValid ? serviceScheduledEventsAndRefreshTime(&now)
                 : ClockService::serviceScheduledEvents();

    if (!cleanerButton) {
        if (nowValid) {
            applyScheduledEvents(events, &now);
        } else {
            applyScheduledEvents(events, nullptr);
        }
        renderCurrentState();
        return;
    }

    // Midnight is still meaningful on a simultaneous button wake. The old
    // reminder is intentionally superseded by the cleaning below.
    logScheduledEvents(events);

    uint8_t cleanerMask =
        static_cast<uint8_t>(determineCleaner(sources.ext1Status)) |
        capturedCleanerMask;
    CleaningHistory::Cleaner cleaner =
        static_cast<CleaningHistory::Cleaner>(
            cleanerMask & static_cast<uint8_t>(CleaningHistory::Both)
        );
    Serial.printf("Woke from cleaner button: %s\n",
                  CleaningHistory::cleanerName(cleaner));

    // A new cleaning supersedes an old reminder even when both wake sources
    // arrived together. Any due midnight was already reconciled above.
    recordCleanerMask(cleanerMask);
}

void handleClockRecoveryWake(const WakeSources &sources)
{
    Serial.println("External clock recovery wake");

    uint8_t initialCleanerMask = static_cast<uint8_t>(
        determineCleaner(sources.ext1Status)
    );
    beginCapturingAwakeCleanerPresses(initialCleanerMask);
    ClockService::syncFromNtp();
    rtcPendingCleanerMask |=
        consumeAwakeCleanerMask() &
        static_cast<uint8_t>(CleaningHistory::Both);

    struct tm now;
    if (!ClockService::getLocalTime(&now)) {
        if (rtcPendingCleanerMask != CleaningHistory::None) {
            rtcCleanTimestampValid = false;
            rtcReminderOverdue = false;
            DisplayUi::showClean(displayState());
        }
        Serial.println("External clock is still invalid; will retry later.");
        return;
    }

    if (rtcPendingCleanerMask != CleaningHistory::None) {
        recordCleanerMask(rtcPendingCleanerMask);
        return;
    }

    ClockService::ScheduledEventMask events =
        serviceScheduledEventsAndRefreshTime(&now);
    applyScheduledEvents(events, &now);
    renderCurrentState();
}

void handleColdBoot()
{
    Serial.println("Cold boot / reset");
    ClockService::syncFromNtp();
    alarmWakeCount = 0;
    rtcCleanTimestampValid = false;
    rtcReminderOverdue = false;
    rtcPendingCleanerMask = CleaningHistory::None;
    rtcLastCleanTime = {};

    struct tm now;
    if (ClockService::getLocalTime(&now)) {
        refreshCleaningHistory(&now);
        ClockService::resetScheduledEvents(&now);
    } else {
        ClockService::resetScheduledEvents();
    }
    DisplayUi::showDirty(displayState());
}

}  // namespace

void setup()
{
    // Work around brownouts caused by the e-paper power-on current spike.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    ClockService::configureLocalTimezone();
    CleaningHistory::beginWakeCycle();
    configureWakePins();

    // Capture wake information before display/RTC initialisation so a cleaner
    // button coincident with the lower-priority recovery timer cannot be
    // released and lost during startup.
    WakeSources wakeSources = determineWakeSources();
    beginCapturingAwakeCleanerPresses(static_cast<uint8_t>(
        determineCleaner(wakeSources.ext1Status)
    ));
    CleaningHistory::setSdAccessHooks(
        pauseOrianaCaptureForSd,
        resumeOrianaCaptureAfterSd
    );

    delay(200);

    if (!DisplayUi::initialise()) {
        while (true) delay(1000);
    }

    Serial.printf("esp_reset_reason: %d\n", (int)esp_reset_reason());
    Serial.printf("esp_rom_get_reset_reason CPU0: %d\n",
                  esp_rom_get_reset_reason(0));
    Serial.printf(
        "Wake cause=%d EXT1=0x%llx; pins O/M/RTC=%d/%d/%d\n",
        static_cast<int>(esp_sleep_get_wakeup_cause()),
        static_cast<unsigned long long>(wakeSources.ext1Status),
        digitalRead(AppConfig::OrianaButtonPin),
        digitalRead(AppConfig::MatthewButtonPin),
        digitalRead(AppConfig::RtcIrqPin)
    );

    if (!ClockService::initialise()) {
        Serial.println("External RTC init failed – halting.");
        while (true) delay(1000);
    }

    ClockService::clearAlarmFlag();

    if (wakeSources.coldBoot) {
        handleColdBoot();
    } else if (wakeSources.clockRecovery) {
        handleClockRecoveryWake(wakeSources);
    } else {
        handleDeepSleepWake(wakeSources);
    }

    // Native USB reconnects after the early setup diagnostics have already
    // been printed. Repeat the latched values after rendering, where the
    // monitor can capture the actual wake source and EXT1 bitmap.
    Serial.printf(
        "Wake summary: reset=%d rom=%d cause=%d EXT1=0x%llx; "
        "pins O/M/RTC=%d/%d/%d\n",
        static_cast<int>(esp_reset_reason()),
        esp_rom_get_reset_reason(0),
        static_cast<int>(esp_sleep_get_wakeup_cause()),
        static_cast<unsigned long long>(wakeSources.ext1Status),
        digitalRead(AppConfig::OrianaButtonPin),
        digitalRead(AppConfig::MatthewButtonPin),
        digitalRead(AppConfig::RtcIrqPin)
    );

    ClockService::logTime();
    delay(50);
    enterDeepSleep();
}

void loop()
{
    // Reached only when GPIO0 has latched programming mode.
    delay(100);
}
