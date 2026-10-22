/*
 * CMake link deps (when Wi-Fi is enabled on this target):
 *   pico_cyw43_arch_lwip_sys_freertos, pico_mbedtls
 *
 * Minimal MQTT 3.1.1 client (TCP; mqtts:// uses TLS with verify disabled).
 */

#include "mqtt_client.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#endif
#include "sys/socket.h"

#define MQTT_TASK_NAME "mqtt_client"
#define MQTT_MAX_HOST 128
#define MQTT_MAX_TOPIC 256
#define MQTT_PACKET_MAX 2048

struct esp_mqtt_client {
    esp_mqtt_client_config_t config;
    char host[MQTT_MAX_HOST];
    int port;
    bool tls;
    int sock;
    TaskHandle_t task;
    volatile bool running;
    volatile bool connected;
    volatile bool stop_requested;
    esp_event_handler_t handler;
    void *handler_arg;
    esp_mqtt_event_id_t registered_event;
    int next_msg_id;
    esp_mqtt_error_codes_t last_error;
    int rx_buf_size;
    int tx_buf_size;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool ssl_ready;
};

static size_t mqtt_encode_length(uint8_t *out, size_t len)
{
    size_t pos = 0;
    do {
        uint8_t byte = (uint8_t)(len % 128U);
        len /= 128U;
        if (len > 0) {
            byte |= 0x80U;
        }
        out[pos++] = byte;
    } while (len > 0);
    return pos;
}

static int mqtt_decode_length(const uint8_t *data, size_t data_len, size_t *offset, size_t *value)
{
    size_t multiplier = 1;
    size_t val = 0;
    size_t pos = *offset;
    for (int i = 0; i < 4; i++) {
        if (pos >= data_len) {
            return -1;
        }
        const uint8_t byte = data[pos++];
        val += (byte & 0x7FU) * multiplier;
        if ((byte & 0x80U) == 0) {
            *offset = pos;
            *value = val;
            return 0;
        }
        multiplier *= 128U;
    }
    return -1;
}

static void mqtt_disconnect(struct esp_mqtt_client *client)
{
    if (client == NULL) {
        return;
    }
    if (client->ssl_ready) {
        mbedtls_ssl_free(&client->ssl);
        mbedtls_ssl_config_free(&client->ssl_conf);
        mbedtls_ctr_drbg_free(&client->ctr_drbg);
        mbedtls_entropy_free(&client->entropy);
        client->ssl_ready = false;
    }
    if (client->sock >= 0) {
        close(client->sock);
        client->sock = -1;
    }
    client->connected = false;
}

static int mqtt_raw_send(struct esp_mqtt_client *client, const void *buf, size_t len)
{
    if (client->sock < 0) {
        return -1;
    }
    if (client->tls && client->ssl_ready) {
        return mbedtls_ssl_write(&client->ssl, buf, len);
    }
    return (int)send(client->sock, buf, len, 0);
}

static int mqtt_raw_recv(struct esp_mqtt_client *client, void *buf, size_t len, int timeout_ms)
{
    if (client->sock < 0) {
        return -1;
    }
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    (void)setsockopt(client->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (client->tls && client->ssl_ready) {
        return mbedtls_ssl_read(&client->ssl, buf, len);
    }
    return (int)recv(client->sock, buf, len, 0);
}

static int mqtt_mbedtls_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct esp_mqtt_client *client = (struct esp_mqtt_client *)ctx;
    return mqtt_raw_send(client, buf, len);
}

static int mqtt_mbedtls_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct esp_mqtt_client *client = (struct esp_mqtt_client *)ctx;
    const int n = mqtt_raw_recv(client, buf, len, client->config.network.timeout_ms);
    if (n == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (n < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return n;
}

static esp_err_t mqtt_setup_tls(struct esp_mqtt_client *client)
{
    mbedtls_ssl_init(&client->ssl);
    mbedtls_ssl_config_init(&client->ssl_conf);
    mbedtls_entropy_init(&client->entropy);
    mbedtls_ctr_drbg_init(&client->ctr_drbg);
    const char *pers = "solaros-mqtt";
    if (mbedtls_ctr_drbg_seed(&client->ctr_drbg,
                              mbedtls_entropy_func,
                              &client->entropy,
                              (const unsigned char *)pers,
                              strlen(pers)) != 0) {
        return ESP_FAIL;
    }
    if (mbedtls_ssl_config_defaults(&client->ssl_conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return ESP_FAIL;
    }
    mbedtls_ssl_conf_rng(&client->ssl_conf,
                         mbedtls_ctr_drbg_random,
                         &client->ctr_drbg);
    if (client->config.broker.verification.crt_bundle_attach != NULL) {
        (void)client->config.broker.verification.crt_bundle_attach(&client->ssl_conf);
    } else {
        mbedtls_ssl_conf_authmode(&client->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    if (mbedtls_ssl_setup(&client->ssl, &client->ssl_conf) != 0) {
        return ESP_FAIL;
    }
    if (mbedtls_ssl_set_hostname(&client->ssl, client->host) != 0) {
        return ESP_FAIL;
    }
    mbedtls_ssl_set_bio(&client->ssl,
                        client,
                        mqtt_mbedtls_send,
                        mqtt_mbedtls_recv,
                        NULL);
    client->ssl_ready = true;
    return ESP_OK;
}

static esp_err_t mqtt_parse_uri(struct esp_mqtt_client *client, const char *uri)
{
    if (uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->tls = false;
    client->port = 1883;
    const char *p = uri;
    if (strncmp(p, "mqtts://", 8) == 0) {
        client->tls = true;
        client->port = 8883;
        p += 8;
    } else if (strncmp(p, "mqtt://", 7) == 0) {
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
    return ESP_OK;
}

static esp_err_t mqtt_connect_tcp(struct esp_mqtt_client *client)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", client->port);
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(client->host, port_str, &hints, &res) != 0 || res == NULL) {
        return ESP_FAIL;
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
        return ESP_FAIL;
    }
    client->sock = sock;
    if (client->tls) {
        const esp_err_t err = mqtt_setup_tls(client);
        if (err != ESP_OK) {
            mqtt_disconnect(client);
            return err;
        }
        int hs = 0;
        while ((hs = mbedtls_ssl_handshake(&client->ssl)) != 0) {
            if (hs != MBEDTLS_ERR_SSL_WANT_READ && hs != MBEDTLS_ERR_SSL_WANT_WRITE) {
                mqtt_disconnect(client);
                return ESP_FAIL;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t mqtt_send_connect(struct esp_mqtt_client *client)
{
    const char *client_id = client->config.credentials.client_id != NULL ?
        client->config.credentials.client_id :
        "solaros-mqtt";
    const char *user = client->config.credentials.username;
    const char *pass = client->config.credentials.authentication.password;

    uint8_t payload[512];
    size_t pos = 0;
    const size_t id_len = strlen(client_id);
    payload[pos++] = (uint8_t)((id_len >> 8) & 0xFF);
    payload[pos++] = (uint8_t)(id_len & 0xFF);
    memcpy(payload + pos, client_id, id_len);
    pos += id_len;

    if (user != NULL && user[0] != '\0') {
        const size_t ulen = strlen(user);
        payload[pos++] = (uint8_t)((ulen >> 8) & 0xFF);
        payload[pos++] = (uint8_t)(ulen & 0xFF);
        memcpy(payload + pos, user, ulen);
        pos += ulen;
        if (pass != NULL) {
            const size_t plen = strlen(pass);
            payload[pos++] = (uint8_t)((plen >> 8) & 0xFF);
            payload[pos++] = (uint8_t)(plen & 0xFF);
            memcpy(payload + pos, pass, plen);
            pos += plen;
        }
    }

    uint8_t packet[MQTT_PACKET_MAX];
    size_t p = 0;
    packet[p++] = 0x10; /* CONNECT */
    p += mqtt_encode_length(packet + p, 10 + pos);
    memcpy(packet + p, "MQTT", 4);
    p += 4;
    packet[p++] = 4; /* protocol level */
    uint8_t flags = 0x02; /* clean session */
    if (user != NULL && user[0] != '\0') {
        flags |= 0x80U;
        if (pass != NULL && pass[0] != '\0') {
            flags |= 0x40U;
        }
    }
    packet[p++] = flags;
    packet[p++] = 0x00; /* keepalive MSB */
    packet[p++] = 60;   /* keepalive LSB */
    memcpy(packet + p, payload, pos);
    p += pos;

    if (mqtt_raw_send(client, packet, p) != (int)p) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void mqtt_dispatch(struct esp_mqtt_client *client,
                          esp_mqtt_event_id_t id,
                          esp_mqtt_event_t *event)
{
    if (client->handler == NULL) {
        return;
    }
    if (client->registered_event != MQTT_EVENT_ANY &&
        client->registered_event != id) {
        return;
    }
    event->event_id = id;
    event->client = client;
    event->error_handle = id == MQTT_EVENT_ERROR ? &client->last_error : NULL;
    client->handler(client->handler_arg, "MQTT_EVENTS", (int32_t)id, event);
}

static void mqtt_handle_publish(struct esp_mqtt_client *client, const uint8_t *data, size_t data_len)
{
    size_t offset = 1;
    size_t rem_len = 0;
    if (mqtt_decode_length(data, data_len, &offset, &rem_len) != 0) {
        return;
    }
    if (offset + 2 > data_len) {
        return;
    }
    const size_t topic_len = ((size_t)data[offset] << 8) | data[offset + 1];
    offset += 2;
    if (offset + topic_len > data_len) {
        return;
    }
    const uint8_t qos = (data[0] >> 1) & 0x03U;
    const uint8_t retain = data[0] & 0x01U;
    char topic_buf[MQTT_MAX_TOPIC + 1];
    const size_t copy_topic = topic_len >= sizeof(topic_buf) ? sizeof(topic_buf) - 1 : topic_len;
    memcpy(topic_buf, data + offset, copy_topic);
    topic_buf[copy_topic] = '\0';
    offset += topic_len;
    if (qos > 0) {
        offset += 2;
    }
    if (offset > data_len) {
        return;
    }
    const size_t payload_len = data_len - offset;
    char payload_buf[MQTT_PACKET_MAX + 1];
    const size_t copy_payload = payload_len >= sizeof(payload_buf) ?
        sizeof(payload_buf) - 1 :
        payload_len;
    if (copy_payload > 0) {
        memcpy(payload_buf, data + offset, copy_payload);
    }
    payload_buf[copy_payload] = '\0';

    esp_mqtt_event_t event = {0};
    event.topic = topic_buf;
    event.topic_len = (int)copy_topic;
    event.data = payload_buf;
    event.data_len = (int)copy_payload;
    event.total_data_len = (int)payload_len;
    event.qos = (int)qos;
    event.retain = retain != 0;
    mqtt_dispatch(client, MQTT_EVENT_DATA, &event);
}

static esp_err_t mqtt_read_packet(struct esp_mqtt_client *client)
{
    uint8_t header_byte = 0;
    int n = mqtt_raw_recv(client, &header_byte, 1, client->config.network.timeout_ms);
    if (n <= 0) {
        return ESP_FAIL;
    }

    size_t rem_len = 0;
    size_t multiplier = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t len_byte = 0;
        n = mqtt_raw_recv(client, &len_byte, 1, client->config.network.timeout_ms);
        if (n <= 0) {
            return ESP_FAIL;
        }
        rem_len += (size_t)(len_byte & 0x7FU) * multiplier;
        if ((len_byte & 0x80U) == 0) {
            break;
        }
        multiplier *= 128U;
    }

    uint8_t *body = malloc(rem_len + 1U);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    while (got < rem_len) {
        n = mqtt_raw_recv(client, body + got, rem_len - got, client->config.network.timeout_ms);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += (size_t)n;
    }

    const uint8_t type = header_byte & 0xF0U;
    esp_mqtt_event_t event = {0};
    if (type == 0x20 && rem_len >= 2) { /* CONNACK */
        if ((body[1] & 0xFF) != 0) {
            client->last_error.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED;
            client->last_error.connect_return_code = body[1] & 0xFF;
            client->last_error.esp_transport_sock_errno = 0;
            client->last_error.esp_tls_last_esp_err = ESP_FAIL;
            mqtt_dispatch(client, MQTT_EVENT_ERROR, &event);
        } else {
            client->connected = true;
            mqtt_dispatch(client, MQTT_EVENT_CONNECTED, &event);
        }
    } else if (type == 0x30) { /* PUBLISH */
        uint8_t *full = malloc(1U + rem_len + 8U);
        if (full != NULL) {
            full[0] = header_byte;
            size_t enc = 0;
            size_t len_tmp = rem_len;
            do {
                uint8_t b = (uint8_t)(len_tmp % 128U);
                len_tmp /= 128U;
                if (len_tmp > 0) {
                    b |= 0x80U;
                }
                full[1 + enc++] = b;
            } while (len_tmp > 0);
            memcpy(full + 1 + enc, body, rem_len);
            mqtt_handle_publish(client, full, 1 + enc + rem_len);
            free(full);
        }
    } else if (type == 0x40) { /* PUBACK */
        event.msg_id = (rem_len >= 2) ? ((body[0] << 8) | body[1]) : 0;
        mqtt_dispatch(client, MQTT_EVENT_PUBLISHED, &event);
    }
    free(body);
    return ESP_OK;
}

static void mqtt_task(void *arg)
{
    struct esp_mqtt_client *client = (struct esp_mqtt_client *)arg;
    while (client->running && !client->stop_requested) {
        if (!client->connected) {
            mqtt_disconnect(client);
            if (mqtt_connect_tcp(client) != ESP_OK) {
                client->last_error.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT;
                client->last_error.esp_transport_sock_errno = errno;
                client->last_error.esp_tls_last_esp_err = ESP_FAIL;
                esp_mqtt_event_t event = {0};
                mqtt_dispatch(client, MQTT_EVENT_ERROR, &event);
                vTaskDelay(pdMS_TO_TICKS(client->config.network.reconnect_timeout_ms > 0 ?
                                             client->config.network.reconnect_timeout_ms :
                                             5000));
                continue;
            }
            if (mqtt_send_connect(client) != ESP_OK) {
                mqtt_disconnect(client);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (mqtt_read_packet(client) != ESP_OK) {
            client->connected = false;
            esp_mqtt_event_t event = {0};
            mqtt_dispatch(client, MQTT_EVENT_DISCONNECTED, &event);
            mqtt_disconnect(client);
            vTaskDelay(pdMS_TO_TICKS(client->config.network.reconnect_timeout_ms > 0 ?
                                         client->config.network.reconnect_timeout_ms :
                                         5000));
        }
    }
    mqtt_disconnect(client);
    client->running = false;
    vTaskDelete(NULL);
}

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
{
    if (config == NULL || config->broker.address.uri == NULL) {
        return NULL;
    }
    struct esp_mqtt_client *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return NULL;
    }
    client->config = *config;
    client->sock = -1;
    client->next_msg_id = 1;
    client->registered_event = MQTT_EVENT_ANY;
    client->rx_buf_size = config->buffer.size > 0 ? config->buffer.size : MQTT_PACKET_MAX;
    client->tx_buf_size = config->buffer.out_size > 0 ? config->buffer.out_size : MQTT_PACKET_MAX;
    if (mqtt_parse_uri(client, config->broker.address.uri) != ESP_OK) {
        free(client);
        return NULL;
    }
    return client;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client,
                                         esp_mqtt_event_id_t event,
                                         esp_event_handler_t event_handler,
                                         void *event_handler_arg)
{
    if (client == NULL || event_handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->handler = event_handler;
    client->handler_arg = event_handler_arg;
    client->registered_event = event;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->running) {
        return ESP_OK;
    }
    client->stop_requested = false;
    client->running = true;
    const int stack_bytes = client->config.task.stack_size > 0 ? client->config.task.stack_size : 6144;
    /* ESP-IDF mqtt stack sizes are bytes; vanilla FreeRTOS wants words. */
    const configSTACK_DEPTH_TYPE stack_words =
        (configSTACK_DEPTH_TYPE)((stack_bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t));
    if (xTaskCreate(mqtt_task, MQTT_TASK_NAME, stack_words, client, tskIDLE_PRIORITY + 2, &client->task) != pdPASS) {
        client->running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->stop_requested = true;
    client->running = false;
    for (int i = 0; i < 100 && client->task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    client->task = NULL;
    mqtt_disconnect(client);
    return ESP_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)esp_mqtt_client_stop(client);
    free(client);
    return ESP_OK;
}

static int mqtt_send_simple(struct esp_mqtt_client *client, uint8_t type, const uint8_t *payload, size_t payload_len)
{
    uint8_t packet[MQTT_PACKET_MAX];
    if (payload_len + 8 > sizeof(packet)) {
        return -1;
    }
    size_t p = 0;
    packet[p++] = type;
    p += mqtt_encode_length(packet + p, payload_len);
    memcpy(packet + p, payload, payload_len);
    p += payload_len;
    if (mqtt_raw_send(client, packet, p) != (int)p) {
        return -1;
    }
    return 0;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t client,
                            const char *topic,
                            const char *data,
                            int len,
                            int qos,
                            int retain)
{
    if (client == NULL || topic == NULL || !client->connected || len < 0) {
        return -1;
    }
    if (len == 0) {
        len = data != NULL ? (int)strlen(data) : 0;
    }
    const size_t topic_len = strlen(topic);
    if (topic_len == 0 || topic_len > MQTT_MAX_TOPIC) {
        return -1;
    }
    uint8_t payload[MQTT_PACKET_MAX];
    size_t pos = 0;
    payload[pos++] = (uint8_t)((topic_len >> 8) & 0xFF);
    payload[pos++] = (uint8_t)(topic_len & 0xFF);
    memcpy(payload + pos, topic, topic_len);
    pos += topic_len;
    const int msg_id = client->next_msg_id;
    if (qos > 0) {
        payload[pos++] = (uint8_t)((msg_id >> 8) & 0xFF);
        payload[pos++] = (uint8_t)(msg_id & 0xFF);
    }
    if (len > 0 && data != NULL) {
        if (pos + (size_t)len > sizeof(payload)) {
            return -1;
        }
        memcpy(payload + pos, data, (size_t)len);
        pos += (size_t)len;
    }
    uint8_t header = 0x30;
    if (retain) {
        header |= 0x01U;
    }
    header |= (uint8_t)((qos & 0x03) << 1);
    client->next_msg_id++;
    if (mqtt_send_simple(client, header, payload, pos) < 0) {
        return -1;
    }
    return msg_id;
}

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos)
{
    if (client == NULL || topic == NULL || !client->connected) {
        return -1;
    }
    const size_t topic_len = strlen(topic);
    if (topic_len == 0 || topic_len + 5 > MQTT_PACKET_MAX) {
        return -1;
    }
    const int msg_id = client->next_msg_id++;
    uint8_t payload[MQTT_PACKET_MAX];
    size_t pos = 0;
    payload[pos++] = (uint8_t)((msg_id >> 8) & 0xFF);
    payload[pos++] = (uint8_t)(msg_id & 0xFF);
    payload[pos++] = (uint8_t)((topic_len >> 8) & 0xFF);
    payload[pos++] = (uint8_t)(topic_len & 0xFF);
    memcpy(payload + pos, topic, topic_len);
    pos += topic_len;
    payload[pos++] = (uint8_t)(qos & 0x03);
    if (mqtt_send_simple(client, 0x82, payload, pos) < 0) {
        return -1;
    }
    return msg_id;
}
