#pragma once
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
enum esp_sleep_wakeup_cause_t {
    ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_EXT1
};
constexpr int ESP_PD_DOMAIN_RTC_PERIPH = 0;
constexpr int ESP_PD_OPTION_ON = 0;
constexpr int ESP_EXT1_WAKEUP_ANY_LOW = 0;
inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_UNDEFINED; }
inline uint64_t esp_sleep_get_ext1_wakeup_status() { return 0; }
inline esp_err_t esp_sleep_pd_config(int, int) { return ESP_OK; }
inline esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t, int) { return ESP_OK; }
inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t) { return ESP_OK; }
inline esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t) { return ESP_OK; }
inline void esp_deep_sleep_start() {}
inline int esp_reset_reason() { return 0; }
