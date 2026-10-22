/*
 * esp_crt_bundle.h - stub for ESP-IDF certificate bundle attach hook.
 *
 * On ESP32, esp_crt_bundle_attach wires Mozilla's CA store into mbedTLS. The
 * Pico port has no bundled store yet; the attach hook configures verification
 * to optional so HTTPS/TLS can come up during bring-up.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_crt_bundle_attach(void *conf);

#ifdef __cplusplus
}
#endif
