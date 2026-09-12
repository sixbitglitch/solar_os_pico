# RP2350 ADC via pico-sdk hardware_adc. On RP2350B the ADC inputs are
# GPIO40-47 (ADC_BASE_PIN == 40), which live on the module's extra
# castellations rather than the PicoCalc's Pico socket.
set(SOLAR_OS_BOARD_ADC_DRIVER "pico")
list(APPEND SOLAR_OS_BOARD_PICO_LIBS
    hardware_adc
)
