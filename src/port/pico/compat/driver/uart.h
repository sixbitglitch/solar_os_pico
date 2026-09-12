/*
 * driver/uart.h - pico-sdk compatibility shim.
 *
 * Only the port/parity/stop-bit enums are needed: shell/solar_os_shell_expansion.c
 * uses them to describe a UART binding. Transfers go through
 * src/services/solar_os_uart.c and the board UART driver.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int uart_port_t;

#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_MAX 2

typedef enum {
    UART_DATA_5_BITS = 0,
    UART_DATA_6_BITS,
    UART_DATA_7_BITS,
    UART_DATA_8_BITS,
} uart_word_length_t;

typedef enum {
    UART_STOP_BITS_1 = 1,
    UART_STOP_BITS_1_5,
    UART_STOP_BITS_2,
} uart_stop_bits_t;

typedef enum {
    UART_PARITY_DISABLE = 0,
    UART_PARITY_EVEN = 2,
    UART_PARITY_ODD = 3,
} uart_parity_t;

typedef enum {
    UART_HW_FLOWCTRL_DISABLE = 0,
    UART_HW_FLOWCTRL_RTS,
    UART_HW_FLOWCTRL_CTS,
    UART_HW_FLOWCTRL_CTS_RTS,
} uart_hw_flowcontrol_t;

#ifdef __cplusplus
}
#endif
