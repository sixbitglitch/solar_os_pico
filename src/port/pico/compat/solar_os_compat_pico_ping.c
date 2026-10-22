/*
 * CMake link deps (when Wi-Fi is enabled on this target):
 *   pico_cyw43_arch_lwip_sys_freertos
 *
 * ICMP echo ping via lwIP raw sockets (ESP-IDF ping_sock API subset).
 */

#include "ping/ping_sock.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/icmp.h"
#include "lwip/raw.h"
#include "lwip/timeouts.h"

#define PING_ID 0x4f53U

typedef struct esp_ping {
    esp_ping_config_t config;
    esp_ping_callbacks_t callbacks;
    struct raw_pcb *pcb;
    TaskHandle_t task;
    volatile bool running;
    volatile bool stop;
    uint16_t seq;
    uint32_t transmitted;
    uint32_t received;
    uint32_t duration_ms;
    uint32_t last_elapsed_ms;
    uint8_t last_ttl;
    ip_addr_t target;
    volatile bool reply_received;
    int64_t send_time_ms;
} ping_session_t;

static int64_t ping_now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    (void)pcb;
    ping_session_t *session = (ping_session_t *)arg;
    if (session == NULL || p == NULL || addr == NULL) {
        return 0;
    }
    if (!ip_addr_cmp(addr, &session->target)) {
        return 0;
    }

    uint8_t *data = (uint8_t *)p->payload;
    if (p->len < sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr)) {
        return 0;
    }
    struct ip_hdr *iphdr = (struct ip_hdr *)data;
    const size_t hlen = IPH_HL(iphdr) * 4U;
    if (p->len < hlen + sizeof(struct icmp_echo_hdr)) {
        return 0;
    }
    struct icmp_echo_hdr *echo = (struct icmp_echo_hdr *)(data + hlen);
    if (ICMPH_TYPE(echo) != ICMP_ER) {
        return 0;
    }
    if (echo->id != lwip_htons(PING_ID) || echo->seqno != lwip_htons(session->seq)) {
        return 0;
    }

    session->last_ttl = IPH_TTL(iphdr);
    session->last_elapsed_ms =
        (uint32_t)(ping_now_ms() - session->send_time_ms);
    session->reply_received = true;
    pbuf_free(p);
    return 1;
}

static esp_err_t ping_send_echo(ping_session_t *session)
{
    if (session == NULL || session->pcb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    session->seq++;
    const size_t data_size = session->config.data_size > 0 ? session->config.data_size : 32;
    const size_t packet_len = sizeof(struct icmp_echo_hdr) + data_size;
    struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)packet_len, PBUF_RAM);
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }

    struct icmp_echo_hdr *echo = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(echo, ICMP_ECHO);
    ICMPH_CODE_SET(echo, 0);
    echo->chksum = 0;
    echo->id = lwip_htons(PING_ID);
    echo->seqno = lwip_htons(session->seq);
    if (data_size > 0) {
        memset((uint8_t *)p->payload + sizeof(*echo), 0x5a, data_size);
    }
    echo->chksum = inet_chksum(echo, (u16_t)packet_len);

    session->reply_received = false;
    session->send_time_ms = ping_now_ms();
    if (raw_sendto(session->pcb, p, &session->target) != ERR_OK) {
        pbuf_free(p);
        return ESP_FAIL;
    }
    pbuf_free(p);
    session->transmitted++;
    return ESP_OK;
}

static void ping_task(void *arg)
{
    ping_session_t *session = (ping_session_t *)arg;
    const int64_t start_ms = ping_now_ms();
    uint32_t sent = 0;

    while (session->running && !session->stop) {
        const bool forever = session->config.count == 0;
        if (!forever && sent >= session->config.count) {
            break;
        }

        if (ping_send_echo(session) != ESP_OK) {
            break;
        }
        sent++;

        const uint32_t timeout_ms = session->config.timeout_ms > 0 ?
            session->config.timeout_ms :
            1000;
        uint32_t waited = 0;
        while (!session->reply_received && waited < timeout_ms && !session->stop) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }

        if (session->reply_received) {
            session->received++;
            if (session->callbacks.on_ping_success != NULL) {
                session->callbacks.on_ping_success(session, session->callbacks.cb_args);
            }
        } else if (session->callbacks.on_ping_timeout != NULL) {
            session->callbacks.on_ping_timeout(session, session->callbacks.cb_args);
        }

        if (session->stop) {
            break;
        }
        if (!forever && sent >= session->config.count) {
            break;
        }
        const uint32_t interval = session->config.interval_ms > 0 ?
            session->config.interval_ms :
            1000;
        vTaskDelay(pdMS_TO_TICKS(interval));
    }

    session->duration_ms = (uint32_t)(ping_now_ms() - start_ms);
    session->running = false;
    if (session->callbacks.on_ping_end != NULL) {
        session->callbacks.on_ping_end(session, session->callbacks.cb_args);
    }
    vTaskDelete(NULL);
}

esp_err_t esp_ping_new_session(const esp_ping_config_t *config,
                               const esp_ping_callbacks_t *cbs,
                               esp_ping_handle_t *out_handle)
{
    if (config == NULL || cbs == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ping_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    session->config = *config;
    session->callbacks = *cbs;
    session->target = config->target_addr;

    session->pcb = raw_new(IP_PROTO_ICMP);
    if (session->pcb == NULL) {
        free(session);
        return ESP_ERR_NO_MEM;
    }
    raw_bind(session->pcb, IP_ADDR_ANY);
    raw_recv(session->pcb, ping_recv, session);

    *out_handle = session;
    return ESP_OK;
}

esp_err_t esp_ping_start(esp_ping_handle_t handle)
{
    ping_session_t *session = handle;
    if (session == NULL || session->running) {
        return ESP_ERR_INVALID_STATE;
    }
    session->running = true;
    session->stop = false;
    if (xTaskCreate(ping_task, "ping", 2048, session, tskIDLE_PRIORITY + 1, &session->task) !=
        pdPASS) {
        session->running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t esp_ping_stop(esp_ping_handle_t handle)
{
    ping_session_t *session = handle;
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    session->stop = true;
    session->running = false;
    for (int i = 0; i < 200 && session->task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

esp_err_t esp_ping_delete_session(esp_ping_handle_t handle)
{
    ping_session_t *session = handle;
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)esp_ping_stop(session);
    if (session->pcb != NULL) {
        raw_remove(session->pcb);
        session->pcb = NULL;
    }
    free(session);
    return ESP_OK;
}

esp_err_t esp_ping_get_profile(esp_ping_handle_t handle,
                               esp_ping_profile_t profile,
                               void *data,
                               uint32_t size)
{
    ping_session_t *session = handle;
    if (session == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (profile) {
    case ESP_PING_PROF_SEQNO:
        if (size < sizeof(uint16_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        *(uint16_t *)data = session->seq;
        return ESP_OK;
    case ESP_PING_PROF_TTL:
        if (size < sizeof(uint8_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        *(uint8_t *)data = session->last_ttl;
        return ESP_OK;
    case ESP_PING_PROF_REQUEST:
        if (size < sizeof(uint32_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        *(uint32_t *)data = session->transmitted;
        return ESP_OK;
    case ESP_PING_PROF_REPLY:
        if (size < sizeof(uint32_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        *(uint32_t *)data = session->received;
        return ESP_OK;
    case ESP_PING_PROF_IPADDR:
        if (size < sizeof(ip_addr_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        *(ip_addr_t *)data = session->target;
        return ESP_OK;
    case ESP_PING_PROF_SIZE:
        if (size < sizeof(uint32_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        *(uint32_t *)data = session->config.data_size;
        return ESP_OK;
    case ESP_PING_PROF_TIMEGAP:
        if (size < sizeof(uint32_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        *(uint32_t *)data = session->last_elapsed_ms;
        return ESP_OK;
    case ESP_PING_PROF_DURATION:
        if (size < sizeof(uint32_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        *(uint32_t *)data = session->duration_ms;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
