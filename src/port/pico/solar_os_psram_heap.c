/*
 * solar_os_psram_heap.c - heap_4-style allocator over mapped PSRAM.
 *
 * Algorithm mirrors FreeRTOS heap_4 (coalescing free list) so fragmentation
 * behaviour matches the internal SRAM heap SolarOS already reasons about.
 */

#include "solar_os_psram_heap.h"

#include <string.h>

#include "FreeRTOS.h"
#include "hardware/psram.h"
#include "task.h"

typedef struct psram_block_link {
    struct psram_block_link *next_free;
    size_t block_size;
} psram_block_link_t;

#define PSRAM_HEAP_STRUCT_SIZE                                                     \
    ((sizeof(psram_block_link_t) + ((size_t)portBYTE_ALIGNMENT - 1U)) &             \
     ~((size_t)portBYTE_ALIGNMENT_MASK))
#define PSRAM_HEAP_MINIMUM_BLOCK_SIZE ((size_t)(PSRAM_HEAP_STRUCT_SIZE << 1))
#define PSRAM_HEAP_ALLOCATED_BIT                                                   \
    (((size_t)1U) << ((sizeof(size_t) * 8U) - 1U))

extern uint32_t __psram_load_end__;
extern uint32_t __psram_end__;

static bool psram_bss_cleared;
static psram_block_link_t psram_start;
static psram_block_link_t *psram_end;
static size_t psram_free_bytes;
static size_t psram_minimum_free_bytes;
static size_t psram_total_bytes;
static uint8_t *psram_heap_base;
static uint8_t *psram_heap_limit;
static bool psram_heap_ready;

static void psram_lock(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskSuspendAll();
    }
}

static void psram_unlock(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void)xTaskResumeAll();
    }
}

static void psram_insert_block_into_free_list(psram_block_link_t *block_to_insert)
{
    psram_block_link_t *iterator;
    uint8_t *p;

    for (iterator = &psram_start;
         iterator->next_free < block_to_insert;
         iterator = iterator->next_free) {
    }

    p = (uint8_t *)iterator;
    if ((p + iterator->block_size) == (uint8_t *)block_to_insert) {
        iterator->block_size += block_to_insert->block_size;
        block_to_insert = iterator;
    }

    p = (uint8_t *)block_to_insert;
    if ((p + block_to_insert->block_size) == (uint8_t *)iterator->next_free) {
        if (iterator->next_free != psram_end) {
            block_to_insert->block_size += iterator->next_free->block_size;
            block_to_insert->next_free = iterator->next_free->next_free;
        } else {
            block_to_insert->next_free = psram_end;
        }
    } else {
        block_to_insert->next_free = iterator->next_free;
    }

    if (iterator != block_to_insert) {
        iterator->next_free = block_to_insert;
    }
}

void solar_os_psram_bss_clear(void)
{
    if (psram_bss_cleared || !psram_is_available()) {
        return;
    }

    /*
     * Linker layout (sections_psram.incl):
     *   .psram_load      [__psram_start__ .. __psram_load_end__)  // copied from flash
     *   .psram_noload    [__psram_load_end__ .. __psram_end__)    // NOLOAD / garbage
     *
     * EXT_RAM_BSS_ATTR maps to .psram_uninitialised.* inside the NOLOAD span.
     * ESP-IDF zeroes SPIRAM BSS at bring-up; pico-sdk does not, so do it here
     * before any SolarOS table in that section is read (resource claims, shell
     * session, etc.).
     */
    uint8_t *const bss_start = (uint8_t *)&__psram_load_end__;
    uint8_t *const bss_end = (uint8_t *)&__psram_end__;
    if (bss_end > bss_start) {
        memset(bss_start, 0, (size_t)(bss_end - bss_start));
    }
    psram_bss_cleared = true;
}

bool solar_os_psram_heap_init(void)
{
    if (psram_heap_ready) {
        return true;
    }
    if (!psram_is_available()) {
        return false;
    }

    /* Heap carving starts after __psram_end__; BSS must be cleared first so
     * callers that only invoke heap_init still get ESP-IDF BSS semantics. */
    solar_os_psram_bss_clear();

    const size_t chip_bytes = psram_get_size();
    if (chip_bytes == 0) {
        return false;
    }

    uintptr_t start = ((uintptr_t)&__psram_end__ + (portBYTE_ALIGNMENT - 1U)) &
                      ~((uintptr_t)portBYTE_ALIGNMENT_MASK);
    const uintptr_t region_base = 0x11000000u;
    const uintptr_t region_end = region_base + (uintptr_t)chip_bytes;
    if (start < region_base) {
        start = region_base;
    }
    if (start >= region_end) {
        return false;
    }

    size_t bytes = (size_t)(region_end - start);
    /* Leave a sentinel block at the end. */
    if (bytes <= (PSRAM_HEAP_STRUCT_SIZE * 2U) + portBYTE_ALIGNMENT) {
        return false;
    }

    psram_heap_base = (uint8_t *)start;
    psram_heap_limit = (uint8_t *)region_end;

    psram_start.next_free = (psram_block_link_t *)psram_heap_base;
    psram_start.block_size = 0;

    uint8_t *aligned_end = (uint8_t *)(region_end - PSRAM_HEAP_STRUCT_SIZE);
    aligned_end = (uint8_t *)(((uintptr_t)aligned_end) &
                              ~((uintptr_t)portBYTE_ALIGNMENT_MASK));
    psram_end = (psram_block_link_t *)aligned_end;
    psram_end->next_free = NULL;
    psram_end->block_size = 0;

    psram_block_link_t *first = (psram_block_link_t *)psram_heap_base;
    first->next_free = psram_end;
    first->block_size = (size_t)((uint8_t *)psram_end - psram_heap_base);

    psram_total_bytes = first->block_size;
    psram_free_bytes = first->block_size;
    psram_minimum_free_bytes = first->block_size;
    psram_heap_ready = true;
    return true;
}

bool solar_os_psram_heap_ready(void)
{
    return psram_heap_ready;
}

bool solar_os_psram_heap_contains(const void *ptr)
{
    if (ptr == NULL || !psram_heap_ready) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)ptr;
    return p >= psram_heap_base && p < psram_heap_limit;
}

void *solar_os_psram_malloc(size_t size)
{
    if (!psram_heap_ready || size == 0) {
        return NULL;
    }

    psram_lock();

    size_t wanted = size + PSRAM_HEAP_STRUCT_SIZE;
    if ((wanted & PSRAM_HEAP_ALLOCATED_BIT) != 0 || wanted < size) {
        psram_unlock();
        return NULL;
    }
    if ((wanted & portBYTE_ALIGNMENT_MASK) != 0) {
        wanted += portBYTE_ALIGNMENT - (wanted & portBYTE_ALIGNMENT_MASK);
    }

    void *result = NULL;
    if (wanted <= psram_free_bytes) {
        psram_block_link_t *previous = &psram_start;
        psram_block_link_t *block = psram_start.next_free;
        while (block->block_size < wanted && block->next_free != NULL) {
            previous = block;
            block = block->next_free;
        }

        if (block != psram_end) {
            result = (void *)(((uint8_t *)previous->next_free) +
                              PSRAM_HEAP_STRUCT_SIZE);
            previous->next_free = block->next_free;

            if ((block->block_size - wanted) > PSRAM_HEAP_MINIMUM_BLOCK_SIZE) {
                psram_block_link_t *new_block =
                    (psram_block_link_t *)(((uint8_t *)block) + wanted);
                new_block->block_size = block->block_size - wanted;
                block->block_size = wanted;
                psram_insert_block_into_free_list(new_block);
            }

            psram_free_bytes -= block->block_size;
            if (psram_free_bytes < psram_minimum_free_bytes) {
                psram_minimum_free_bytes = psram_free_bytes;
            }
            block->block_size |= PSRAM_HEAP_ALLOCATED_BIT;
            block->next_free = NULL;
        }
    }

    psram_unlock();
    return result;
}

void *solar_os_psram_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        return NULL;
    }
    if (count > (SIZE_MAX / size)) {
        return NULL;
    }
    const size_t total = count * size;
    void *ptr = solar_os_psram_malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void solar_os_psram_free(void *ptr)
{
    if (ptr == NULL || !psram_heap_ready) {
        return;
    }
    if (!solar_os_psram_heap_contains(ptr)) {
        return;
    }

    uint8_t *p = (uint8_t *)ptr - PSRAM_HEAP_STRUCT_SIZE;
    psram_block_link_t *block = (psram_block_link_t *)p;

    if ((block->block_size & PSRAM_HEAP_ALLOCATED_BIT) == 0 ||
        block->next_free != NULL) {
        return;
    }

    psram_lock();
    block->block_size &= ~PSRAM_HEAP_ALLOCATED_BIT;
    psram_free_bytes += block->block_size;
    psram_insert_block_into_free_list(block);
    psram_unlock();
}

void *solar_os_psram_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return solar_os_psram_malloc(size);
    }
    if (size == 0) {
        solar_os_psram_free(ptr);
        return NULL;
    }
    if (!solar_os_psram_heap_contains(ptr)) {
        return NULL;
    }

    uint8_t *raw = (uint8_t *)ptr - PSRAM_HEAP_STRUCT_SIZE;
    psram_block_link_t *block = (psram_block_link_t *)raw;
    const size_t old_size =
        (block->block_size & ~PSRAM_HEAP_ALLOCATED_BIT) - PSRAM_HEAP_STRUCT_SIZE;

    void *fresh = solar_os_psram_malloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old_size < size ? old_size : size);
    solar_os_psram_free(ptr);
    return fresh;
}

size_t solar_os_psram_heap_total_size(void)
{
    return psram_heap_ready ? psram_total_bytes : 0;
}

size_t solar_os_psram_heap_free_size(void)
{
    return psram_heap_ready ? psram_free_bytes : 0;
}

size_t solar_os_psram_heap_largest_free_block(void)
{
    if (!psram_heap_ready) {
        return 0;
    }

    size_t largest = 0;
    psram_lock();
    for (psram_block_link_t *block = psram_start.next_free;
         block != NULL && block != psram_end;
         block = block->next_free) {
        if (block->block_size > largest) {
            largest = block->block_size;
        }
    }
    psram_unlock();

    if (largest > PSRAM_HEAP_STRUCT_SIZE) {
        return largest - PSRAM_HEAP_STRUCT_SIZE;
    }
    return 0;
}

size_t solar_os_psram_heap_minimum_free_size(void)
{
    return psram_heap_ready ? psram_minimum_free_bytes : 0;
}
