/*
 * uart_port_pico.c - RP2350 backend for the solar_os_uart service's driver
 * interface (src/drivers/uart_port.h), against pico-sdk hardware_uart.
 *
 * The original src/drivers/uart_port.c is excluded from this target's build
 * (see boards/drivers/uart_pico.cmake): it is written directly against
 * ESP-IDF's ring-buffered UART driver (uart_driver_install/uart_write_bytes/
 * uart_read_bytes/uart_detect_bitrate_start), which has no RP2350
 * equivalent to shim onto. This file implements the same solar_os-facing
 * contract directly: an IRQ-driven RX ring buffer (pico-sdk's hardware_uart
 * only exposes "is a byte available", not a buffered byte count, so the
 * count solar_os_uart_bus_rx_buffered() needs is tracked by hand) and a
 * blocking write via uart_write_blocking().
 *
 * NOT IMPLEMENTED: autobaud detection (uart_port_autobaud_start/stop/cancel).
 * ESP-IDF measures an unknown baud rate by timing RX edge widths in hardware
 * (uart_detect_bitrate_start). RP2350's UART block has no equivalent
 * free-running bit-timing counter; a from-scratch implementation would need
 * a PIO program timestamping edges on the RX pin before the UART peripheral
 * claims it, which is a substantial, timing-critical piece of work this pass
 * did not attempt - see doc/ports/picocalc.md. These three calls return
 * ESP_ERR_NOT_SUPPORTED rather than pretending to measure a rate that was
 * never actually sampled.
 */

/* hardware/uart.h must come first: it and the compat driver/uart.h shim
 * (pulled in by uart_port.h below) both define a type named uart_parity_t,
 * and the shim skips its own definition once _HARDWARE_UART_H is already
 * defined - see that header's comment. */
#include "hardware/uart.h"

#include "uart_port.h"

#include <string.h>

#include "FreeRTOS.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "semphr.h"
#include "task.h"

#define UART_PORT_RX_RING_SIZE 256U

typedef struct {
    uart_inst_t *inst;
    uint irq_num;
    volatile uint8_t rx_ring[UART_PORT_RX_RING_SIZE];
    volatile size_t rx_head; /* next slot the ISR will write */
    volatile size_t rx_tail; /* next slot the reader will take */
    SemaphoreHandle_t rx_ready; /* counting semaphore, given once per byte */
} uart_port_state_t;

static uart_port_state_t uart_states[UART_NUM_MAX];
static uart_port_config_t active_configs[UART_NUM_MAX];
static bool ready[UART_NUM_MAX];

static bool valid_port(uart_port_t port_num)
{
    return port_num >= UART_NUM_0 && port_num < UART_NUM_MAX;
}

static uart_inst_t *uart_inst_for(uart_port_t port_num)
{
    return port_num == UART_NUM_0 ? uart0 : uart1;
}

/* Runs in interrupt context: drains the peripheral's hardware FIFO into the
 * ring buffer. A full ring drops the newest bytes rather than overwriting
 * unread ones - callers see this as a gap, not corrupted data. */
static void uart_port_rx_isr(uart_port_state_t *state)
{
    while (uart_is_readable(state->inst)) {
        const uint8_t byte = (uint8_t)uart_getc(state->inst);
        const size_t next_head = (state->rx_head + 1U) % UART_PORT_RX_RING_SIZE;
        if (next_head == state->rx_tail) {
            continue; /* ring full: drop the byte */
        }
        state->rx_ring[state->rx_head] = byte;
        state->rx_head = next_head;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(state->rx_ready, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void uart0_isr_trampoline(void) { uart_port_rx_isr(&uart_states[UART_NUM_0]); }
static void uart1_isr_trampoline(void) { uart_port_rx_isr(&uart_states[UART_NUM_1]); }

esp_err_t uart_port_init(const uart_port_config_t *config)
{
    if (config == NULL || !valid_port(config->port_num) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->tx_pin) ||
        !GPIO_IS_VALID_GPIO(config->rx_pin) ||
        config->tx_pin == config->rx_pin || config->baud_rate == 0 ||
        config->rx_buffer_size == 0 || config->tx_buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uart_port_state_t *state = &uart_states[config->port_num];

    if (!ready[config->port_num]) {
        state->inst = uart_inst_for(config->port_num);
        state->irq_num = config->port_num == UART_NUM_0 ? UART0_IRQ : UART1_IRQ;
        state->rx_head = 0;
        state->rx_tail = 0;
        if (state->rx_ready == NULL) {
            state->rx_ready = xSemaphoreCreateCounting(UART_PORT_RX_RING_SIZE, 0);
            if (state->rx_ready == NULL) {
                return ESP_ERR_NO_MEM;
            }
        }

        uart_init(state->inst, config->baud_rate);
        gpio_set_function(config->tx_pin, GPIO_FUNC_UART);
        gpio_set_function(config->rx_pin, GPIO_FUNC_UART);
        uart_set_format(state->inst, 8, 1, UART_PARITY_NONE);
        uart_set_fifo_enabled(state->inst, true);

        irq_set_exclusive_handler(state->irq_num,
                                  config->port_num == UART_NUM_0 ?
                                      uart0_isr_trampoline : uart1_isr_trampoline);
        irq_set_enabled(state->irq_num, true);
        uart_set_irq_enables(state->inst, true, false);

        ready[config->port_num] = true;
    } else {
        uart_set_baudrate(state->inst, config->baud_rate);
    }

    active_configs[config->port_num] = *config;
    return ESP_OK;
}

esp_err_t uart_port_deinit(uart_port_t port_num)
{
    if (!valid_port(port_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ready[port_num]) {
        return ESP_OK;
    }

    uart_port_state_t *state = &uart_states[port_num];
    uart_set_irq_enables(state->inst, false, false);
    irq_set_enabled(state->irq_num, false);
    uart_deinit(state->inst);

    const uart_port_config_t config = active_configs[port_num];
    ready[port_num] = false;
    active_configs[port_num] = (uart_port_config_t) {0};
    (void)gpio_reset_pin(config.tx_pin);
    (void)gpio_reset_pin(config.rx_pin);
    return ESP_OK;
}

bool uart_port_is_ready(uart_port_t port_num)
{
    return valid_port(port_num) && ready[port_num];
}

esp_err_t uart_port_set_baud_rate(uart_port_t port_num, uint32_t baud_rate)
{
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (baud_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uart_set_baudrate(uart_states[port_num].inst, baud_rate);
    active_configs[port_num].baud_rate = baud_rate;
    return ESP_OK;
}

esp_err_t uart_port_autobaud_start(uart_port_t port_num)
{
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uart_port_autobaud_stop(uart_port_t port_num,
                                  uart_port_autobaud_result_t *result)
{
    (void)port_num;
    if (result != NULL) {
        *result = (uart_port_autobaud_result_t) {0};
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uart_port_autobaud_cancel(uart_port_t port_num)
{
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t uart_port_write(uart_port_t port_num,
                          const uint8_t *data,
                          size_t len,
                          size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    uart_write_blocking(uart_states[port_num].inst, data, len);
    if (written != NULL) {
        *written = len;
    }
    return ESP_OK;
}

esp_err_t uart_port_read(uart_port_t port_num,
                         uint8_t *data,
                         size_t len,
                         uint32_t timeout_ms,
                         size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    uart_port_state_t *state = &uart_states[port_num];
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    size_t count = 0;

    while (count < len) {
        TickType_t remaining;
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            if (count > 0) {
                break;
            }
            remaining = 0;
        } else {
            remaining = deadline - now;
        }

        if (xSemaphoreTake(state->rx_ready, remaining) != pdTRUE) {
            break;
        }
        taskENTER_CRITICAL();
        data[count] = state->rx_ring[state->rx_tail];
        state->rx_tail = (state->rx_tail + 1U) % UART_PORT_RX_RING_SIZE;
        taskEXIT_CRITICAL();
        count++;
    }

    if (read_len != NULL) {
        *read_len = count;
    }
    return ESP_OK;
}

esp_err_t uart_port_get_rx_buffered(uart_port_t port_num, size_t *buffered)
{
    if (buffered == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *buffered = 0;
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }

    const uart_port_state_t *state = &uart_states[port_num];
    taskENTER_CRITICAL();
    *buffered = (state->rx_head + UART_PORT_RX_RING_SIZE - state->rx_tail) %
        UART_PORT_RX_RING_SIZE;
    taskEXIT_CRITICAL();
    return ESP_OK;
}

bool uart_port_get_config(uart_port_t port_num, uart_port_config_t *config)
{
    if (!uart_port_is_ready(port_num) || config == NULL) {
        return false;
    }
    *config = active_configs[port_num];
    return true;
}
