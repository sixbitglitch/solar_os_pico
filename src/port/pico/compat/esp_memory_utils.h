/*
 * esp_memory_utils.h - pico-sdk compatibility shim.
 *
 * services/solar_os_memory.c uses these to classify a pointer's region for
 * the `mem` shell command.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "solar_os_psram_heap.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline bool esp_ptr_external_ram(const void *p)
{
    if (p == NULL) {
        return false;
    }
    const uintptr_t addr = (uintptr_t)p;
    /* Entire QMI CS1 window used for the Pico Plus 2 W PSRAM. */
    if (addr >= 0x11000000u && addr < 0x11800000u) {
        return true;
    }
    return solar_os_psram_heap_contains(p);
}

static inline bool esp_ptr_internal(const void *p)
{
    return p != NULL && !esp_ptr_external_ram(p);
}

static inline bool esp_ptr_in_drom(const void *p)
{
    (void)p;
    return false;
}

static inline bool esp_ptr_dma_capable(const void *p)
{
    /* RP2350 DMA cannot reach QMI PSRAM; treat external pointers as non-DMA. */
    return p != NULL && !esp_ptr_external_ram(p);
}

#ifdef __cplusplus
}
#endif
