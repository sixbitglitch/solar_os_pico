#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

/* Matches PICOCALC_KBD_I2C_ADDRESS; duplicated here so the expansion binding
 * spec does not have to pull in the pico-sdk-dependent driver header. */
#define SOLAR_OS_PICOCALC_KEYBOARD_ADDRESS 0x1fU

esp_err_t solar_os_picocalc_keyboard_attach(const char *name,
                                            const solar_os_expansion_binding_t *bindings,
                                            size_t binding_count);
esp_err_t solar_os_picocalc_keyboard_detach(const char *name);

/*
 * True once a co-processor has been attached and answered. The display
 * driver's brightness op and the battery driver both ride the same I2C link,
 * so they check this before issuing a register access.
 */
bool solar_os_picocalc_keyboard_available(void);
