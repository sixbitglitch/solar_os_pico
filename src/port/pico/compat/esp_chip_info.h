/*
 * esp_chip_info.h - pico-sdk compatibility shim.
 *
 * Reports the RP2350 through ESP-IDF's struct so identity/`hw` shell output
 * keeps working. model is reported as an out-of-range value rather than
 * pretending to be an ESP32 part.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHIP_FEATURE_EMB_FLASH  (1 << 0)
#define CHIP_FEATURE_WIFI_BGN   (1 << 1)
#define CHIP_FEATURE_BLE        (1 << 4)
#define CHIP_FEATURE_BT         (1 << 5)
#define CHIP_FEATURE_EMB_PSRAM  (1 << 6)

typedef enum {
    CHIP_ESP32 = 1,
    CHIP_ESP32S2 = 2,
    CHIP_ESP32S3 = 9,
    /* Not an ESP part. Kept distinct so nothing mistakes it for one. */
    CHIP_RP2350 = 0x2350,
} esp_chip_model_t;

typedef struct {
    esp_chip_model_t model;
    uint32_t features;
    uint16_t revision;
    uint8_t cores;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t *out_info);

#ifdef __cplusplus
}
#endif
