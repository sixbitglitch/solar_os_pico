/*
 * esp_heap_caps.h - pico-sdk compatibility shim.
 *
 * RP2350 has one uniform SRAM heap, so the capability bits carry no placement
 * meaning and the allocators are plain malloc/free. The reporting functions
 * are not stubs, though: they answer from FreeRTOS heap_4 statistics, because
 * src/solar_os_task.c gates every task launch on them (see
 * solar_os_task_can_create) and a constant answer would break admission
 * control rather than merely degrade it.
 *
 * MALLOC_CAP_SPIRAM deliberately reports zero free and zero total. Nothing in
 * the picocalc-core flavour allocates with it - the PSRAM paths are compiled
 * out by CONFIG_SPIRAM=0 - and reporting zero is the truthful answer for a
 * target that does not bring its PSRAM up.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_EXEC         (1 << 0)
#define MALLOC_CAP_32BIT        (1 << 1)
#define MALLOC_CAP_8BIT         (1 << 2)
#define MALLOC_CAP_DMA          (1 << 3)
#define MALLOC_CAP_PID2         (1 << 4)
#define MALLOC_CAP_SPIRAM       (1 << 10)
#define MALLOC_CAP_INTERNAL     (1 << 11)
#define MALLOC_CAP_DEFAULT      (1 << 12)
#define MALLOC_CAP_IRAM_8BIT    (1 << 13)
#define MALLOC_CAP_RETENTION    (1 << 14)
#define MALLOC_CAP_RTCRAM       (1 << 15)

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);

size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);

#ifdef __cplusplus
}
#endif
