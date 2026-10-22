#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the PicoCalc co-processor fuel gauge as the SolarOS battery provider. */
esp_err_t solar_os_picocalc_battery_attach(void);
esp_err_t solar_os_picocalc_battery_detach(void);

#ifdef __cplusplus
}
#endif
