#pragma once

#include "freertos/FreeRTOS.h"
#include <task.h>   /* the real FreeRTOS-Kernel header: distinct relative path, so this cannot recurse */

/*
 * ESP-IDF-only: "pin this task to no particular core". On the RP2350 SMP port
 * the equivalent is an affinity mask with both cores set. Defined here so that
 * src/solar_os_task.c and the ~40 call sites that pass tskNO_AFFINITY compile
 * unchanged.
 */
#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY ((BaseType_t)0x7FFFFFFF)
#endif
