/*
 * esp_netif_sntp.h - pico-sdk compatibility shim.
 *
 * SNTP needs a network stack. The net package (CYW43439 + lwIP) is deferred
 * for this pass, so time sync is reported unsupported and
 * services/solar_os_time.c falls back to its manual/RTC path.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *server;
} esp_sntp_config_t;

static inline esp_err_t esp_netif_sntp_init(const void *config)
{ (void)config; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t esp_netif_sntp_sync_wait(uint32_t ticks)
{ (void)ticks; return ESP_ERR_NOT_SUPPORTED; }
static inline void esp_netif_sntp_deinit(void) { }

#ifdef __cplusplus
}
#endif
