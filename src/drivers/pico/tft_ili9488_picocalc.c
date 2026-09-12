/*
 * tft_ili9488_picocalc.c - ILI9488 320x320 panel driver for the PicoCalc.
 *
 * See tft_ili9488_picocalc.h for the two ways this deliberately differs from
 * the ILI9341 reference driver (18bpp wire format, co-processor backlight).
 *
 * Never run on hardware. The init sequence is transcribed from clockworkpi's
 * own reference driver, but nothing about SPI timing, refresh behaviour or
 * the panel's actual window offsets has been observed.
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

/* --- ILI9488 commands ---------------------------------------------------- */
#define ILI9488_SWRESET 0x01
#define ILI9488_SLPOUT  0x11
#define ILI9488_INVON   0x21
#define ILI9488_DISPON  0x29
#define ILI9488_CASET   0x2A
#define ILI9488_PASET   0x2B
#define ILI9488_RAMWR   0x2C
#define ILI9488_MADCTL  0x36
#define ILI9488_COLMOD  0x3A

/*
 * 0x66 = 18 bits/pixel. This is NOT a preference: ILI9488's 4-wire serial
 * interface does not implement the 16bpp memory-write format at all.
 */
#define ILI9488_COLMOD_18BPP 0x66

/* u8g2 tiles are 8x8 monochrome. */
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
 *
 * The controller takes the top 6 bits of each byte. RGB565's 5-bit red/blue
 * are widened to 6 bits by replicating the most significant bit into the new
 * low bit, which keeps full white at full white (a plain left-shift would make
 * 0x1F map to 0x3E, so white would read slightly grey).
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

/* --- SPI plumbing -------------------------------------------------------- */

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

/* Command followed by its parameter bytes, CS held across the pair. */
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

/* Open a GRAM window. Coordinates are inclusive on both ends. */
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

/* --- Panel initialisation ------------------------------------------------ */

/*
 * Transcribed from clockworkpi's pico_lcd_init(). The gamma tables and the
 * power/VCOM/frame-rate values are panel-specific and are NOT interchangeable
 * with an ILI9341 sequence, which is why they are reproduced verbatim rather
 * than adapted.
 */
static void panel_reset(tft_ili9488_t *display)
{
    if (display->config.reset_pin < 0) {
        write_command(display, ILI9488_SWRESET);
        sleep_ms(150);
        return;
    }

    const uint pin = (uint)display->config.reset_pin;
    gpio_put(pin, 1);
    sleep_ms(10);
    gpio_put(pin, 0);
    sleep_ms(10);
    gpio_put(pin, 1);
    /* The reference driver waits 200 ms here; the datasheet's minimum is
     * 120 ms after reset release before the first command. */
    sleep_ms(200);
}

static void panel_init_sequence(tft_ili9488_t *display)
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

    write_command(display, 0xC1); /* Power Control 2 */
    write_data_byte(display, 0x41);

    write_command_data(display, 0xC5, vcom_control, sizeof(vcom_control));

    write_command(display, ILI9488_MADCTL);
    write_data_byte(display, display->config.madctl);

    write_command(display, ILI9488_COLMOD);
    write_data_byte(display, ILI9488_COLMOD_18BPP);

    write_command(display, 0xB0); /* Interface Mode Control */
    write_data_byte(display, 0x00);

    write_command(display, 0xB1); /* Frame Rate Control */
    write_data_byte(display, 0xA0);

    /* The reference driver enables inversion on this panel. Without it the
     * image comes out as a photographic negative. */
    write_command(display, ILI9488_INVON);

    write_command(display, 0xB4); /* Display Inversion Control */
    write_data_byte(display, 0x02);

    write_command_data(display, 0xB6, display_function, sizeof(display_function));

    write_command(display, 0xB7); /* Entry Mode Set */
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

/* --- Painting ------------------------------------------------------------ */

/* Fill a rectangle with one colour, streaming it out without a full buffer. */
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
    rgb565_to_rgb666(colour, pixel);

    /* One reusable run of pixels keeps the transfer count down without
     * allocating a whole-screen buffer. */
    enum { RUN_PIXELS = 64 };
    uint8_t run[RUN_PIXELS * 3];
    for (size_t i = 0; i < RUN_PIXELS; i++) {
        memcpy(&run[i * 3], pixel, sizeof(pixel));
    }

    select(display);
    set_window(display, x, y, width, height);

    uint32_t remaining = (uint32_t)width * height;
    while (remaining > 0) {
        const uint32_t chunk = remaining > RUN_PIXELS ? RUN_PIXELS : remaining;
        write_data(display, run, (size_t)chunk * 3U);
        remaining -= chunk;
    }

    deselect(display);
    return ESP_OK;
}

/*
 * Paint one u8g2 tile run.
 *
 * u8g2 hands over `count` 8x8 tiles as 8 bytes each, one byte per column, bit
 * 0 = top pixel. They are expanded to an 8-row by (count*8)-column RGB666
 * block and pushed as a single window.
 */
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
    const size_t row_bytes = (size_t)width * 3U;
    if (display->line_buffer == NULL || display->line_buffer_size < row_bytes) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t foreground[3];
    uint8_t background[3];
    rgb565_to_rgb666(display->foreground_rgb565, foreground);
    rgb565_to_rgb666(display->background_rgb565, background);

    select(display);
    set_window(display,
               (uint16_t)(x_tile * TILE_PIXELS),
               (uint16_t)(y_tile * TILE_PIXELS),
               width,
               TILE_PIXELS);

    for (uint8_t row = 0; row < TILE_PIXELS; row++) {
        uint8_t *out = display->line_buffer;
        for (uint8_t column = 0; column < width; column++) {
            const uint8_t bits = tile_data[column];
            const bool lit = (bits & (uint8_t)(1U << row)) != 0;
            memcpy(out, lit ? foreground : background, 3);
            out += 3;
        }
        write_data(display, display->line_buffer, row_bytes);
    }

    deselect(display);
    return ESP_OK;
}

/* --- u8x8 callbacks ------------------------------------------------------ */

/*
 * u8g2 is reused unmodified; these callbacks are the whole integration
 * surface. The byte callback is a no-op because this driver talks to the
 * panel directly rather than routing u8x8's byte protocol - u8g2 only ever
 * reaches the panel here through DRAW_TILE.
 */
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
        /* Backlight is the only power control available: the panel itself is
         * left on so the framebuffer contents survive. */
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

/* --- Public API ---------------------------------------------------------- */

esp_err_t tft_ili9488_init(tft_ili9488_t *display, const tft_ili9488_config_t *config)
{
    if (display == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->spi_index > 1 || config->width == 0 || config->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->config = *config;
    display->foreground_rgb565 = 0xFFFFU;
    display->background_rgb565 = 0x0000U;
    display->backlight_percent = 100U;

    display->tile_width = (uint16_t)(config->width / TILE_PIXELS);
    display->tile_height = (uint16_t)(config->height / TILE_PIXELS);
    if (display->tile_width == 0 || display->tile_height == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* u8g2 full-frame buffer: one byte per 8-pixel column per tile row. */
    display->buffer_size = (size_t)display->tile_width * display->tile_height * TILE_PIXELS;
    display->buffer = calloc(1, display->buffer_size);
    if (display->buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* One full pixel row of the widest possible tile run, in RGB666. */
    display->line_buffer_size = (size_t)config->width * 3U;
    display->line_buffer = calloc(1, display->line_buffer_size);
    if (display->line_buffer == NULL) {
        free(display->buffer);
        display->buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* SPI + control pins. CS and DC are software-driven so a command and its
     * data can share one chip-select assertion. */
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
    panel_init_sequence(display);

    /* u8x8 geometry. */
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

    /* Clear to background so the panel does not show power-on noise. */
    (void)fill_rect(display, 0, 0, config->width, config->height,
                    display->background_rgb565);

    display->ready = true;
    ESP_LOGI(TAG,
             "ILI9488 %ux%u on spi%u at %lu Hz, COLMOD 0x%02x (18bpp), MADCTL 0x%02x",
             (unsigned)config->width,
             (unsigned)config->height,
             (unsigned)config->spi_index,
             (unsigned long)config->spi_clock_hz,
             ILI9488_COLMOD_18BPP,
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
    /*
     * REG_ID_BKL takes 0..255. The STM32 firmware's LCD_BACKLIGHT_STEP is 16,
     * so the useful resolution is coarser than one percent, but sending the
     * scaled value lets the co-processor do its own quantisation.
     */
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

/*
 * Present an indexed/mono surface.
 *
 * Only the formats this panel's pipeline actually produces are handled:
 * INDEX8 with an RGB565 palette (the terminal's colour path) and MONO1 (the
 * u8g2 path). INDEX2 is rejected rather than silently mis-rendered.
 */
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

    const size_t row_bytes = (size_t)surface->width * 3U;
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
                /* Least-significant pixel first within each byte. */
                const size_t offset = (size_t)y * surface->stride + (x / 8U);
                if (offset >= surface->data_size) {
                    deselect(display);
                    return ESP_ERR_INVALID_SIZE;
                }
                /* Unlike solar_os_display_raster_t, the surface type carries
                 * no palette_inverted flag - inversion for surfaces is applied
                 * upstream by the presenter. */
                const bool lit = (surface->data[offset] & (1U << (x % 8U))) != 0;
                colour = lit ? display->foreground_rgb565 : display->background_rgb565;
                break;
            }
            default:
                deselect(display);
                return ESP_ERR_NOT_SUPPORTED;
            }

            rgb565_to_rgb666(colour, out);
            out += 3;
        }

        write_data(display, display->line_buffer, row_bytes);
    }

    deselect(display);
    return ESP_OK;
}

/*
 * Present a raster at a position, with optional nearest-neighbour scaling from
 * the source geometry to the destination rectangle.
 */
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

    const size_t row_bytes = (size_t)frame->width * 3U;
    if (display->line_buffer_size < row_bytes) {
        return ESP_ERR_NO_MEM;
    }

    select(display);
    set_window(display, frame->x, frame->y, frame->width, frame->height);

    for (uint16_t y = 0; y < frame->height; y++) {
        /* Nearest neighbour: map destination row to source row. */
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
                const bool lit = (frame->data[offset] & (1U << (src_x % 8U))) != 0;
                const bool on = frame->palette_inverted ? !lit : lit;
                colour = on ? display->foreground_rgb565 : display->background_rgb565;
                break;
            }
            default:
                deselect(display);
                return ESP_ERR_NOT_SUPPORTED;
            }

            rgb565_to_rgb666(colour, out);
            out += 3;
        }

        write_data(display, display->line_buffer, row_bytes);
    }

    deselect(display);
    return ESP_OK;
}
