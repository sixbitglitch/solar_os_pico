# SD card over SPI plus elm-chan FatFs. Deliberately NOT the expansion_sdspi
# package, whose drivers/sd_card.c is ESP-IDF sdspi/FATFS glue.
set(SOLAR_OS_BOARD_STORAGE_DRIVER "sd_spi_pico")
include("${CMAKE_CURRENT_LIST_DIR}/spi_pico.cmake")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/sd_spi_pico.c"
    "board/pico/solar_os_board_storage_sd_pico.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_spi
)
set(SOLAR_OS_BOARD_NEEDS_FATFS ON)
