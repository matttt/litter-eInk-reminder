#pragma once
#include <esp_sleep.h>
using gpio_num_t = int;
inline void rtc_gpio_deinit(gpio_num_t) {}
inline esp_err_t rtc_gpio_pullup_en(gpio_num_t) { return ESP_OK; }
inline esp_err_t rtc_gpio_pulldown_dis(gpio_num_t) { return ESP_OK; }
