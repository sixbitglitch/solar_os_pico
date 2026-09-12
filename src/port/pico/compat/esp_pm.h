/*
 * esp_pm.h - pico-sdk compatibility shim.
 *
 * ESP-IDF's dynamic frequency scaling has no direct RP2350 counterpart that
 * this port uses; clock rate is fixed at boot by pico-sdk. Reported as
 * unsupported so services/solar_os_power.c takes its "no DFS" branch.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int max_freq_mhz;
    int min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;

typedef void *esp_pm_lock_handle_t;

typedef enum {
    ESP_PM_CPU_FREQ_MAX,
    ESP_PM_APB_FREQ_MAX,
    ESP_PM_NO_LIGHT_SLEEP,
} esp_pm_lock_type_t;

static inline esp_err_t esp_pm_configure(const void *config)
{ (void)config; return ESP_ERR_NOT_SUPPORTED; }

static inline esp_err_t esp_pm_lock_create(esp_pm_lock_type_t type,
                                           int arg,
                                           const char *name,
                                           esp_pm_lock_handle_t *out)
{ (void)type; (void)arg; (void)name; if (out) { *out = NULL; } return ESP_ERR_NOT_SUPPORTED; }

static inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h)
{ (void)h; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h)
{ (void)h; return ESP_ERR_NOT_SUPPORTED; }

#ifdef __cplusplus
}
#endif
