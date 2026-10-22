/*
 * solar_os_wifi_pico.c - CYW43439 / lwIP backend for SolarOS on Pico Plus 2 W.
 *
 * Implements the solar_os_wifi.h STA + scan surface on
 * pico_cyw43_arch_lwip_sys_freertos. SoftAP / NAT / repeater / connectionless
 * paths return ESP_ERR_NOT_SUPPORTED.
 */

#include "solar_os_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netif.h"
#include "nvs.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

static const char *TAG = "wifi-pico";

#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_ENABLED_KEY "enabled"
#define WIFI_CONNECT_TIMEOUT_MS 30000U
#define WIFI_SCAN_TIMEOUT_MS 10000U

typedef struct {
    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char password[SOLAR_OS_WIFI_PASSWORD_MAX];
} wifi_saved_profile_t;

static bool wifi_ready;
static bool wifi_started;
static bool wifi_enabled_boot = true;
static bool wifi_enabled_next = true;
static bool wifi_boot_policy_loaded;
static solar_os_wifi_state_t wifi_state = SOLAR_OS_WIFI_STATE_OFF;
static char wifi_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
static char wifi_ip[16];
static char wifi_gateway[16];
static char wifi_netmask[16];
static int8_t wifi_rssi;
static uint8_t wifi_channel;
static wifi_saved_profile_t wifi_profiles[SOLAR_OS_WIFI_PROFILE_MAX];
static uint8_t wifi_profile_count;
static solar_os_wifi_ap_t wifi_scan_buf[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
static size_t wifi_scan_count;
static bool wifi_scan_busy;
static SemaphoreHandle_t wifi_lock;

static void wifi_strlcpy(char *dst, const char *src, size_t dst_size)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}

static void wifi_lock_take(void)
{
    if (wifi_lock != NULL) {
        xSemaphoreTake(wifi_lock, portMAX_DELAY);
    }
}

static void wifi_lock_give(void)
{
    if (wifi_lock != NULL) {
        xSemaphoreGive(wifi_lock);
    }
}

static void wifi_load_boot_policy(void)
{
    if (wifi_boot_policy_loaded) {
        return;
    }
    wifi_boot_policy_loaded = true;
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t enabled = 1;
    if (nvs_get_u8(nvs, WIFI_NVS_ENABLED_KEY, &enabled) == ESP_OK) {
        wifi_enabled_boot = enabled != 0;
        wifi_enabled_next = wifi_enabled_boot;
    }
    nvs_close(nvs);
}

static void wifi_format_ip4(char *out, size_t out_len, uint32_t addr)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    snprintf(out,
             out_len,
             "%u.%u.%u.%u",
             (unsigned)(addr & 0xffU),
             (unsigned)((addr >> 8) & 0xffU),
             (unsigned)((addr >> 16) & 0xffU),
             (unsigned)((addr >> 24) & 0xffU));
}

static void wifi_refresh_netif(void)
{
    wifi_ip[0] = '\0';
    wifi_gateway[0] = '\0';
    wifi_netmask[0] = '\0';

    struct netif *netif = netif_default;
    if (netif == NULL || !netif_is_up(netif) || !netif_is_link_up(netif)) {
        return;
    }
    if (!ip4_addr_isany_val(*netif_ip4_addr(netif))) {
        wifi_format_ip4(wifi_ip, sizeof(wifi_ip), netif_ip4_addr(netif)->addr);
        wifi_format_ip4(wifi_gateway, sizeof(wifi_gateway), netif_ip4_gw(netif)->addr);
        wifi_format_ip4(wifi_netmask, sizeof(wifi_netmask), netif_ip4_netmask(netif)->addr);
        if (wifi_state == SOLAR_OS_WIFI_STATE_CONNECTING ||
            wifi_state == SOLAR_OS_WIFI_STATE_CONNECTED) {
            wifi_state = SOLAR_OS_WIFI_STATE_CONNECTED;
        }
    }
}

static int wifi_scan_result_cb(void *env, const cyw43_ev_scan_result_t *result)
{
    (void)env;
    if (result == NULL) {
        return 0;
    }
    wifi_lock_take();
    if (wifi_scan_count < SOLAR_OS_WIFI_SCAN_MAX_RESULTS) {
        solar_os_wifi_ap_t *ap = &wifi_scan_buf[wifi_scan_count++];
        memset(ap, 0, sizeof(*ap));
        size_t copy = result->ssid_len;
        if (copy > SOLAR_OS_WIFI_SSID_MAX) {
            copy = SOLAR_OS_WIFI_SSID_MAX;
        }
        memcpy(ap->ssid, result->ssid, copy);
        ap->ssid[copy] = '\0';
        ap->rssi = (int8_t)result->rssi;
        ap->channel = (uint8_t)result->channel;
        ap->hidden = (result->ssid_len == 0);
        if ((result->auth_mode & 0x04) != 0) {
            wifi_strlcpy(ap->auth, "WPA2", sizeof(ap->auth));
        } else if ((result->auth_mode & 0x02) != 0) {
            wifi_strlcpy(ap->auth, "WPA", sizeof(ap->auth));
        } else {
            wifi_strlcpy(ap->auth, "OPEN", sizeof(ap->auth));
        }
    }
    wifi_lock_give();
    return 0;
}

static esp_err_t wifi_remember_profile(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < wifi_profile_count; i++) {
        if (strncmp(wifi_profiles[i].ssid, ssid, sizeof(wifi_profiles[i].ssid)) == 0) {
            if (password != NULL) {
                wifi_strlcpy(wifi_profiles[i].password,
                        password,
                        sizeof(wifi_profiles[i].password));
            }
            return ESP_OK;
        }
    }
    if (wifi_profile_count >= SOLAR_OS_WIFI_PROFILE_MAX) {
        memmove(&wifi_profiles[0],
                &wifi_profiles[1],
                sizeof(wifi_profiles[0]) * (SOLAR_OS_WIFI_PROFILE_MAX - 1U));
        wifi_profile_count = SOLAR_OS_WIFI_PROFILE_MAX - 1U;
    }
    wifi_strlcpy(wifi_profiles[wifi_profile_count].ssid, ssid, sizeof(wifi_profiles[0].ssid));
    wifi_strlcpy(wifi_profiles[wifi_profile_count].password,
            password != NULL ? password : "",
            sizeof(wifi_profiles[0].password));
    wifi_profile_count++;
    return ESP_OK;
}

esp_err_t solar_os_wifi_init(void)
{
    wifi_load_boot_policy();
    if (!wifi_enabled_boot) {
        ESP_LOGI(TAG, "Wi-Fi disabled by saved boot setting");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (wifi_ready) {
        return ESP_OK;
    }
    if (wifi_lock == NULL) {
        wifi_lock = xSemaphoreCreateMutex();
        if (wifi_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (cyw43_arch_init() != 0) {
        ESP_LOGE(TAG, "cyw43_arch_init failed");
        return ESP_FAIL;
    }
    cyw43_arch_enable_sta_mode();
    wifi_ready = true;
    wifi_started = true;
    wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
    ESP_LOGI(TAG, "CYW43439 STA ready");
    return ESP_OK;
}

bool solar_os_wifi_enabled_for_current_boot(void)
{
    wifi_load_boot_policy();
    return wifi_enabled_boot;
}

bool solar_os_wifi_enabled_for_next_boot(void)
{
    wifi_load_boot_policy();
    return wifi_enabled_next;
}

esp_err_t solar_os_wifi_set_enabled_for_next_boot(bool enabled)
{
    wifi_load_boot_policy();
    wifi_enabled_next = enabled;
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return ESP_OK;
    }
    (void)nvs_set_u8(nvs, WIFI_NVS_ENABLED_KEY, enabled ? 1U : 0U);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t solar_os_wifi_start(void)
{
    esp_err_t err = solar_os_wifi_init();
    if (err == ESP_ERR_NOT_ALLOWED) {
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }
    wifi_lock_take();
    cyw43_arch_enable_sta_mode();
    wifi_started = true;
    if (wifi_state == SOLAR_OS_WIFI_STATE_OFF) {
        wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
    }
    wifi_lock_give();
    return ESP_OK;
}

esp_err_t solar_os_wifi_stop(void)
{
    if (!wifi_ready) {
        return ESP_OK;
    }
    wifi_lock_take();
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_disable_sta_mode();
    wifi_started = false;
    wifi_state = SOLAR_OS_WIFI_STATE_OFF;
    wifi_ssid[0] = '\0';
    wifi_ip[0] = '\0';
    wifi_lock_give();
    return ESP_OK;
}

esp_err_t solar_os_wifi_prepare_sleep(void)
{
    return solar_os_wifi_stop();
}

esp_err_t solar_os_wifi_resume(void)
{
    return solar_os_wifi_start();
}

esp_netif_t *solar_os_wifi_get_sta_netif(void)
{
    return NULL;
}

esp_err_t solar_os_wifi_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    wifi_lock_take();
    wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
    wifi_strlcpy(wifi_ssid, ssid, sizeof(wifi_ssid));
    wifi_ip[0] = '\0';
    wifi_lock_give();

    const uint32_t auth = (password != NULL && password[0] != '\0')
                              ? CYW43_AUTH_WPA2_AES_PSK
                              : CYW43_AUTH_OPEN;
    const int rc = cyw43_arch_wifi_connect_timeout_ms(
        ssid, password, auth, WIFI_CONNECT_TIMEOUT_MS);

    wifi_lock_take();
    if (rc == 0) {
        (void)wifi_remember_profile(ssid, password);
        wifi_refresh_netif();
        if (wifi_ip[0] == '\0') {
            /* Link up; DHCP may still be settling. */
            wifi_state = SOLAR_OS_WIFI_STATE_CONNECTED;
        }
        err = ESP_OK;
        ESP_LOGI(TAG, "joined %s ip=%s", ssid, wifi_ip[0] != '\0' ? wifi_ip : "(dhcp)");
    } else {
        wifi_state = SOLAR_OS_WIFI_STATE_FAILED;
        wifi_ssid[0] = '\0';
        err = ESP_FAIL;
        ESP_LOGW(TAG, "join %s failed (%d)", ssid, rc);
    }
    wifi_lock_give();
    return err;
}

esp_err_t solar_os_wifi_connect_saved(void)
{
    if (wifi_profile_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const wifi_saved_profile_t *profile = &wifi_profiles[wifi_profile_count - 1U];
    return solar_os_wifi_connect(profile->ssid, profile->password);
}

esp_err_t solar_os_wifi_disconnect(void)
{
    if (!wifi_ready) {
        return ESP_OK;
    }
    wifi_lock_take();
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    wifi_state = SOLAR_OS_WIFI_STATE_DISCONNECTED;
    wifi_ssid[0] = '\0';
    wifi_ip[0] = '\0';
    wifi_lock_give();
    return ESP_OK;
}

esp_err_t solar_os_wifi_forget(void)
{
    if (wifi_profile_count == 0) {
        return ESP_OK;
    }
    wifi_profile_count--;
    memset(&wifi_profiles[wifi_profile_count], 0, sizeof(wifi_profiles[0]));
    return ESP_OK;
}

esp_err_t solar_os_wifi_forget_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < wifi_profile_count; i++) {
        if (strncmp(wifi_profiles[i].ssid, ssid, sizeof(wifi_profiles[i].ssid)) == 0) {
            memmove(&wifi_profiles[i],
                    &wifi_profiles[i + 1],
                    sizeof(wifi_profiles[0]) * (wifi_profile_count - i - 1U));
            wifi_profile_count--;
            memset(&wifi_profiles[wifi_profile_count], 0, sizeof(wifi_profiles[0]));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_wifi_forget_all(void)
{
    memset(wifi_profiles, 0, sizeof(wifi_profiles));
    wifi_profile_count = 0;
    return ESP_OK;
}

esp_err_t solar_os_wifi_known(solar_os_wifi_profile_t *profiles,
                              size_t max_profiles,
                              size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = wifi_profile_count;
    if (n > max_profiles) {
        n = max_profiles;
    }
    if (profiles != NULL) {
        for (size_t i = 0; i < n; i++) {
            memset(&profiles[i], 0, sizeof(profiles[i]));
            wifi_strlcpy(profiles[i].ssid, wifi_profiles[i].ssid, sizeof(profiles[i].ssid));
            profiles[i].preferred = (i + 1U == wifi_profile_count);
        }
    }
    *count = n;
    return ESP_OK;
}

bool solar_os_wifi_is_known_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < wifi_profile_count; i++) {
        if (strncmp(wifi_profiles[i].ssid, ssid, sizeof(wifi_profiles[i].ssid)) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_wifi_ap_start(const char *ssid, const char *password, const char *auth)
{
    (void)ssid;
    (void)password;
    (void)auth;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_ap_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_ap_saved_get(solar_os_wifi_ap_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_ap_save(const char *ssid, const char *password, const char *auth)
{
    (void)ssid;
    (void)password;
    (void)auth;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_ap_forget(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_nat_set(bool enabled)
{
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_repeater_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_repeater_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_scan(solar_os_wifi_ap_t *aps, size_t max_aps, size_t *found)
{
    if (found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    wifi_lock_take();
    wifi_scan_count = 0;
    wifi_scan_busy = true;
    wifi_state = SOLAR_OS_WIFI_STATE_SCANNING;
    wifi_lock_give();

    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, wifi_scan_result_cb) != 0) {
        wifi_lock_take();
        wifi_scan_busy = false;
        wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
        wifi_lock_give();
        return ESP_FAIL;
    }

    const absolute_time_t deadline = make_timeout_time_ms(WIFI_SCAN_TIMEOUT_MS);
    while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(deadline)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    wifi_lock_take();
    wifi_scan_busy = false;
    if (wifi_state == SOLAR_OS_WIFI_STATE_SCANNING) {
        wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
    }
    size_t n = wifi_scan_count;
    if (n > max_aps) {
        n = max_aps;
    }
    if (aps != NULL && n > 0) {
        memcpy(aps, wifi_scan_buf, n * sizeof(aps[0]));
    }
    *found = n;
    wifi_lock_give();
    return ESP_OK;
}

esp_err_t solar_os_wifi_scan_start_async(void)
{
    size_t found = 0;
    return solar_os_wifi_scan(NULL, 0, &found);
}

esp_err_t solar_os_wifi_scan_results(solar_os_wifi_ap_t *aps, size_t max_aps, size_t *found)
{
    if (found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_lock_take();
    size_t n = wifi_scan_count;
    if (n > max_aps) {
        n = max_aps;
    }
    if (aps != NULL && n > 0) {
        memcpy(aps, wifi_scan_buf, n * sizeof(aps[0]));
    }
    *found = n;
    const bool busy = wifi_scan_busy;
    wifi_lock_give();
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t solar_os_wifi_scan_cancel_async(void)
{
    return ESP_OK;
}

esp_err_t solar_os_wifi_connectionless_acquire(const char *owner,
                                               uint8_t requested_channel,
                                               uint8_t *actual_channel)
{
    (void)owner;
    (void)requested_channel;
    (void)actual_channel;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_connectionless_release(const char *owner)
{
    (void)owner;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_wifi_latency_acquire(const char *owner)
{
    (void)owner;
    return ESP_OK;
}

esp_err_t solar_os_wifi_latency_release(const char *owner)
{
    (void)owner;
    return ESP_OK;
}

void solar_os_wifi_get_status(solar_os_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    wifi_lock_take();
    if (wifi_ready && wifi_started) {
        wifi_refresh_netif();
        const int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_JOIN || link == CYW43_LINK_UP) {
            if (wifi_state != SOLAR_OS_WIFI_STATE_CONNECTED) {
                wifi_state = SOLAR_OS_WIFI_STATE_CONNECTED;
            }
        }
    }
    status->state = wifi_state;
    status->initialized = wifi_ready;
    status->started = wifi_started;
    status->connected = (wifi_state == SOLAR_OS_WIFI_STATE_CONNECTED);
    status->has_ip = (wifi_ip[0] != '\0');
    status->has_saved_config = (wifi_profile_count > 0);
    status->rssi = wifi_rssi;
    status->channel = wifi_channel;
    status->saved_profile_count = wifi_profile_count;
    wifi_strlcpy(status->ssid, wifi_ssid, sizeof(status->ssid));
    wifi_strlcpy(status->ip, wifi_ip, sizeof(status->ip));
    wifi_strlcpy(status->gateway, wifi_gateway, sizeof(status->gateway));
    wifi_strlcpy(status->netmask, wifi_netmask, sizeof(status->netmask));
    if (wifi_profile_count > 0) {
        wifi_strlcpy(status->saved_ssid,
                wifi_profiles[wifi_profile_count - 1U].ssid,
                sizeof(status->saved_ssid));
    }
    wifi_lock_give();
}

void solar_os_wifi_get_status_text(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    if (!status.initialized) {
        snprintf(buffer, len, "wifi off");
        return;
    }
    if (status.connected && status.has_ip) {
        snprintf(buffer, len, "%s %s", status.ssid, status.ip);
        return;
    }
    snprintf(buffer, len, "%s", solar_os_wifi_state_name(status.state));
}

const char *solar_os_wifi_state_name(solar_os_wifi_state_t state)
{
    switch (state) {
    case SOLAR_OS_WIFI_STATE_OFF:
        return "off";
    case SOLAR_OS_WIFI_STATE_IDLE:
        return "idle";
    case SOLAR_OS_WIFI_STATE_SCANNING:
        return "scanning";
    case SOLAR_OS_WIFI_STATE_CONNECTING:
        return "connecting";
    case SOLAR_OS_WIFI_STATE_CONNECTED:
        return "connected";
    case SOLAR_OS_WIFI_STATE_DISCONNECTED:
        return "disconnected";
    case SOLAR_OS_WIFI_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
