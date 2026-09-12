/*
 * hal/adc_types.h - pico-sdk compatibility shim.
 *
 * services/solar_os_adc.c describes channels and attenuation in these terms.
 * RP2350's SAR ADC has a fixed 0-3.3 V range with no programmable
 * attenuator, so only the 12 dB (full-scale) entry is meaningful; the others
 * exist so the shared source's switch statements stay total.
 */
#pragma once

typedef enum {
    ADC_UNIT_1 = 0,
    ADC_UNIT_2 = 1,
} adc_unit_t;

typedef enum {
    ADC_CHANNEL_0 = 0,
    ADC_CHANNEL_1,
    ADC_CHANNEL_2,
    ADC_CHANNEL_3,
    ADC_CHANNEL_4,
    ADC_CHANNEL_5,
    ADC_CHANNEL_6,
    ADC_CHANNEL_7,
    ADC_CHANNEL_8,
    ADC_CHANNEL_9,
} adc_channel_t;

typedef enum {
    ADC_ATTEN_DB_0 = 0,
    ADC_ATTEN_DB_2_5 = 1,
    ADC_ATTEN_DB_6 = 2,
    /* The only attenuation RP2350 actually has: the full 0-3.3 V input span. */
    ADC_ATTEN_DB_12 = 3,
} adc_atten_t;

typedef enum {
    ADC_BITWIDTH_DEFAULT = 0,
    ADC_BITWIDTH_9 = 9,
    ADC_BITWIDTH_10 = 10,
    ADC_BITWIDTH_11 = 11,
    /* RP2350's SAR ADC is 12-bit. */
    ADC_BITWIDTH_12 = 12,
    ADC_BITWIDTH_13 = 13,
} adc_bitwidth_t;
