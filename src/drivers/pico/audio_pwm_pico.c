/*
 * PicoCalc / Pico Plus 2 W PWM audio backend.
 *
 * Implements drivers/audio_pwm.h using pico-sdk hardware_pwm and an alarm
 * pool callback to clock out 16 kHz samples. PicoCalc wiring: GPIO26 left,
 * GPIO27 right (stereo on both channels when opened on GPIO26/27).
 *
 * Link: hardware_pwm, hardware_timer, pico_time.
 */

#include "audio_pwm.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#ifndef AUDIO_PWM_PICO_PIN_LEFT
#define AUDIO_PWM_PICO_PIN_LEFT 26U
#endif
#ifndef AUDIO_PWM_PICO_PIN_RIGHT
#define AUDIO_PWM_PICO_PIN_RIGHT 27U
#endif

typedef struct {
    bool open;
    uint pin;
    uint slice;
    uint channel;
    bool stereo;
    uint channel_right;
    uint8_t volume;
    const int16_t *pending;
    size_t pending_frames;
    size_t pending_index;
    uint8_t pending_channels;
} audio_pwm_pico_t;

static audio_pwm_pico_t audio_pwm;
static repeating_timer_t audio_pwm_timer;

static uint16_t sample_to_duty(int16_t sample, uint8_t volume)
{
    /* Map s16 -> unsigned PWM duty with volume 0..100. */
    int32_t scaled = (int32_t)sample * (int32_t)volume;
    scaled /= 100;
    /* Center at mid-rail; 8-bit effective depth on a high carrier. */
    int32_t centered = ((scaled + 32768) * 255) / 65535;
    if (centered < 0) {
        centered = 0;
    }
    if (centered > 255) {
        centered = 255;
    }
    return (uint16_t)centered;
}

static bool audio_pwm_timer_cb(repeating_timer_t *timer)
{
    (void)timer;
    if (!audio_pwm.open || audio_pwm.pending == NULL ||
        audio_pwm.pending_index >= audio_pwm.pending_frames) {
        return true;
    }

    const size_t frame = audio_pwm.pending_index;
    int16_t sample_left =
        audio_pwm.pending[frame * audio_pwm.pending_channels];
    int16_t sample_right = sample_left;
    if (audio_pwm.pending_channels >= 2U) {
        sample_right =
            audio_pwm.pending[frame * audio_pwm.pending_channels + 1U];
    }
    pwm_set_chan_level(audio_pwm.slice,
                       audio_pwm.channel,
                       sample_to_duty(sample_left, audio_pwm.volume));
    if (audio_pwm.stereo) {
        pwm_set_chan_level(audio_pwm.slice,
                           audio_pwm.channel_right,
                           sample_to_duty(sample_right, audio_pwm.volume));
    }
    audio_pwm.pending_index++;
    return true;
}

esp_err_t audio_pwm_open(gpio_num_t pin)
{
    if (audio_pwm.open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint pin_u = (uint)pin;
    gpio_set_function(pin_u, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(pin_u);
    const uint channel = pwm_gpio_to_channel(pin_u);
    const bool stereo_pair =
        pin_u == AUDIO_PWM_PICO_PIN_LEFT || pin_u == AUDIO_PWM_PICO_PIN_RIGHT;
    uint channel_right = channel;
    if (stereo_pair) {
        gpio_set_function(AUDIO_PWM_PICO_PIN_RIGHT, GPIO_FUNC_PWM);
        channel_right = pwm_gpio_to_channel(AUDIO_PWM_PICO_PIN_RIGHT);
    }

    pwm_config cfg = pwm_get_default_config();
    /* ~78 kHz carrier with 8-bit wrap, matching AUDIO_PWM_CARRIER_HZ order. */
    pwm_config_set_clkdiv(&cfg, 6.25f);
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(slice, &cfg, true);
    pwm_set_chan_level(slice, channel, 128);
    if (stereo_pair) {
        pwm_set_chan_level(slice, channel_right, 128);
    }

    audio_pwm = (audio_pwm_pico_t){
        .open = true,
        .pin = pin_u,
        .slice = slice,
        .channel = channel,
        .stereo = stereo_pair,
        .channel_right = channel_right,
        .volume = 100,
    };

    /* 16 kHz sample clock. */
    if (!add_repeating_timer_us(-1000000 / (int)AUDIO_PWM_SAMPLE_RATE,
                                audio_pwm_timer_cb,
                                NULL,
                                &audio_pwm_timer)) {
        pwm_set_enabled(slice, false);
        audio_pwm.open = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_pwm_write_s16(const int16_t *samples,
                              size_t frames,
                              uint8_t channels,
                              uint8_t volume)
{
    if (!audio_pwm.open || samples == NULL || frames == 0 ||
        (channels != 1U && channels != 2U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (volume > 100U) {
        volume = 100U;
    }

    /* Busy-wait previous block so this stays synchronous like the ESP driver. */
    while (audio_pwm.pending != NULL &&
           audio_pwm.pending_index < audio_pwm.pending_frames) {
        tight_loop_contents();
    }

    audio_pwm.volume = volume;
    audio_pwm.pending = samples;
    audio_pwm.pending_frames = frames;
    audio_pwm.pending_index = 0;
    audio_pwm.pending_channels = channels;

    while (audio_pwm.pending_index < audio_pwm.pending_frames) {
        tight_loop_contents();
    }
    audio_pwm.pending = NULL;
    return ESP_OK;
}

void audio_pwm_close(void)
{
    if (!audio_pwm.open) {
        return;
    }
    cancel_repeating_timer(&audio_pwm_timer);
    pwm_set_chan_level(audio_pwm.slice, audio_pwm.channel, 0);
    pwm_set_enabled(audio_pwm.slice, false);
    memset(&audio_pwm, 0, sizeof(audio_pwm));
}
