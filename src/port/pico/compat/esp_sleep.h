/*
 * esp_sleep.h - pico-sdk compatibility shim.
 *
 * Deep sleep is NOT implemented for this target. RP2350's equivalent lives in
 * pico-sdk's hardware_powman (dormant/sleep modes with a powman alarm or GPIO
 * wake), which is a separate piece of work and is listed as deferred in
 * doc/ports/picocalc.md.
 *
 * These return ESP_ERR_NOT_SUPPORTED rather than ESP_OK on purpose: a caller
 * that thinks it armed a wake source and then slept forever is worse than one
 * that is told it cannot.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_GPIO,
    ESP_SLEEP_WAKEUP_UART,
} esp_sleep_wakeup_cause_t;

typedef enum {
    ESP_EXT1_WAKEUP_ALL_LOW = 0,
    ESP_EXT1_WAKEUP_ANY_HIGH = 1,
    ESP_EXT1_WAKEUP_ANY_LOW = 2,
} esp_sleep_ext1_wakeup_mode_t;

static inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us)
{ (void)us; return ESP_ERR_NOT_SUPPORTED; }

static inline esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t mask,
                                                     esp_sleep_ext1_wakeup_mode_t mode)
{ (void)mask; (void)mode; return ESP_ERR_NOT_SUPPORTED; }

static inline esp_err_t esp_sleep_enable_ext0_wakeup(int pin, int level)
{ (void)pin; (void)level; return ESP_ERR_NOT_SUPPORTED; }

static inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void)
{ return ESP_SLEEP_WAKEUP_UNDEFINED; }

/* Not implemented: returns instead of sleeping, so callers fall through to
 * their "sleep failed" path rather than hanging. */
static inline esp_err_t esp_light_sleep_start(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void esp_deep_sleep_start(void) { }

#ifdef __cplusplus
}
#endif
