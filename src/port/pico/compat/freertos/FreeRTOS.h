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
/*
 * task.h is where taskENTER_CRITICAL()/taskEXIT_CRITICAL() are actually
 * defined (as taskENTER_CRITICAL() -> portENTER_CRITICAL(), the real
 * zero-argument port macro backed by vTaskEnterCritical() in tasks.c). Pull
 * it in here, before redefining portENTER_CRITICAL below, so that
 * taskENTER_CRITICAL/taskEXIT_CRITICAL are already valid macros by the time
 * any shared source's own "freertos/FreeRTOS.h" + "freertos/task.h" include
 * pair reaches this file - a shared source that includes only
 * "freertos/FreeRTOS.h" first (the common order) would otherwise see
 * taskENTER_CRITICAL as a plain, undeclared identifier at this point in the
 * file, since it is defined by task.h, not FreeRTOS.h itself. */
#include <task.h>

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

/*
 * Most of this tree calls the plain upstream taskENTER_CRITICAL()/
 * taskEXIT_CRITICAL() (zero arguments), which task.h itself expands to the
 * zero-argument portENTER_CRITICAL()/portEXIT_CRITICAL() already provided by
 * portmacro.h (SMP RP2350: vTaskEnterCritical()/vTaskExitCritical()). Only
 * src/solar_os_task.c calls the ESP-IDF spinlock-argument form,
 * portENTER_CRITICAL(&mux). A plain "#define portENTER_CRITICAL(mux)"
 * would break every zero-argument caller, since C macros pick exactly one
 * parameter count per name - so this is a variadic macro instead: it matches
 * both portENTER_CRITICAL() and portENTER_CRITICAL(&mux), discards whatever
 * (if anything) was passed, and calls the real underlying primitive
 * directly. It deliberately does NOT go through taskENTER_CRITICAL(), which
 * would expand back to portENTER_CRITICAL() and recurse into this same
 * macro (the preprocessor blocks that by leaving the innermost occurrence
 * unexpanded, which is worse: an undeclared-identifier build error instead
 * of working code).
 */
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(...) vTaskEnterCritical()
#define portEXIT_CRITICAL(...)  vTaskExitCritical()

#define portENTER_CRITICAL_ISR(mux) \
    do { (void)(mux); taskENTER_CRITICAL_FROM_ISR(); } while (0)
#define portEXIT_CRITICAL_ISR(mux) \
    do { (void)(mux); taskEXIT_CRITICAL_FROM_ISR(0); } while (0)

#define portENTER_CRITICAL_SAFE(mux) portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_SAFE(mux)  portEXIT_CRITICAL(mux)

/*
 * ESP-IDF-only: "am I currently executing in an interrupt handler". Upstream
 * FreeRTOS calls the equivalent xPortIsInsideInterrupt(), which this
 * Community-Supported RP2350 port does not implement either, so this reads
 * the Cortex-M IPSR register directly (the standard portable way to answer
 * this question on any Arm-M core: IPSR is 0 in thread mode and the
 * exception number in handler mode).
 */
static inline BaseType_t xPortInIsrContext(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return (BaseType_t)(ipsr != 0);
}

#ifdef __cplusplus
}
#endif
