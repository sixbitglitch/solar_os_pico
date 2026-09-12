# PicoCalc 320x320 ILI9488 on spi1. Backlight is not a GPIO: it is set through
# the STM32 co-processor's REG_ID_BKL over the keyboard I2C bus, so this
# fragment pulls in hardware_i2c as well as hardware_spi.
set(SOLAR_OS_BOARD_DISPLAY_DRIVER "ili9488_picocalc")
include("${CMAKE_CURRENT_LIST_DIR}/spi_pico.cmake")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/tft_ili9488_picocalc.c"
    # This board's display is fixed/built-in (soldered to the mainboard, like
    # the keyboard), not a pluggable expansion module, so it follows the same
    # pattern as solar_os_board_battery_adc.c/solar_os_board_storage_sd.c:
    # one file implementing the solar_os_board_display.h contract directly.
    # board/solar_os_board_display_expansion.c (the ESP32-side "possibly-
    # pluggable TFT" registry indirection - register_primary()/a getter-style
    # solar_os_board_display_init()) is deliberately NOT included: this file
    # already defines solar_os_board_display_init() itself, and linking both
    # is a duplicate-symbol error.
    "board/pico/solar_os_board_display_ili9488_picocalc.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_spi
    hardware_i2c
    hardware_dma
)
