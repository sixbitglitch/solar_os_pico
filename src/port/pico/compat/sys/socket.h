/*
 * sys/socket.h - pico-sdk / newlib stand-in.
 *
 * arm-none-eabi newlib does not ship BSD sockets. lwIP does when LWIP_SOCKET
 * is enabled (see targets/picocalc/config/lwipopts.h). Shared SolarOS sources
 * include <sys/socket.h> the way ESP-IDF does; this shim keeps those includes
 * compiling on RP2350.
 */
#pragma once

#include "lwip/opt.h"

#if LWIP_SOCKET
#include "lwip/sockets.h"
#else
/* Translation units may include this header unconditionally while guarding
 * the actual socket calls behind package flags. Provide empty coverage. */
#endif
