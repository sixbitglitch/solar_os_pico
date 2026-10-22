#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WS_TRANSPORT_OPCODES_CONT = 0x00,
    WS_TRANSPORT_OPCODES_TEXT = 0x01,
    WS_TRANSPORT_OPCODES_BINARY = 0x02,
    WS_TRANSPORT_OPCODES_CLOSE = 0x08,
    WS_TRANSPORT_OPCODES_PING = 0x09,
    WS_TRANSPORT_OPCODES_PONG = 0x0A,
    WS_TRANSPORT_OPCODES_FIN = 0x80,
} ws_transport_opcodes_t;

typedef struct {
    const char *ws_path;
    const char *sub_protocol;
    const char *user_agent;
    const char *headers;
    bool propagate_control_frames;
} esp_transport_ws_config_t;

esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent);
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t t,
                                      const esp_transport_ws_config_t *config);
int esp_transport_ws_send_raw(esp_transport_handle_t t,
                              ws_transport_opcodes_t opcode,
                              const char *data,
                              int len,
                              int timeout_ms);
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t t);
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t t);
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t t);

#ifdef __cplusplus
}
#endif
