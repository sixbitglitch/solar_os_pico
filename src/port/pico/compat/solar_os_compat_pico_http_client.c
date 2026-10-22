/*
 * CMake link deps (when Wi-Fi is enabled on this target):
 *   pico_cyw43_arch_lwip_sys_freertos, pico_mbedtls
 *
 * ESP-IDF esp_http_client compatibility for SolarOS on RP2350 + lwIP.
 */

#include "esp_http_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#endif
#include "sys/socket.h"

static char *solar_strcasestr(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return (char *)haystack;
    }
    for (const char *h = haystack; *h != '\0'; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a != '\0' && *b != '\0' &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return (char *)h;
        }
    }
    return NULL;
}

#define HTTP_CLIENT_MAX_HEADERS 32
#define HTTP_CLIENT_HEADER_NAME_MAX 64
#define HTTP_CLIENT_HEADER_VALUE_MAX 512
#define HTTP_CLIENT_HOST_MAX 128
#define HTTP_CLIENT_PATH_MAX 512
#define HTTP_CLIENT_LINE_MAX 1024

static void compat_strlcpy(char *dst, const char *src, size_t size)
{
    if (size == 0) {
        return;
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

typedef struct http_header {
    char name[HTTP_CLIENT_HEADER_NAME_MAX];
    char value[HTTP_CLIENT_HEADER_VALUE_MAX];
    bool used;
} http_header_t;

struct esp_http_client {
    esp_http_client_config_t config;
    char url[HTTP_CLIENT_PATH_MAX + HTTP_CLIENT_HOST_MAX + 16];
    char host[HTTP_CLIENT_HOST_MAX];
    char path[HTTP_CLIENT_PATH_MAX];
    int port;
    bool tls;
    esp_http_client_method_t method;
    http_header_t headers[HTTP_CLIENT_MAX_HEADERS];
    const char *post_data;
    int post_len;
    int timeout_ms;
    int sock;
    bool connected;
    bool cancel;
    int redirect_count;
    int status_code;
    int64_t content_length;
    bool chunked;
    bool headers_done;
    bool body_complete;
    int64_t body_read;
    size_t chunk_remaining;
    char *rx_buffer;
    int rx_buffer_size;
    char *header_scratch;
    size_t header_scratch_len;
    size_t header_scratch_cap;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool ssl_ready;
};

static esp_err_t http_client_parse_url(struct esp_http_client *client, const char *url);
static esp_err_t http_client_connect(struct esp_http_client *client);
static void http_client_disconnect(struct esp_http_client *client);
static esp_err_t http_client_send_request(struct esp_http_client *client,
                                          int body_len);
static esp_err_t http_client_read_line(struct esp_http_client *client,
                                       char *line,
                                       size_t line_size,
                                       int timeout_ms);
static esp_err_t http_client_consume_headers(struct esp_http_client *client);
static esp_err_t http_client_emit_event(struct esp_http_client *client,
                                        esp_http_client_event_id_t id,
                                        void *data,
                                        int data_len,
                                        const char *key,
                                        const char *value);
static int http_client_recv_raw(struct esp_http_client *client,
                                void *buf,
                                size_t len,
                                int timeout_ms);
static int http_client_send_raw(struct esp_http_client *client,
                                const void *buf,
                                size_t len);

esp_err_t esp_crt_bundle_attach(void *conf)
{
    if (conf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_ssl_config *ssl_conf = (mbedtls_ssl_config *)conf;
    mbedtls_ssl_conf_authmode(ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    return ESP_OK;
}

static const char *http_method_string(esp_http_client_method_t method)
{
    switch (method) {
    case HTTP_METHOD_POST:
        return "POST";
    case HTTP_METHOD_PUT:
        return "PUT";
    case HTTP_METHOD_PATCH:
        return "PATCH";
    case HTTP_METHOD_DELETE:
        return "DELETE";
    case HTTP_METHOD_HEAD:
        return "HEAD";
    case HTTP_METHOD_OPTIONS:
        return "OPTIONS";
    case HTTP_METHOD_GET:
    default:
        return "GET";
    }
}

static void http_client_clear_headers(struct esp_http_client *client)
{
    for (size_t i = 0; i < HTTP_CLIENT_MAX_HEADERS; i++) {
        client->headers[i].used = false;
        client->headers[i].name[0] = '\0';
        client->headers[i].value[0] = '\0';
    }
}

static esp_err_t http_client_set_socket_timeout(int sock, int timeout_ms)
{
    if (timeout_ms <= 0) {
        return ESP_OK;
    }
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return ESP_FAIL;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int http_client_mbedtls_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct esp_http_client *client = (struct esp_http_client *)ctx;
    return http_client_send_raw(client, buf, len);
}

static int http_client_mbedtls_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct esp_http_client *client = (struct esp_http_client *)ctx;
    const int n = http_client_recv_raw(client, buf, len, client->timeout_ms);
    if (n == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return n;
}

static void http_client_teardown_tls(struct esp_http_client *client)
{
    if (client == NULL || !client->ssl_ready) {
        return;
    }
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_config_free(&client->ssl_conf);
    mbedtls_ctr_drbg_free(&client->ctr_drbg);
    mbedtls_entropy_free(&client->entropy);
    client->ssl_ready = false;
}

static esp_err_t http_client_setup_tls(struct esp_http_client *client)
{
    if (client->ssl_ready) {
        return ESP_OK;
    }

    mbedtls_ssl_init(&client->ssl);
    mbedtls_ssl_config_init(&client->ssl_conf);
    mbedtls_entropy_init(&client->entropy);
    mbedtls_ctr_drbg_init(&client->ctr_drbg);

    const char *pers = "solaros-http";
    if (mbedtls_ctr_drbg_seed(&client->ctr_drbg,
                              mbedtls_entropy_func,
                              &client->entropy,
                              (const unsigned char *)pers,
                              strlen(pers)) != 0) {
        http_client_teardown_tls(client);
        return ESP_FAIL;
    }

    if (mbedtls_ssl_config_defaults(&client->ssl_conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        http_client_teardown_tls(client);
        return ESP_FAIL;
    }

    mbedtls_ssl_conf_rng(&client->ssl_conf,
                         mbedtls_ctr_drbg_random,
                         &client->ctr_drbg);
    if (client->config.crt_bundle_attach != NULL) {
        (void)client->config.crt_bundle_attach(&client->ssl_conf);
    } else {
        mbedtls_ssl_conf_authmode(&client->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    if (mbedtls_ssl_setup(&client->ssl, &client->ssl_conf) != 0) {
        http_client_teardown_tls(client);
        return ESP_FAIL;
    }

    if (mbedtls_ssl_set_hostname(&client->ssl, client->host) != 0) {
        http_client_teardown_tls(client);
        return ESP_FAIL;
    }

    mbedtls_ssl_set_bio(&client->ssl,
                        client,
                        http_client_mbedtls_send,
                        http_client_mbedtls_recv,
                        NULL);
    client->ssl_ready = true;
    return ESP_OK;
}

static esp_err_t http_client_parse_url(struct esp_http_client *client, const char *url)
{
    if (client == NULL || url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    compat_strlcpy(client->url, url, sizeof(client->url));
    const char *p = url;
    client->tls = false;
    client->port = 80;

    if (strncmp(p, "https://", 8) == 0) {
        client->tls = true;
        client->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const char *slash = strchr(p, '/');
    const char *colon = memchr(p, ':', slash != NULL ? (size_t)(slash - p) : strlen(p));
    size_t host_len = 0;
    if (colon != NULL && (slash == NULL || colon < slash)) {
        host_len = (size_t)(colon - p);
        client->port = atoi(colon + 1);
        if (client->port <= 0 || client->port > 65535) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (slash != NULL) {
        host_len = (size_t)(slash - p);
    } else {
        host_len = strlen(p);
    }

    if (host_len == 0 || host_len >= sizeof(client->host)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(client->host, p, host_len);
    client->host[host_len] = '\0';

    if (slash != NULL) {
        compat_strlcpy(client->path, slash, sizeof(client->path));
    } else {
        compat_strlcpy(client->path, "/", sizeof(client->path));
    }
    return ESP_OK;
}

static int http_client_send_raw(struct esp_http_client *client,
                                const void *buf,
                                size_t len)
{
    if (client == NULL || client->sock < 0) {
        return -1;
    }
    if (client->tls && client->ssl_ready) {
        const int w = mbedtls_ssl_write(&client->ssl, buf, len);
        return w;
    }
    return (int)send(client->sock, buf, len, 0);
}

static int http_client_recv_raw(struct esp_http_client *client,
                                void *buf,
                                size_t len,
                                int timeout_ms)
{
    if (client == NULL || client->sock < 0) {
        return -1;
    }
    (void)http_client_set_socket_timeout(client->sock, timeout_ms);
    if (client->tls && client->ssl_ready) {
        return mbedtls_ssl_read(&client->ssl, buf, len);
    }
    return (int)recv(client->sock, buf, len, 0);
}

static void http_client_disconnect(struct esp_http_client *client)
{
    if (client == NULL) {
        return;
    }
    if (client->tls) {
        http_client_teardown_tls(client);
    }
    if (client->sock >= 0) {
        close(client->sock);
        client->sock = -1;
    }
    client->connected = false;
}

static esp_err_t http_client_connect(struct esp_http_client *client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->connected && client->sock >= 0) {
        return ESP_OK;
    }

    http_client_disconnect(client);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", client->port);

    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    const int gai = getaddrinfo(client->host, port_str, &hints, &res);
    if (gai != 0 || res == NULL) {
        return ESP_ERR_HTTP_CONNECT;
    }

    int sock = -1;
    for (struct addrinfo *it = res; it != NULL; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) {
        return ESP_ERR_HTTP_CONNECT;
    }

    client->sock = sock;
    (void)http_client_set_socket_timeout(sock, client->timeout_ms);

    if (client->tls) {
        const esp_err_t tls_err = http_client_setup_tls(client);
        if (tls_err != ESP_OK) {
            http_client_disconnect(client);
            return tls_err;
        }
        int handshake = 0;
        while ((handshake = mbedtls_ssl_handshake(&client->ssl)) != 0) {
            if (handshake != MBEDTLS_ERR_SSL_WANT_READ &&
                handshake != MBEDTLS_ERR_SSL_WANT_WRITE) {
                http_client_disconnect(client);
                return ESP_ERR_HTTP_CONNECT;
            }
            if (client->cancel) {
                http_client_disconnect(client);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    client->connected = true;
    (void)http_client_emit_event(client, HTTP_EVENT_ON_CONNECTED, NULL, 0, NULL, NULL);
    return ESP_OK;
}

static esp_err_t http_client_emit_event(struct esp_http_client *client,
                                        esp_http_client_event_id_t id,
                                        void *data,
                                        int data_len,
                                        const char *key,
                                        const char *value)
{
    if (client == NULL || client->config.event_handler == NULL) {
        return ESP_OK;
    }
    esp_http_client_event_t evt = {
        .event_id = id,
        .client = client,
        .data = data,
        .data_len = data_len,
        .user_data = client->config.user_data,
        .header_key = (char *)key,
        .header_value = (char *)value,
    };
    return client->config.event_handler(&evt);
}

static esp_err_t http_client_append_header_line(struct esp_http_client *client,
                                                const char *line)
{
    const char *colon = strchr(line, ':');
    if (colon == NULL) {
        return ESP_OK;
    }
    char key[HTTP_CLIENT_HEADER_NAME_MAX];
    char value[HTTP_CLIENT_HEADER_VALUE_MAX];
    size_t key_len = (size_t)(colon - line);
    while (key_len > 0 && isspace((unsigned char)line[key_len - 1])) {
        key_len--;
    }
    if (key_len >= sizeof(key)) {
        return ESP_OK;
    }
    memcpy(key, line, key_len);
    key[key_len] = '\0';

    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    compat_strlcpy(value, v, sizeof(value));

    for (size_t i = 0; i < HTTP_CLIENT_MAX_HEADERS; i++) {
        if (client->headers[i].used &&
            strcasecmp(client->headers[i].name, key) == 0) {
            compat_strlcpy(client->headers[i].value, value, sizeof(client->headers[i].value));
            (void)http_client_emit_event(client,
                                         HTTP_EVENT_ON_HEADER,
                                         NULL,
                                         0,
                                         key,
                                         value);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < HTTP_CLIENT_MAX_HEADERS; i++) {
        if (!client->headers[i].used) {
            compat_strlcpy(client->headers[i].name, key, sizeof(client->headers[i].name));
            compat_strlcpy(client->headers[i].value, value, sizeof(client->headers[i].value));
            client->headers[i].used = true;
            (void)http_client_emit_event(client,
                                         HTTP_EVENT_ON_HEADER,
                                         NULL,
                                         0,
                                         key,
                                         value);
            return ESP_OK;
        }
    }
    return ESP_OK;
}

static esp_err_t http_client_read_line(struct esp_http_client *client,
                                       char *line,
                                       size_t line_size,
                                       int timeout_ms)
{
    size_t pos = 0;
    while (pos + 1 < line_size) {
        if (client->cancel) {
            return ESP_ERR_INVALID_STATE;
        }
        char ch = 0;
        const int n = http_client_recv_raw(client, &ch, 1, timeout_ms);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return ESP_ERR_HTTP_EAGAIN;
            }
            return ESP_ERR_HTTP_FETCH_HEADER;
        }
        if (n == 0) {
            return ESP_ERR_HTTP_CONNECTION_CLOSED;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line[pos++] = ch;
        }
    }
    line[pos] = '\0';
    return ESP_OK;
}

static esp_err_t http_client_parse_status_line(struct esp_http_client *client,
                                               const char *line)
{
    int version = 0;
    char reason[64];
    if (sscanf(line, "HTTP/%d.%*d %d %63[^\r\n]", &version, &client->status_code, reason) < 2) {
        return ESP_ERR_HTTP_INVALID_TRANSPORT;
    }
    client->content_length = -1;
    client->chunked = false;
    return ESP_OK;
}

static esp_err_t http_client_consume_headers(struct esp_http_client *client)
{
    if (client->headers_done) {
        return ESP_OK;
    }

    char line[HTTP_CLIENT_LINE_MAX];
    esp_err_t err = http_client_read_line(client, line, sizeof(line), client->timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    err = http_client_parse_status_line(client, line);
    if (err != ESP_OK) {
        return err;
    }

    while (true) {
        err = http_client_read_line(client, line, sizeof(line), client->timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        if (line[0] == '\0') {
            break;
        }
        (void)http_client_append_header_line(client, line);
        if (strcasecmp(line, "Transfer-Encoding: chunked") == 0 ||
            (strncmp(line, "Transfer-Encoding:", 18) == 0 &&
             solar_strcasestr(line, "chunked") != NULL)) {
            client->chunked = true;
        }
        char *cl = solar_strcasestr(line, "Content-Length:");
        if (cl != NULL) {
            client->content_length = atoll(cl + strlen("Content-Length:"));
        }
    }

    client->headers_done = true;
    return ESP_OK;
}

static esp_err_t http_client_send_request(struct esp_http_client *client, int body_len)
{
    char request[HTTP_CLIENT_LINE_MAX];
    int req_len = snprintf(request,
                           sizeof(request),
                           "%s %s HTTP/1.1\r\nHost: %s\r\n",
                           http_method_string(client->method),
                           client->path,
                           client->host);
    if (req_len <= 0 || (size_t)req_len >= sizeof(request)) {
        return ESP_ERR_INVALID_SIZE;
    }

    bool has_content_length = false;
    bool has_connection = false;
    for (size_t i = 0; i < HTTP_CLIENT_MAX_HEADERS; i++) {
        if (!client->headers[i].used) {
            continue;
        }
        if (strcasecmp(client->headers[i].name, "Content-Length") == 0) {
            has_content_length = true;
        }
        if (strcasecmp(client->headers[i].name, "Connection") == 0) {
            has_connection = true;
        }
        req_len += snprintf(request + req_len,
                            sizeof(request) - (size_t)req_len,
                            "%s: %s\r\n",
                            client->headers[i].name,
                            client->headers[i].value);
        if (req_len <= 0 || (size_t)req_len >= sizeof(request)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (client->config.user_agent != NULL && client->config.user_agent[0] != '\0') {
        req_len += snprintf(request + req_len,
                            sizeof(request) - (size_t)req_len,
                            "User-Agent: %s\r\n",
                            client->config.user_agent);
    }

    if (body_len > 0 && !has_content_length) {
        req_len += snprintf(request + req_len,
                            sizeof(request) - (size_t)req_len,
                            "Content-Length: %d\r\n",
                            body_len);
    }
    if (!has_connection) {
        req_len += snprintf(request + req_len,
                            sizeof(request) - (size_t)req_len,
                            "Connection: keep-alive\r\n");
    }
    req_len += snprintf(request + req_len,
                        sizeof(request) - (size_t)req_len,
                        "\r\n");
    if (req_len <= 0 || (size_t)req_len >= sizeof(request)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (http_client_send_raw(client, request, (size_t)req_len) != req_len) {
        return ESP_ERR_HTTP_WRITE_DATA;
    }
    (void)http_client_emit_event(client, HTTP_EVENT_HEADERS_SENT, NULL, 0, NULL, NULL);
    return ESP_OK;
}

static esp_err_t http_client_reset_response_state(struct esp_http_client *client)
{
    client->status_code = -1;
    client->content_length = -1;
    client->chunked = false;
    client->headers_done = false;
    client->body_complete = false;
    client->body_read = 0;
    client->chunk_remaining = 0;
    if (client->header_scratch != NULL) {
        client->header_scratch_len = 0;
    }
    return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    if (config == NULL || config->url == NULL) {
        return NULL;
    }

    struct esp_http_client *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return NULL;
    }

    client->config = *config;
    client->method = config->method;
    client->timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 10000;
    client->sock = -1;
    client->post_data = NULL;
    client->post_len = 0;
    client->status_code = -1;
    client->content_length = -1;

    const int rx_size = config->buffer_size > 0 ? config->buffer_size : 1024;
    client->rx_buffer_size = rx_size;
    client->rx_buffer = malloc((size_t)rx_size);
    if (client->rx_buffer == NULL) {
        free(client);
        return NULL;
    }

    if (http_client_parse_url(client, config->url) != ESP_OK) {
        free(client->rx_buffer);
        free(client);
        return NULL;
    }

    http_client_clear_headers(client);
    return client;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    http_client_disconnect(client);
    free(client->rx_buffer);
    free(client->header_scratch);
    free(client);
    return ESP_OK;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url)
{
    if (client == NULL || url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return http_client_parse_url(client, url);
}

esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,
                                     esp_http_client_method_t method)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->method = method;
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key,
                                     const char *value)
{
    if (client == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < HTTP_CLIENT_MAX_HEADERS; i++) {
        if (client->headers[i].used &&
            strcasecmp(client->headers[i].name, key) == 0) {
            compat_strlcpy(client->headers[i].value, value, sizeof(client->headers[i].value));
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < HTTP_CLIENT_MAX_HEADERS; i++) {
        if (!client->headers[i].used) {
            compat_strlcpy(client->headers[i].name, key, sizeof(client->headers[i].name));
            compat_strlcpy(client->headers[i].value, value, sizeof(client->headers[i].value));
            client->headers[i].used = true;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_http_client_delete_all_headers(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    http_client_clear_headers(client);
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
                                         const char *data,
                                         int len)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        client->post_data = NULL;
        client->post_len = 0;
        for (size_t i = 0; i < HTTP_CLIENT_MAX_HEADERS; i++) {
            if (client->headers[i].used &&
                strcasecmp(client->headers[i].name, "Content-Type") == 0) {
                client->headers[i].used = false;
                return ESP_ERR_NOT_FOUND;
            }
        }
        return ESP_ERR_NOT_FOUND;
    }
    client->post_data = data;
    client->post_len = len;
    return esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
}

esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->config.user_data = data;
    return ESP_OK;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->timeout_ms = timeout_ms > 0 ? timeout_ms : client->timeout_ms;
    if (client->sock >= 0) {
        (void)http_client_set_socket_timeout(client->sock, client->timeout_ms);
    }
    return ESP_OK;
}

esp_err_t esp_http_client_reset_redirect_counter(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->redirect_count = 0;
    return ESP_OK;
}

esp_err_t esp_http_client_cancel_request(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->cancel = true;
    return ESP_OK;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    http_client_disconnect(client);
    return ESP_OK;
}

esp_err_t esp_http_client_clear_response_buffer(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return http_client_reset_response_state(client);
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return client != NULL ? client->status_code : -1;
}

int64_t esp_http_client_get_content_length(esp_http_client_handle_t client)
{
    return client != NULL ? client->content_length : -1;
}

bool esp_http_client_is_chunked_response(esp_http_client_handle_t client)
{
    return client != NULL && client->chunked;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{
    return client != NULL && client->body_complete;
}

static esp_err_t http_client_read_chunk_size(struct esp_http_client *client, size_t *chunk_size)
{
    char line[32];
    esp_err_t err = http_client_read_line(client, line, sizeof(line), client->timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    char *end = NULL;
    unsigned long value = strtoul(line, &end, 16);
    if (end == line) {
        return ESP_ERR_HTTP_FETCH_HEADER;
    }
    *chunk_size = (size_t)value;
    return ESP_OK;
}

static int http_client_read_body_once(struct esp_http_client *client,
                                      char *buffer,
                                      int len,
                                      int timeout_ms)
{
    if (client == NULL || len <= 0) {
        return 0;
    }
    if (!client->headers_done) {
        return -ESP_ERR_HTTP_FETCH_HEADER;
    }
    if (client->body_complete) {
        return 0;
    }

    if (client->chunked) {
        if (client->chunk_remaining == 0) {
            esp_err_t err = http_client_read_chunk_size(client, &client->chunk_remaining);
            if (err != ESP_OK) {
                return (int)-err;
            }
            if (client->chunk_remaining == 0) {
                char trailer[HTTP_CLIENT_LINE_MAX];
                while (true) {
                    err = http_client_read_line(client,
                                                trailer,
                                                sizeof(trailer),
                                                timeout_ms);
                    if (err != ESP_OK) {
                        return (int)-err;
                    }
                    if (trailer[0] == '\0') {
                        break;
                    }
                }
                client->body_complete = true;
                return 0;
            }
        }
        const int to_read =
            (int)(client->chunk_remaining < (size_t)len ? client->chunk_remaining : (size_t)len);
        const int n = http_client_recv_raw(client, buffer, (size_t)to_read, timeout_ms);
        if (n <= 0) {
            if (n == 0) {
                return 0;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -ESP_ERR_HTTP_EAGAIN;
            }
            return -ESP_ERR_HTTP_CONNECTION_CLOSED;
        }
        client->chunk_remaining -= (size_t)n;
        if (client->chunk_remaining == 0) {
            char crlf[2];
            (void)http_client_recv_raw(client, crlf, 2, timeout_ms);
        }
        client->body_read += n;
        return n;
    }

    const int n = http_client_recv_raw(client, buffer, (size_t)len, timeout_ms);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -ESP_ERR_HTTP_EAGAIN;
        }
        return -ESP_ERR_HTTP_CONNECTION_CLOSED;
    }
    if (n == 0) {
        client->body_complete = true;
        return 0;
    }
    client->body_read += n;
    if (client->content_length >= 0 && client->body_read >= client->content_length) {
        client->body_complete = true;
    }
    return n;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    return http_client_read_body_once(client, buffer, len, client->timeout_ms);
}

int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len)
{
    if (client == NULL || len < 0 || (len > 0 && buffer == NULL)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    const int w = http_client_send_raw(client, buffer, (size_t)len);
    if (w != len) {
        return -1;
    }
    return w;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->cancel = false;
    (void)http_client_reset_response_state(client);

    esp_err_t err = http_client_connect(client);
    if (err != ESP_OK) {
        return err;
    }

    const int body_len = write_len > 0 ? write_len :
        (client->post_len > 0 ? client->post_len : 0);
    err = http_client_send_request(client, body_len);
    return err;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return -ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = http_client_consume_headers(client);
    if (err != ESP_OK) {
        return (int64_t)-err;
    }
    return client->content_length;
}

esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *location = NULL;
    for (size_t i = 0; i < HTTP_CLIENT_MAX_HEADERS; i++) {
        if (client->headers[i].used &&
            strcasecmp(client->headers[i].name, "Location") == 0) {
            location = client->headers[i].value;
            break;
        }
    }
    if (location == NULL || location[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    client->redirect_count++;
    if (client->config.max_redirection_count > 0 &&
        client->redirect_count > client->config.max_redirection_count) {
        return ESP_ERR_HTTP_MAX_REDIRECT;
    }
    http_client_disconnect(client);
    return http_client_parse_url(client, location);
}

static esp_err_t http_client_drain_body(struct esp_http_client *client)
{
    if (client->method == HTTP_METHOD_HEAD) {
        client->body_complete = true;
        return ESP_OK;
    }
    while (!client->body_complete && !client->cancel) {
        const int n = http_client_read_body_once(client,
                                                 client->rx_buffer,
                                                 client->rx_buffer_size,
                                                 client->timeout_ms);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (n == -ESP_ERR_HTTP_EAGAIN) {
                continue;
            }
            return (esp_err_t)(-n);
        }
        (void)http_client_emit_event(client,
                                     HTTP_EVENT_ON_DATA,
                                     client->rx_buffer,
                                     n,
                                     NULL,
                                     NULL);
    }
    (void)http_client_emit_event(client, HTTP_EVENT_ON_FINISH, NULL, 0, NULL, NULL);
    return ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    client->cancel = false;
    const int body_len = client->post_len > 0 ? client->post_len : 0;

    esp_err_t err = http_client_connect(client);
    if (err != ESP_OK) {
        return err;
    }

    (void)http_client_reset_response_state(client);
    err = http_client_send_request(client, body_len);
    if (err != ESP_OK) {
        return err;
    }

    if (body_len > 0 && client->post_data != NULL) {
        const int w = http_client_send_raw(client, client->post_data, (size_t)body_len);
        if (w != body_len) {
            return ESP_ERR_HTTP_WRITE_DATA;
        }
    }

    err = http_client_consume_headers(client);
    if (err != ESP_OK) {
        return err;
    }

    if (client->status_code == 401) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = http_client_drain_body(client);
    if (err != ESP_OK) {
        return err;
    }
    if (client->cancel) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
