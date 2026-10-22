#pragma once

#include "esp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_transport_handle_t esp_transport_ssl_init(void);
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t t,
                                         transport_crt_bundle_attach_t attach_fn);

#ifdef __cplusplus
}
#endif
