/*
 * nvs.h - pico-sdk compatibility shim.
 *
 * ESP-IDF's non-volatile key/value store. Thirteen files in this flavour use
 * it (terminal preferences, identity, display settings, input mapping, time
 * zone, log config, battery calibration, ...), so leaving it out was not an
 * option.
 *
 * IMPORTANT - READ BEFORE TRUSTING PERSISTENCE:
 * The backing store here is RAM, not flash. Reads and writes are fully
 * functional and consistent *within a boot*, so everything layered on NVS
 * behaves correctly while running; nothing persists across a reset. Writing a
 * real flash-backed store on RP2350 (pico-sdk hardware_flash, with a
 * wear-levelled region carved out of the 16 MB device below the firmware)
 * is listed as deferred work in doc/ports/picocalc.md.
 *
 * This is deliberately a working RAM store rather than a set of
 * ESP_ERR_NVS_NOT_FOUND stubs: stubs would make every settings read fail and
 * send a dozen services down error paths that have never been exercised,
 * which hides real problems behind a fake one.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_DEFAULT_PART_NAME "nvs"
#define NVS_KEY_NAME_MAX_SIZE 16
#define NVS_NS_NAME_MAX_SIZE 16

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

typedef enum {
    NVS_TYPE_U8 = 0x01,
    NVS_TYPE_I8 = 0x11,
    NVS_TYPE_U16 = 0x02,
    NVS_TYPE_I16 = 0x12,
    NVS_TYPE_U32 = 0x04,
    NVS_TYPE_I32 = 0x14,
    NVS_TYPE_U64 = 0x08,
    NVS_TYPE_I64 = 0x18,
    NVS_TYPE_STR = 0x21,
    NVS_TYPE_BLOB = 0x42,
    NVS_TYPE_ANY = 0xff,
} nvs_type_t;

typedef struct {
    size_t used_entries;
    size_t free_entries;
    /* ESP-IDF distinguishes "free" (not written) from "available" (free minus
     * the entries reserved for internal bookkeeping). The RAM-backed store has
     * no such reservation, so the two are equal here. */
    size_t available_entries;
    size_t total_entries;
    size_t namespace_count;
} nvs_stats_t;

typedef struct {
    char namespace_name[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
} nvs_entry_info_t;

struct nvs_opaque_iterator;
typedef struct nvs_opaque_iterator *nvs_iterator_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_set_i8(nvs_handle_t handle, const char *key, int8_t value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
esp_err_t nvs_set_i16(nvs_handle_t handle, const char *key, int16_t value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value);
esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value);
esp_err_t nvs_set_i64(nvs_handle_t handle, const char *key, int64_t value);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_get_i8(nvs_handle_t handle, const char *key, int8_t *out_value);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value);
esp_err_t nvs_get_i16(nvs_handle_t handle, const char *key, int16_t *out_value);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value);
esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *out_value);
esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *out_value);
esp_err_t nvs_get_i64(nvs_handle_t handle, const char *key, int64_t *out_value);
/* Both follow ESP-IDF's two-call convention: pass out_value == NULL to learn
 * the required length, then call again with a buffer. */
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);

esp_err_t nvs_find_key(nvs_handle_t handle, const char *key, nvs_type_t *out_type);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t handle);

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *stats);
esp_err_t nvs_get_used_entry_count(nvs_handle_t handle, size_t *used_entries);

esp_err_t nvs_entry_find(const char *part_name,
                         const char *namespace_name,
                         nvs_type_t type,
                         nvs_iterator_t *output_iterator);
esp_err_t nvs_entry_next(nvs_iterator_t *iterator);
esp_err_t nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *out_info);
void nvs_release_iterator(nvs_iterator_t iterator);

#ifdef __cplusplus
}
#endif
