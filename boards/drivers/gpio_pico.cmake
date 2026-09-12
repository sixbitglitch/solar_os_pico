# RP2350 GPIO via pico-sdk hardware_gpio.
set(SOLAR_OS_BOARD_GPIO_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/gpio_port_pico.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_gpio
)
