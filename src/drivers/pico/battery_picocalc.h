/*
 * battery_picocalc.h - PicoCalc battery state.
 *
 * HARDWARE PATH (confirmed, primary source):
 * The PicoCalc does NOT bring a battery voltage divider to a Pico ADC pin.
 * Battery monitoring lives on the STM32F103 co-processor, which talks to an
 * AXP2101 PMU over its own private I2C bus (PB10/PB11 - see CONFIG_PMU_SDA /
 * CONFIG_PMU_SCL in the vendor firmware's conf_app.h) and republishes the
 * result to the host as register 0x0b (REG_ID_BAT).
 *
 * Source: github.com/clockworkpi/PicoCalc, Code/picocalc_keyboard/reg.h
 * (REG_ID_BAT = 0x0b), picocalc_keyboard.ino (sync_bat(), which writes the
 * AXP2101's getBatteryPercent() into that register and sets bit 7 while
 * charging), and Code/picocalc_helloworld/i2ckbd/i2ckbd.c (read_battery(),
 * the vendor's own host-side reader). This is the vendor's firmware, not
 * community reverse-engineering.
 *
 * THE UNITS PROBLEM, STATED PLAINLY:
 * solar_os_board_battery_sample_t carries battery_mv - millivolts. The
 * co-processor reports a PERCENTAGE. There is no register that exposes the
 * AXP2101's raw millivolt reading to the host; getBattVoltage() is called only
 * for the STM32's own serial debug output and is never published.
 *
 * So this driver reports what it actually knows (percent, charging flag) and,
 * for the generic millivolt field, converts using a documented single-cell
 * Li-ion curve with calibrated = false. That flag is the existing contract's
 * way of saying "this number is an estimate" and every consumer already
 * handles it. The percentage itself is exact; only the derived voltage is
 * approximate, and it is labelled as such.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* State of charge, 0..100, straight from the AXP2101's fuel gauge. */
    uint8_t percent;
    bool charging;
    /* True when the PMU reports a battery present. The co-processor encodes
     * "no battery" as percent == 0, so this is derived, not independent. */
    bool present;
    /* Estimated from percent - see the units note above. Never calibrated. */
    uint16_t battery_mv;
} battery_picocalc_sample_t;

/*
 * No bus setup of its own: the co-processor's I2C link is brought up by the
 * keyboard driver, which owns it. Returns ESP_ERR_INVALID_STATE if the
 * keyboard has not attached yet, so the caller can retry after boot ordering
 * settles rather than latching a permanent failure.
 */
esp_err_t battery_picocalc_init(void);
esp_err_t battery_picocalc_read(battery_picocalc_sample_t *sample);

#ifdef __cplusplus
}
#endif
