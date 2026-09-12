# RP2350 SPI via pico-sdk hardware_spi.
#
# The ILI9488 display on spi1 drives its peripheral directly and is not part
# of this generic path. SPI0 is shared with the SD card
# (src/drivers/pico/sd_spi_pico.c): both it and drivers/pico/spi_bus_pico.c
# (the RP2350 backend for drivers/spi_bus.c, the original, unmodified
# ESP-IDF-facing solar_os_buses SPI backend built here) take the same mutex
# around every chip-select-asserted transfer - see spi_bus_pico.c's file
# header and doc/ports/picocalc.md.
set(SOLAR_OS_BOARD_SPI_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/spi_bus.c"
    "drivers/pico/spi_bus_pico.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_spi
)
