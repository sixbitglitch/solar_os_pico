# RP2350 I2C via pico-sdk hardware_i2c.
#
# I2C1 is shared: the STM32 co-processor (src/drivers/pico/
# picocalc_keyboard.c) is the physical bus owner, but it now goes through
# solar_os_bus_acquire()/i2c_bus_start_config() to bring the peripheral up
# (see solar_os_picocalc_keyboard_attach()) instead of calling i2c_init()
# itself, and locks with i2c_bus_lock()/unlock() around each exchange - the
# same mutex drivers/i2c_bus.c's own i2c_bus_transmit_handle()/
# receive_handle() take. That is what makes it safe to also build
# drivers/i2c_bus.c (the original, unmodified ESP-IDF-facing solar_os_buses
# backend) here: a generic `i2c`/expansion-device transaction cannot land in
# the middle of a keyboard exchange, or vice versa. See
# doc/ports/picocalc.md.
set(SOLAR_OS_BOARD_I2C_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/i2c_bus.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_i2c
)
