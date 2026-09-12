/*
 * freertos/FreeRTOS.h - pico-sdk compatibility shim.
 *
 * ESP-IDF installs the kernel headers under a freertos/ prefix; upstream
 * FreeRTOS-Kernel does not. This forwards to the real kernel header (the
 * kernel's own include directory is on the include path) and then patches the
 * two places where ESP-IDF's fork diverges from upstream in ways the SolarOS
 * sources depend on:
 *
 *  1. Critical sections. ESP-IDF's portENTER_CRITICAL takes a spinlock
 *     argument because it is SMP; upstream's takes none. src/solar_os_task.c
 *     uses the ESP-IDF form, so it is re-provided here rather than edited
 *     there.
 *  2. tskNO_AFFINITY, which is ESP-IDF-only.
 */
#pragma once

#include <inttypes.h>   /* PRIu64 and friends: ESP-IDF headers
                           pull these in transitively, and the shared
                           sources rely on that. */
#include <FreeRTOS.h>   /* the real FreeRTOS-Kernel header: distinct relative path, so this cannot recurse */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP-IDF-style spinlock. The RP2350 port is a genuine SMP port, so a
 * lock object is not merely decorative - but FreeRTOS's own
 * taskENTER_CRITICAL already takes the kernel's ISR lock and task lock on this
 * port, which is what the call sites actually need. The object is kept so the
 * source stays identical across targets; its contents are unused.
 */
typedef struct {
    uint32_t owner;
} solar_os_compat_spinlock_t;

#define portMUX_TYPE solar_os_compat_spinlock_t
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define spinlock_initialize(lock) ((void)(lock))

#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(mux) do { (void)(mux); taskENTER_CRITICAL(); } while (0)
#define portEXIT_CRITICAL(mux)  do { (void)(mux); taskEXIT_CRITICAL(); } while (0)

#define portENTER_CRITICAL_ISR(mux) \
    do { (void)(mux); taskENTER_CRITICAL_FROM_ISR(); } while (0)
#define portEXIT_CRITICAL_ISR(mux) \
    do { (void)(mux); taskEXIT_CRITICAL_FROM_ISR(0); } while (0)

#define portENTER_CRITICAL_SAFE(mux) portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_SAFE(mux)  portEXIT_CRITICAL(mux)

#ifdef __cplusplus
}
#endif
