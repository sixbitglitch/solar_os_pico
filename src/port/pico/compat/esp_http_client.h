/*
 * esp_http_client.h - ESP-IDF HTTP client API subset for the Pico lwIP port.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_http_client *esp_http_client_handle_t;

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_NOTIFY,
    HTTP_METHOD_SUBSCRIBE,
    HTTP_METHOD_UNSUBSCRIBE,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_COPY,
    HTTP_METHOD_MOVE,
    HTTP_METHOD_LOCK,
    HTTP_METHOD_UNLOCK,
    HTTP_METHOD_PROPFIND,
    HTTP_METHOD_PROPPATCH,
    HTTP_METHOD_MKCOL,
    HTTP_METHOD_REPORT,
    HTTP_METHOD_MAX,
} esp_http_client_method_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_HEADER_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    int data_len;
    void *data;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef esp_err_t (*esp_err_t_cb)(void *conf);

typedef struct {
    const char                  *url;
    esp_http_client_method_t    method;
    const char                  *user_agent;
    int                         timeout_ms;
    bool                        disable_auto_redirect;
    http_event_handle_cb        event_handler;
    int                         buffer_size;
    int                         buffer_size_tx;
    void                        *user_data;
    int                         max_redirection_count;
    esp_err_t_cb                crt_bundle_attach;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,
                                     esp_http_client_method_t method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key,
                                     const char *value);
esp_err_t esp_http_client_delete_all_headers(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
                                         const char *data,
                                         int len);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);
esp_err_t esp_http_client_reset_redirect_counter(esp_http_client_handle_t client);
esp_err_t esp_http_client_cancel_request(esp_http_client_handle_t client);

esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client);
esp_err_t esp_http_client_clear_response_buffer(esp_http_client_handle_t client);

int esp_http_client_get_status_code(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif
