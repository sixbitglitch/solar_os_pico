/*
 * esp_memory_utils.h - pico-sdk compatibility shim.
 *
 * services/solar_os_memory.c uses these to classify a pointer's region for
 * the `mem` shell command. On RP2350 every heap pointer is internal SRAM, so
 * the answers are constants - but they are the *correct* constants for this
 * target, not placeholders.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool esp_ptr_internal(const void *p) { (void)p; return true; }
/* No PSRAM is mapped on this target, so nothing is ever "external". */
static inline bool esp_ptr_external_ram(const void *p) { (void)p; return false; }
static inline bool esp_ptr_in_drom(const void *p) { (void)p; return false; }
static inline bool esp_ptr_dma_capable(const void *p) { (void)p; return true; }

#ifdef __cplusplus
}
#endif
