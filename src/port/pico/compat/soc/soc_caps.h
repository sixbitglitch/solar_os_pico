/*
 * soc/soc_caps.h - pico-sdk compatibility shim.
 *
 * RP2350B peripheral counts, in the shape services/solar_os_expansion.c
 * expects.
 */
#pragma once

#define SOC_I2C_NUM         2
#define SOC_SPI_PERIPH_NUM  2
#define SOC_UART_NUM        2
#define SOC_ADC_PERIPH_NUM  1
/* ADC0-ADC7 on GPIO40-47; channel 8 is the internal temperature sensor. */
#define SOC_ADC_MAX_CHANNEL_NUM 8
#define SOC_GPIO_PIN_COUNT  48
#define SOC_I2S_NUM         0
