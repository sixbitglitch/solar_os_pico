# RP2350 SPI via pico-sdk hardware_spi.
#
# The two on-board SPI devices (ILI9488 on spi1, SD card on spi0) drive their
# peripherals directly from their own drivers. A general
# drivers/pico/spi_bus_pico.c backing solar_os_buses is deferred; see
# doc/ports/picocalc.md.
set(SOLAR_OS_BOARD_SPI_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_spi
)
