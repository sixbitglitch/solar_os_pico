/*
 * driver/i2c_master.h - pico-sdk compatibility shim.
 *
 * ESP-IDF's bus/device-handle I2C API. Eleven files in this flavour include
 * it, but almost all of them only need the handle TYPES to declare fields and
 * pass them around - the actual transfers on this target happen in
 * src/drivers/pico/picocalc_keyboard.c against pico-sdk's hardware_i2c.
 *
 * The functions below are therefore declared, and implemented in
 * solar_os_compat_pico_i2c.c on top of hardware_i2c, so that any code path
 * that does reach them works rather than linking against nothing.
 *
 * KNOWN GAP: ESP-IDF models a bus as an object you add devices to, each with
 * its own address and speed. RP2350's hardware_i2c has no such concept - the
 * address is per-transfer. The shim keeps a small table mapping device handles
 * to (bus, address) so the API shape survives; it does NOT implement per-device
 * clock speeds, so a bus with devices at different speeds will run them all at
 * the bus speed. Nothing in this flavour does that (the co-processor is the
 * only device), but a future expansion device might.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;

typedef enum {
    I2C_CLK_SRC_DEFAULT = 0,
} i2c_clock_source_t;

typedef enum {
    I2C_ADDR_BIT_LEN_7 = 0,
    I2C_ADDR_BIT_LEN_10 = 1,
} i2c_addr_bit_len_t;

typedef struct {
    i2c_port_t i2c_port;
    gpio_num_t sda_io_num;
    gpio_num_t scl_io_num;
    i2c_clock_source_t clk_source;
    uint8_t glitch_ignore_cnt;
    int intr_priority;
    size_t trans_queue_depth;
    struct {
        uint32_t enable_internal_pullup: 1;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *out_handle);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle);
esp_err_t i2c_master_get_bus_handle(i2c_port_t port, i2c_master_bus_handle_t *out_handle);

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *out_handle);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms);

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t *data,
                              size_t length,
                              int timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t device,
                             uint8_t *data,
                             size_t length,
                             int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                      const uint8_t *write_data,
                                      size_t write_length,
                                      uint8_t *read_data,
                                      size_t read_length,
                                      int timeout_ms);

#ifdef __cplusplus
}
#endif
