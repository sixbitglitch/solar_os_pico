#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H

#define HAVE_UNISTD_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SNPRINTF 1
#define HAVE_GETTIMEOFDAY 1
#define LIBSSH2_MBEDTLS 1

/*
 * BSD sockets + struct iovec come from lwIP via the pico port shim
 * (src/port/pico/compat/sys/socket.h). Include early so libssh2_priv.h
 * sees a complete iovec without needing <sys/uio.h>.
 */
#include <sys/socket.h>

#endif
