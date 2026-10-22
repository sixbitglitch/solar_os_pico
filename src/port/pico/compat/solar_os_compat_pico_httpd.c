/*
 * CMake link deps (when Wi-Fi is enabled on this target):
 *   pico_cyw43_arch_lwip_sys_freertos
 *
 * Lightweight HTTP/1.1 server for SolarOS route dispatch on lwIP sockets.
 */

#include "esp_http_server.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys/socket.h"

#define HTTPD_MAX_HANDLERS 16
#define HTTPD_MAX_HEADERS 24
#define HTTPD_HEADER_NAME 64
#define HTTPD_HEADER_VALUE 256
#define HTTPD_URI_BUF 512
#define HTTPD_RECV_BUF 4096

static void compat_strlcpy(char *dst, const char *src, size_t size)
{
    if (size == 0) {
        return;
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

typedef struct {
    char name[HTTPD_HEADER_NAME];
    char value[HTTPD_HEADER_VALUE];
} httpd_header_t;

typedef struct {
    int sock;
    char recv_buf[HTTPD_RECV_BUF];
    size_t recv_len;
    httpd_header_t headers[HTTPD_MAX_HEADERS];
    size_t header_count;
    httpd_method_t method;
    char uri[HTTPD_URI_BUF];
    int content_len;
    size_t body_received;
    bool headers_sent;
    bool chunked;
    char status_line[48];
    char content_type[64];
} httpd_conn_t;

typedef struct httpd_handle {
    httpd_config_t config;
    httpd_uri_t handlers[HTTPD_MAX_HANDLERS];
    size_t handler_count;
    int listen_sock;
    TaskHandle_t task;
    volatile bool running;
} httpd_server_t;

static httpd_method_t httpd_parse_method(const char *token)
{
    if (strcasecmp(token, "GET") == 0) {
        return HTTP_GET;
    }
    if (strcasecmp(token, "POST") == 0) {
        return HTTP_POST;
    }
    if (strcasecmp(token, "PUT") == 0) {
        return HTTP_PUT;
    }
    if (strcasecmp(token, "HEAD") == 0) {
        return HTTP_HEAD;
    }
    if (strcasecmp(token, "DELETE") == 0) {
        return HTTP_DELETE;
    }
    if (strcasecmp(token, "OPTIONS") == 0) {
        return HTTP_OPTIONS;
    }
    return HTTP_GET;
}

bool httpd_uri_match_wildcard(const char *uri_template,
                              const char *uri_to_match,
                              size_t match_upto)
{
    if (uri_template == NULL || uri_to_match == NULL) {
        return false;
    }
    const size_t tpl_len = strlen(uri_template);
    const size_t uri_len = match_upto > 0 ? match_upto : strlen(uri_to_match);
    if (tpl_len > 0 && uri_template[tpl_len - 1] == '*') {
        const size_t prefix = tpl_len - 1;
        return uri_len >= prefix && memcmp(uri_template, uri_to_match, prefix) == 0;
    }
    return strlen(uri_to_match) == uri_len &&
        strncmp(uri_template, uri_to_match, uri_len) == 0;
}

static httpd_conn_t *httpd_req_conn(httpd_req_t *req)
{
    return req != NULL ? (httpd_conn_t *)req->aux : NULL;
}

static int httpd_conn_send(httpd_conn_t *conn, const void *data, size_t len)
{
    if (conn == NULL || conn->sock < 0) {
        return -1;
    }
    size_t sent = 0;
    while (sent < len) {
        const int n = send(conn->sock, (const char *)data + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

static esp_err_t httpd_conn_send_headers(httpd_conn_t *conn)
{
    if (conn == NULL || conn->headers_sent) {
        return ESP_OK;
    }
    char line[512];
    int len = snprintf(line,
                       sizeof(line),
                       "HTTP/1.1 %s\r\n",
                       conn->status_line[0] != '\0' ? conn->status_line : "200 OK");
    if (conn->content_type[0] != '\0') {
        len += snprintf(line + len,
                        sizeof(line) - (size_t)len,
                        "Content-Type: %s\r\n",
                        conn->content_type);
    }
    for (size_t i = 0; i < conn->header_count; i++) {
        len += snprintf(line + len,
                        sizeof(line) - (size_t)len,
                        "%s: %s\r\n",
                        conn->headers[i].name,
                        conn->headers[i].value);
    }
    if (conn->chunked) {
        len += snprintf(line + len,
                        sizeof(line) - (size_t)len,
                        "Transfer-Encoding: chunked\r\n");
    }
    len += snprintf(line + len, sizeof(line) - (size_t)len, "Connection: close\r\n\r\n");
    if (httpd_conn_send(conn, line, (size_t)len) != len) {
        return ESP_FAIL;
    }
    conn->headers_sent = true;
    return ESP_OK;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field)
{
    httpd_conn_t *conn = httpd_req_conn(req);
    if (conn == NULL || field == NULL) {
        return 0;
    }
    for (size_t i = 0; i < conn->header_count; i++) {
        if (strcasecmp(conn->headers[i].name, field) == 0) {
            return strlen(conn->headers[i].value);
        }
    }
    return 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,
                                      const char *field,
                                      char *val,
                                      size_t val_size)
{
    if (req == NULL || field == NULL || val == NULL || val_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_conn_t *conn = httpd_req_conn(req);
    if (conn == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < conn->header_count; i++) {
        if (strcasecmp(conn->headers[i].name, field) == 0) {
            compat_strlcpy(val, conn->headers[i].value, val_size);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len)
{
    httpd_conn_t *conn = httpd_req_conn(req);
    if (conn == NULL || buf == NULL || buf_len == 0) {
        return ESP_FAIL;
    }
    if (conn->content_len >= 0 &&
        conn->body_received >= (size_t)conn->content_len) {
        return 0;
    }
    const int n = recv(conn->sock, buf, buf_len, 0);
    if (n <= 0) {
        return n;
    }
    conn->body_received += (size_t)n;
    return n;
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    httpd_conn_t *conn = httpd_req_conn(req);
    if (conn == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    compat_strlcpy(conn->status_line, status, sizeof(conn->status_line));
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    httpd_conn_t *conn = httpd_req_conn(req);
    if (conn == NULL || type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    compat_strlcpy(conn->content_type, type, sizeof(conn->content_type));
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value)
{
    httpd_conn_t *conn = httpd_req_conn(req);
    if (conn == NULL || field == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (conn->header_count >= HTTPD_MAX_HEADERS) {
        return ESP_ERR_NO_MEM;
    }
    compat_strlcpy(conn->headers[conn->header_count].name, field, HTTPD_HEADER_NAME);
    compat_strlcpy(conn->headers[conn->header_count].value, value, HTTPD_HEADER_VALUE);
    conn->header_count++;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, size_t buf_len)
{
    httpd_conn_t *conn = httpd_req_conn(req);
    if (conn == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((int)buf_len == HTTPD_RESP_USE_STRLEN && buf != NULL) {
        buf_len = strlen(buf);
    }
    if (!conn->chunked) {
        char cl_value[24];
        snprintf(cl_value, sizeof(cl_value), "%u", (unsigned)buf_len);
        (void)httpd_resp_set_hdr(req, "Content-Length", cl_value);
    }
    esp_err_t err = httpd_conn_send_headers(conn);
    if (err != ESP_OK) {
        return err;
    }
    if (buf != NULL && buf_len > 0) {
        if (httpd_conn_send(conn, buf, buf_len) != (int)buf_len) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *str)
{
    return httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *chunk, size_t chunk_len)
{
    httpd_conn_t *conn = httpd_req_conn(req);
    if (conn == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!conn->headers_sent) {
        conn->chunked = true;
        esp_err_t err = httpd_conn_send_headers(conn);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (chunk == NULL && chunk_len == 0) {
        const char end[] = "0\r\n\r\n";
        return httpd_conn_send(conn, end, sizeof(end) - 1) == (int)(sizeof(end) - 1) ?
            ESP_OK :
            ESP_FAIL;
    }
    char header[16];
    const int hlen = snprintf(header, sizeof(header), "%x\r\n", (unsigned)chunk_len);
    if (httpd_conn_send(conn, header, (size_t)hlen) != hlen) {
        return ESP_FAIL;
    }
    if (chunk_len > 0 && httpd_conn_send(conn, chunk, chunk_len) != (int)chunk_len) {
        return ESP_FAIL;
    }
    if (httpd_conn_send(conn, "\r\n", 2) != 2) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req, const char *str)
{
    if (str == NULL) {
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    return httpd_resp_send_chunk(req, str, strlen(str));
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    char status[32];
    snprintf(status, sizeof(status), "%d", (int)error);
    switch (error) {
    case HTTPD_401_UNAUTHORIZED:
        compat_strlcpy(status, "401 Unauthorized", sizeof(status));
        break;
    case HTTPD_403_FORBIDDEN:
        compat_strlcpy(status, "403 Forbidden", sizeof(status));
        break;
    case HTTPD_404_NOT_FOUND:
        compat_strlcpy(status, "404 Not Found", sizeof(status));
        break;
    case HTTPD_414_URI_TOO_LONG:
        compat_strlcpy(status, "414 URI Too Long", sizeof(status));
        break;
    case HTTPD_500_INTERNAL_SERVER_ERROR:
        compat_strlcpy(status, "500 Internal Server Error", sizeof(status));
        break;
    case HTTPD_503_SERVICE_UNAVAILABLE:
        compat_strlcpy(status, "503 Service Unavailable", sizeof(status));
        break;
    default:
        break;
    }
    (void)httpd_resp_set_status(req, status);
    (void)httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg != NULL ? msg : status, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_404(httpd_req_t *req)
{
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
}

static esp_err_t httpd_parse_request(httpd_conn_t *conn)
{
    while (conn->recv_len < sizeof(conn->recv_buf) - 1) {
        const int n = recv(conn->sock,
                           conn->recv_buf + conn->recv_len,
                           sizeof(conn->recv_buf) - 1 - conn->recv_len,
                           0);
        if (n <= 0) {
            return ESP_FAIL;
        }
        conn->recv_len += (size_t)n;
        conn->recv_buf[conn->recv_len] = '\0';
        if (strstr(conn->recv_buf, "\r\n\r\n") != NULL) {
            break;
        }
    }

    char *line = conn->recv_buf;
    char *save = NULL;
    char *req_line = strtok_r(line, "\r\n", &save);
    if (req_line == NULL) {
        return ESP_FAIL;
    }
    char method[16];
    char version[16];
    if (sscanf(req_line, "%15s %511s %15s", method, conn->uri, version) < 2) {
        return ESP_FAIL;
    }
    conn->method = httpd_parse_method(method);
    conn->content_len = 0;
    conn->header_count = 0;

    char *hdr = NULL;
    while ((hdr = strtok_r(NULL, "\r\n", &save)) != NULL) {
        if (hdr[0] == '\0') {
            break;
        }
        char *colon = strchr(hdr, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ') {
            value++;
        }
        if (conn->header_count < HTTPD_MAX_HEADERS) {
            compat_strlcpy(conn->headers[conn->header_count].name, hdr, HTTPD_HEADER_NAME);
            compat_strlcpy(conn->headers[conn->header_count].value, value, HTTPD_HEADER_VALUE);
            conn->header_count++;
        }
        if (strcasecmp(hdr, "Content-Length") == 0) {
            conn->content_len = atoi(value);
        }
    }
    return ESP_OK;
}

static const httpd_uri_t *httpd_find_handler(httpd_server_t *server, httpd_conn_t *conn)
{
    const char *path = conn->uri;
    const char *query = strchr(path, '?');
    const size_t match_upto = query != NULL ? (size_t)(query - path) : strlen(path);

    for (size_t i = 0; i < server->handler_count; i++) {
        const httpd_uri_t *uri = &server->handlers[i];
        if (uri->method != conn->method) {
            continue;
        }
        if (server->config.uri_match_fn != NULL) {
            if (server->config.uri_match_fn(uri->uri, path, match_upto)) {
                return uri;
            }
        } else if (strncmp(uri->uri, path, match_upto) == 0 &&
                   strlen(uri->uri) == match_upto) {
            return uri;
        }
    }
    return NULL;
}

static void httpd_handle_client(httpd_server_t *server, int client_sock)
{
    httpd_conn_t conn = {
        .sock = client_sock,
        .content_len = -1,
    };
    compat_strlcpy(conn.status_line, "200 OK", sizeof(conn.status_line));

    if (httpd_parse_request(&conn) != ESP_OK) {
        close(client_sock);
        return;
    }

    const httpd_uri_t *uri = httpd_find_handler(server, &conn);
    httpd_req_t req = {
        .handle = server,
        .method = conn.method,
        .uri = conn.uri,
        .content_len = conn.content_len,
        .aux = &conn,
    };

    if (uri == NULL || uri->handler == NULL) {
        (void)httpd_resp_send_404(&req);
    } else {
        req.user_ctx = uri->user_ctx;
        (void)uri->handler(&req);
    }
    close(client_sock);
}

static void httpd_server_task(void *arg)
{
    httpd_server_t *server = (httpd_server_t *)arg;
    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        const int client = accept(server->listen_sock,
                                  (struct sockaddr *)&client_addr,
                                  &addr_len);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        httpd_handle_client(server, client);
    }
    vTaskDelete(NULL);
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    if (handle == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_server_t *server = calloc(1, sizeof(*server));
    if (server == NULL) {
        return ESP_ERR_NO_MEM;
    }
    server->config = *config;
    server->running = true;
    server->listen_sock = -1;

    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        free(server);
        return ESP_FAIL;
    }
    int yes = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config->server_port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(sock, config->backlog_conn > 0 ? config->backlog_conn : 5) != 0) {
        close(sock);
        free(server);
        return ESP_FAIL;
    }
    server->listen_sock = sock;

    const uint32_t stack_bytes = config->stack_size > 0 ? config->stack_size : 4096;
    const configSTACK_DEPTH_TYPE stack_words =
        (configSTACK_DEPTH_TYPE)((stack_bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t));
    if (xTaskCreate(httpd_server_task,
                    "httpd",
                    stack_words,
                    server,
                    config->task_priority > 0 ? config->task_priority : (tskIDLE_PRIORITY + 5),
                    &server->task) != pdPASS) {
        close(sock);
        free(server);
        return ESP_ERR_NO_MEM;
    }

    *handle = server;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->running = false;
    if (handle->listen_sock >= 0) {
        close(handle->listen_sock);
        handle->listen_sock = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    free(handle);
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    if (handle == NULL || uri_handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->handler_count >= HTTPD_MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    handle->handlers[handle->handler_count++] = *uri_handler;
    return ESP_OK;
}
