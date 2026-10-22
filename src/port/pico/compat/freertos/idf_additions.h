/*
 * freertos/idf_additions.h - pico-sdk compatibility shim.
 *
 * ESP-IDF's non-standard additions to FreeRTOS: core pinning plus the
 * capability-aware create/delete helpers used when CONFIG_SPIRAM is on.
 */
#pragma once

#include <stdint.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RP2350 is dual-core and the FreeRTOS RP2350 port is built SMP, so a real
 * affinity mask is available. core_id is honoured when it names core 0 or 1
 * and treated as "either core" otherwise (which is what tskNO_AFFINITY means).
 *
 * stack_depth is in BYTES (ESP-IDF convention). The pico shim converts to
 * FreeRTOS words before calling the kernel; do not pass word counts here.
 */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task_code,
                                   const char *name,
                                   const uint32_t stack_depth,
                                   void *parameters,
                                   UBaseType_t priority,
                                   TaskHandle_t *task_handle,
                                   const BaseType_t core_id);

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task_code,
                                           const char *name,
                                           const uint32_t stack_depth,
                                           void *parameters,
                                           UBaseType_t priority,
                                           TaskHandle_t *task_handle,
                                           const BaseType_t core_id,
                                           UBaseType_t caps);

void vTaskDeleteWithCaps(TaskHandle_t task);

QueueHandle_t xQueueCreateWithCaps(UBaseType_t length,
                                   UBaseType_t item_size,
                                   UBaseType_t caps);

void vQueueDeleteWithCaps(QueueHandle_t queue);

SemaphoreHandle_t xSemaphoreCreateMutexWithCaps(UBaseType_t caps);

/* PSRAM heap helpers on ESP-IDF; map to the internal heap on Pico. */
static inline void vSemaphoreDeleteWithCaps(SemaphoreHandle_t sem)
{
    if (sem == NULL) {
        return;
    }
    StaticSemaphore_t *buffer = NULL;
    if (xSemaphoreGetStaticBuffer(sem, &buffer) == pdTRUE) {
        vSemaphoreDelete(sem);
        heap_caps_free(buffer);
    } else {
        vSemaphoreDelete(sem);
    }
}

#ifdef __cplusplus
}
#endif
