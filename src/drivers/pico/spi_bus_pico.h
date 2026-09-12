/*
 * spi_bus_pico.h - shared-bus lock for RP2350 SPI, exported for
 * sd_spi_pico.c.
 *
 * See spi_bus_pico.c's file header for why this exists: SPI0 carries both
 * the SD card (sd_spi_pico.c, direct hardware_spi access) and the generic
 * ESP-IDF-shaped solar_os_buses/"spi" shell command path (this file, backing
 * src/drivers/spi_bus.c). solar_os_pico_spi0_lock()/unlock() is the single
 * mutex both sides take around an actual chip-select-asserted transfer, so
 * one cannot land in the middle of the other's transaction.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void solar_os_pico_spi0_lock(void);
void solar_os_pico_spi0_unlock(void);

#ifdef __cplusplus
}
#endif
