/*
 * solar_os_compat_pico_deferred_stubs.c - link-time stand-ins for subsystems
 * this port genuinely defers: flash-backed storage (drivers/flash_storage.c),
 * the RAM filesystem (services/solar_os_ramfs.c), the USB CDC console
 * (services/solar_os_cdc.c) and NVS-to-file backup
 * (services/solar_os_nvs_backup.c). Those four .c files are excluded from the
 * build (see targets/picocalc/CMakeLists.txt SOLAR_OS_DEFERRED_SRCS) because
 * each needs real RP2350-side work - a flash/partition abstraction with wear
 * levelling, a VFS layer, pico-sdk's TinyUSB CDC stack, and the same flash
 * abstraction again - none of which exists yet. Other already-ported files
 * (main.c, solar_os_storage.c, the shell) call their public APIs directly and
 * unconditionally, so *something* has to satisfy the linker.
 *
 * Every function below reports "not available" rather than silently doing
 * nothing or looping in a fake success: callers already handle ESP_FAIL /
 * false / empty status because the same code paths run on ESP-IDF boards that
 * simply don't have flash storage, RAM disks, or a USB CDC console enabled.
 * See doc/ports/picocalc.md for what a real implementation of each would need.
 */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "flash_storage.h"
#include "solar_os_cdc.h"
#include "solar_os_nvs_backup.h"
#include "solar_os_ramfs.h"

esp_err_t flash_storage_mount(const char *mount_point)
{
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t flash_storage_unmount(void)
{
    return ESP_OK;
}

esp_err_t flash_storage_format(const char *mount_point)
{
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
}

bool flash_storage_is_mounted(void)
{
    return false;
}

const char *flash_storage_mount_point(void)
{
    return "";
}

uint8_t flash_storage_logical_volume(void)
{
    return FLASH_STORAGE_LOGICAL_VOLUME_INVALID;
}

uint64_t flash_storage_size_bytes(void)
{
    return 0;
}

void flash_storage_get_status(char *buffer, size_t len)
{
    if (buffer != NULL && len > 0) {
        snprintf(buffer, len, "not available on this target");
    }
}

esp_err_t flash_storage_get_usage(uint64_t *total_bytes,
                                  uint64_t *used_bytes,
                                  uint64_t *free_bytes)
{
    if (total_bytes != NULL) {
        *total_bytes = 0;
    }
    if (used_bytes != NULL) {
        *used_bytes = 0;
    }
    if (free_bytes != NULL) {
        *free_bytes = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_ramfs_mount(const char *mount_point, size_t quota_bytes)
{
    (void)mount_point;
    (void)quota_bytes;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_ramfs_unmount(const char *mount_point)
{
    (void)mount_point;
    return ESP_OK;
}

size_t solar_os_ramfs_mount_count(void)
{
    return 0;
}

bool solar_os_ramfs_get_info(size_t index, solar_os_ramfs_info_t *info)
{
    (void)index;
    (void)info;
    return false;
}

bool solar_os_ramfs_path_has_mount_prefix(const char *path)
{
    (void)path;
    return false;
}

esp_err_t solar_os_ramfs_path_mount_point(const char *path,
                                          char *mount_point,
                                          size_t mount_point_len)
{
    (void)path;
    if (mount_point != NULL && mount_point_len > 0) {
        mount_point[0] = '\0';
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_ramfs_get_usage_for_path(const char *path,
                                            uint64_t *total_bytes,
                                            uint64_t *used_bytes,
                                            uint64_t *free_bytes)
{
    (void)path;
    if (total_bytes != NULL) {
        *total_bytes = 0;
    }
    if (used_bytes != NULL) {
        *used_bytes = 0;
    }
    if (free_bytes != NULL) {
        *free_bytes = 0;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_cdc_init(void)
{
    /* pico-sdk's own USB CDC (TinyUSB, via pico_stdio_usb) is a separate
     * stdio backend, not this service - see doc/ports/picocalc.md. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_nvs_backup_create(const char *path,
                                     solar_os_nvs_backup_result_t *result)
{
    (void)path;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_nvs_backup_restore(const char *path,
                                      solar_os_nvs_backup_result_t *result)
{
    (void)path;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    return ESP_ERR_NOT_SUPPORTED;
}
