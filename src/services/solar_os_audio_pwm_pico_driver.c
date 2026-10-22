#include "solar_os_audio_pwm.h"

/*
 * PicoCalc board manifests attach "audio-pwm-pico". The ESP LEDC package uses
 * the sibling "audio-pwm" name; keep the C driver names aligned with the
 * expansion_drivers.toml keys so board defaults actually attach.
 */
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "pwm", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_PWM, .role = "pwm", .required = true},
};

const solar_os_expansion_driver_t solar_os_audio_pwm_expansion_driver = {
    .name = "audio-pwm-pico",
    .category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    .summary = "Pico PWM mono audio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_PWM,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_audio_pwm_attach,
    .detach = solar_os_audio_pwm_detach,
};
