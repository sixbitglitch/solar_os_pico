/*
 * solar_os_board_display_ili9488_picocalc.c - board display ops for the
 * PicoCalc's ILI9488 panel.
 *
 * Fills in the solar_os_board_display_ops vtable declared in
 * src/board/solar_os_board_display.h, exactly as the ESP32 TFT service does in
 * src/services/solar_os_tft_display.c. Everything above this file - the
 * display service, the frame presenter, the TUI - talks only to that vtable,
 * so no code above the board layer knows which panel is fitted.
 */

#include "solar_os_board_display.h"

#include <string.h>

#include "esp_log.h"
#include "solar_os_board.h"
#include "tft_ili9488_picocalc.h"

static const char *TAG = "board-display";

#ifndef SOLAR_OS_BOARD_PIN_LCD_RST
#define SOLAR_OS_BOARD_PIN_LCD_RST (-1)
#endif

static tft_ili9488_t panel;
static solar_os_board_display_t board_display;

static esp_err_t ops_runtime_ready(solar_os_board_display_t *display)
{
    (void)display;
    return ESP_OK;
}

static esp_err_t ops_resume(solar_os_board_display_t *display)
{
    return tft_ili9488_resume(display->driver);
}

static void ops_deinit(solar_os_board_display_t *display)
{
    tft_ili9488_deinit(display->driver);
    display->ready = false;
}

static bool ops_brightness_supported(const solar_os_board_display_t *display)
{
    return tft_ili9488_backlight_supported(display->driver);
}

static esp_err_t ops_get_brightness(const solar_os_board_display_t *display,
                                    uint8_t *percent)
{
    return tft_ili9488_get_backlight(display->driver, percent);
}

static esp_err_t ops_set_brightness(solar_os_board_display_t *display, uint8_t percent)
{
    return tft_ili9488_set_backlight(display->driver, percent);
}

static esp_err_t ops_set_colors(solar_os_board_display_t *display,
                                uint32_t foreground_rgb888,
                                uint32_t background_rgb888)
{
    return tft_ili9488_set_colors(display->driver, foreground_rgb888, background_rgb888);
}

static const char *ops_controller_mode(const solar_os_board_display_t *display)
{
    (void)display;
    /* ILI9488 has no reflective/transmissive mode switch the way the ST7305
     * does; the panel runs one way. */
    return "color";
}

static const char *ops_controller_mode_values(const solar_os_board_display_t *display)
{
    (void)display;
    return "color";
}

static esp_err_t ops_set_controller_mode(solar_os_board_display_t *display,
                                         const char *mode)
{
    (void)display;
    if (mode != NULL && strcmp(mode, "color") == 0) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ops_set_high_refresh_override(solar_os_board_display_t *display,
                                               bool enabled,
                                               uint16_t hz_tenths)
{
    (void)display;
    (void)enabled;
    (void)hz_tenths;
    /* No partial/high-refresh mode to override on this controller. */
    return ESP_ERR_NOT_SUPPORTED;
}

/*
 * Present a monochrome XBM. Wrapped into a MONO1 surface so there is exactly
 * one blit implementation in the driver rather than two that can drift.
 */
static esp_err_t ops_present_mono_xbm(solar_os_board_display_t *display,
                                      const uint8_t *bitmap,
                                      size_t bitmap_size,
                                      uint16_t x,
                                      uint16_t y,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t stride,
                                      bool palette_inverted)
{
    if (bitmap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_display_raster_t raster = {
        .data = bitmap,
        .data_size = bitmap_size,
        .source_width = width,
        .source_height = height,
        .source_stride = stride,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .format = SOLAR_OS_DISPLAY_FORMAT_MONO1,
        .palette_inverted = palette_inverted,
    };
    return tft_ili9488_present_frame(display->driver, &raster);
}

static esp_err_t ops_present_surface(solar_os_board_display_t *display,
                                     const solar_os_display_surface_t *surface)
{
    return tft_ili9488_present_surface(display->driver, surface);
}

static esp_err_t ops_present_frame(solar_os_board_display_t *display,
                                   const solar_os_display_raster_t *frame)
{
    return tft_ili9488_present_frame(display->driver, frame);
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = ops_runtime_ready,
    .resume = ops_resume,
    .deinit = ops_deinit,
    .brightness_supported = ops_brightness_supported,
    .get_brightness = ops_get_brightness,
    .set_brightness = ops_set_brightness,
    .set_colors = ops_set_colors,
    .controller_mode = ops_controller_mode,
    .controller_mode_values = ops_controller_mode_values,
    .set_controller_mode = ops_set_controller_mode,
    .set_high_refresh_override = ops_set_high_refresh_override,
    .present_mono_xbm = ops_present_mono_xbm,
    .present_surface = ops_present_surface,
    .present_frame = ops_present_frame,
};

esp_err_t solar_os_board_display_init(solar_os_board_display_t *display)
{
    if (display == NULL) {
        display = &board_display;
    }

    const tft_ili9488_config_t config = {
        .spi_index = 1, /* PicoCalc wires the panel to spi1 */
        .sck_pin = SOLAR_OS_BOARD_PIN_LCD_SCK,
        .mosi_pin = SOLAR_OS_BOARD_PIN_LCD_MOSI,
        .miso_pin = SOLAR_OS_BOARD_PIN_LCD_MISO,
        .cs_pin = SOLAR_OS_BOARD_PIN_LCD_CS,
        .dc_pin = SOLAR_OS_BOARD_PIN_LCD_DC,
        .reset_pin = SOLAR_OS_BOARD_PIN_LCD_RST,
        .spi_clock_hz = SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
        .width = SOLAR_OS_BOARD_DISPLAY_WIDTH,
        .height = SOLAR_OS_BOARD_DISPLAY_HEIGHT,
        .col_offset = SOLAR_OS_BOARD_DISPLAY_COL_OFFSET,
        .row_offset = SOLAR_OS_BOARD_DISPLAY_ROW_OFFSET,
        .madctl = SOLAR_OS_BOARD_DISPLAY_MADCTL,
        .backlight_via_coprocessor = true,
        .rotation = SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION,
    };

    const esp_err_t err = tft_ili9488_init(&panel, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }

    display->ops = &display_ops;
    display->driver = &panel;
    display->driver_name = SOLAR_OS_BOARD_DISPLAY_DRIVER_NAME;
    display->controller = SOLAR_OS_BOARD_DISPLAY_CONTROLLER;
    display->u8g2 = tft_ili9488_get_u8g2(&panel);
    display->width = SOLAR_OS_BOARD_DISPLAY_WIDTH;
    display->height = SOLAR_OS_BOARD_DISPLAY_HEIGHT;
    display->surface_formats =
        SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    display->frame_formats =
        SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    /*
     * Throughput estimate, NOT a measurement. At 50 MHz (see
     * SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ's comment in the board manifest)
     * with 3 bytes per pixel a full 320x320 frame is ~102 KB, so roughly
     * 60 fps is the theoretical ceiling before per-transfer overhead. 38 fps
     * is advertised to leave headroom for that overhead; this is still a
     * hardware_spi polling transfer, not the PIO+DMA double-buffered
     * approach that reaches a full 60 fps in community ports of this same
     * hardware (see doc/ports/picocalc.md section 7.4) - and none of it has
     * run on hardware.
     */
    display->preferred_stream_fps = 38;
    display->max_stream_pixels_per_second = 320U * 320U * 38U;
    display->ready = true;

    return ESP_OK;
}

/*
 * The rest of the solar_os_board_display.h contract, below.
 *
 * On ESP32 boards these are provided once, generically, by
 * src/board/solar_os_board_display_expansion.c, because a display there is a
 * pluggable expansion module: solar_os_board_display_init() above is instead
 * a *getter* that copies whichever display an expansion driver most recently
 * registered via solar_os_board_display_register_primary(). This board's
 * ILI9488 is fixed hardware, soldered to the mainboard like the keyboard -
 * the same reason solar_os_board_battery_adc.c and
 * solar_os_board_storage_sd.c implement their contracts directly rather than
 * through a registry - so solar_os_board_display_init() above does the real
 * bring-up itself and board_display_expansion.c is not built for this board
 * (see boards/drivers/display_ili9488_picocalc.cmake: including both is a
 * duplicate-symbol link error, since both define
 * solar_os_board_display_init()).
 *
 * Every function below, however, is a pure dispatch through *display (or a
 * plain field read) with no dependency on that registry, so it is copied
 * here verbatim rather than reinvented: same contract, same behaviour, one
 * fewer file linked for a board that does not need the indirection.
 */

esp_err_t solar_os_board_display_runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL && display->ops->runtime_ready != NULL ?
        display->ops->runtime_ready(display) : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL && display->ops->resume != NULL ?
        display->ops->resume(display) : ESP_ERR_INVALID_STATE;
}

void solar_os_board_display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->ops != NULL && display->ops->deinit != NULL) {
        display->ops->deinit(display);
    }
}

u8g2_t *solar_os_board_display_u8g2(solar_os_board_display_t *display)
{
    return display != NULL ? display->u8g2 : NULL;
}

const char *solar_os_board_display_driver_name(const solar_os_board_display_t *display)
{
    return display != NULL && display->driver_name != NULL ? display->driver_name : "unknown";
}

const char *solar_os_board_display_controller(const solar_os_board_display_t *display)
{
    return display != NULL && display->controller != NULL ? display->controller : "unknown";
}

uint16_t solar_os_board_display_width(const solar_os_board_display_t *display)
{
    return display != NULL ? display->width : 0;
}

uint16_t solar_os_board_display_height(const solar_os_board_display_t *display)
{
    return display != NULL ? display->height : 0;
}

bool solar_os_board_display_ready(const solar_os_board_display_t *display)
{
    return display != NULL && display->ready;
}

uint32_t solar_os_board_display_surface_formats(
    const solar_os_board_display_t *display)
{
    return display != NULL ? display->surface_formats : 0U;
}

uint32_t solar_os_board_display_frame_formats(
    const solar_os_board_display_t *display)
{
    return display != NULL ? display->frame_formats : 0U;
}

uint16_t solar_os_board_display_preferred_stream_fps(
    const solar_os_board_display_t *display)
{
    return display != NULL ? display->preferred_stream_fps : 0U;
}

uint32_t solar_os_board_display_max_stream_pixels_per_second(
    const solar_os_board_display_t *display)
{
    return display != NULL ? display->max_stream_pixels_per_second : 0U;
}

bool solar_os_board_display_brightness_supported(const solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL &&
        display->ops->brightness_supported != NULL &&
        display->ops->brightness_supported(display);
}

esp_err_t solar_os_board_display_get_brightness(const solar_os_board_display_t *display,
                                                uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return display != NULL && display->ops != NULL && display->ops->get_brightness != NULL ?
        display->ops->get_brightness(display, percent) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_set_brightness(solar_os_board_display_t *display,
                                                uint8_t percent)
{
    return display != NULL && display->ops != NULL && display->ops->set_brightness != NULL ?
        display->ops->set_brightness(display, percent) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_set_colors(solar_os_board_display_t *display,
                                            uint32_t foreground_rgb888,
                                            uint32_t background_rgb888)
{
    return display != NULL && display->ops != NULL && display->ops->set_colors != NULL ?
        display->ops->set_colors(display, foreground_rgb888, background_rgb888) : ESP_OK;
}

const char *solar_os_board_display_controller_mode(const solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL && display->ops->controller_mode != NULL ?
        display->ops->controller_mode(display) : NULL;
}

const char *solar_os_board_display_controller_mode_values(const solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL &&
        display->ops->controller_mode_values != NULL ?
        display->ops->controller_mode_values(display) : NULL;
}

esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *display,
                                                     const char *mode)
{
    return display != NULL && display->ops != NULL &&
        display->ops->set_controller_mode != NULL ?
        display->ops->set_controller_mode(display, mode) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_set_high_refresh_override(
    solar_os_board_display_t *display,
    bool enabled,
    uint16_t hz_tenths)
{
    return display != NULL && display->ops != NULL &&
        display->ops->set_high_refresh_override != NULL ?
        display->ops->set_high_refresh_override(display, enabled, hz_tenths) :
        ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_present_mono_xbm(solar_os_board_display_t *display,
                                                  const uint8_t *bitmap,
                                                  size_t bitmap_size,
                                                  uint16_t x,
                                                  uint16_t y,
                                                  uint16_t width,
                                                  uint16_t height,
                                                  uint16_t stride,
                                                  bool palette_inverted)
{
    return display != NULL && display->ops != NULL && display->ops->present_mono_xbm != NULL ?
        display->ops->present_mono_xbm(display,
                                       bitmap,
                                       bitmap_size,
                                       x,
                                       y,
                                       width,
                                       height,
                                       stride,
                                       palette_inverted) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_present_surface(
    solar_os_board_display_t *display,
    const solar_os_display_surface_t *surface)
{
    return display != NULL && display->ops != NULL &&
        display->ops->present_surface != NULL ?
        display->ops->present_surface(display, surface) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_present_frame(
    solar_os_board_display_t *display,
    const solar_os_display_raster_t *frame)
{
    return display != NULL && display->ops != NULL &&
        display->ops->present_frame != NULL ?
        display->ops->present_frame(display, frame) : ESP_ERR_NOT_SUPPORTED;
}
