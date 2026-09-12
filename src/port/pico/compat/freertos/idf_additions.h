/*
 * freertos/idf_additions.h - pico-sdk compatibility shim.
 *
 * ESP-IDF's non-standard additions to FreeRTOS. Only the core-pinning
 * creators are needed by this file set; the *WithCaps variants are referenced
 * only inside #if CONFIG_SPIRAM blocks that are compiled out on this target
 * (see sdkconfig.h), so they are deliberately NOT provided - if a future
 * change switches PSRAM on, the build should fail loudly here rather than
 * silently allocate from the wrong heap.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RP2350 is dual-core and the FreeRTOS RP2350 port is built SMP, so a real
 * affinity mask is available. core_id is honoured when it names core 0 or 1
 * and treated as "either core" otherwise (which is what tskNO_AFFINITY means).
 */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task_code,
                                   const char *name,
                                   const uint32_t stack_depth,
                                   void *parameters,
                                   UBaseType_t priority,
                                   TaskHandle_t *task_handle,
                                   const BaseType_t core_id);

#ifdef __cplusplus
}
#endif
