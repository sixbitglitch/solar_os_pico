/*
 * esp_sleep.h - pico-sdk compatibility shim.
 *
 * Deep sleep is NOT implemented for this target. RP2350's equivalent lives in
 * pico-sdk's hardware_powman (dormant/sleep modes with a powman alarm or GPIO
 * wake), which is a separate piece of work and is listed as deferred in
 * doc/ports/picocalc.md.
 *
 * These return ESP_ERR_NOT_SUPPORTED rather than ESP_OK on purpose: a caller
 * that thinks it armed a wake source and then slept forever is worse than one
 * that is told it cannot.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The full ESP-IDF enum. Most of these name ESP32 peripherals RP2350 does not
 * have (ULP coprocessor, touchpad, BT/Wi-Fi wake). They are declared anyway so
 * that the shell's wake-source reporting compiles without editing its switch
 * statements; esp_sleep_get_wakeup_cause() on this target only ever returns
 * ESP_SLEEP_WAKEUP_UNDEFINED.
 */
typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_TOUCHPAD,
    ESP_SLEEP_WAKEUP_ULP,
    ESP_SLEEP_WAKEUP_GPIO,
    ESP_SLEEP_WAKEUP_UART,
    ESP_SLEEP_WAKEUP_WIFI,
    ESP_SLEEP_WAKEUP_COCPU,
    ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG,
    ESP_SLEEP_WAKEUP_BT,
} esp_sleep_wakeup_cause_t;

/*
 * Power domains. RP2350's low-power story runs through hardware_powman and
 * does not decompose this way, so configuring one is accepted and ignored -
 * unlike the wake sources, where claiming success would be dangerous.
 */
typedef enum {
    ESP_PD_DOMAIN_RTC_PERIPH = 0,
    ESP_PD_DOMAIN_RTC_SLOW_MEM,
    ESP_PD_DOMAIN_RTC_FAST_MEM,
    ESP_PD_DOMAIN_XTAL,
    ESP_PD_DOMAIN_VDDSDIO,
    ESP_PD_DOMAIN_MAX,
} esp_sleep_pd_domain_t;

typedef enum {
    ESP_PD_OPTION_OFF = 0,
    ESP_PD_OPTION_ON,
    ESP_PD_OPTION_AUTO,
} esp_sleep_pd_option_t;

typedef enum {
    ESP_EXT1_WAKEUP_ALL_LOW = 0,
    ESP_EXT1_WAKEUP_ANY_HIGH = 1,
    ESP_EXT1_WAKEUP_ANY_LOW = 2,
} esp_sleep_ext1_wakeup_mode_t;

static inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us)
{ (void)us; return ESP_ERR_NOT_SUPPORTED; }

static inline esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t mask,
                                                     esp_sleep_ext1_wakeup_mode_t mode)
{ (void)mask; (void)mode; return ESP_ERR_NOT_SUPPORTED; }

/* Newer ESP-IDF versions split ext1 wake into a GPIO-mask ("_io") form. Same
 * not-implemented contract as the rest of this header. */
static inline esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask,
                                                        esp_sleep_ext1_wakeup_mode_t mode)
{ (void)mask; (void)mode; return ESP_ERR_NOT_SUPPORTED; }

static inline esp_err_t esp_sleep_disable_ext1_wakeup_io(uint64_t mask)
{ (void)mask; return ESP_ERR_NOT_SUPPORTED; }

static inline uint64_t esp_sleep_get_ext1_wakeup_status(void)
{ return 0; }

static inline esp_err_t esp_sleep_enable_ext0_wakeup(int pin, int level)
{ (void)pin; (void)level; return ESP_ERR_NOT_SUPPORTED; }

static inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void)
{ return ESP_SLEEP_WAKEUP_UNDEFINED; }

static inline esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t source)
{ (void)source; return ESP_ERR_NOT_SUPPORTED; }

/* Accepted and ignored: there is no equivalent domain to keep powered. */
static inline esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t domain,
                                            esp_sleep_pd_option_t option)
{ (void)domain; (void)option; return ESP_OK; }

/* Not implemented: returns instead of sleeping, so callers fall through to
 * their "sleep failed" path rather than hanging. */
static inline esp_err_t esp_light_sleep_start(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void esp_deep_sleep_start(void) { }

#ifdef __cplusplus
}
#endif
