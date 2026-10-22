/*
 * solar_os_psram_heap.h - Capability heap over the Pico Plus 2 W APS6404.
 *
 * pico-sdk's hardware_psram brings the chip up and maps it at 0x11000000; this
 * module turns the leftover (after any __uninitialized_psram linker sections)
 * into a heap_4-style allocator that backs MALLOC_CAP_SPIRAM.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zero .psram_uninitialised (EXT_RAM_BSS_ATTR) after hardware_psram is up.
 * pico-sdk loads .psram_initialised but leaves the NOLOAD region untouched;
 * SolarOS treats that region as BSS and requires it cleared before use.
 * Safe to call more than once; no-op if PSRAM is unavailable. */
void solar_os_psram_bss_clear(void);

/* Initialise from leftover PSRAM after linker .psram_* sections. Safe to call
 * more than once; subsequent calls are no-ops. Returns false if PSRAM is not
 * available or too small for a heap. */
bool solar_os_psram_heap_init(void);

bool solar_os_psram_heap_ready(void);
bool solar_os_psram_heap_contains(const void *ptr);

void *solar_os_psram_malloc(size_t size);
void *solar_os_psram_calloc(size_t count, size_t size);
void *solar_os_psram_realloc(void *ptr, size_t size);
void solar_os_psram_free(void *ptr);

size_t solar_os_psram_heap_total_size(void);
size_t solar_os_psram_heap_free_size(void);
size_t solar_os_psram_heap_largest_free_block(void);
size_t solar_os_psram_heap_minimum_free_size(void);

#ifdef __cplusplus
}
#endif
