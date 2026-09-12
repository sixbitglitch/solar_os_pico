/*
 * esp_ota_ops.h - pico-sdk compatibility shim.
 *
 * OTA is not supported on this target and is listed as deferred in
 * doc/ports/picocalc.md. It would need a partition scheme (see
 * esp_partition.h) plus an RP2350 bootloader that can pick between slots;
 * pico-sdk's own answer is the UF2 bootrom, which is a different model
 * entirely.
 *
 * Reports "no OTA partition" rather than pretending an update could start.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432

typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint32_t reserv2[20];
} esp_app_desc_t;

const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition,
                                            esp_app_desc_t *app_desc);
const esp_app_desc_t *esp_app_get_description(void);

#ifdef __cplusplus
}
#endif
