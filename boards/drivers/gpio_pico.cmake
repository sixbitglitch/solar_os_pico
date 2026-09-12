# RP2350 GPIO via pico-sdk hardware_gpio.
#
# No source of its own yet: the ESP-IDF GPIO API that the shared sources call
# (gpio_config/gpio_set_level/...) is mapped onto pico-sdk in the compatibility
# shim, src/port/pico/compat/solar_os_compat_pico_gpio.c. A dedicated
# drivers/pico/gpio_port_pico.c implementing the solar_os_gpio service's
# backend is still outstanding - see doc/ports/picocalc.md.
set(SOLAR_OS_BOARD_GPIO_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_gpio
)
