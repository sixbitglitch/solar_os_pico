/*
 * solar_os_compat_pico_malloc.c - libc malloc prefers PSRAM on PicoCalc.
 *
 * Replaces pico-sdk's pico_malloc wrappers. Those only mutex-protect newlib's
 * sbrk heap in SRAM and panic on OOM. SolarOS already has two heaps:
 *   MALLOC_CAP_SPIRAM   -> APS6404 via solar_os_psram_heap (~8 MB)
 *   MALLOC_CAP_INTERNAL -> FreeRTOS heap_4 in SRAM
 *
 * Route malloc/calloc/realloc/free through heap_caps so ordinary libc users
 * (mbedtls, lwIP helpers, cJSON, miniz, third-party code) land in PSRAM
 * first, keeping the small internal heap for DMA, TCBs, and ISR-safe work.
 *
 * Do not fall back to newlib sbrk: mixing that with heap_caps_free would
 * corrupt FreeRTOS heap_4. Returning NULL on exhaustion matches SolarOS
 * callers and avoids pico_malloc's panic-on-OOM lockup.
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"

void *__wrap_malloc(size_t size)
{
    if (size == 0U) {
        return NULL;
    }
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (count == 0U || size == 0U) {
        return NULL;
    }
    if (size > (SIZE_MAX / count)) {
        return NULL;
    }
    return heap_caps_calloc(count, size, MALLOC_CAP_DEFAULT);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    }
    if (size == 0U) {
        heap_caps_free(ptr);
        return NULL;
    }
    return heap_caps_realloc(ptr, size, MALLOC_CAP_DEFAULT);
}

void __wrap_free(void *ptr)
{
    heap_caps_free(ptr);
}
