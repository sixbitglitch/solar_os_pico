# RP2350 PWM via pico-sdk hardware_pwm.
set(SOLAR_OS_BOARD_PWM_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_pwm
)
