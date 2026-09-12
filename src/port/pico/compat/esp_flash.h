/*
 * esp_flash.h - pico-sdk compatibility shim.
 *
 * Only the chip-size query is used (main.c reports it at boot). RP2350 has no
 * equivalent runtime query - flash size is a build-time property of the board
 * - so this reports the Pico Plus 2 W's 16 MB via PICO_FLASH_SIZE_BYTES.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_flash_t esp_flash_t;
extern esp_flash_t *esp_flash_default_chip;

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size);

#ifdef __cplusplus
}
#endif
