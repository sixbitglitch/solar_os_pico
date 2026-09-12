/*
 * esp_bt.h - pico-sdk compatibility shim.
 *
 * There is no Bluetooth in this target at all: the bluetooth flavour group is
 * off and no BLE source is linked (see doc/ports/picocalc.md - the BLE stack
 * is deliberately NOT stubbed with dead code, it is simply absent). This
 * header exists only because services/solar_os_power.c includes it to release
 * the BT controller's memory on ESP32; that call is a no-op here.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_BT_MODE_IDLE = 0,
    ESP_BT_MODE_BLE = 1,
    ESP_BT_MODE_CLASSIC_BT = 2,
    ESP_BT_MODE_BTDM = 3,
} esp_bt_mode_t;

/* Nothing to release: the CYW43439 radio is never brought up in this pass. */
static inline esp_err_t esp_bt_controller_mem_release(esp_bt_mode_t mode)
{ (void)mode; return ESP_OK; }

static inline esp_err_t esp_bt_mem_release(esp_bt_mode_t mode)
{ (void)mode; return ESP_OK; }

#ifdef __cplusplus
}
#endif
