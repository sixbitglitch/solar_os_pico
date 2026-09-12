# Battery state is read from the STM32 co-processor (REG_ID_BAT), not from an
# RP2350 ADC channel, so this does not include adc_pico.cmake.
set(SOLAR_OS_BOARD_BATTERY_DRIVER "picocalc")
include("${CMAKE_CURRENT_LIST_DIR}/i2c_pico.cmake")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/battery_picocalc.c"
    "board/pico/solar_os_board_battery_picocalc.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_i2c
)
