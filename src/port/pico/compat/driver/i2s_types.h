/*
 * driver/i2s_types.h - pico-sdk compatibility shim.
 *
 * Included by the generated board profile unconditionally. The PicoCalc port
 * has no I2S audio path in this pass (the audio package is deferred), so this
 * only has to satisfy the include and the runtime I2S port mask, which the
 * manifest leaves empty.
 */
#pragma once

typedef int i2s_port_t;

#define I2S_NUM_0 0
#define I2S_NUM_1 1
#define I2S_NUM_AUTO (-1)
