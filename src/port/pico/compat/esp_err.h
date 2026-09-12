/*
 * esp_err.h - pico-sdk compatibility shim.
 *
 * Mirrors the subset of ESP-IDF's error type used by the hardware-agnostic
 * SolarOS sources. Values match ESP-IDF's so that any code comparing against
 * a literal (there should be none, but there is a lot of code here) behaves
 * identically. See doc/ports/picocalc.md section 4.
 */
#pragma once

#include <inttypes.h>   /* PRIu64 and friends: ESP-IDF headers
                           pull these in transitively, and the shared
                           sources rely on that. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        -1

#define ESP_ERR_NO_MEM              0x101
#define ESP_ERR_INVALID_ARG         0x102
#define ESP_ERR_INVALID_STATE       0x103
#define ESP_ERR_INVALID_SIZE        0x104
#define ESP_ERR_NOT_FOUND           0x105
#define ESP_ERR_NOT_SUPPORTED       0x106
#define ESP_ERR_TIMEOUT             0x107
#define ESP_ERR_INVALID_RESPONSE    0x108
#define ESP_ERR_INVALID_CRC         0x109
#define ESP_ERR_INVALID_VERSION     0x10A
#define ESP_ERR_INVALID_MAC         0x10B
#define ESP_ERR_NOT_FINISHED        0x10C
#define ESP_ERR_NOT_ALLOWED         0x10D

#define ESP_ERR_NVS_BASE            0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND       (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH   (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY       (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_NAME    (ESP_ERR_NVS_BASE + 0x06)
#define ESP_ERR_NVS_INVALID_HANDLE  (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_REMOVE_FAILED   (ESP_ERR_NVS_BASE + 0x08)
#define ESP_ERR_NVS_KEY_TOO_LONG    (ESP_ERR_NVS_BASE + 0x09)
#define ESP_ERR_NVS_PAGE_FULL       (ESP_ERR_NVS_BASE + 0x0a)
#define ESP_ERR_NVS_INVALID_STATE   (ESP_ERR_NVS_BASE + 0x0b)
#define ESP_ERR_NVS_INVALID_LENGTH  (ESP_ERR_NVS_BASE + 0x0c)
#define ESP_ERR_NVS_NO_FREE_PAGES   (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_VALUE_TOO_LONG  (ESP_ERR_NVS_BASE + 0x0e)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)

#define ESP_ERR_FLASH_BASE          0x6000
#define ESP_ERR_FLASH_OP_FAIL       (ESP_ERR_FLASH_BASE + 1)
#define ESP_ERR_FLASH_OP_TIMEOUT    (ESP_ERR_FLASH_BASE + 2)

/* Human-readable name for an esp_err_t. Returns a pointer to static storage
 * for known codes and to a per-call static buffer otherwise, matching
 * ESP-IDF's contract closely enough for the logging call sites. */
const char *esp_err_to_name(esp_err_t code);

#ifdef __cplusplus
}
#endif

/* ESP_ERROR_CHECK lives here rather than in esp_check.h to match upstream
 * ESP-IDF's own layering: it is defined in esp_err.h there too, so any file
 * that includes only esp_err.h (or esp_log.h, which reaches this file via
 * esp_timer.h) gets it without needing a separate #include added - main.c is
 * exactly such a file. Include order below is deliberate: esp_log.h is
 * pulled in only after esp_err_t and the ESP_ERR_* codes above are already
 * defined, so the circular esp_log.h -> esp_timer.h -> esp_err.h include
 * resolves cleanly via the pragma-once guards. */
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF aborts on failure here. Doing the same on RP2350 would reboot the
 * device with no diagnostics, so this logs loudly and continues. Call sites in
 * this tree use it for init steps that are already tolerant of failure. */
#define ESP_ERROR_CHECK(x)                                                    \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                             \
        if (err_rc_ != ESP_OK) {                                             \
            ESP_LOGE("check",                                                \
                     "%s:%d %s failed: %s",                                  \
                     __FILE__,                                               \
                     __LINE__,                                               \
                     #x,                                                     \
                     esp_err_to_name(err_rc_));                              \
        }                                                                     \
    } while (0)

#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) (x)

#ifdef __cplusplus
}
#endif
