/*
 * esp_netif.h - pico-sdk compatibility shim.
 *
 * There is no network stack in this target: the net package (CYW43439 driver
 * + lwIP) is deferred, and every wifi-gated flavour group is off. This header
 * exists only so that sources which include it for a type definition still
 * compile; nothing here connects to anything.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_netif_obj esp_netif_t;

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

static inline esp_err_t esp_netif_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{ (void)key; return NULL; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *netif,
                                              esp_netif_ip_info_t *info)
{ (void)netif; (void)info; return ESP_ERR_NOT_SUPPORTED; }

#ifdef __cplusplus
}
#endif
