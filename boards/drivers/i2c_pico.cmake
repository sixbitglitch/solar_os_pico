# RP2350 I2C via pico-sdk hardware_i2c.
#
# The PicoCalc's only I2C device is the STM32 co-processor, whose driver
# (src/drivers/pico/picocalc_keyboard.c) owns the peripheral directly. A
# general drivers/pico/i2c_bus_pico.c backing the solar_os_buses abstraction -
# which is what `i2c` shell commands and arbitrary expansion devices need - is
# deferred; see doc/ports/picocalc.md.
set(SOLAR_OS_BOARD_I2C_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_i2c
)
