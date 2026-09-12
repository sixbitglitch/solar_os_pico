# RP2350 SPI via pico-sdk hardware_spi.
set(SOLAR_OS_BOARD_SPI_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/spi_bus_pico.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_spi
)
