/*
 * esp_heap_caps.h - pico-sdk compatibility shim.
 *
 * Internal allocations use FreeRTOS heap_4 (SRAM). External (SPIRAM)
 * allocations use the PSRAM heap brought up by solar_os_psram_heap once
 * hardware_psram has mapped the Pico Plus 2 W APS6404.
 *
 * solar_os_task_can_create() gates every task launch on free-size answers, so
 * the reporting functions return live statistics rather than constants.
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

void *heap_caps_malloc_prefer(size_t size, size_t num, ...);
void *heap_caps_calloc_prefer(size_t n, size_t size, size_t num, ...);

size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);

#ifdef __cplusplus
}
#endif
