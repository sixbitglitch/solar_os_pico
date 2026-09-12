/*
 * adc_port_pico.c - RP2350 backend for the solar_os_adc service's driver
 * interface (src/drivers/adc_port.h), against pico-sdk hardware_adc.
 *
 * The original src/drivers/adc_port.c is excluded from this target's build
 * (see boards/drivers/adc_pico.cmake): it is built on ESP-IDF's newer
 * esp_adc oneshot + calibration API (adc_oneshot_new_unit,
 * adc_cali_create_scheme_curve_fitting, ...), which is a much larger surface
 * than this compatibility layer shims elsewhere. Replacing it with a direct
 * RP2350 implementation was simpler and more honest than emulating that
 * whole API.
 *
 * RP2350B's SAR ADC has 8 channels on GPIO40-47 (ADC_BASE_PIN..+7), a fixed
 * 0-3.3 V input range with no programmable attenuator, and a fixed 12-bit
 * result - so adc_atten_t/adc_bitwidth_t are accepted (for call-site source
 * compatibility) and ignored. There is no factory calibration data on this
 * chip the way ESP32 has, so every sample reports calibrated = false and
 * voltage_mv is a plain linear conversion against the 3.3 V reference -
 * accurate only as far as the board's actual supply voltage matches that
 * exactly. See doc/ports/picocalc.md: the PicoCalc's Pico socket does not
 * expose GPIO40-47 at all, so this only matters for a future expansion
 * module wired to the Pico Plus 2 W module's own castellations.
 */

#include "adc_port.h"

#include "FreeRTOS.h"
#include "hardware/adc.h"
#include "semphr.h"

#define ADC_PORT_CHANNEL_COUNT 8U
#define ADC_PORT_VREF_MV 3300U
#define ADC_PORT_MAX_RAW 4095U /* 12-bit */

static bool adc_hw_initialized;
static SemaphoreHandle_t adc_mutex;

static esp_err_t adc_port_ensure_init_locked(void)
{
    if (!adc_hw_initialized) {
        adc_init();
        adc_hw_initialized = true;
    }
    return ESP_OK;
}

esp_err_t adc_port_init(void)
{
    if (adc_mutex == NULL) {
        adc_mutex = xSemaphoreCreateMutex();
        if (adc_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(adc_mutex, portMAX_DELAY);
    const esp_err_t ret = adc_port_ensure_init_locked();
    xSemaphoreGive(adc_mutex);
    return ret;
}

bool adc_port_is_adc_capable(gpio_num_t pin, adc_unit_t *unit, adc_channel_t *channel)
{
    if (pin < ADC_BASE_PIN || pin >= (gpio_num_t)(ADC_BASE_PIN + ADC_PORT_CHANNEL_COUNT)) {
        return false;
    }
    if (unit != NULL) {
        *unit = ADC_UNIT_1;
    }
    if (channel != NULL) {
        *channel = (adc_channel_t)(pin - ADC_BASE_PIN);
    }
    return true;
}

esp_err_t adc_port_configure_pin(gpio_num_t pin, adc_atten_t atten, adc_bitwidth_t bitwidth)
{
    (void)atten;
    (void)bitwidth;
    if (!adc_port_is_adc_capable(pin, NULL, NULL)) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = adc_port_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(adc_mutex, portMAX_DELAY);
    adc_gpio_init(pin);
    xSemaphoreGive(adc_mutex);
    return ESP_OK;
}

esp_err_t adc_port_read(gpio_num_t pin, adc_port_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    adc_unit_t unit;
    adc_channel_t channel;
    if (!adc_port_is_adc_capable(pin, &unit, &channel)) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t ret = adc_port_configure_pin(pin, ADC_ATTEN_DB_12, ADC_BITWIDTH_12);
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(adc_mutex, portMAX_DELAY);
    adc_select_input((unsigned int)channel);
    const uint16_t raw = adc_read();
    xSemaphoreGive(adc_mutex);

    *sample = (adc_port_sample_t) {
        .pin = pin,
        .raw = raw,
        .voltage_mv = (uint16_t)(((uint32_t)raw * ADC_PORT_VREF_MV) / ADC_PORT_MAX_RAW),
        .unit = unit,
        .channel = channel,
        .calibrated = false,
    };
    return ESP_OK;
}
