# PicoCalc 320x320 ILI9488 on spi1. Backlight is not a GPIO: it is set through
# the STM32 co-processor's REG_ID_BKL over the keyboard I2C bus, so this
# fragment pulls in hardware_i2c as well as hardware_spi.
set(SOLAR_OS_BOARD_DISPLAY_DRIVER "ili9488_picocalc")
include("${CMAKE_CURRENT_LIST_DIR}/spi_pico.cmake")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/tft_ili9488_picocalc.c"
    "board/pico/solar_os_board_display_ili9488_picocalc.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_spi
    hardware_i2c
    hardware_dma
)
