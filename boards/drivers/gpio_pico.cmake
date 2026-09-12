# RP2350 GPIO via pico-sdk hardware_gpio.
#
# The ESP-IDF GPIO API the shared sources call (gpio_config/gpio_set_level/...)
# is mapped onto pico-sdk in the compatibility shim,
# src/port/pico/compat/solar_os_compat_pico_gpio.c. drivers/gpio_port.c (the
# original, unmodified ESP-IDF-facing solar_os_gpio backend) only calls that
# same API plus the GPIO_IS_VALID_GPIO/GPIO_IS_VALID_OUTPUT_GPIO macros (added
# to the compat driver/gpio.h shim), so it builds as-is against this target -
# no RP2350-specific replacement was needed.
set(SOLAR_OS_BOARD_GPIO_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/gpio_port.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_gpio
)
