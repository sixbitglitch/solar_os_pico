/*
 * solar_os_pico_main.c - RP2350 entry point.
 *
 * ESP-IDF calls app_main() from a FreeRTOS task it has already started, with
 * the scheduler running. pico-sdk has no such arrangement: main() runs on the
 * bare C runtime with no scheduler, and vTaskStartScheduler() never returns.
 *
 * This file is the adapter between the two, and it is why src/main.c needs no
 * edits: app_main() is still called exactly once, from a task, with the
 * scheduler running and with stdio already up.
 */

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pico/stdlib.h"

static const char *TAG = "boot";

/* ESP-IDF's application entry point, defined in src/main.c. */
extern void app_main(void);

/*
 * Stack for the task that runs app_main().
 *
 * app_main() performs the whole boot sequence - board bring-up, service
 * registration, launching the shell - before returning, so this is sized well
 * above a default worker. The figure is a starting point and has never been
 * measured against a real high-water mark; configCHECK_FOR_STACK_OVERFLOW is
 * on so an undersized stack reports itself rather than corrupting memory.
 */
#define SOLAR_OS_MAIN_TASK_STACK 8192U
#define SOLAR_OS_MAIN_TASK_PRIORITY (tskIDLE_PRIORITY + 5)

static void solar_os_main_task(void *arg)
{
    (void)arg;

    app_main();

    /*
     * ESP-IDF lets app_main() return and keeps the system running on its other
     * tasks. Deleting this task reproduces that: the scheduler carries on with
     * the shell, jobs and driver workers app_main() started.
     */
    ESP_LOGI(TAG, "app_main returned; boot task exiting");
    vTaskDelete(NULL);
}

int main(void)
{
    /* Bring stdio up before anything logs. Which backends exist is decided by
     * pico_enable_stdio_uart/usb in targets/picocalc/CMakeLists.txt. */
    stdio_init_all();

    ESP_LOGI(TAG, "SolarOS on RP2350 starting");

    if (xTaskCreate(solar_os_main_task,
                    "solar_os_main",
                    SOLAR_OS_MAIN_TASK_STACK,
                    NULL,
                    SOLAR_OS_MAIN_TASK_PRIORITY,
                    NULL) != pdPASS) {
        /* Nothing else can run, so say so on the console rather than resetting
         * into an identical failure. */
        panic("could not create the SolarOS boot task");
    }

    vTaskStartScheduler();

    /* Only reached if the scheduler could not start at all. */
    panic("FreeRTOS scheduler failed to start");
    return 0;
}

/*
 * FreeRTOS hooks. Both are enabled in FreeRTOSConfig.h deliberately: this port
 * has never run, so a stack overflow or a failed allocation should announce
 * itself loudly at the moment it happens rather than surface later as
 * inexplicable corruption.
 */
void vApplicationMallocFailedHook(void)
{
    panic("FreeRTOS heap exhausted (configTOTAL_HEAP_SIZE too small?)");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    panic("stack overflow in task '%s'", name != NULL ? name : "?");
}
