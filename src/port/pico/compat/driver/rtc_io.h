/*
 * driver/rtc_io.h - pico-sdk compatibility shim.
 *
 * RP2350's low-power domain does not expose an RTC-GPIO mux the way ESP32
 * does. Wake-on-pin would go through hardware_powman instead. Nothing in this
 * flavour's deep-sleep path is implemented (see doc/ports/picocalc.md), so
 * these report "unsupported" rather than silently doing nothing.
 */
#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline bool rtc_gpio_is_valid_gpio(gpio_num_t pin) { (void)pin; return false; }
static inline esp_err_t rtc_gpio_init(gpio_num_t pin) { (void)pin; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t rtc_gpio_pullup_en(gpio_num_t pin) { (void)pin; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin) { (void)pin; return ESP_ERR_NOT_SUPPORTED; }

#ifdef __cplusplus
}
#endif
