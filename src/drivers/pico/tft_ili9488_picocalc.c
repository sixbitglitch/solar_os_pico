/*
 * tft_ili9488_picocalc.c - PicoCalc 320x320 colour panel driver.
 *
 * Current PicoCalc units ship an ST7365P driven in RGB565 (COLMOD 0x55), which
 * is what jeffory/PicOS and other on-device ports use. Older reference code
 * treated the panel as ILI9488 in 18bpp (COLMOD 0x66). Sending 3 bytes/pixel
 * to a panel expecting 2 produces a black screen with sparse random pixels -
 * exactly the failure mode observed on hardware when this driver still forced
 * 0x66.
 *
 * Wire format is selected by config.colmod. Pixel packing always starts from
 * RGB565 in the SolarOS pipeline; conversion to the wire format happens here.
 */

#include "tft_ili9488_picocalc.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "picocalc_keyboard.h"

static const char *TAG = "ili9488";

#define ILI9488_SWRESET 0x01
#define ILI9488_SLPOUT  0x11
#define ILI9488_INVON   0x21
#define ILI9488_INVOFF  0x20
#define ILI9488_DISPON  0x29
#define ILI9488_CASET   0x2A
#define ILI9488_PASET   0x2B
#define ILI9488_RAMWR   0x2C
#define ILI9488_MADCTL  0x36
#define ILI9488_COLMOD  0x3A
#define ILI9488_RDDID   0x04

#define TILE_PIXELS 8U

static tft_ili9488_t *active_display;

static spi_inst_t *spi_for(const tft_ili9488_t *display)
{
    return display->config.spi_index == 0 ? spi0 : spi1;
}

static uint16_t rgb888_to_rgb565(uint32_t rgb888)
{
    return (uint16_t)(((rgb888 >> 8) & 0xF800U) |
                      ((rgb888 >> 5) & 0x07E0U) |
                      ((rgb888 >> 3) & 0x001FU));
}

/*
 * Expand one RGB565 pixel into the three bytes ILI9488 expects in 18bpp mode.
 * Top 6 bits of each byte are significant.
 */
static inline void rgb565_to_rgb666(uint16_t value, uint8_t *out)
{
    const uint8_t r5 = (uint8_t)((value >> 11) & 0x1FU);
    const uint8_t g6 = (uint8_t)((value >> 5) & 0x3FU);
    const uint8_t b5 = (uint8_t)(value & 0x1FU);

    const uint8_t r6 = (uint8_t)((r5 << 1) | (r5 >> 4));
    const uint8_t b6 = (uint8_t)((b5 << 1) | (b5 >> 4));

    out[0] = (uint8_t)(r6 << 2);
    out[1] = (uint8_t)(g6 << 2);
    out[2] = (uint8_t)(b6 << 2);
}

/* ST7365P / RGB565 over SPI wants big-endian 16-bit pixels. */
static inline void rgb565_to_be16(uint16_t value, uint8_t *out)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)(value & 0xFFU);
}

static inline void pack_pixel(const tft_ili9488_t *display, uint16_t value, uint8_t *out)
{
    if (display->bytes_per_pixel == 2U) {
        rgb565_to_be16(value, out);
    } else {
        rgb565_to_rgb666(value, out);
    }
}

static void select(tft_ili9488_t *display)
{
    gpio_put(display->config.cs_pin, 0);
}

static void deselect(tft_ili9488_t *display)
{
    gpio_put(display->config.cs_pin, 1);
}

static void write_command(tft_ili9488_t *display, uint8_t command)
{
    gpio_put(display->config.dc_pin, 0);
    spi_write_blocking(spi_for(display), &command, 1);
    gpio_put(display->config.dc_pin, 1);
}

static void write_data(tft_ili9488_t *display, const uint8_t *data, size_t length)
{
    gpio_put(display->config.dc_pin, 1);
    spi_write_blocking(spi_for(display), data, length);
}

static void write_data_byte(tft_ili9488_t *display, uint8_t value)
{
    write_data(display, &value, 1);
}

static void write_command_data(tft_ili9488_t *display,
                               uint8_t command,
                               const uint8_t *data,
                               size_t length)
{
    write_command(display, command);
    if (length > 0) {
        write_data(display, data, length);
    }
}

static void set_window(tft_ili9488_t *display,
                       uint16_t x,
                       uint16_t y,
                       uint16_t width,
                       uint16_t height)
{
    const uint16_t x0 = x + display->config.col_offset;
    const uint16_t x1 = (uint16_t)(x0 + width - 1U);
    const uint16_t y0 = y + display->config.row_offset;
    const uint16_t y1 = (uint16_t)(y0 + height - 1U);

    const uint8_t columns[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1,
    };
    const uint8_t rows[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1,
    };

    write_command_data(display, ILI9488_CASET, columns, sizeof(columns));
    write_command_data(display, ILI9488_PASET, rows, sizeof(rows));
    write_command(display, ILI9488_RAMWR);
}

static void panel_reset(tft_ili9488_t *display)
{
    if (display->config.reset_pin < 0) {
        select(display);
        write_command(display, ILI9488_SWRESET);
        deselect(display);
        sleep_ms(150);
        return;
    }

    const uint pin = (uint)display->config.reset_pin;
    /* Pulse low then release, matching working on-device ports. */
    gpio_put(pin, 0);
    sleep_ms(10);
    gpio_put(pin, 1);
    sleep_ms(120);

    select(display);
    write_command(display, ILI9488_SWRESET);
    deselect(display);
    sleep_ms(10);
}

/*
 * Pulse one command (and optional params) with CS high between transactions.
 * Matches PicOS / Picoware: they never hold CS across the SLPOUT delay.
 */
static void panel_cmd(tft_ili9488_t *display, uint8_t command)
{
    select(display);
    write_command(display, command);
    deselect(display);
}

static void panel_cmd_byte(tft_ili9488_t *display, uint8_t command, uint8_t value)
{
    select(display);
    write_command(display, command);
    write_data_byte(display, value);
    deselect(display);
}

/*
 * ST7365P / current PicoCalc path. Validated by independent on-device ports
 * (PicOS, Picoware) against the same Pimoroni Pico Plus 2 W + PicoCalc pairing.
 */
static void panel_init_rgb565(tft_ili9488_t *display)
{
    panel_cmd_byte(display, ILI9488_COLMOD, TFT_PICOCALC_COLMOD_RGB565);
    panel_cmd_byte(display, ILI9488_MADCTL, display->config.madctl);
    /*
     * PicOS/Picoware enable INVON. Some units look optically inverted with it;
     * board manifest can force off via SOLAR_OS_BOARD_DISPLAY_INVON=0.
     */
#if !defined(SOLAR_OS_BOARD_DISPLAY_INVON) || SOLAR_OS_BOARD_DISPLAY_INVON
    panel_cmd(display, ILI9488_INVON);
#else
    panel_cmd(display, ILI9488_INVOFF);
#endif
    panel_cmd_byte(display, 0xB7, 0xC6); /* Entry Mode Set */

    panel_cmd(display, ILI9488_SLPOUT);
    sleep_ms(10); /* PicOS / Picoware: >= 5 ms after SLPOUT */
    panel_cmd(display, ILI9488_DISPON);
}

/*
 * Legacy ILI9488 18bpp path, transcribed from clockworkpi's pico_lcd_init()
 * #ifdef ILI9488 branch. Keep for units that still need COLMOD 0x66.
 */
static void panel_init_rgb666(tft_ili9488_t *display)
{
    static const uint8_t positive_gamma[15] = {
        0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
        0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
    };
    static const uint8_t negative_gamma[15] = {
        0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
        0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
    };
    static const uint8_t power_control_1[2] = {0x17, 0x15};
    static const uint8_t vcom_control[3] = {0x00, 0x12, 0x80};
    static const uint8_t display_function[3] = {0x02, 0x02, 0x3B};
    static const uint8_t adjust_control_3[4] = {0xA9, 0x51, 0x2C, 0x82};

    select(display);

    write_command_data(display, 0xE0, positive_gamma, sizeof(positive_gamma));
    write_command_data(display, 0xE1, negative_gamma, sizeof(negative_gamma));
    write_command_data(display, 0xC0, power_control_1, sizeof(power_control_1));

    write_command(display, 0xC1);
    write_data_byte(display, 0x41);

    write_command_data(display, 0xC5, vcom_control, sizeof(vcom_control));

    write_command(display, ILI9488_MADCTL);
    write_data_byte(display, display->config.madctl);

    write_command(display, ILI9488_COLMOD);
    write_data_byte(display, TFT_PICOCALC_COLMOD_RGB666);

    write_command(display, 0xB0);
    write_data_byte(display, 0x00);

    write_command(display, 0xB1);
    write_data_byte(display, 0xA0);

    write_command(display, ILI9488_INVON);

    write_command(display, 0xB4);
    write_data_byte(display, 0x02);

    write_command_data(display, 0xB6, display_function, sizeof(display_function));

    write_command(display, 0xB7);
    write_data_byte(display, 0xC6);

    write_command(display, 0xE9);
    write_data_byte(display, 0x00);

    write_command_data(display, 0xF7, adjust_control_3, sizeof(adjust_control_3));

    write_command(display, ILI9488_SLPOUT);
    sleep_ms(120);
    write_command(display, ILI9488_DISPON);
    sleep_ms(120);

    deselect(display);
}

static void panel_init_sequence(tft_ili9488_t *display)
{
    if (display->bytes_per_pixel == 2U) {
        panel_init_rgb565(display);
    } else {
        panel_init_rgb666(display);
    }
}

static void log_panel_id(tft_ili9488_t *display)
{
    uint8_t id[4] = {0};
    select(display);
    write_command(display, ILI9488_RDDID);
    /* Dummy clock then three ID bytes; many panels ignore MISO - best effort. */
    spi_read_blocking(spi_for(display), 0x00, id, sizeof(id));
    deselect(display);
    ESP_LOGI(TAG, "panel RDDID: %02x %02x %02x %02x (COLMOD 0x%02x, %u bpp)",
             id[0], id[1], id[2], id[3],
             display->config.colmod,
             (unsigned)(display->bytes_per_pixel * 8U));
}

static esp_err_t fill_rect(tft_ili9488_t *display,
                           uint16_t x,
                           uint16_t y,
                           uint16_t width,
                           uint16_t height,
                           uint16_t colour)
{
    if (width == 0 || height == 0) {
        return ESP_OK;
    }

    uint8_t pixel[3];
    pack_pixel(display, colour, pixel);
    const size_t bpp = display->bytes_per_pixel;

    enum { RUN_PIXELS = 64 };
    uint8_t run[RUN_PIXELS * 3];
    for (size_t i = 0; i < RUN_PIXELS; i++) {
        memcpy(&run[i * bpp], pixel, bpp);
    }

    select(display);
    set_window(display, x, y, width, height);

    uint32_t remaining = (uint32_t)width * height;
    while (remaining > 0) {
        const uint32_t chunk = remaining > RUN_PIXELS ? RUN_PIXELS : remaining;
        write_data(display, run, (size_t)chunk * bpp);
        remaining -= chunk;
    }

    deselect(display);
    return ESP_OK;
}

static esp_err_t draw_tile_run(tft_ili9488_t *display,
                               const uint8_t *tile_data,
                               uint8_t x_tile,
                               uint8_t y_tile,
                               uint8_t count)
{
    if (count == 0) {
        return ESP_OK;
    }

    const uint16_t width = (uint16_t)(count * TILE_PIXELS);
    const size_t bpp = display->bytes_per_pixel;
    const size_t row_bytes = (size_t)width * bpp;
    if (display->line_buffer == NULL || display->line_buffer_size < row_bytes) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t foreground[3];
    uint8_t background[3];
    pack_pixel(display, display->foreground_rgb565, foreground);
    pack_pixel(display, display->background_rgb565, background);

    select(display);
    set_window(display,
               (uint16_t)(x_tile * TILE_PIXELS),
               (uint16_t)(y_tile * TILE_PIXELS),
               width,
               TILE_PIXELS);

    /*
     * Match tft_ili9341 expand helper: bit clear -> foreground (ink),
     * bit set -> background (paper). Terminal fills paper with draw-color 1.
     */
    for (uint8_t row = 0; row < TILE_PIXELS; row++) {
        uint8_t *out = display->line_buffer;
        for (uint8_t column = 0; column < width; column++) {
            const uint8_t bits = tile_data[column];
            const bool lit = (bits & (uint8_t)(1U << row)) != 0;
            memcpy(out, lit ? background : foreground, bpp);
            out += bpp;
        }
        write_data(display, display->line_buffer, row_bytes);
    }

    deselect(display);
    return ESP_OK;
}

/*
 * Blit the whole u8g2 tile buffer in one GRAM window.
 *
 * Per-tile DRAW_TILE (40x40 = 1600 windows) is too slow on hardware_spi and
 * looks like a hang during splash/terminal present. Full-buffer SendBuffer
 * always starts at tile (0,0); we flush once there and skip the rest.
 */
static esp_err_t flush_full_u8g2_buffer(tft_ili9488_t *display)
{
    if (display->buffer == NULL || display->line_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t width = display->config.width;
    const uint16_t height = display->config.height;
    const size_t bpp = display->bytes_per_pixel;
    const size_t row_bytes = (size_t)width * bpp;
    if (display->line_buffer_size < row_bytes) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t foreground[3];
    uint8_t background[3];
    pack_pixel(display, display->foreground_rgb565, foreground);
    pack_pixel(display, display->background_rgb565, background);

    const size_t tile_row_stride = (size_t)display->tile_width * TILE_PIXELS;

    select(display);
    set_window(display, 0, 0, width, height);

    for (uint16_t y = 0; y < height; y++) {
        const uint16_t tile_y = (uint16_t)(y / TILE_PIXELS);
        const uint8_t bit = (uint8_t)(1U << (y % TILE_PIXELS));
        const uint8_t *tile_row =
            display->buffer + (size_t)tile_y * tile_row_stride;
        uint8_t *out = display->line_buffer;

        for (uint16_t x = 0; x < width; x++) {
            const uint16_t tile_x = (uint16_t)(x / TILE_PIXELS);
            const uint8_t column_byte =
                tile_row[(size_t)tile_x * TILE_PIXELS + (x % TILE_PIXELS)];
            const bool lit = (column_byte & bit) != 0;
            memcpy(out, lit ? background : foreground, bpp);
            out += bpp;
        }

        write_data(display, display->line_buffer, row_bytes);
    }

    deselect(display);
    return ESP_OK;
}

static uint8_t u8x8_byte_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)message;
    (void)arg_int;
    (void)arg_ptr;
    return 1;
}

static uint8_t u8x8_display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int, void *arg_ptr)
{
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        if (active_display == NULL) {
            return 0;
        }
        u8x8_d_helper_display_setup_memory(u8x8, &active_display->display_info);
        return 1;
    }

    tft_ili9488_t *display = active_display;
    if (display == NULL) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        u8x8_d_helper_display_init(u8x8);
        return 1;

    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        (void)tft_ili9488_set_backlight(display, arg_int ? 0 : 100);
        return 1;

    case U8X8_MSG_DISPLAY_SET_FLIP_MODE:
    case U8X8_MSG_DISPLAY_SET_CONTRAST:
        return 1;

    case U8X8_MSG_DISPLAY_DRAW_TILE: {
        const u8x8_tile_t *tile = arg_ptr;
        if (tile == NULL || tile->tile_ptr == NULL || tile->cnt == 0) {
            return 1;
        }

        /*
         * Full-buffer present always begins at (0,0). One GRAM window for the
         * whole panel; ignore the remaining ~1599 tile callbacks.
         */
        if (tile->x_pos == 0 && tile->y_pos == 0) {
            const esp_err_t err = flush_full_u8g2_buffer(display);
            display->skip_remaining_tiles = true;
            if (err != ESP_OK) {
                display->last_error = err;
                display->skip_remaining_tiles = false;
                return 0;
            }
            return 1;
        }

        if (display->skip_remaining_tiles) {
            if (tile->y_pos + 1U >= display->tile_height &&
                (uint16_t)tile->x_pos + tile->cnt >= display->tile_width) {
                display->skip_remaining_tiles = false;
            }
            return 1;
        }

        if (tile->x_pos >= display->tile_width || tile->y_pos >= display->tile_height) {
            return 1;
        }
        uint8_t count = tile->cnt;
        if ((uint16_t)tile->x_pos + count > display->tile_width) {
            count = (uint8_t)(display->tile_width - tile->x_pos);
        }
        const esp_err_t err = draw_tile_run(display, tile->tile_ptr, tile->x_pos,
                                            tile->y_pos, count);
        if (err != ESP_OK) {
            display->last_error = err;
            return 0;
        }
        return 1;
    }

    default:
        return 0;
    }
}

esp_err_t tft_ili9488_init(tft_ili9488_t *display, const tft_ili9488_config_t *config)
{
    if (display == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->spi_index > 1 || config->width == 0 || config->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t colmod = config->colmod;
    if (colmod == 0U) {
        colmod = TFT_PICOCALC_COLMOD_RGB565;
    }
    if (colmod != TFT_PICOCALC_COLMOD_RGB565 && colmod != TFT_PICOCALC_COLMOD_RGB666) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->config = *config;
    display->config.colmod = colmod;
    display->bytes_per_pixel = (colmod == TFT_PICOCALC_COLMOD_RGB565) ? 2U : 3U;
    /* Match SolarOS display defaults: black ink on white paper. */
    display->foreground_rgb565 = 0x0000U;
    display->background_rgb565 = 0xFFFFU;
    display->backlight_percent = 100U;

    display->tile_width = (uint16_t)(config->width / TILE_PIXELS);
    display->tile_height = (uint16_t)(config->height / TILE_PIXELS);
    if (display->tile_width == 0 || display->tile_height == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    display->buffer_size = (size_t)display->tile_width * display->tile_height * TILE_PIXELS;
    display->buffer = calloc(1, display->buffer_size);
    if (display->buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Allocate for the wider of the two formats so mode switches stay safe. */
    display->line_buffer_size = (size_t)config->width * 3U;
    display->line_buffer = calloc(1, display->line_buffer_size);
    if (display->line_buffer == NULL) {
        free(display->buffer);
        display->buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    spi_init(spi_for(display), config->spi_clock_hz);
    spi_set_format(spi_for(display), 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(config->sck_pin, GPIO_FUNC_SPI);
    gpio_set_function(config->mosi_pin, GPIO_FUNC_SPI);
    gpio_set_function(config->miso_pin, GPIO_FUNC_SPI);

    gpio_init(config->cs_pin);
    gpio_set_dir(config->cs_pin, GPIO_OUT);
    gpio_put(config->cs_pin, 1);

    gpio_init(config->dc_pin);
    gpio_set_dir(config->dc_pin, GPIO_OUT);
    gpio_put(config->dc_pin, 1);

    if (config->reset_pin >= 0) {
        gpio_init((uint)config->reset_pin);
        gpio_set_dir((uint)config->reset_pin, GPIO_OUT);
        gpio_put((uint)config->reset_pin, 1);
    }

    panel_reset(display);
    ESP_LOGI(TAG, "panel reset done");
    panel_init_sequence(display);
    ESP_LOGI(TAG, "panel init sequence done (COLMOD 0x%02x)", display->config.colmod);
    log_panel_id(display);

    display->display_info = (u8x8_display_info_t){
        .chip_enable_level = 0,
        .chip_disable_level = 1,
        .reset_pulse_width_ms = 20,
        .post_reset_wait_ms = 120,
        .sck_clock_hz = config->spi_clock_hz,
        .spi_mode = 0,
        .i2c_bus_clock_100kHz = 4,
        .tile_width = (uint8_t)display->tile_width,
        .tile_height = (uint8_t)display->tile_height,
        .pixel_width = config->width,
        .pixel_height = config->height,
    };

    active_display = display;
    u8g2_SetupDisplay(&display->u8g2, u8x8_display_cb, u8x8_dummy_cb, u8x8_byte_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->buffer,
                     (uint8_t)display->tile_height,
                     u8g2_ll_hvline_vertical_top_lsb,
                     config->rotation != NULL ? config->rotation : U8G2_R0);
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);

    /*
     * Brief black clear so bring-up can tell SPI works without leaving the
     * RGB diagnostic bars on screen through splash (matches the default
     * white-on-black terminal palette).
     */
    (void)fill_rect(display, 0, 0, config->width, config->height, 0x0000U);
    ESP_LOGI(TAG, "cleared panel to black");

    display->ready = true;
    ESP_LOGI(TAG,
             "panel %ux%u on spi%u at %lu Hz, COLMOD 0x%02x (%u bpp), MADCTL 0x%02x",
             (unsigned)config->width,
             (unsigned)config->height,
             (unsigned)config->spi_index,
             (unsigned long)config->spi_clock_hz,
             display->config.colmod,
             (unsigned)(display->bytes_per_pixel * 8U),
             config->madctl);
    return ESP_OK;
}

esp_err_t tft_ili9488_resume(tft_ili9488_t *display)
{
    if (display == NULL || !display->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    panel_reset(display);
    panel_init_sequence(display);
    u8g2_SetPowerSave(&display->u8g2, 0);
    return ESP_OK;
}

void tft_ili9488_deinit(tft_ili9488_t *display)
{
    if (display == NULL) {
        return;
    }
    if (display->ready) {
        spi_deinit(spi_for(display));
    }
    free(display->buffer);
    free(display->line_buffer);
    display->buffer = NULL;
    display->line_buffer = NULL;
    display->ready = false;
    if (active_display == display) {
        active_display = NULL;
    }
}

u8g2_t *tft_ili9488_get_u8g2(tft_ili9488_t *display)
{
    return display != NULL ? &display->u8g2 : NULL;
}

bool tft_ili9488_backlight_supported(const tft_ili9488_t *display)
{
    return display != NULL && display->config.backlight_via_coprocessor;
}

esp_err_t tft_ili9488_get_backlight(const tft_ili9488_t *display, uint8_t *percent)
{
    if (display == NULL || percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display->config.backlight_via_coprocessor) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *percent = display->backlight_percent;
    return ESP_OK;
}

esp_err_t tft_ili9488_set_backlight(tft_ili9488_t *display, uint8_t percent)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display->config.backlight_via_coprocessor) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (percent > 100U) {
        percent = 100U;
    }
    const uint8_t level = (uint8_t)(((uint32_t)percent * 255U) / 100U);
    const esp_err_t err = picocalc_kbd_set_lcd_backlight(level);
    if (err != ESP_OK) {
        return err;
    }
    display->backlight_percent = percent;
    return ESP_OK;
}

esp_err_t tft_ili9488_set_colors(tft_ili9488_t *display,
                                 uint32_t foreground_rgb888,
                                 uint32_t background_rgb888)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    display->foreground_rgb565 = rgb888_to_rgb565(foreground_rgb888);
    display->background_rgb565 = rgb888_to_rgb565(background_rgb888);
    return ESP_OK;
}

esp_err_t tft_ili9488_present_surface(tft_ili9488_t *display,
                                      const solar_os_display_surface_t *surface)
{
    if (display == NULL || surface == NULL || surface->data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (surface->width == 0 || surface->height == 0) {
        return ESP_OK;
    }
    if (surface->width > display->config.width ||
        surface->height > display->config.height) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t bpp = display->bytes_per_pixel;
    const size_t row_bytes = (size_t)surface->width * bpp;
    if (display->line_buffer_size < row_bytes) {
        return ESP_ERR_NO_MEM;
    }

    select(display);
    set_window(display, 0, 0, surface->width, surface->height);

    for (uint16_t y = 0; y < surface->height; y++) {
        uint8_t *out = display->line_buffer;

        for (uint16_t x = 0; x < surface->width; x++) {
            uint16_t colour;

            switch (surface->format) {
            case SOLAR_OS_DISPLAY_FORMAT_INDEX8: {
                const size_t offset = (size_t)y * surface->stride + x;
                if (offset >= surface->data_size) {
                    deselect(display);
                    return ESP_ERR_INVALID_SIZE;
                }
                const uint8_t index = surface->data[offset];
                colour = (surface->palette_rgb565 != NULL && index < surface->palette_size)
                             ? surface->palette_rgb565[index]
                             : display->background_rgb565;
                break;
            }
            case SOLAR_OS_DISPLAY_FORMAT_MONO1: {
                const size_t offset = (size_t)y * surface->stride + (x / 8U);
                if (offset >= surface->data_size) {
                    deselect(display);
                    return ESP_ERR_INVALID_SIZE;
                }
                /* Same polarity as draw_tile_run / ili9341: 1=background. */
                const bool lit = (surface->data[offset] & (1U << (x % 8U))) != 0;
                colour = lit ? display->background_rgb565 : display->foreground_rgb565;
                break;
            }
            default:
                deselect(display);
                return ESP_ERR_NOT_SUPPORTED;
            }

            pack_pixel(display, colour, out);
            out += bpp;
        }

        write_data(display, display->line_buffer, row_bytes);
    }

    deselect(display);
    return ESP_OK;
}

esp_err_t tft_ili9488_present_frame(tft_ili9488_t *display,
                                    const solar_os_display_raster_t *frame)
{
    if (display == NULL || frame == NULL || frame->data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame->width == 0 || frame->height == 0 ||
        frame->source_width == 0 || frame->source_height == 0) {
        return ESP_OK;
    }
    if ((uint32_t)frame->x + frame->width > display->config.width ||
        (uint32_t)frame->y + frame->height > display->config.height) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (frame->clear_background) {
        uint16_t colour = display->background_rgb565;
        if (frame->palette_rgb565 != NULL &&
            frame->background_index < frame->palette_size) {
            colour = frame->palette_rgb565[frame->background_index];
        }
        const esp_err_t err = fill_rect(display, 0, 0, display->config.width,
                                        display->config.height, colour);
        if (err != ESP_OK) {
            return err;
        }
    }

    const size_t bpp = display->bytes_per_pixel;
    const size_t row_bytes = (size_t)frame->width * bpp;
    if (display->line_buffer_size < row_bytes) {
        return ESP_ERR_NO_MEM;
    }

    select(display);
    set_window(display, frame->x, frame->y, frame->width, frame->height);

    for (uint16_t y = 0; y < frame->height; y++) {
        const uint16_t src_y =
            (uint16_t)(((uint32_t)y * frame->source_height) / frame->height);
        uint8_t *out = display->line_buffer;

        for (uint16_t x = 0; x < frame->width; x++) {
            const uint16_t src_x =
                (uint16_t)(((uint32_t)x * frame->source_width) / frame->width);
            uint16_t colour;

            switch (frame->format) {
            case SOLAR_OS_DISPLAY_FORMAT_INDEX8: {
                const size_t offset = (size_t)src_y * frame->source_stride + src_x;
                if (offset >= frame->data_size) {
                    deselect(display);
                    return ESP_ERR_INVALID_SIZE;
                }
                const uint8_t index = frame->data[offset];
                colour = (frame->palette_rgb565 != NULL && index < frame->palette_size)
                             ? frame->palette_rgb565[index]
                             : display->background_rgb565;
                break;
            }
            case SOLAR_OS_DISPLAY_FORMAT_MONO1: {
                const size_t offset =
                    (size_t)src_y * frame->source_stride + (src_x / 8U);
                if (offset >= frame->data_size) {
                    deselect(display);
                    return ESP_ERR_INVALID_SIZE;
                }
                /* Same polarity as draw_tile_run / ili9341: 1=background. */
                const bool lit = (frame->data[offset] & (1U << (src_x % 8U))) != 0;
                const bool paper = frame->palette_inverted ? !lit : lit;
                colour = paper ? display->background_rgb565
                               : display->foreground_rgb565;
                break;
            }
            default:
                deselect(display);
                return ESP_ERR_NOT_SUPPORTED;
            }

            pack_pixel(display, colour, out);
            out += bpp;
        }

        write_data(display, display->line_buffer, row_bytes);
    }

    deselect(display);
    return ESP_OK;
}
