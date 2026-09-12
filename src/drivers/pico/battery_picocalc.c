/*
 * battery_picocalc.c - see battery_picocalc.h for the hardware path and the
 * percent-vs-millivolt caveat.
 */

#include "battery_picocalc.h"

#include <string.h>

#include "esp_log.h"
#include "picocalc_keyboard.h"
#include "solar_os_picocalc_keyboard.h"

static const char *TAG = "battery-picocalc";

/*
 * Single-cell Li-ion discharge approximation, used ONLY to fill the generic
 * battery_mv field from the fuel gauge's percentage (see the header).
 *
 * Piecewise-linear over three segments, because a single straight line between
 * 3.0 V and 4.2 V is badly wrong in the middle of the curve where real cells
 * sit on a plateau:
 *
 *    0%  -> 3300 mV   (AXP2101's default shutdown threshold region)
 *   20%  -> 3600 mV   (knee at the bottom of the plateau)
 *   80%  -> 3950 mV   (plateau)
 *  100%  -> 4200 mV   (full)
 *
 * These are nominal figures for a generic single-cell pack, not measurements
 * from a PicoCalc. Any consumer that needs a real voltage must not use this;
 * that is what calibrated = false signals.
 */
static uint16_t estimate_millivolts(uint8_t percent)
{
    if (percent >= 100U) {
        return 4200U;
    }
    if (percent >= 80U) {
        /* 80..100% spans 3950..4200 mV. */
        return (uint16_t)(3950U + ((uint32_t)(percent - 80U) * 250U) / 20U);
    }
    if (percent >= 20U) {
        /* 20..80% spans 3600..3950 mV. */
        return (uint16_t)(3600U + ((uint32_t)(percent - 20U) * 350U) / 60U);
    }
    /* 0..20% spans 3300..3600 mV. */
    return (uint16_t)(3300U + ((uint32_t)percent * 300U) / 20U);
}

esp_err_t battery_picocalc_init(void)
{
    /*
     * The I2C link belongs to the keyboard driver. Rather than bring the bus
     * up a second time (which would fight over the peripheral), this just
     * checks the link is live.
     */
    if (!solar_os_picocalc_keyboard_available()) {
        ESP_LOGW(TAG,
                 "co-processor link not up yet; battery reads will retry "
                 "(the keyboard driver owns the I2C bus)");
        return ESP_ERR_INVALID_STATE;
    }

    battery_picocalc_sample_t sample = {0};
    const esp_err_t err = battery_picocalc_read(&sample);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "AXP2101 via co-processor: %u%%%s",
             (unsigned)sample.percent,
             sample.charging ? " (charging)" : "");
    return ESP_OK;
}

esp_err_t battery_picocalc_read(battery_picocalc_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_picocalc_keyboard_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t percent = 0;
    bool charging = false;
    const esp_err_t err = picocalc_kbd_read_battery(&percent, &charging);
    if (err != ESP_OK) {
        return err;
    }

    memset(sample, 0, sizeof(*sample));
    sample->percent = percent;
    sample->charging = charging;
    /*
     * sync_bat() clamps a disconnected battery to 0. A charging pack reading
     * 0% is still a present pack, so presence is "non-zero OR charging".
     */
    sample->present = (percent > 0U) || charging;
    sample->battery_mv = sample->present ? estimate_millivolts(percent) : 0U;
    return ESP_OK;
}
