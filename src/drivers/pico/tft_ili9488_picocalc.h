/*
 * tft_ili9488_picocalc.h - 320x320 colour panel on the Clockwork PicoCalc.
 *
 * The panel controller has shipped as more than one part across PicoCalc
 * production. clockworkpi's older reference driver treated it as ILI9488 in
 * 18bpp (COLMOD 0x66); current units and independent ports that actually run
 * on hardware (e.g. jeffory/PicOS) identify it as ST7365P and drive it in
 * 16bpp RGB565 (COLMOD 0x55). Both share the same SPI pins, MADCTL, and the
 * MIPI DCS command set.
 *
 * Default wire format is RGB565 (0x55). Set config.colmod to 0x66 only for
 * units that need the old ILI9488 18bpp path.
 *
 * Backlight is driven by the STM32 co-processor (REG_ID_BKL over keyboard
 * I2C), not a Pico GPIO - see picocalc_keyboard.c.
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

#define TFT_PICOCALC_COLMOD_RGB565 0x55U
#define TFT_PICOCALC_COLMOD_RGB666 0x66U

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
    /* COLMOD value: TFT_PICOCALC_COLMOD_RGB565 (default) or _RGB666. */
    uint8_t colmod;
    /* True when brightness is set through the STM32 co-processor rather than
     * a GPIO. Always true on the PicoCalc. */
    bool backlight_via_coprocessor;
    const u8g2_cb_t *rotation;
} tft_ili9488_config_t;

typedef struct {
    u8g2_t u8g2;
    uint8_t *buffer;      /* u8g2 tile buffer */
    uint8_t *line_buffer; /* one scanline expanded to wire pixels */
    size_t buffer_size;
    size_t line_buffer_size;
    uint16_t foreground_rgb565;
    uint16_t background_rgb565;
    uint16_t tile_width;
    uint16_t tile_height;
    uint8_t backlight_percent;
    uint8_t bytes_per_pixel; /* 2 for RGB565, 3 for RGB666 */
    bool ready;
    /* After a full-buffer flush on tile (0,0), ignore the rest of that SendBuffer. */
    bool skip_remaining_tiles;
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
