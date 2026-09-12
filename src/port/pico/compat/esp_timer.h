/*
 * esp_timer.h - pico-sdk compatibility shim.
 *
 * Only esp_timer_get_time() is used by this file set. pico-sdk's
 * time_us_64() has the same contract: microseconds since boot, monotonic.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
