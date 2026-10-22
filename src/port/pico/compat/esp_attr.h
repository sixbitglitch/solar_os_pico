/*
 * esp_attr.h - pico-sdk compatibility shim.
 *
 * With PSRAM enabled, EXT_RAM_BSS_ATTR places large SolarOS static tables in
 * the APS6404 via pico-sdk's .psram_uninitialised section. That section is
 * NOLOAD (not zeroed by crt0); solar_os_psram_bss_clear() zeroes it after
 * hardware_psram is up so it matches ESP-IDF SPIRAM BSS semantics.
 * IRAM/DRAM/RTC attributes have no RP2350 counterpart and stay empty.
 */
#pragma once

#include "pico/platform/sections.h"

#define EXT_RAM_BSS_ATTR __uninitialized_psram("solar_os")
#define EXT_RAM_NOINIT_ATTR __uninitialized_psram("solar_os")
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_IRAM_ATTR
#define __NOINIT_ATTR
#define NOINLINE_ATTR __attribute__((noinline))
#define FORCE_INLINE_ATTR __attribute__((always_inline)) inline
