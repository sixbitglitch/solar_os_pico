/*
 * esp_http_server.h - ESP-IDF HTTP server API subset for the Pico lwIP port.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct httpd_handle *httpd_handle_t;
typedef struct httpd_req httpd_req_t;

typedef enum {
    HTTP_GET = 1,
    HTTP_POST,
    HTTP_PUT,
    HTTP_PATCH,
    HTTP_DELETE,
    HTTP_HEAD,
    HTTP_NOTIFY,
    HTTP_OPTIONS,
    HTTP_COPY,
    HTTP_MOVE,
    HTTP_LOCK,
    HTTP_UNLOCK,
    HTTP_PROPFIND,
    HTTP_PROPPATCH,
    HTTP_MKCOL,
    HTTP_REPORT,
    HTTP_MAX,
} httpd_method_t;

typedef bool (*httpd_uri_match_fn_t)(const char *reference_uri,
                                     const char *uri_to_match,
                                     size_t match_upto);

typedef esp_err_t (*httpd_handler_t)(httpd_req_t *req);

typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_handler_t handler;
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    unsigned task_priority;
    uint32_t stack_size;
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t max_resp_headers;
    uint16_t backlog_conn;
    bool lru_purge_enable;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    void *global_user_ctx;
    void *global_transport_ctx;
    httpd_uri_match_fn_t uri_match_fn;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG()                       \
    {                                                \
        .task_priority = 5,                          \
        .stack_size = 4096,                          \
        .server_port = 80,                           \
        .ctrl_port = 32768,                          \
        .max_open_sockets = 4,                       \
        .max_uri_handlers = 8,                       \
        .max_resp_headers = 8,                       \
        .backlog_conn = 5,                           \
        .lru_purge_enable = false,                     \
        .recv_wait_timeout = 5,                      \
        .send_wait_timeout = 5,                       \
        .global_user_ctx = NULL,                     \
        .global_transport_ctx = NULL,                \
        .uri_match_fn = NULL,                        \
    }

#define HTTPD_RESP_USE_STRLEN (-1)

#ifndef CONFIG_HTTPD_MAX_URI_LEN
#define CONFIG_HTTPD_MAX_URI_LEN 512
#endif

#define HTTPD_TYPE_JSON   "application/json"
#define HTTPD_TYPE_TEXT   "text/plain"
#define HTTPD_TYPE_OCTET  "application/octet-stream"
#define HTTPD_TYPE_HTML   "text/html"

typedef enum {
    HTTPD_200_OK = 200,
    HTTPD_204_NO_CONTENT = 204,
    HTTPD_301_MOVED_PERMANENTLY = 301,
    HTTPD_302_FOUND = 302,
    HTTPD_304_NOT_MODIFIED = 304,
    HTTPD_400_BAD_REQUEST = 400,
    HTTPD_401_UNAUTHORIZED = 401,
    HTTPD_403_FORBIDDEN = 403,
    HTTPD_404_NOT_FOUND = 404,
    HTTPD_414_URI_TOO_LONG = 414,
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
    HTTPD_503_SERVICE_UNAVAILABLE = 503,
} httpd_err_code_t;

struct httpd_req {
    httpd_handle_t handle;
    httpd_method_t method;
    char *uri;
    int content_len;
    void *sess_ctx;
    void *user_ctx;
    void *aux;
};

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,
                                      const char *field,
                                      char *val,
                                      size_t val_size);
int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len);

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, size_t buf_len);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *str);
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *chunk, size_t chunk_len);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req, const char *str);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg);
esp_err_t httpd_resp_send_404(httpd_req_t *req);

bool httpd_uri_match_wildcard(const char *uri_template,
                              const char *uri_to_match,
                              size_t match_upto);

#ifdef __cplusplus
}
#endif
