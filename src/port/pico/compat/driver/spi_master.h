/*
 * driver/spi_master.h - pico-sdk compatibility shim.
 *
 * SolarOS's SPI plumbing goes through src/services/solar_os_buses.c and the
 * board SPI driver, not through ESP-IDF's spi_master API directly; the shared
 * sources only need the host enum and an opaque device handle to name things.
 * The actual transfers on this target are done by
 * src/drivers/pico/spi_bus_pico.c against pico-sdk's hardware_spi.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RP2350 has two SPI blocks. Named with the ESP-IDF spellings so shared
 * sources that print or switch on a host identifier keep compiling. */
typedef enum {
    SPI1_HOST = 0,
    SPI2_HOST = 1,
    SPI3_HOST = 2,
    SPI_HOST_MAX = 3,
} spi_host_device_t;

typedef struct spi_device_t *spi_device_handle_t;

#ifdef __cplusplus
}
#endif
