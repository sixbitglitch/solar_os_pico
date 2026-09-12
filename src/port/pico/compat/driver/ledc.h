/*
 * driver/ledc.h - pico-sdk compatibility shim.
 *
 * ESP-IDF's LED PWM controller. RP2350's equivalent is hardware_pwm, which is
 * organised very differently (8 slices x 2 channels, each slice with its own
 * wrap and divider, rather than ESP-IDF's timer/channel/speed-mode triple).
 *
 * Only the enums and config structs are provided, which is all the shared
 * sources need to describe a PWM binding. A real RP2350 PWM backend
 * (drivers/pico/pwm_port_pico.c) is outstanding - see doc/ports/picocalc.md.
 */
#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LEDC_LOW_SPEED_MODE = 0,
    LEDC_SPEED_MODE_MAX,
} ledc_mode_t;

typedef enum {
    LEDC_TIMER_0 = 0,
    LEDC_TIMER_1,
    LEDC_TIMER_2,
    LEDC_TIMER_3,
    LEDC_TIMER_MAX,
} ledc_timer_t;

typedef enum {
    LEDC_CHANNEL_0 = 0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
    LEDC_CHANNEL_4,
    LEDC_CHANNEL_5,
    LEDC_CHANNEL_MAX,
} ledc_channel_t;

typedef enum {
    LEDC_TIMER_1_BIT = 1,
    LEDC_TIMER_8_BIT = 8,
    LEDC_TIMER_10_BIT = 10,
    LEDC_TIMER_12_BIT = 12,
    LEDC_TIMER_13_BIT = 13,
} ledc_timer_bit_t;

typedef enum {
    LEDC_AUTO_CLK = 0,
} ledc_clk_cfg_t;

typedef enum {
    LEDC_INTR_DISABLE = 0,
    LEDC_INTR_FADE_END,
} ledc_intr_type_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_intr_type_t intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
} ledc_channel_config_t;

esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);
esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t channel, uint32_t duty);
esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t channel);
esp_err_t ledc_stop(ledc_mode_t mode, ledc_channel_t channel, uint32_t idle_level);
uint32_t ledc_get_duty(ledc_mode_t mode, ledc_channel_t channel);

#ifdef __cplusplus
}
#endif
