/*
 * esp_check.h - pico-sdk compatibility shim.
 *
 * ESP-IDF's early-return-on-error macros. Semantics match ESP-IDF: evaluate
 * the expression exactly once, log and return (or goto) when it is not ESP_OK.
 */
#pragma once

#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_ERROR(x, tag, format, ...)                              \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                              \
        if (err_rc_ != ESP_OK) {                                              \
            ESP_LOGE(tag, format, ##__VA_ARGS__);                             \
            return err_rc_;                                                   \
        }                                                                     \
    } while (0)

#define ESP_RETURN_ON_ERROR_ISR(x, tag, format, ...)                          \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                              \
        if (err_rc_ != ESP_OK) {                                              \
            return err_rc_;                                                   \
        }                                                                     \
    } while (0)

#define ESP_RETURN_ON_FALSE(a, err_code, tag, format, ...)                    \
    do {                                                                      \
        if (!(a)) {                                                           \
            ESP_LOGE(tag, format, ##__VA_ARGS__);                             \
            return (err_code);                                                \
        }                                                                     \
    } while (0)

#define ESP_RETURN_VOID_ON_ERROR(x, tag, format, ...)                         \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                              \
        if (err_rc_ != ESP_OK) {                                              \
            ESP_LOGE(tag, format, ##__VA_ARGS__);                             \
            return;                                                           \
        }                                                                     \
    } while (0)

#define ESP_RETURN_VOID_ON_FALSE(a, tag, format, ...)                         \
    do {                                                                      \
        if (!(a)) {                                                           \
            ESP_LOGE(tag, format, ##__VA_ARGS__);                             \
            return;                                                           \
        }                                                                     \
    } while (0)

#define ESP_GOTO_ON_ERROR(x, goto_tag, tag, format, ...)                      \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                              \
        if (err_rc_ != ESP_OK) {                                              \
            ret = err_rc_;                                                    \
            ESP_LOGE(tag, format, ##__VA_ARGS__);                             \
            goto goto_tag;                                                    \
        }                                                                     \
    } while (0)

#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, tag, format, ...)            \
    do {                                                                      \
        if (!(a)) {                                                           \
            ret = (err_code);                                                 \
            ESP_LOGE(tag, format, ##__VA_ARGS__);                             \
            goto goto_tag;                                                    \
        }                                                                     \
    } while (0)

/* ESP_ERROR_CHECK / ESP_ERROR_CHECK_WITHOUT_ABORT live in esp_err.h, not here:
 * that matches upstream ESP-IDF's own layering, and it means a file that only
 * includes esp_log.h or esp_err.h (as several shared sources do) still gets
 * them without needing a new #include added. */
