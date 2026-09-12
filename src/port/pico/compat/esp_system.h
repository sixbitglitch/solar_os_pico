/*
 * esp_system.h - pico-sdk compatibility shim.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
} esp_reset_reason_t;

/* Maps RP2350's watchdog/power-on reason registers onto the ESP-IDF enum. */
esp_reset_reason_t esp_reset_reason(void);

/* Reboots via the RP2350 watchdog. Does not return. */
void esp_restart(void);

uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
const char *esp_get_idf_version(void);

#ifdef __cplusplus
}
#endif
