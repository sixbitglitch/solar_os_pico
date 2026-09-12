/*
 * esp_attr.h - pico-sdk compatibility shim.
 *
 * ESP-IDF section-placement attributes. On RP2350 there is no PSRAM in use
 * and no IRAM/flash cache split to manage from C, so every one of these
 * collapses to nothing and the objects land in ordinary .bss/.text.
 * EXT_RAM_BSS_ATTR in particular is what moves a few large static arrays back
 * from PSRAM into .bss - see doc/ports/picocalc.md section 2.
 */
#pragma once

#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_IRAM_ATTR
#define __NOINIT_ATTR
#define NOINLINE_ATTR __attribute__((noinline))
#define FORCE_INLINE_ATTR __attribute__((always_inline)) inline
