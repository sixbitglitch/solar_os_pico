#pragma once

/*
 * Hand-written ESP-IDF sdkconfig stand-in for the PicoCalc (RP2350) target.
 * See doc/ports/picocalc.md §2 and §4.1.
 *
 * CONFIG_SPIRAM=1 enables the SolarOS PSRAM paths (queues via WithCaps, memory
 * policy preferring external). The Pico Plus 2 W APS6404 is brought up by
 * hardware_psram and exposed through solar_os_psram_heap.
 */

#define CONFIG_SPIRAM 1
#define CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY 1
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 0

#define CONFIG_IDF_TARGET_ESP32 0
#define CONFIG_IDF_TARGET_ESP32S2 0
#define CONFIG_IDF_TARGET_ESP32S3 0
#define CONFIG_IDF_TARGET_ESP32C3 0
#define CONFIG_IDF_TARGET_ESP32C6 0
#define CONFIG_IDF_TARGET_ESP32H2 0
#define CONFIG_IDF_TARGET_ESP32P4 0
