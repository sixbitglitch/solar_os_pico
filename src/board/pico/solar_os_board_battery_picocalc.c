/*
 * solar_os_board_battery_picocalc.c - board battery service on the PicoCalc.
 *
 * Mirrors src/board/solar_os_board_battery_adc.c: a thin adapter from the
 * driver's sample struct to the board service's. The only wrinkle is units -
 * the PicoCalc's fuel gauge reports percent, not millivolts, so the estimated
 * voltage is handed over with calibrated = false. See
 * src/drivers/pico/battery_picocalc.h for the full explanation.
 */

#include "solar_os_board_battery.h"

#include <stddef.h>

#include "battery_picocalc.h"

esp_err_t solar_os_board_battery_init(void)
{
    return battery_picocalc_init();
}

esp_err_t solar_os_board_battery_read(solar_os_board_battery_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    battery_picocalc_sample_t driver_sample;
    const esp_err_t err = battery_picocalc_read(&driver_sample);
    if (err != ESP_OK) {
        return err;
    }

    sample->battery_mv = driver_sample.battery_mv;
    /*
     * Always false. The percentage is exact, but the millivolt figure this
     * field carries is derived from a generic Li-ion curve, and claiming it is
     * calibrated would make `batmon` and the battery shell output overstate
     * their accuracy.
     */
    sample->calibrated = false;
    return ESP_OK;
}
