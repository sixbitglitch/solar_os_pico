/*
 * nvs_flash.h - pico-sdk compatibility shim. See nvs.h for the persistence
 * caveat: the store is RAM-backed on this target.
 */
#pragma once

#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_init_partition(const char *partition_label);
esp_err_t nvs_flash_deinit(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_flash_erase_partition(const char *partition_label);

#ifdef __cplusplus
}
#endif
