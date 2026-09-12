# RP2350 PWM via pico-sdk hardware_pwm.
#
# drivers/pwm_port.c (the original, unmodified ESP-IDF-facing solar_os_pwm
# backend) is built entirely on the LEDC shim (driver/ledc.h + the
# ledc_timer_config/ledc_channel_config/... backing in
# src/port/pico/compat/solar_os_compat_pico_periph.c, which maps LEDC channels
# onto hardware_pwm slices), so it builds as-is against this target - no
# RP2350-specific replacement was needed.
set(SOLAR_OS_BOARD_PWM_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pwm_port.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_pwm
)
