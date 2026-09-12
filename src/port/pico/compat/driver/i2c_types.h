/*
 * driver/i2c_types.h - pico-sdk compatibility shim.
 *
 * The generated board profile names an I2C port numerically for this target
 * (SOLAR_OS_BOARD_I2C_PORT = 1 -> RP2350's i2c1), so only the port type and
 * the two port constants are needed.
 */
#pragma once

typedef int i2c_port_t;

#define I2C_NUM_0 0
#define I2C_NUM_1 1
#define I2C_NUM_MAX 2
