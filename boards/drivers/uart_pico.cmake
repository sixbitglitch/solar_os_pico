# RP2350 UART via pico-sdk hardware_uart.
set(SOLAR_OS_BOARD_UART_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_uart
)
