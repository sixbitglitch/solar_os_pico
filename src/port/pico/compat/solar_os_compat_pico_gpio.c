/*
 * solar_os_compat_pico_gpio.c - ESP-IDF GPIO API on pico-sdk hardware_gpio.
 *
 * The shared SolarOS sources drive GPIO through ESP-IDF's driver/gpio.h
 * (gpio_config / gpio_set_level / gpio_get_level and friends). Rather than
 * edit those call sites, this maps the API onto RP2350's.
 *
 * Semantic differences worth knowing, since they are not one-to-one:
 *
 *  - ESP-IDF's gpio_config takes a 64-bit pin mask and applies one setting to
 *    every selected pin. RP2350's API is per-pin, so this loops.
 *  - RP2350 pulls are set independently of direction, and a pin can have both
 *    pull-up and pull-down enabled. gpio_set_pulls() handles that combination
 *    directly.
 *  - GPIO interrupts are NOT wired up here. Nothing in the picocalc-core
 *    flavour registers a GPIO ISR (the keyboard is polled, the SD card-detect
 *    line is read on demand), so gpio_config's intr_type is accepted and
 *    ignored rather than half-implemented. If a future driver needs edge
 *    interrupts, gpio_set_irq_enabled_with_callback() is the pico-sdk entry
 *    point and this is where it belongs.
 */

#include "driver/gpio.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"

static bool compat_pin_valid(gpio_num_t pin)
{
    return pin >= 0 && pin < (gpio_num_t)NUM_BANK0_GPIOS;
}

static esp_err_t compat_apply_mode(gpio_num_t pin, gpio_mode_t mode)
{
    switch (mode) {
    case GPIO_MODE_DISABLE:
        /* Leave the pin as a SIO input with no pulls: the closest RP2350 has
         * to "not driven, not read". */
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
        return ESP_OK;
    case GPIO_MODE_INPUT:
        gpio_set_dir(pin, GPIO_IN);
        return ESP_OK;
    case GPIO_MODE_OUTPUT:
    case GPIO_MODE_INPUT_OUTPUT:
        /* RP2350 always allows reading back an output pin, so plain output and
         * input-output are the same configuration. */
        gpio_set_dir(pin, GPIO_OUT);
        return ESP_OK;
    case GPIO_MODE_OUTPUT_OD:
    case GPIO_MODE_INPUT_OUTPUT_OD:
        /*
         * RP2350 has no open-drain output driver. Emulating it means driving
         * low and switching to input for high, which only the caller can do
         * correctly at the point of each level change. Refusing is better than
         * silently configuring a push-pull output onto a bus that expects
         * open-drain - that can contend and damage the other driver.
         */
        return ESP_ERR_NOT_SUPPORTED;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (gpio_num_t pin = 0; pin < (gpio_num_t)NUM_BANK0_GPIOS; pin++) {
        if ((config->pin_bit_mask & (1ULL << (unsigned)pin)) == 0) {
            continue;
        }

        gpio_init(pin);
        const esp_err_t err = compat_apply_mode(pin, config->mode);
        if (err != ESP_OK) {
            return err;
        }
        gpio_set_pulls(pin,
                       config->pull_up_en == GPIO_PULLUP_ENABLE,
                       config->pull_down_en == GPIO_PULLDOWN_ENABLE);
    }

    /* config->intr_type is deliberately ignored - see the file comment. */
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    if (!compat_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
    return ESP_OK;
}

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode)
{
    if (!compat_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return compat_apply_mode(pin, mode);
}

esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t pull)
{
    if (!compat_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (pull) {
    case GPIO_FLOATING:
        gpio_disable_pulls(pin);
        return ESP_OK;
    case GPIO_PULLUP_ONLY:
        gpio_set_pulls(pin, true, false);
        return ESP_OK;
    case GPIO_PULLDOWN_ONLY:
        gpio_set_pulls(pin, false, true);
        return ESP_OK;
    case GPIO_PULLUP_PULLDOWN:
        gpio_set_pulls(pin, true, true);
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (!compat_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_put(pin, level != 0);
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin)
{
    if (!compat_pin_valid(pin)) {
        return 0;
    }
    return gpio_get(pin) ? 1 : 0;
}

esp_err_t gpio_pullup_en(gpio_num_t pin)
{
    if (!compat_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_pull_up(pin);
    return ESP_OK;
}

esp_err_t gpio_pullup_dis(gpio_num_t pin)
{
    if (!compat_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_set_pulls(pin, false, gpio_is_pulled_down(pin));
    return ESP_OK;
}

esp_err_t gpio_pulldown_en(gpio_num_t pin)
{
    if (!compat_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_pull_down(pin);
    return ESP_OK;
}

esp_err_t gpio_pulldown_dis(gpio_num_t pin)
{
    if (!compat_pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_set_pulls(pin, gpio_is_pulled_up(pin), false);
    return ESP_OK;
}
