/*
 * ping/ping_sock.h - ESP-IDF ping session API for lwIP ICMP on RP2350.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_ping *esp_ping_handle_t;

typedef struct {
    uint32_t count;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint32_t data_size;
    ip_addr_t target_addr;
} esp_ping_config_t;

#define ESP_PING_DEFAULT_CONFIG() \
    {                             \
        .count = 5,               \
        .interval_ms = 1000,      \
        .timeout_ms = 1000,       \
        .data_size = 32,          \
    }

typedef enum {
    ESP_PING_PROF_SEQNO,
    ESP_PING_PROF_TTL,
    ESP_PING_PROF_REQUEST,
    ESP_PING_PROF_REPLY,
    ESP_PING_PROF_IPADDR,
    ESP_PING_PROF_SIZE,
    ESP_PING_PROF_TIMEGAP,
    ESP_PING_PROF_DURATION,
} esp_ping_profile_t;

typedef void (*esp_ping_callback_t)(esp_ping_handle_t hdl, void *args);

typedef struct {
    void *cb_args;
    esp_ping_callback_t on_ping_success;
    esp_ping_callback_t on_ping_timeout;
    esp_ping_callback_t on_ping_end;
} esp_ping_callbacks_t;

esp_err_t esp_ping_new_session(const esp_ping_config_t *config,
                               const esp_ping_callbacks_t *cbs,
                               esp_ping_handle_t *out_handle);
esp_err_t esp_ping_start(esp_ping_handle_t handle);
esp_err_t esp_ping_stop(esp_ping_handle_t handle);
esp_err_t esp_ping_delete_session(esp_ping_handle_t handle);
esp_err_t esp_ping_get_profile(esp_ping_handle_t handle,
                               esp_ping_profile_t profile,
                               void *data,
                               uint32_t size);

#ifdef __cplusplus
}
#endif
