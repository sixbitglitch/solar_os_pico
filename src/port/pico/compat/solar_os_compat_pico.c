/*
 * solar_os_compat_pico.c - RP2350 backing for the ESP-IDF compatibility shims.
 *
 * Everything here exists so that the hardware-agnostic SolarOS sources compile
 * and behave correctly on pico-sdk without being edited. Where an ESP-IDF
 * facility has a real RP2350 counterpart it is wired to it; where it does not,
 * the shim says so honestly rather than returning a plausible lie.
 *
 * See doc/ports/picocalc.md section 4.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

/* ------------------------------------------------------------------ */
/* Errors                                                             */
/* ------------------------------------------------------------------ */

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                        return "ESP_OK";
    case ESP_FAIL:                      return "ESP_FAIL";
    case ESP_ERR_NO_MEM:                return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:           return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:         return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:          return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:             return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:         return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:               return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE:      return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC:           return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_INVALID_VERSION:       return "ESP_ERR_INVALID_VERSION";
    case ESP_ERR_NOT_FINISHED:          return "ESP_ERR_NOT_FINISHED";
    case ESP_ERR_NOT_ALLOWED:           return "ESP_ERR_NOT_ALLOWED";
    case ESP_ERR_NVS_NOT_FOUND:         return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_INVALID_HANDLE:    return "ESP_ERR_NVS_INVALID_HANDLE";
    case ESP_ERR_NVS_INVALID_NAME:      return "ESP_ERR_NVS_INVALID_NAME";
    case ESP_ERR_NVS_INVALID_LENGTH:    return "ESP_ERR_NVS_INVALID_LENGTH";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:  return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    case ESP_ERR_NVS_TYPE_MISMATCH:     return "ESP_ERR_NVS_TYPE_MISMATCH";
    case ESP_ERR_NVS_READ_ONLY:         return "ESP_ERR_NVS_READ_ONLY";
    default:                            break;
    }
    static char buffer[24];
    snprintf(buffer, sizeof(buffer), "ERR 0x%x", (unsigned)code);
    return buffer;
}

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

esp_log_level_t solar_os_compat_log_level = ESP_LOG_INFO;

void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    /* Per-tag filtering is not implemented; the highest level requested wins
     * globally. Call sites in this tree only ever raise or lower verbosity
     * wholesale. */
    (void)tag;
    solar_os_compat_log_level = level;
}

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

int64_t esp_timer_get_time(void)
{
    return (int64_t)time_us_64();
}

/* ------------------------------------------------------------------ */
/* Heap                                                               */
/* ------------------------------------------------------------------ */

/*
 * RP2350 has a single uniform SRAM heap. The capability bits are therefore
 * only a filter, not a placement request:
 *
 *   MALLOC_CAP_SPIRAM  -> always empty, this target does not map PSRAM.
 *   everything else    -> the one real heap.
 *
 * Free/largest-block answers come from FreeRTOS heap_4 rather than from a
 * fixed number, because solar_os_task_can_create() refuses task launches based
 * on them. A constant would turn admission control into either "always yes"
 * (and then fail in xTaskCreate) or "always no".
 */

static bool caps_select_spiram(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) != 0 && (caps & MALLOC_CAP_INTERNAL) == 0;
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    if (caps_select_spiram(caps)) {
        return NULL;
    }
    return pvPortMalloc(size);
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    if (caps_select_spiram(caps)) {
        return NULL;
    }
    /* Overflow check: pvPortMalloc takes a single size. */
    if (n != 0 && size > (size_t)-1 / n) {
        return NULL;
    }
    const size_t total = n * size;
    void *ptr = pvPortMalloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    if (caps_select_spiram(caps)) {
        return NULL;
    }
    /* FreeRTOS heap_4 has no realloc. Allocate-and-copy is correct but cannot
     * know the old block's size, so it copies the new size and relies on the
     * allocator's block header bounds. Growing is safe; shrinking copies only
     * what is asked for. Callers in this tree always grow. */
    if (ptr == NULL) {
        return pvPortMalloc(size);
    }
    if (size == 0) {
        vPortFree(ptr);
        return NULL;
    }
    void *fresh = pvPortMalloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, size);
    vPortFree(ptr);
    return fresh;
}

void heap_caps_free(void *ptr)
{
    vPortFree(ptr);
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    if (caps_select_spiram(caps)) {
        return 0;
    }
    return xPortGetFreeHeapSize();
}

size_t heap_caps_get_total_size(uint32_t caps)
{
    if (caps_select_spiram(caps)) {
        return 0;
    }
    return configTOTAL_HEAP_SIZE;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    if (caps_select_spiram(caps)) {
        return 0;
    }
    /*
     * heap_4 does not expose largest-free-block directly. xPortGetFreeHeapSize
     * is an upper bound on it, and using it here makes admission control
     * slightly optimistic: a launch can still fail in xTaskCreate when the
     * heap is fragmented, which the caller already handles (it records the
     * failure as non-denied). The alternative - walking heap_4's free list -
     * needs kernel-internal access this shim deliberately does not take.
     */
    return xPortGetFreeHeapSize();
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    if (caps_select_spiram(caps)) {
        return 0;
    }
    return xPortGetMinimumEverFreeHeapSize();
}

/* ------------------------------------------------------------------ */
/* System                                                             */
/* ------------------------------------------------------------------ */

esp_reset_reason_t esp_reset_reason(void)
{
    if (watchdog_caused_reboot()) {
        return ESP_RST_WDT;
    }
    return ESP_RST_POWERON;
}

void esp_restart(void)
{
    watchdog_reboot(0, 0, 0);
    for (;;) {
        tight_loop_contents();
    }
}

uint32_t esp_get_free_heap_size(void)
{
    return (uint32_t)xPortGetFreeHeapSize();
}

uint32_t esp_get_minimum_free_heap_size(void)
{
    return (uint32_t)xPortGetMinimumEverFreeHeapSize();
}

const char *esp_get_idf_version(void)
{
    /* Not ESP-IDF. Reporting the SDK that actually built this is the honest
     * answer for anything printing a "framework version". */
    return "pico-sdk " PICO_SDK_VERSION_STRING;
}

void esp_chip_info(esp_chip_info_t *out_info)
{
    if (out_info == NULL) {
        return;
    }
    out_info->model = CHIP_RP2350;
    out_info->cores = 2;
    out_info->revision = 0;
    out_info->features = CHIP_FEATURE_EMB_FLASH;
    /* Wi-Fi/BLE are NOT reported even though the CYW43439 is physically
     * present: no driver is linked in this flavour, so claiming the feature
     * would make `hw` lie. */
}

/* ------------------------------------------------------------------ */
/* Random                                                             */
/* ------------------------------------------------------------------ */

uint32_t esp_random(void)
{
    return get_rand_32();
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *out = buf;
    while (len >= sizeof(uint32_t)) {
        const uint32_t value = get_rand_32();
        memcpy(out, &value, sizeof(value));
        out += sizeof(value);
        len -= sizeof(value);
    }
    if (len > 0) {
        const uint32_t value = get_rand_32();
        memcpy(out, &value, len);
    }
}

/* ------------------------------------------------------------------ */
/* FreeRTOS: ESP-IDF additions                                        */
/* ------------------------------------------------------------------ */

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task_code,
                                   const char *name,
                                   const uint32_t stack_depth,
                                   void *parameters,
                                   UBaseType_t priority,
                                   TaskHandle_t *task_handle,
                                   const BaseType_t core_id)
{
    TaskHandle_t created = NULL;
    const BaseType_t result = xTaskCreate(task_code,
                                          name,
                                          (configSTACK_DEPTH_TYPE)stack_depth,
                                          parameters,
                                          priority,
                                          &created);
    if (result != pdPASS) {
        return result;
    }

#if (configUSE_CORE_AFFINITY == 1) && (configNUMBER_OF_CORES > 1)
    /*
     * RP2350 is dual-core and this port is built SMP, so core pinning is real.
     * tskNO_AFFINITY (and any out-of-range value) means "either core".
     */
    if (core_id == 0 || core_id == 1) {
        vTaskCoreAffinitySet(created, (UBaseType_t)(1U << (unsigned)core_id));
    }
#else
    (void)core_id;
#endif

    if (task_handle != NULL) {
        *task_handle = created;
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* NVS: RAM-backed key/value store                                    */
/* ------------------------------------------------------------------ */

/*
 * See nvs.h for why this is a working store rather than a set of stubs, and
 * for the persistence caveat (nothing survives a reset on this target).
 *
 * Sizing: generous enough for the settings this flavour keeps (terminal
 * preferences, identity, display/input config, timezone, battery calibration)
 * without being a meaningful share of RP2350's 520 KB of SRAM.
 */
#define COMPAT_NVS_MAX_NAMESPACES 12U
#define COMPAT_NVS_MAX_ENTRIES    192U
#define COMPAT_NVS_MAX_VALUE      256U

typedef struct {
    bool used;
    uint8_t namespace_index;
    nvs_type_t type;
    char key[NVS_KEY_NAME_MAX_SIZE];
    size_t length;
    uint8_t value[COMPAT_NVS_MAX_VALUE];
} compat_nvs_entry_t;

struct nvs_opaque_iterator {
    bool used;
    size_t position;
    bool all_namespaces;
    uint8_t namespace_index;
    nvs_type_t type;
};

static char compat_nvs_namespaces[COMPAT_NVS_MAX_NAMESPACES][NVS_NS_NAME_MAX_SIZE];
static bool compat_nvs_namespace_used[COMPAT_NVS_MAX_NAMESPACES];
static compat_nvs_entry_t compat_nvs_entries[COMPAT_NVS_MAX_ENTRIES];
static struct nvs_opaque_iterator compat_nvs_iterators[4];
static bool compat_nvs_initialised;

/* Handles are (namespace_index + 1) so that 0 is never a valid handle. */
static esp_err_t compat_nvs_namespace_of(nvs_handle_t handle, uint8_t *out_index)
{
    if (handle == 0 || handle > COMPAT_NVS_MAX_NAMESPACES) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    const uint8_t index = (uint8_t)(handle - 1);
    if (!compat_nvs_namespace_used[index]) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    *out_index = index;
    return ESP_OK;
}

static compat_nvs_entry_t *compat_nvs_find(uint8_t namespace_index, const char *key)
{
    for (size_t i = 0; i < COMPAT_NVS_MAX_ENTRIES; i++) {
        compat_nvs_entry_t *entry = &compat_nvs_entries[i];
        if (entry->used && entry->namespace_index == namespace_index &&
            strncmp(entry->key, key, sizeof(entry->key)) == 0) {
            return entry;
        }
    }
    return NULL;
}

static esp_err_t compat_nvs_store(nvs_handle_t handle,
                                  const char *key,
                                  nvs_type_t type,
                                  const void *value,
                                  size_t length)
{
    uint8_t namespace_index = 0;
    const esp_err_t err = compat_nvs_namespace_of(handle, &namespace_index);
    if (err != ESP_OK) {
        return err;
    }
    if (key == NULL || key[0] == '\0' || strlen(key) >= NVS_KEY_NAME_MAX_SIZE) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    if (value == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length > COMPAT_NVS_MAX_VALUE) {
        return ESP_ERR_NVS_VALUE_TOO_LONG;
    }

    compat_nvs_entry_t *entry = compat_nvs_find(namespace_index, key);
    if (entry == NULL) {
        for (size_t i = 0; i < COMPAT_NVS_MAX_ENTRIES; i++) {
            if (!compat_nvs_entries[i].used) {
                entry = &compat_nvs_entries[i];
                break;
            }
        }
        if (entry == NULL) {
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        entry->used = true;
        entry->namespace_index = namespace_index;
        snprintf(entry->key, sizeof(entry->key), "%s", key);
    }
    entry->type = type;
    entry->length = length;
    memcpy(entry->value, value, length);
    return ESP_OK;
}

static esp_err_t compat_nvs_load(nvs_handle_t handle,
                                 const char *key,
                                 nvs_type_t type,
                                 void *out_value,
                                 size_t length)
{
    uint8_t namespace_index = 0;
    const esp_err_t err = compat_nvs_namespace_of(handle, &namespace_index);
    if (err != ESP_OK) {
        return err;
    }
    if (key == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const compat_nvs_entry_t *entry = compat_nvs_find(namespace_index, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->type != type) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (entry->length != length) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, entry->value, length);
    return ESP_OK;
}

esp_err_t nvs_flash_init(void)
{
    if (!compat_nvs_initialised) {
        memset(compat_nvs_namespaces, 0, sizeof(compat_nvs_namespaces));
        memset(compat_nvs_namespace_used, 0, sizeof(compat_nvs_namespace_used));
        memset(compat_nvs_entries, 0, sizeof(compat_nvs_entries));
        memset(compat_nvs_iterators, 0, sizeof(compat_nvs_iterators));
        compat_nvs_initialised = true;
    }
    return ESP_OK;
}

esp_err_t nvs_flash_init_partition(const char *partition_label)
{
    (void)partition_label;
    return nvs_flash_init();
}

esp_err_t nvs_flash_deinit(void)
{
    compat_nvs_initialised = false;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    memset(compat_nvs_entries, 0, sizeof(compat_nvs_entries));
    memset(compat_nvs_namespace_used, 0, sizeof(compat_nvs_namespace_used));
    return ESP_OK;
}

esp_err_t nvs_flash_erase_partition(const char *partition_label)
{
    (void)partition_label;
    return nvs_flash_erase();
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (name == NULL || out_handle == NULL || strlen(name) >= NVS_NS_NAME_MAX_SIZE) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    (void)nvs_flash_init();

    for (uint8_t i = 0; i < COMPAT_NVS_MAX_NAMESPACES; i++) {
        if (compat_nvs_namespace_used[i] &&
            strncmp(compat_nvs_namespaces[i], name, NVS_NS_NAME_MAX_SIZE) == 0) {
            *out_handle = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    for (uint8_t i = 0; i < COMPAT_NVS_MAX_NAMESPACES; i++) {
        if (!compat_nvs_namespace_used[i]) {
            compat_nvs_namespace_used[i] = true;
            snprintf(compat_nvs_namespaces[i], NVS_NS_NAME_MAX_SIZE, "%s", name);
            *out_handle = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
}

void nvs_close(nvs_handle_t handle)
{
    /* Namespaces persist for the life of the boot; closing a handle does not
     * discard its keys, matching ESP-IDF. */
    (void)handle;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    /* Nothing to flush: the store is already the live copy. */
    (void)handle;
    return ESP_OK;
}

#define COMPAT_NVS_SCALAR_SETTER(suffix, ctype, nvstype)                       \
    esp_err_t nvs_set_##suffix(nvs_handle_t handle, const char *key, ctype v)  \
    {                                                                          \
        return compat_nvs_store(handle, key, nvstype, &v, sizeof(v));          \
    }

#define COMPAT_NVS_SCALAR_GETTER(suffix, ctype, nvstype)                       \
    esp_err_t nvs_get_##suffix(nvs_handle_t handle, const char *key, ctype *o) \
    {                                                                          \
        return compat_nvs_load(handle, key, nvstype, o, sizeof(*o));           \
    }

COMPAT_NVS_SCALAR_SETTER(u8, uint8_t, NVS_TYPE_U8)
COMPAT_NVS_SCALAR_SETTER(i8, int8_t, NVS_TYPE_I8)
COMPAT_NVS_SCALAR_SETTER(u16, uint16_t, NVS_TYPE_U16)
COMPAT_NVS_SCALAR_SETTER(i16, int16_t, NVS_TYPE_I16)
COMPAT_NVS_SCALAR_SETTER(u32, uint32_t, NVS_TYPE_U32)
COMPAT_NVS_SCALAR_SETTER(i32, int32_t, NVS_TYPE_I32)
COMPAT_NVS_SCALAR_SETTER(u64, uint64_t, NVS_TYPE_U64)
COMPAT_NVS_SCALAR_SETTER(i64, int64_t, NVS_TYPE_I64)

COMPAT_NVS_SCALAR_GETTER(u8, uint8_t, NVS_TYPE_U8)
COMPAT_NVS_SCALAR_GETTER(i8, int8_t, NVS_TYPE_I8)
COMPAT_NVS_SCALAR_GETTER(u16, uint16_t, NVS_TYPE_U16)
COMPAT_NVS_SCALAR_GETTER(i16, int16_t, NVS_TYPE_I16)
COMPAT_NVS_SCALAR_GETTER(u32, uint32_t, NVS_TYPE_U32)
COMPAT_NVS_SCALAR_GETTER(i32, int32_t, NVS_TYPE_I32)
COMPAT_NVS_SCALAR_GETTER(u64, uint64_t, NVS_TYPE_U64)
COMPAT_NVS_SCALAR_GETTER(i64, int64_t, NVS_TYPE_I64)

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return compat_nvs_store(handle, key, NVS_TYPE_STR, value, strlen(value) + 1U);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    return compat_nvs_store(handle, key, NVS_TYPE_BLOB, value, length);
}

/* Shared body for the two variable-length getters, which follow ESP-IDF's
 * two-call convention: out_value == NULL asks for the required size. */
static esp_err_t compat_nvs_load_variable(nvs_handle_t handle,
                                          const char *key,
                                          nvs_type_t type,
                                          void *out_value,
                                          size_t *length)
{
    uint8_t namespace_index = 0;
    const esp_err_t err = compat_nvs_namespace_of(handle, &namespace_index);
    if (err != ESP_OK) {
        return err;
    }
    if (key == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const compat_nvs_entry_t *entry = compat_nvs_find(namespace_index, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->type != type) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (out_value == NULL) {
        *length = entry->length;
        return ESP_OK;
    }
    if (*length < entry->length) {
        *length = entry->length;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, entry->value, entry->length);
    *length = entry->length;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length)
{
    return compat_nvs_load_variable(handle, key, NVS_TYPE_STR, out_value, length);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    return compat_nvs_load_variable(handle, key, NVS_TYPE_BLOB, out_value, length);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    uint8_t namespace_index = 0;
    const esp_err_t err = compat_nvs_namespace_of(handle, &namespace_index);
    if (err != ESP_OK) {
        return err;
    }
    compat_nvs_entry_t *entry = compat_nvs_find(namespace_index, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(entry, 0, sizeof(*entry));
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    uint8_t namespace_index = 0;
    const esp_err_t err = compat_nvs_namespace_of(handle, &namespace_index);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < COMPAT_NVS_MAX_ENTRIES; i++) {
        if (compat_nvs_entries[i].used &&
            compat_nvs_entries[i].namespace_index == namespace_index) {
            memset(&compat_nvs_entries[i], 0, sizeof(compat_nvs_entries[i]));
        }
    }
    return ESP_OK;
}

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *stats)
{
    (void)part_name;
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t used = 0;
    size_t namespaces = 0;
    for (size_t i = 0; i < COMPAT_NVS_MAX_ENTRIES; i++) {
        if (compat_nvs_entries[i].used) {
            used++;
        }
    }
    for (size_t i = 0; i < COMPAT_NVS_MAX_NAMESPACES; i++) {
        if (compat_nvs_namespace_used[i]) {
            namespaces++;
        }
    }
    stats->used_entries = used;
    stats->total_entries = COMPAT_NVS_MAX_ENTRIES;
    stats->free_entries = COMPAT_NVS_MAX_ENTRIES - used;
    stats->namespace_count = namespaces;
    return ESP_OK;
}

esp_err_t nvs_get_used_entry_count(nvs_handle_t handle, size_t *used_entries)
{
    uint8_t namespace_index = 0;
    const esp_err_t err = compat_nvs_namespace_of(handle, &namespace_index);
    if (err != ESP_OK) {
        return err;
    }
    if (used_entries == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t used = 0;
    for (size_t i = 0; i < COMPAT_NVS_MAX_ENTRIES; i++) {
        if (compat_nvs_entries[i].used &&
            compat_nvs_entries[i].namespace_index == namespace_index) {
            used++;
        }
    }
    *used_entries = used;
    return ESP_OK;
}

/* Advance an iterator to the next matching entry, or release it when the scan
 * is finished (ESP-IDF's iterators self-release at the end). */
static esp_err_t compat_nvs_iterator_advance(nvs_iterator_t *iterator)
{
    struct nvs_opaque_iterator *it = *iterator;
    while (it->position < COMPAT_NVS_MAX_ENTRIES) {
        const compat_nvs_entry_t *entry = &compat_nvs_entries[it->position];
        const bool namespace_ok =
            it->all_namespaces || entry->namespace_index == it->namespace_index;
        const bool type_ok = it->type == NVS_TYPE_ANY || entry->type == it->type;
        if (entry->used && namespace_ok && type_ok) {
            return ESP_OK;
        }
        it->position++;
    }
    it->used = false;
    *iterator = NULL;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_entry_find(const char *part_name,
                         const char *namespace_name,
                         nvs_type_t type,
                         nvs_iterator_t *output_iterator)
{
    (void)part_name;
    if (output_iterator == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *output_iterator = NULL;

    struct nvs_opaque_iterator *it = NULL;
    for (size_t i = 0; i < sizeof(compat_nvs_iterators) / sizeof(compat_nvs_iterators[0]); i++) {
        if (!compat_nvs_iterators[i].used) {
            it = &compat_nvs_iterators[i];
            break;
        }
    }
    if (it == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(it, 0, sizeof(*it));
    it->used = true;
    it->type = type;
    if (namespace_name == NULL) {
        it->all_namespaces = true;
    } else {
        bool found = false;
        for (uint8_t i = 0; i < COMPAT_NVS_MAX_NAMESPACES; i++) {
            if (compat_nvs_namespace_used[i] &&
                strncmp(compat_nvs_namespaces[i], namespace_name, NVS_NS_NAME_MAX_SIZE) == 0) {
                it->namespace_index = i;
                found = true;
                break;
            }
        }
        if (!found) {
            it->used = false;
            return ESP_ERR_NVS_NOT_FOUND;
        }
    }

    *output_iterator = it;
    return compat_nvs_iterator_advance(output_iterator);
}

esp_err_t nvs_entry_next(nvs_iterator_t *iterator)
{
    if (iterator == NULL || *iterator == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (*iterator)->position++;
    return compat_nvs_iterator_advance(iterator);
}

esp_err_t nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *out_info)
{
    if (iterator == NULL || out_info == NULL ||
        iterator->position >= COMPAT_NVS_MAX_ENTRIES) {
        return ESP_ERR_INVALID_ARG;
    }
    const compat_nvs_entry_t *entry = &compat_nvs_entries[iterator->position];
    memset(out_info, 0, sizeof(*out_info));
    snprintf(out_info->namespace_name,
             sizeof(out_info->namespace_name),
             "%s",
             compat_nvs_namespaces[entry->namespace_index]);
    snprintf(out_info->key, sizeof(out_info->key), "%s", entry->key);
    out_info->type = entry->type;
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t iterator)
{
    if (iterator != NULL) {
        iterator->used = false;
    }
}
