# RP2350 UART via pico-sdk hardware_uart.
#
# drivers/uart_port.c (the original, ESP-IDF-facing solar_os_uart backend) is
# built on ESP-IDF's ring-buffered UART driver (uart_driver_install/
# uart_detect_bitrate_start), which has no RP2350 equivalent. drivers/pico/
# uart_port_pico.c implements the same solar_os_uart backend contract
# directly against hardware_uart with its own IRQ-driven RX ring buffer.
# Autobaud detection is NOT implemented - see that file's header comment and
# doc/ports/picocalc.md.
set(SOLAR_OS_BOARD_UART_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/uart_port_pico.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_uart
)
