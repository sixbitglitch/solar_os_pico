/*
 * esp_log.h - pico-sdk compatibility shim.
 *
 * ESP-IDF's logging macros, reimplemented on printf. pico-sdk routes stdout to
 * whichever stdio driver the target enables (UART0 and/or USB CDC), so these
 * land wherever pico_enable_stdio_* points.
 *
 * The format is deliberately close to ESP-IDF's ("I (ms) tag: message") so
 * that log output and any log-scraping stay recognisable across the two
 * targets.
 */
#pragma once

#include <stdio.h>

#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

/* Runtime level filter. Compile-time filtering is not implemented: the
 * flash budget on a 16 MB module does not make it worth the divergence. */
extern esp_log_level_t solar_os_compat_log_level;

void esp_log_level_set(const char *tag, esp_log_level_t level);

#define SOLAR_OS_COMPAT_LOG(level, letter, tag, fmt, ...)                     \
    do {                                                                      \
        if ((level) <= solar_os_compat_log_level) {                           \
            printf("%c (%lu) %s: " fmt "\n",                                  \
                   (letter),                                                  \
                   (unsigned long)(esp_timer_get_time() / 1000),              \
                   (tag),                                                     \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

#define ESP_LOGE(tag, fmt, ...) \
    SOLAR_OS_COMPAT_LOG(ESP_LOG_ERROR, 'E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) \
    SOLAR_OS_COMPAT_LOG(ESP_LOG_WARN, 'W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) \
    SOLAR_OS_COMPAT_LOG(ESP_LOG_INFO, 'I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) \
    SOLAR_OS_COMPAT_LOG(ESP_LOG_DEBUG, 'D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) \
    SOLAR_OS_COMPAT_LOG(ESP_LOG_VERBOSE, 'V', tag, fmt, ##__VA_ARGS__)

#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
#define ESP_EARLY_LOGD ESP_LOGD
#define ESP_EARLY_LOGV ESP_LOGV

#define ESP_DRAM_LOGE ESP_LOGE
#define ESP_DRAM_LOGW ESP_LOGW
#define ESP_DRAM_LOGI ESP_LOGI

#ifdef __cplusplus
}
#endif
