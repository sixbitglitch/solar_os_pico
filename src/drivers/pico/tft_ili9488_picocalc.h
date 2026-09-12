/*
 * tft_ili9488_picocalc.h - 320x320 ILI9488 panel on the Clockwork PicoCalc.
 *
 * Modelled on src/drivers/tft_ili9341.c/.h: u8g2 is reused as-is (vendored,
 * portable C under components/u8g2/src/clib), and this driver supplies the
 * u8x8 byte/display callbacks rather than reimplementing a renderer.
 *
 * TWO THINGS DIFFER FROM THE ILI9341 DRIVER, AND BOTH MATTER:
 *
 * 1. PIXEL FORMAT. ILI9488 over 4-wire SPI cannot accept 16bpp data for a
 *    memory write - the controller only supports 18bpp (3 bytes per pixel,
 *    RGB666) on the serial interface. This is why clockworkpi's own reference
 *    driver sets COLMOD to 0x66 rather than 0x55. The surface pipeline above
 *    this driver stays RGB565 (foreground_rgb565 / background_rgb565, matching
 *    every other colour panel in the tree); conversion to RGB666 happens in
 *    the SPI write path. Choosing RGB565 on the wire instead would compile and
 *    then produce a garbled display.
 *
 * 2. BACKLIGHT. The PicoCalc's LCD backlight is not connected to a Pico GPIO.
 *    It is driven by the STM32 co-processor and set by writing register 0x05
 *    (REG_ID_BKL) over the keyboard I2C link. So brightness control routes
 *    through src/drivers/pico/picocalc_keyboard.c, and needs the keyboard
 *    driver to have attached first.
 *
 * Init sequence source: github.com/clockworkpi/PicoCalc,
 * Code/picocalc_helloworld/lcdspi/lcdspi.c, pico_lcd_init() ILI9488 branch.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 0 or 1: which RP2350 SPI block. The PicoCalc wires the panel to spi1. */
    uint8_t spi_index;
    uint8_t sck_pin;
    uint8_t mosi_pin;
    uint8_t miso_pin;
    uint8_t cs_pin;
    uint8_t dc_pin;
    int reset_pin; /* -1 if unwired */
    uint32_t spi_clock_hz;
    uint16_t width;
    uint16_t height;
    uint16_t col_offset;
    uint16_t row_offset;
    uint8_t madctl;
    /* True when brightness is set through the STM32 co-processor rather than
     * a GPIO. Always true on the PicoCalc. */
    bool backlight_via_coprocessor;
    const u8g2_cb_t *rotation;
} tft_ili9488_config_t;

typedef struct {
    u8g2_t u8g2;
    uint8_t *buffer;      /* u8g2 tile buffer */
    uint8_t *line_buffer; /* one tile row expanded to RGB666 */
    size_t buffer_size;
    size_t line_buffer_size;
    uint16_t foreground_rgb565;
    uint16_t background_rgb565;
    uint16_t tile_width;
    uint16_t tile_height;
    uint8_t backlight_percent;
    bool ready;
    esp_err_t last_error;
    tft_ili9488_config_t config;
    u8x8_display_info_t display_info;
} tft_ili9488_t;

esp_err_t tft_ili9488_init(tft_ili9488_t *display, const tft_ili9488_config_t *config);
esp_err_t tft_ili9488_resume(tft_ili9488_t *display);
void tft_ili9488_deinit(tft_ili9488_t *display);
u8g2_t *tft_ili9488_get_u8g2(tft_ili9488_t *display);

bool tft_ili9488_backlight_supported(const tft_ili9488_t *display);
esp_err_t tft_ili9488_get_backlight(const tft_ili9488_t *display, uint8_t *percent);
esp_err_t tft_ili9488_set_backlight(tft_ili9488_t *display, uint8_t percent);

esp_err_t tft_ili9488_set_colors(tft_ili9488_t *display,
                                 uint32_t foreground_rgb888,
                                 uint32_t background_rgb888);

esp_err_t tft_ili9488_present_surface(tft_ili9488_t *display,
                                      const solar_os_display_surface_t *surface);
esp_err_t tft_ili9488_present_frame(tft_ili9488_t *display,
                                    const solar_os_display_raster_t *frame);

#ifdef __cplusplus
}
#endif
