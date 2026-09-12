# RP2350 I2C via pico-sdk hardware_i2c.
set(SOLAR_OS_BOARD_I2C_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/i2c_bus_pico.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_i2c
)
