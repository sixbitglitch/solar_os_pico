/*
 * Bridge the PicoCalc board battery (STM32 REG_ID_BAT) into solar_os_battery.
 */

#include "solar_os_picocalc_battery.h"

#include <string.h>

#include "battery_picocalc.h"
#include "esp_log.h"
#include "solar_os_battery.h"

static const char *TAG = "picocalc-bat";
static const char *PROVIDER_NAME = "picocalc";
static bool attached;

static esp_err_t provider_read(void *user, solar_os_battery_sample_t *sample)
{
    (void)user;
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    battery_picocalc_sample_t driver = {0};
    const esp_err_t err = battery_picocalc_read(&driver);
    if (err != ESP_OK) {
        return err;
    }

    *sample = (solar_os_battery_sample_t){
        .battery_mv = driver.battery_mv,
        .calibrated = false,
        .external_power_valid = true,
        .external_power = driver.charging,
        .charging_valid = true,
        .charging = driver.charging,
        .percent_valid = true,
        .percent = driver.percent,
    };
    return ESP_OK;
}

esp_err_t solar_os_picocalc_battery_attach(void)
{
    if (attached) {
        return ESP_OK;
    }

    const esp_err_t init_err = battery_picocalc_init();
    if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "battery probe: %s (will retry on read)", esp_err_to_name(init_err));
    }

    const solar_os_battery_provider_t provider = {
        .read = provider_read,
        .user = NULL,
    };
    const esp_err_t err = solar_os_battery_register_provider(PROVIDER_NAME, &provider);
    if (err != ESP_OK) {
        return err;
    }

    attached = true;
    ESP_LOGI(TAG, "battery provider registered (AXP2101 via keyboard co-processor)");
    return ESP_OK;
}

esp_err_t solar_os_picocalc_battery_detach(void)
{
    if (!attached) {
        return ESP_OK;
    }
    const esp_err_t err = solar_os_battery_unregister_provider(PROVIDER_NAME);
    attached = false;
    return err;
}
