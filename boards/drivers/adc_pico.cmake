# RP2350 ADC via pico-sdk hardware_adc. On RP2350B the ADC inputs are
# GPIO40-47 (ADC_BASE_PIN == 40), which live on the module's extra
# castellations rather than the PicoCalc's Pico socket.
#
# drivers/adc_port.c (the original, ESP-IDF-facing solar_os_adc backend) is
# built on ESP-IDF's esp_adc oneshot + calibration API, which nothing else in
# this port shims - the surface is too large to be worth emulating for one
# caller. drivers/pico/adc_port_pico.c implements the same solar_os_adc
# backend contract directly against hardware_adc instead.
set(SOLAR_OS_BOARD_ADC_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pico/adc_port_pico.c"
)
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_adc
)
