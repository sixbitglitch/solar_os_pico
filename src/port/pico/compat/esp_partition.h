/*
 * esp_partition.h - pico-sdk compatibility shim.
 *
 * RP2350 has no partition table: pico-sdk lays the firmware out as one image
 * in XIP flash. Carving a partition scheme out of the 16 MB device (which is
 * what NVS persistence and OTA would both need) is deferred work - see
 * doc/ports/picocalc.md.
 *
 * The lookup functions therefore find nothing, which is the truthful answer.
 * Callers in this tree all handle a NULL partition.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_PARTITION_TYPE_APP = 0x00,
    ESP_PARTITION_TYPE_DATA = 0x01,
    ESP_PARTITION_TYPE_ANY = 0xff,
} esp_partition_type_t;

typedef enum {
    ESP_PARTITION_SUBTYPE_DATA_OTA = 0x00,
    ESP_PARTITION_SUBTYPE_DATA_NVS = 0x02,
    ESP_PARTITION_SUBTYPE_DATA_FAT = 0x81,
    ESP_PARTITION_SUBTYPE_ANY = 0xff,
} esp_partition_subtype_t;

typedef struct {
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address;
    uint32_t size;
    uint32_t erase_size;
    char label[17];
    bool encrypted;
} esp_partition_t;

typedef struct esp_partition_iterator_opaque_t *esp_partition_iterator_t;

/* Always NULL: there is no partition table on this target. */
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label);
esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
                                            esp_partition_subtype_t subtype,
                                            const char *label);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator);
void esp_partition_iterator_release(esp_partition_iterator_t iterator);

esp_err_t esp_partition_read(const esp_partition_t *partition,
                             size_t offset,
                             void *dst,
                             size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition,
                              size_t offset,
                              const void *src,
                              size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset,
                                    size_t size);

#ifdef __cplusplus
}
#endif
