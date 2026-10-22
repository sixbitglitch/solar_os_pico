/*
 * esp_transport.h - ESP-IDF tcp_transport subset for the Pico lwIP port.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_transport_item_t *esp_transport_handle_t;

typedef esp_err_t (*transport_crt_bundle_attach_t)(void *conf);

int esp_transport_connect(esp_transport_handle_t t,
                          const char *host,
                          int port,
                          int timeout_ms);
int esp_transport_read(esp_transport_handle_t t, char *buffer, int len, int timeout_ms);
int esp_transport_poll_read(esp_transport_handle_t t, int timeout_ms);
esp_err_t esp_transport_close(esp_transport_handle_t t);
esp_err_t esp_transport_destroy(esp_transport_handle_t t);

#ifdef __cplusplus
}
#endif
