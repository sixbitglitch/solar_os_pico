/*
 * FreeRTOSConfig.h - SolarOS on RP2350 (Pimoroni Pico Plus 2 W).
 *
 * Based on pico-sdk's own reference configuration for the RP2350 SMP port,
 * adjusted for what SolarOS actually needs. The settings that are NOT
 * defaults, and why:
 *
 *  configNUMBER_OF_CORES 2    RP2350 is dual-core and the jobs framework
 *                             expects real concurrency. This is also what
 *                             makes xTaskCreatePinnedToCore's affinity
 *                             argument meaningful rather than ignored.
 *  configUSE_CORE_AFFINITY 1  Required for vTaskCoreAffinitySet, which the
 *                             ESP-IDF core-pinning shim maps onto.
 *  configTOTAL_HEAP_SIZE      RP2350 has 520 KB SRAM. 256 KB is left to the
 *                             FreeRTOS heap; the rest covers .bss/.data
 *                             (the display shadow buffers are the big
 *                             consumers) and the main stack. This number is
 *                             a starting point, NOT a measured budget - it
 *                             has never been validated on hardware.
 *  configUSE_TASK_NOTIFICATIONS  The jobs framework's stop protocol is built
 *                             entirely on ulTaskNotifyTake/xTaskNotifyGive.
 *  configCHECK_FOR_STACK_OVERFLOW / configUSE_MALLOC_FAILED_HOOK
 *                             On during bring-up: this port has never run,
 *                             so failing loudly beats corrupting memory.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <assert.h>

#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                ((configSTACK_DEPTH_TYPE)512)
#define configMAX_TASK_NAME_LEN                 24
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
/* Backs heap_caps_* in the compatibility shim. See the note above before
 * changing: task admission decisions are made against this heap's free size. */
#define configTOTAL_HEAP_SIZE                   (256 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configGENERATE_RUN_TIME_STATS           0
/* The `top` shell command enumerates tasks through the trace facility. */
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configUSE_CO_ROUTINES                   0

#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                16
#define configTIMER_TASK_STACK_DEPTH            1024

#if FREE_RTOS_KERNEL_SMP /* set by the RP2xxx SMP port */
#ifndef configNUMBER_OF_CORES
#define configNUMBER_OF_CORES                   2
#endif
#define configNUM_CORES                         configNUMBER_OF_CORES
#define configTICK_CORE                         0
#define configRUN_MULTIPLE_PRIORITIES           1
#if configNUMBER_OF_CORES > 1
#define configUSE_CORE_AFFINITY                 1
#endif
#define configUSE_PASSIVE_IDLE_HOOK             0
#endif

/* Let FreeRTOS primitives and pico-sdk's own sync/time primitives interoperate
 * (a pico-sdk mutex taken from a FreeRTOS task blocks the task, not the core).
 * The SD and display drivers call pico-sdk sleep helpers from tasks, so this
 * matters. */
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1

#define configASSERT(x)                         assert(x)

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1

/*
 * Cortex-M33 port options. RP2350's Arm cores have an FPU, and the
 * RP2350_ARM_NTZ port errors out at compile time unless configENABLE_FPU is
 * stated explicitly. TrustZone is off (this is the NTZ - non-TrustZone -
 * port), and with no secure/non-secure split the kernel runs "secure only".
 */
#if PICO_RP2350
#define configENABLE_FPU                        1
#define configENABLE_MVE                        0
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
/*
 * Interrupts at or above this NVIC priority value are masked inside kernel
 * critical sections; only interrupts at or below it may call FromISR APIs.
 * 16 is what pico-sdk's own RP2350 FreeRTOS reference configuration uses -
 * RP2350's Cortex-M33 implements 4 priority bits in the top nibble, so 16 is
 * one step below the highest (0) and leaves the top level for latency-critical
 * handlers that never touch the kernel.
 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16
#endif

#endif /* FREERTOS_CONFIG_H */
