/*
 * esp_random.h - pico-sdk compatibility shim.
 *
 * Backed by RP2350's hardware TRNG through pico-sdk's get_rand_32().
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
