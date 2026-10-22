/*
 * mqtt_client.h - ESP-IDF MQTT client API subset for the Pico lwIP port.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data);

typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;

typedef enum {
    MQTT_EVENT_ANY = -1,
    MQTT_EVENT_CONNECTED = 0,
    MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_SUBSCRIBED,
    MQTT_EVENT_UNSUBSCRIBED,
    MQTT_EVENT_PUBLISHED,
    MQTT_EVENT_DATA,
    MQTT_EVENT_ERROR,
    MQTT_EVENT_BEFORE_CONNECT,
    MQTT_EVENT_DELETED,
} esp_mqtt_event_id_t;

typedef enum {
    MQTT_ERROR_TYPE_NONE = 0,
    MQTT_ERROR_TYPE_TCP_TRANSPORT,
    MQTT_ERROR_TYPE_CONNECTION_REFUSED,
} mqtt_error_type_t;

typedef struct {
    esp_err_t esp_tls_last_esp_err;
    int esp_transport_sock_errno;
    mqtt_error_type_t error_type;
    int connect_return_code;
} esp_mqtt_error_codes_t;

typedef struct {
    esp_mqtt_event_id_t event_id;
    esp_mqtt_client_handle_t client;
    int msg_id;
    char *topic;
    int topic_len;
    char *data;
    int data_len;
    int total_data_len;
    int current_data_offset;
    int qos;
    bool retain;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t;

typedef esp_err_t (*esp_err_t_cb)(void *conf);

typedef struct {
    struct {
        struct {
            const char *uri;
        } address;
        struct {
            esp_err_t_cb crt_bundle_attach;
        } verification;
    } broker;
    struct {
        const char *username;
        const char *client_id;
        struct {
            const char *password;
        } authentication;
    } credentials;
    struct {
        int timeout_ms;
        int reconnect_timeout_ms;
    } network;
    struct {
        int stack_size;
    } task;
    struct {
        int size;
        int out_size;
    } buffer;
} esp_mqtt_client_config_t;

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client,
                                         esp_mqtt_event_id_t event,
                                         esp_event_handler_t event_handler,
                                         void *event_handler_arg);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client,
                            const char *topic,
                            const char *data,
                            int len,
                            int qos,
                            int retain);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos);

#ifdef __cplusplus
}
#endif
