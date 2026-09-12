/*
 * inttypes.h - toolchain gap filler for the arm-none-eabi + newlib combination
 * this port is built with.
 *
 * THIS IS NOT AN ESP-IDF SHIM. It works around a real header-packaging quirk:
 *
 * newlib's <inttypes.h> guards the 64-bit format macros (PRId64, PRIu64,
 * PRIx64, SCNu64, ...) behind __int64_t_defined, which newlib's own <stdint.h>
 * is responsible for setting. But GCC ships its own <stdint.h>, and on this
 * toolchain that is the one that wins the include search:
 *
 *   .. /usr/lib/gcc/arm-none-eabi/13.2.1/include/stdint.h
 *   ... /usr/include/newlib/inttypes.h
 *
 * GCC's stdint.h does not define __int64_t_defined, so newlib's guard never
 * opens and PRIu64 comes out undefined even in a file that includes
 * <inttypes.h> correctly. Roughly a dozen SolarOS sources print 64-bit values
 * and hit this.
 *
 * Rather than edit those sources (they are correct as written) or define
 * __int64_t_defined globally on the command line (which reaches into newlib's
 * internals and would silently change behaviour if the toolchain is fixed),
 * this forwards to the real header and then fills in only what is actually
 * missing. On a toolchain where newlib's guard works, every #ifndef below is
 * already satisfied and this file adds nothing.
 *
 * The definitions match what newlib's __PRI64/__SCN64 would expand to: on
 * arm-none-eabi, int64_t is long long, so the length modifier is "ll".
 */
#pragma once

#include_next <inttypes.h>

/* Sanity: if int64_t really is not long long, the macros below would be wrong.
 * Fail loudly rather than print garbage. */
#include <stdint.h>

#ifndef PRId64
#define PRId64 "lld"
#endif
#ifndef PRIi64
#define PRIi64 "lli"
#endif
#ifndef PRIo64
#define PRIo64 "llo"
#endif
#ifndef PRIu64
#define PRIu64 "llu"
#endif
#ifndef PRIx64
#define PRIx64 "llx"
#endif
#ifndef PRIX64
#define PRIX64 "llX"
#endif

#ifndef SCNd64
#define SCNd64 "lld"
#endif
#ifndef SCNi64
#define SCNi64 "lli"
#endif
#ifndef SCNo64
#define SCNo64 "llo"
#endif
#ifndef SCNu64
#define SCNu64 "llu"
#endif
#ifndef SCNx64
#define SCNx64 "llx"
#endif

#ifndef PRIdLEAST64
#define PRIdLEAST64 PRId64
#endif
#ifndef PRIuLEAST64
#define PRIuLEAST64 PRIu64
#endif
#ifndef PRIxLEAST64
#define PRIxLEAST64 PRIx64
#endif
#ifndef PRIdFAST64
#define PRIdFAST64 PRId64
#endif
#ifndef PRIuFAST64
#define PRIuFAST64 PRIu64
#endif
#ifndef PRIxFAST64
#define PRIxFAST64 PRIx64
#endif
