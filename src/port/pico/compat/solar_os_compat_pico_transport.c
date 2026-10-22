/*
 * esp_transport_* stubs for Pico bring-up. Enough for solar_os_net_session.c
 * to link; TCP/TLS/WS sessions return errors until a full port lands.
 */

#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TRANSPORT_KIND_TCP,
    TRANSPORT_KIND_SSL,
    TRANSPORT_KIND_WS,
} transport_kind_t;

struct esp_transport_item_t {
    transport_kind_t kind;
    esp_transport_handle_t parent;
    char ws_path[256];
};

static esp_transport_handle_t transport_alloc(transport_kind_t kind,
                                              esp_transport_handle_t parent)
{
    struct esp_transport_item_t *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->kind = kind;
    t->parent = parent;
    return t;
}

esp_transport_handle_t esp_transport_tcp_init(void)
{
    return transport_alloc(TRANSPORT_KIND_TCP, NULL);
}

esp_transport_handle_t esp_transport_ssl_init(void)
{
    return transport_alloc(TRANSPORT_KIND_SSL, NULL);
}

void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t t,
                                         transport_crt_bundle_attach_t attach_fn)
{
    (void)t;
    (void)attach_fn;
}

esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent)
{
    return transport_alloc(TRANSPORT_KIND_WS, parent);
}

esp_err_t esp_transport_ws_set_config(esp_transport_handle_t t,
                                      const esp_transport_ws_config_t *config)
{
    if (t == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->ws_path != NULL) {
        strncpy(t->ws_path, config->ws_path, sizeof(t->ws_path) - 1U);
        t->ws_path[sizeof(t->ws_path) - 1U] = '\0';
    }
    return ESP_OK;
}

int esp_transport_connect(esp_transport_handle_t t,
                          const char *host,
                          int port,
                          int timeout_ms)
{
    (void)t;
    (void)host;
    (void)port;
    (void)timeout_ms;
    return -1;
}

int esp_transport_read(esp_transport_handle_t t,
                       char *buffer,
                       int len,
                       int timeout_ms)
{
    (void)t;
    (void)buffer;
    (void)len;
    (void)timeout_ms;
    return -1;
}

int esp_transport_poll_read(esp_transport_handle_t t, int timeout_ms)
{
    (void)t;
    (void)timeout_ms;
    return -1;
}

esp_err_t esp_transport_close(esp_transport_handle_t t)
{
    (void)t;
    return ESP_OK;
}

esp_err_t esp_transport_destroy(esp_transport_handle_t t)
{
    free(t);
    return ESP_OK;
}

int esp_transport_ws_send_raw(esp_transport_handle_t t,
                              ws_transport_opcodes_t opcode,
                              const char *data,
                              int len,
                              int timeout_ms)
{
    (void)t;
    (void)opcode;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return -1;
}

int esp_transport_ws_get_read_payload_len(esp_transport_handle_t t)
{
    (void)t;
    return 0;
}

ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t t)
{
    (void)t;
    return WS_TRANSPORT_OPCODES_BINARY;
}

bool esp_transport_ws_get_fin_flag(esp_transport_handle_t t)
{
    (void)t;
    return true;
}
